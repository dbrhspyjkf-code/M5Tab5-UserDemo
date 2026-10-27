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
static constexpr int CELL_W = 152;              // 1064/7
static constexpr int CELL_H = 96;               // 6 行 × 96 + 表头
static constexpr int GRID_W = COLS * CELL_W;
static constexpr int GRID_X = (1280 - GRID_W) / 2;

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

    // ── 标题行: ◀ 2026年10月 ▶ ──
    lv_obj_t* prev = lv_button_create(_scr);
    lv_obj_set_size(prev, 110, 64);
    lv_obj_set_pos(prev, 60, 28);
    lv_obj_set_style_bg_color(prev, lv_color_hex(cHeader()), 0);
    lv_obj_set_style_radius(prev, 14, 0);
    lv_obj_set_style_shadow_width(prev, 0, 0);
    lv_obj_add_event_cb(prev, _prevClick_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* pl = lv_label_create(prev);
    lv_label_set_text(pl, "<");     // LV_SYMBOL_* 是 FA 私有区, zh 字体无字形
    lv_obj_set_style_text_font(pl, zh30(), 0);
    lv_obj_set_style_text_color(pl, lv_color_hex(cText()), 0);
    lv_obj_center(pl);

    lv_obj_t* next = lv_button_create(_scr);
    lv_obj_set_size(next, 110, 64);
    lv_obj_align(next, LV_ALIGN_TOP_RIGHT, -60, 28);
    lv_obj_set_style_bg_color(next, lv_color_hex(cHeader()), 0);
    lv_obj_set_style_radius(next, 14, 0);
    lv_obj_set_style_shadow_width(next, 0, 0);
    lv_obj_add_event_cb(next, _nextClick_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* nl = lv_label_create(next);
    lv_label_set_text(nl, ">");
    lv_obj_set_style_text_font(nl, zh30(), 0);
    lv_obj_set_style_text_color(nl, lv_color_hex(cText()), 0);
    lv_obj_center(nl);

    _title = lv_label_create(_scr);
    lv_obj_set_style_text_font(_title, zh30(), 0);
    lv_obj_set_style_text_color(_title, lv_color_hex(cText()), 0);
    lv_obj_align(_title, LV_ALIGN_TOP_MID, 0, 42);

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

    // 标题
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "%d 年 %d 月", _year, _month);
    lv_label_set_text(_title, tbuf);

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

            char db[8];
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
