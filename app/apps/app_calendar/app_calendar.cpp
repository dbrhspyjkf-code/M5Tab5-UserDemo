#include "app_calendar.h"
#include <mooncake_log.h>
#include <smooth_lvgl.h>
#include <hal/hal.h>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../utils/datetime/lunar_calendar.h"
#include "../utils/theme/theme.h"
#ifndef PLATFORM_BUILD_DESKTOP
#include "../utils/assets/assets_partition.h"
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
// 30px 全字库 (标题/星期/农历日)
static const lv_font_t* zh30()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
// 20px (详情行)
static const lv_font_t* zh20()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh30() { return &font_puhui_20_4; }
static const lv_font_t* zh20() { return &font_puhui_20_4; }
#endif

static const std::string TAG = "calendar";

static constexpr int COLS = 7;                  // 周日~周六
static constexpr int ROWS = 6;                  // 最多 6 行
static constexpr int CELL_W = 176;              // (1280-2*24)/7, 铺满 (2026-10-27)
static constexpr int CELL_H = 84;               // 6 行, 底部留详情行
static constexpr int GRID_W = COLS * CELL_W;
static constexpr int GRID_X = (1280 - GRID_W) / 2;   // = 24

// 星期头
static const char* const kWd[] = {"日", "一", "二", "三", "四", "五", "六"};

AppCalendar::AppCalendar()
{
    setAppInfo().name = "万年历";
}

void AppCalendar::onOpen()
{
    mclog::tagInfo(TAG, "on open");
    if (_scr) return;   // 防重入 (工具页往返, onOpen 会再次触发)
    _alive = std::make_shared<bool>(true);

    // 默认落在今天
    std::time_t t = std::time(nullptr);
    std::tm lt = {};
    localtime_r(&t, &lt);
    _year  = lt.tm_year + 1900;
    _month = lt.tm_mon + 1;
    _detail_day = lt.tm_mday;

    _buildUi();
    lv_screen_load(_scr);
    _installSwipeGesture();
}

void AppCalendar::onClose()
{
    mclog::tagInfo(TAG, "on close");
    _removeSwipeGesture();
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _title = _grid = _detail_lbl = nullptr;
    _year_lbl = _month_lbl = _year_dd = _month_dd = nullptr;
    _day_cells.clear();
    if (_close_cb) _close_cb();
}

// ── 主题色便捷取值 ──────────────────────────────────────────────────────────
namespace {
inline uint32_t cHeader()  { return theme::cur().card[1]; }
inline uint32_t cText()    { return theme::cur().text_hi; }
inline uint32_t cDim()     { return theme::cur().text_dim; }
inline uint32_t cAccent()  { return theme::cur().accent; }
inline uint32_t cDanger()  { return theme::cur().danger; }
}

// 该月天数 (公历)
static int days_in_month(int y, int m)
{
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2) {
        const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return leap ? 29 : 28;
    }
    return d[m - 1];
}

// 该月 1 号是星期几 (0=日)
static int first_weekday(int y, int m)
{
    std::tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon  = m - 1;
    t.tm_mday = 1;
    std::mktime(&t);
    return t.tm_wday;
}

void AppCalendar::_buildUi()
{
    LvglLockGuard lock;
    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── 标题行: << < [2026 年][10 月] > >> ──
    // << >> 翻年, < > 翻月; 年/月文字点击弹下拉直接选 (2026-10-27 用户指定)
    auto mk_arrow = [&](int x, const char* txt, lv_event_cb_t cb) {
        lv_obj_t* b = lv_button_create(_scr);
        lv_obj_set_size(b, 84, 64);
        lv_obj_set_pos(b, x, 28);
        lv_obj_set_style_bg_color(b, lv_color_hex(cHeader()), 0);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);   // ASCII: zh 字体无 FA/SYMBOL 字形
        lv_obj_set_style_text_font(l, zh30(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(cText()), 0);
        lv_obj_center(l);
        return b;
    };
    // 与日期网格左右边缘对齐 (2026-10-27 用户指定)
    mk_arrow(GRID_X,          "<<", _prevYearClick_cb);
    mk_arrow(GRID_X + 100,    "<",  _prevClick_cb);
    mk_arrow(GRID_X + GRID_W - 84 - 100, ">", _nextClick_cb);
    mk_arrow(GRID_X + GRID_W - 84,       ">>", _nextYearClick_cb);

    // 年/月可点击标签 (点击弹下拉)
    _year_lbl = lv_label_create(_scr);
    lv_obj_set_style_text_font(_year_lbl, zh30(), 0);
    lv_obj_set_style_text_color(_year_lbl, lv_color_hex(cText()), 0);
    lv_obj_set_pos(_year_lbl, 436, 42);
    lv_obj_add_flag(_year_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_year_lbl, _yearDd_cb, LV_EVENT_CLICKED, this);

    _month_lbl = lv_label_create(_scr);
    lv_obj_set_style_text_font(_month_lbl, zh30(), 0);
    lv_obj_set_style_text_color(_month_lbl, lv_color_hex(cText()), 0);
    lv_obj_set_pos(_month_lbl, 660, 42);
    lv_obj_add_flag(_month_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_month_lbl, _monthDd_cb, LV_EVENT_CLICKED, this);

    // ── 星期表头 ──
    for (int i = 0; i < COLS; i++) {
        lv_obj_t* w = lv_label_create(_scr);
        lv_label_set_text(w, kWd[i]);
        lv_obj_set_style_text_font(w, zh30(), 0);
        lv_obj_set_style_text_color(w, lv_color_hex(i == 0 || i == 6 ? cDanger() : cDim()), 0);
        lv_obj_set_pos(w, GRID_X + i * CELL_W + CELL_W / 2 - 12, 116);
    }

    // ── 日期网格 (6 行 × 7 列, 每 cell 上下两行: 公历日 + 农历) ──
    _grid = lv_obj_create(_scr);
    lv_obj_set_size(_grid, GRID_W, ROWS * CELL_H);
    lv_obj_set_pos(_grid, GRID_X, 164);
    lv_obj_set_style_bg_opa(_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_grid, 0, 0);
    lv_obj_set_style_pad_all(_grid, 0, 0);
    lv_obj_clear_flag(_grid, LV_OBJ_FLAG_SCROLLABLE);

    _refreshMonth();
}

void AppCalendar::_clearGrid()
{
    for (auto c : _day_cells) {
        if (c) lv_obj_del(c);
    }
    _day_cells.clear();
}

void AppCalendar::_refreshMonth()
{
    LvglLockGuard lock;
    if (!_grid) return;
    _clearGrid();

    // 年/月标签
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "%d 年", _year);
    lv_label_set_text(_year_lbl, tbuf);
    snprintf(tbuf, sizeof(tbuf), "%d 月", _month);
    lv_label_set_text(_month_lbl, tbuf);

    const int first = first_weekday(_year, _month);
    const int ndays = days_in_month(_year, _month);
    std::time_t now_t = std::time(nullptr);
    std::tm now_lt = {};
    localtime_r(&now_t, &now_lt);
    const bool is_current_month = (now_lt.tm_year + 1900 == _year && now_lt.tm_mon + 1 == _month);

    // 农历整月批量换算 (cell: 公历日大字 + 农历小字)
    for (int slot = 0; slot < COLS * ROWS; slot++) {
        const int day = slot - first + 1;
        const bool valid = day >= 1 && day <= ndays;
        const int col = slot % COLS, row = slot / COLS;

        lv_obj_t* cell = lv_obj_create(_grid);
        lv_obj_set_size(cell, CELL_W - 6, CELL_H - 6);
        lv_obj_set_pos(cell, col * CELL_W + 3, row * CELL_H + 3);
        lv_obj_set_style_bg_color(cell, lv_color_hex(cHeader()), 0);
        lv_obj_set_style_radius(cell, 12, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* d_lbl = lv_label_create(cell);
        lv_obj_t* l_lbl = lv_label_create(cell);
        lv_obj_set_style_text_font(d_lbl, zh30(), 0);
        lv_obj_set_style_text_font(l_lbl, zh20(), 0);
        lv_obj_align(d_lbl, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_align(l_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);

        if (!valid) {
            lv_obj_set_style_bg_opa(cell, LV_OPA_30, 0);
            lv_label_set_text(d_lbl, "");
            lv_label_set_text(l_lbl, "");
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        } else {
            // 周末表头红, 日期当天周末字也偏红
            std::tm t = {};
            t.tm_year = _year - 1900;
            t.tm_mon  = _month - 1;
            t.tm_mday = day;
            std::mktime(&t);
            const bool weekend = (t.tm_wday == 0 || t.tm_wday == 6);

            // 农历: 初一显示月份名, 其余显示日
            char lunar_buf[32];
            lunar::LunarDate ld{};
            std::string lunar_txt;
            if (lunar::solar_to_lunar(_year, _month, day, &ld)) {
                static const char* kM[] = {"正月", "二月", "三月", "四月", "五月", "六月",
                                           "七月", "八月", "九月", "十月", "冬月", "腊月"};
                static const char* kD[] = {
                    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
                    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
                    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};
                if (ld.day == 1) {
                    lunar_txt = std::string(ld.is_leap_month ? "闰" : "") + kM[ld.month - 1];
                } else {
                    lunar_txt = (ld.is_leap_month ? "闰" : "") + std::string(kD[ld.day - 1]);
                }
            }

            char db[16];
            snprintf(db, sizeof(db), "%d", day);
            lv_label_set_text(d_lbl, db);
            lv_label_set_text(l_lbl, lunar_txt.c_str());

            // 今天: accent 边框高亮
            if (is_current_month && day == now_lt.tm_mday) {
                lv_obj_set_style_border_width(cell, 3, 0);
                lv_obj_set_style_border_color(cell, lv_color_hex(cAccent()), 0);
            }

            const uint32_t day_col = weekend ? cDanger() : cText();
            lv_obj_set_style_text_color(d_lbl, lv_color_hex(day_col), 0);
            lv_obj_set_style_text_color(l_lbl, lv_color_hex(cDim()), 0);

            lv_obj_set_user_data(cell, (void*)(intptr_t)day);
            lv_obj_add_event_cb(cell, _dayClick_cb, LV_EVENT_CLICKED, this);
        }
        _day_cells.push_back(cell);
    }

    // 底部详情: 默认显示今天 (当月) / 1 号
    int d = (is_current_month) ? now_lt.tm_mday : 1;
    _showDayDetail(d);
}

void AppCalendar::_showDayDetail(int day)
{
    if (!_detail_lbl) {
        _detail_lbl = lv_label_create(_scr);
        lv_obj_set_style_text_font(_detail_lbl, zh20() ? zh20() : zh30(), 0);
        lv_obj_set_style_text_color(_detail_lbl, lv_color_hex(cDim()), 0);
        lv_obj_align(_detail_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);
    }
    static const char* kWdFull[] = {"星期日", "星期一", "星期二", "星期三",
                                    "星期四", "星期五", "星期六"};
    std::tm t = {};
    t.tm_year = _year - 1900;
    t.tm_mon  = _month - 1;
    t.tm_mday = day;
    std::mktime(&t);

    char lunar_buf[32];
    std::string lunar_txt = "农历 --";
    if (lunar::format_lunar_date(_year, _month, day, lunar_buf, sizeof(lunar_buf))) {
        lunar_txt = std::string("农历 ") + lunar_buf;
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "%d年%d月%d日  %s  %s",
             _year, _month, day, kWdFull[t.tm_wday], lunar_txt.c_str());
    lv_label_set_text(_detail_lbl, buf);
}

void AppCalendar::_shiftMonth(int delta)
{
    _month += delta;
    if (_month > 12) { _month = 1; ++_year; }
    if (_month < 1)  { _month = 12; --_year; }
    if (_year < lunar::kBaseYear) { _year = lunar::kBaseYear; _month = 1; }
    if (_year > lunar::kLastYear) { _year = lunar::kLastYear; _month = 12; }
    _refreshMonth();
}

void AppCalendar::_prevClick_cb(lv_event_t* e)
{
    static_cast<AppCalendar*>(lv_event_get_user_data(e))->_shiftMonth(-1);
}

void AppCalendar::_nextClick_cb(lv_event_t* e)
{
    static_cast<AppCalendar*>(lv_event_get_user_data(e))->_shiftMonth(1);
}

void AppCalendar::_shiftYear(int delta)
{
    _year += delta;
    if (_year < lunar::kBaseYear) _year = lunar::kBaseYear;
    if (_year > lunar::kLastYear) _year = lunar::kLastYear;
    _refreshMonth();
}

void AppCalendar::_prevYearClick_cb(lv_event_t* e)
{
    static_cast<AppCalendar*>(lv_event_get_user_data(e))->_shiftYear(-1);
}

void AppCalendar::_nextYearClick_cb(lv_event_t* e)
{
    static_cast<AppCalendar*>(lv_event_get_user_data(e))->_shiftYear(1);
}

namespace {
// 下拉打开后的收尾: 列表字体放大 + 全屏拦截层 (点外部=关闭下拉).
// LVGL dropdown 默认点外部不关闭; 拦截层是最可控的方案.
struct DdShieldCtx {
    AppCalendar* app;
    lv_obj_t*    dd;
};
void finalize_dropdown(AppCalendar* self, lv_obj_t* scr, lv_obj_t* dd)
{
    lv_obj_t* list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_set_style_text_font(list, zh30(), 0);   // 列表数字放大到 30px
        lv_obj_set_style_pad_ver(list, 8, 0);
    }
    lv_obj_t* shield = lv_obj_create(scr);
    lv_obj_set_size(shield, 1280, 720);
    lv_obj_set_pos(shield, 0, 0);
    lv_obj_set_style_bg_opa(shield, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shield, 0, 0);
    lv_obj_clear_flag(shield, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(shield, LV_OBJ_FLAG_CLICKABLE);
    auto* ctx = new DdShieldCtx{self, dd};
    lv_obj_add_event_cb(shield, [](lv_event_t* ev) {
        auto* c = static_cast<DdShieldCtx*>(lv_event_get_user_data(ev));
        lv_dropdown_close(c->dd);                       // 触发 CANCEL → 既有清理
        lv_obj_delete_async(lv_event_get_current_target_obj(ev));
        delete c;
    }, LV_EVENT_CLICKED, ctx);
    lv_obj_move_to_index(shield, 0);   // 垫底 (dropdown list 挂更新层, 在其上)
}
}  // namespace

// 点击"年"标签: 弹全屏年份下拉 (1900-2100, 201 行). 选中即跳转.
void AppCalendar::_yearDd_cb(lv_event_t* e)
{
    auto* self = static_cast<AppCalendar*>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    if (!self->_scr || self->_year_dd) return;

    self->_year_dd = lv_dropdown_create(self->_scr);
    // 注意: 必须用 set_options (内部复制) —— set_options_static 只存指针,
    // 局部 std::string 析构后悬空, 下拉列表渲染空白 (2026-10-27 实锤)
    std::string opts;
    for (int y = lunar::kBaseYear; y <= lunar::kLastYear; ++y) {
        opts += std::to_string(y);
        if (y < lunar::kLastYear) opts += "\n";
    }
    lv_dropdown_set_options(self->_year_dd, opts.c_str());
    lv_dropdown_set_selected(self->_year_dd, self->_year - lunar::kBaseYear);
    lv_obj_set_size(self->_year_dd, 200, 64);
    lv_obj_set_pos(self->_year_dd, 436, 96);
    lv_obj_set_style_text_font(self->_year_dd, zh30(), 0);
    lv_obj_set_style_text_color(self->_year_dd, lv_color_hex(cText()), 0);
    lv_obj_set_style_bg_color(self->_year_dd, lv_color_hex(cHeader()), 0);
    lv_dropdown_set_symbol(self->_year_dd, nullptr);   // 默认 FA 箭头 U+F078, zh 字体无字形
    // 打开列表
    lv_dropdown_open(self->_year_dd);
    finalize_dropdown(self, self->_scr, self->_year_dd);
    lv_obj_add_event_cb(self->_year_dd, [](lv_event_t* ev) {
        auto* s2 = static_cast<AppCalendar*>(lv_event_get_user_data(ev));
        lv_obj_t* dd = lv_event_get_current_target_obj(ev);
        const lv_event_code_t code = lv_event_get_code(ev);
        if (code == LV_EVENT_VALUE_CHANGED) {
            char buf[16] = {0};
            lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
            s2->_year = atoi(buf);
            s2->_refreshMonth();
        }
        // 选中或点外部关闭后才销毁; 其余事件(打开瞬间的
        // FOCUSED/PRESSED 等)绝不处理, 否则下拉闪现即逝
        if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_READY ||
            code == LV_EVENT_CANCEL) {
            if (s2->_year_dd == dd) s2->_year_dd = nullptr;
            lv_obj_delete_async(dd);
        }
    }, LV_EVENT_VALUE_CHANGED, self);
    lv_obj_add_event_cb(self->_year_dd, [](lv_event_t* ev) {
        auto* s2 = static_cast<AppCalendar*>(lv_event_get_user_data(ev));
        lv_obj_t* dd = lv_event_get_current_target_obj(ev);
        if (s2->_year_dd == dd) s2->_year_dd = nullptr;
        lv_obj_delete_async(dd);
    }, LV_EVENT_READY, self);
    lv_obj_add_event_cb(self->_year_dd, [](lv_event_t* ev) {
        auto* s2 = static_cast<AppCalendar*>(lv_event_get_user_data(ev));
        lv_obj_t* dd = lv_event_get_current_target_obj(ev);
        if (s2->_year_dd == dd) s2->_year_dd = nullptr;
        lv_obj_delete_async(dd);
    }, LV_EVENT_CANCEL, self);
}

// 点击"月"标签: 弹 1-12 月下拉
void AppCalendar::_monthDd_cb(lv_event_t* e)
{
    auto* self = static_cast<AppCalendar*>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    if (!self->_scr || self->_month_dd) return;

    self->_month_dd = lv_dropdown_create(self->_scr);
    lv_dropdown_set_options(self->_month_dd, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12");
    lv_dropdown_set_selected(self->_month_dd, self->_month - 1);
    lv_obj_set_size(self->_month_dd, 200, 64);
    lv_obj_set_pos(self->_month_dd, 660, 96);
    lv_obj_set_style_text_font(self->_month_dd, zh30(), 0);
    lv_obj_set_style_text_color(self->_month_dd, lv_color_hex(cText()), 0);
    lv_obj_set_style_bg_color(self->_month_dd, lv_color_hex(cHeader()), 0);
    lv_dropdown_set_symbol(self->_month_dd, nullptr);
    lv_dropdown_open(self->_month_dd);
    finalize_dropdown(self, self->_scr, self->_month_dd);
    lv_obj_add_event_cb(self->_month_dd, [](lv_event_t* ev) {
        auto* s2 = static_cast<AppCalendar*>(lv_event_get_user_data(ev));
        lv_obj_t* dd = lv_event_get_current_target_obj(ev);
        const lv_event_code_t code = lv_event_get_code(ev);
        if (code == LV_EVENT_VALUE_CHANGED) {
            s2->_month = lv_dropdown_get_selected(dd) + 1;
            s2->_refreshMonth();
        }
        if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_READY ||
            code == LV_EVENT_CANCEL) {
            if (s2->_month_dd == dd) s2->_month_dd = nullptr;
            lv_obj_delete_async(dd);
        }
    }, LV_EVENT_VALUE_CHANGED, self);
    lv_obj_add_event_cb(self->_month_dd, [](lv_event_t* ev) {
        auto* s2 = static_cast<AppCalendar*>(lv_event_get_user_data(ev));
        lv_obj_t* dd = lv_event_get_current_target_obj(ev);
        if (s2->_month_dd == dd) s2->_month_dd = nullptr;
        lv_obj_delete_async(dd);
    }, LV_EVENT_READY, self);
    lv_obj_add_event_cb(self->_month_dd, [](lv_event_t* ev) {
        auto* s2 = static_cast<AppCalendar*>(lv_event_get_user_data(ev));
        lv_obj_t* dd = lv_event_get_current_target_obj(ev);
        if (s2->_month_dd == dd) s2->_month_dd = nullptr;
        lv_obj_delete_async(dd);
    }, LV_EVENT_CANCEL, self);
}

void AppCalendar::_dayClick_cb(lv_event_t* e)
{
    auto* self = static_cast<AppCalendar*>(lv_event_get_user_data(e));
    const int day = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (day >= 1) self->_showDayDetail(day);
}

// ── 上拨退出 (app_stocks 同款: GESTURE + 位移兜底) ─────────────────────────
namespace {
struct CalSwipeCtx {
    AppCalendar* app;
    std::weak_ptr<bool> alive;
    int press_x = -1, press_y = -1;
};
}

void AppCalendar::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new SwipeCtx{this, _alive};
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<CalSwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    auto* defer = new CalSwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* c = static_cast<CalSwipeCtx*>(u);
                        auto a = c->alive.lock();
                        if (a && *a) c->app->_requestClose();
                        delete c;
                    }, defer);
                }
            }, LV_EVENT_GESTURE, _gesture_ctx);
            // 位移兜底 (慢速/合成滑动 GESTURE 不触发)
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<CalSwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                const lv_event_code_t code = lv_event_get_code(e);
                if (code == LV_EVENT_PRESSING) {
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    ctx->press_x = p.x;
                    ctx->press_y = p.y;
                } else if (code == LV_EVENT_RELEASED) {
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    const int dx = p.x - ctx->press_x, dy = p.y - ctx->press_y;
                    if (dy < -120 && abs(dy) > abs(dx) * 2) {
                        auto* defer = new CalSwipeCtx{ctx->app, ctx->alive};
                        lv_async_call([](void* u) {
                            auto* c = static_cast<CalSwipeCtx*>(u);
                            auto a = c->alive.lock();
                            if (a && *a) c->app->_requestClose();
                            delete c;
                        }, defer);
                    }
                }
            }, LV_EVENT_ALL, _gesture_ctx);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppCalendar::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
}
