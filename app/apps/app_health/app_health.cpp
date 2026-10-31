#include "app_health.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include "../utils/ui/canvas_draw.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#ifndef PLATFORM_BUILD_DESKTOP
#include "../utils/assets/assets_partition.h"
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
// 30px 全字库 (标题)
static const lv_font_t* zh30()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
// 20px (单位/时间)
static const lv_font_t* zh20()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}
// 24px (指标名; 2026-10-31 用户嫌 20px 太小)
static const lv_font_t* zh24()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f ? f : (lv_font_t*)nullptr;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh30() { return &font_puhui_20_4; }
static const lv_font_t* zh24() { return &font_puhui_20_4; }
static const lv_font_t* zh20() { return &font_puhui_20_4; }
#endif

static const std::string TAG = "health";

// ── 指标表 (固定顺序 = 卡片顺序; 色彩 = iOS 系统色) ──────────────────────────
// glyph: 0=脚印(步数) 1=导航箭头(距离) 2=心形(心率) 3=心形+底线(静息)
//        4=心电锯齿(HRV) 5=水滴(血氧) 6=三段波(呼吸) 7=火苗(能量)
//        8=圆+指针(锻炼) 9=阶梯(爬楼) 10=温度计(腕温)
struct MetricDef {
    const char* type;     // hermes /api/health types 键
    const char* name;     // 卡片名
    uint32_t    color;    // 图标块底色 (iOS 系统色)
    const char* unit;     // 单位 (中文)
    int         glyph;
    int         decimals; // 小数位 (-1 = 整数)
};
static const MetricDef kMetrics[] = {
    {"step_count",               "步数",     0xFF9500, "步",     0, -1},
    {"walking_running_distance", "步行距离", 0x34C759, "km",     1,  2},
    {"heart_rate",               "心率",     0xFF3B30, "bpm",    2,  0},
    {"resting_heart_rate",       "静息心率", 0xFF2D55, "bpm",    3,  0},
    {"heart_rate_variability",   "心率变异", 0x30B0C7, "ms",     4,  1},
    {"blood_oxygen_saturation",  "血氧",     0x007AFF, "%",      5,  0},
    {"respiratory_rate",         "呼吸速率", 0x5AC8FA, "次/分",  6,  1},
    {"active_energy_burned",     "活动能量", 0xFF9F0A, "kcal",   7,  0},
    {"apple_exercise_time",      "锻炼时间", 0x30D158, "分钟",   8,  0},
    {"flights_climbed",          "爬楼层",   0x5856D6, "层",     9, -1},
    {"wrist_temperature",        "手腕温度", 0xFF6B00, "°C",    10,  1},
    {"sleep_analysis",           "睡眠",     0x5E5CE6, "小时",   11, -2},  // -2=h:mm
};
static constexpr int kMetricCount = (int)(sizeof(kMetrics) / sizeof(kMetrics[0]));

// 深色苹果风调色
static constexpr uint32_t COL_BG     = 0x000000;  // 屏底
static constexpr uint32_t COL_CARD   = 0x1C1C1E;  // 卡底
static constexpr uint32_t COL_WHITE  = 0xFFFFFF;  // 大数值
static constexpr uint32_t COL_GRAY   = 0x98989F;  // 指标名/单位
static constexpr uint32_t COL_DIM    = 0x6C6C70;  // 时间

// 走势图画布调色 (canvas 1280x656, 顶栏 64px 之外)
static constexpr uint32_t COL_CHART_BG = 0x0D0D10;  // 图底
static constexpr uint32_t COL_GRID     = 0x26262B;  // 网格
static constexpr uint32_t COL_AXIS     = 0x4A4A52;  // 基线
static constexpr uint32_t COL_MA7      = 0xF1C40F;  // 7 日均线 (黄)

// 布局: 顶栏 64 + 网格区 656; 3 列卡 402x138 (2026-10-31 用户指定一行 3 列,
// 11 卡 4 行一屏全显, 无需滚动 — 底缘上拨手势零冲突)
static constexpr int CARD_W = 402, CARD_H = 138;
static constexpr int CARD_X0 = 21, CARD_Y0 = 8;
static constexpr int CARD_GAP_X = 16, CARD_GAP_Y = 16;

// ── 详情页助手 ───────────────────────────────────────────────────────────────
namespace {

// civil 日序 (Howard Hinnant 算法, 纯算术无 libc 时区依赖): ISO 日期 ↔ 连续天数,
// 供历史序列缺日补 0 与 x 轴取「月/日」
static int days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}
static void civil_from_days(int z, int& y, int& m, int& d)
{
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
}

// 网格上限取整到 1/2/5×10^n (刻度干净)
static double nice_ceil(double v)
{
    if (v <= 0) return 1;
    const double mag = pow(10.0, floor(log10(v)));
    const double nn = v / mag;
    double s = 1;
    if (nn > 5) s = 10;
    else if (nn > 2) s = 5;
    else if (nn > 1) s = 2;
    return s * mag;
}

// 完整值 (高低值标注): 与卡片同规则 (睡眠 h:mm / 负小数位=整数)
static void fmt_full_val(double v, int decimals, char* buf, size_t n)
{
    if (decimals == -2) snprintf(buf, n, "%d:%02d", (int)(v / 60), ((int)v) % 60);
    else if (decimals < 0) snprintf(buf, n, "%d", (int)llround(v));
    else snprintf(buf, n, "%.*f", decimals, v);
}

// 轴刻度紧凑值: 大数压成 "x.x万" (zh 字体渲染)
static void fmt_axis_val(double v, int decimals, char* buf, size_t n)
{
    if (decimals == -2) snprintf(buf, n, "%d:%02d", (int)(v / 60), ((int)v) % 60);
    else if (v >= 10000) snprintf(buf, n, "%.1f万", v / 10000.0);
    else if (decimals < 0) snprintf(buf, n, "%d", (int)llround(v));
    else snprintf(buf, n, "%.*f", decimals, v);
}

// 卡片描边闪烁 160ms 后执行 open (长按反馈; app_stocks 同款, outline 不动卡底色)
void flash_card_then(lv_obj_t* card, uint32_t flash_col, std::function<void()> open)
{
    if (!card) return;
    struct Ctx {
        lv_obj_t* card;
        std::function<void()> open;
    };
    auto* ctx = new Ctx{card, std::move(open)};
    lv_obj_set_style_outline_color(card, lv_color_hex(flash_col), 0);
    lv_obj_set_style_outline_width(card, 6, 0);
    lv_timer_create([](lv_timer_t* t) {
        auto* c = static_cast<Ctx*>(lv_timer_get_user_data(t));
        lv_obj_set_style_outline_width(c->card, 0, 0);
        c->open();
        lv_timer_del(t);
        delete c;
    }, 160, ctx);
}

}  // namespace

// ── 彩色圆角块 (承载预渲染字形) ─────────────────────────────────────────────
static lv_obj_t* gh_rect(lv_obj_t* parent, uint32_t color,
                         int x, int y, int w, int h, int radius)
{
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_set_size(r, w, h);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, radius, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_shadow_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

// ── 预渲染抗锯齿字形 (health_icons.c, PIL 4x 超采样生成, 白色 recolor) ────────
// glyph 下标与 kMetrics 对齐: 2/3 共用心形 (Apple 同款, 靠块色区分)
extern const lv_image_dsc_t hl_steps;
extern const lv_image_dsc_t hl_distance;
extern const lv_image_dsc_t hl_heart;
extern const lv_image_dsc_t hl_hrv;
extern const lv_image_dsc_t hl_oxygen;
extern const lv_image_dsc_t hl_breath;
extern const lv_image_dsc_t hl_flame;
extern const lv_image_dsc_t hl_exercise;
extern const lv_image_dsc_t hl_stairs;
extern const lv_image_dsc_t hl_temp;
extern const lv_image_dsc_t hl_moon;
static const lv_image_dsc_t* const kGlyphs[] = {
    &hl_steps,      // 0 步数
    &hl_distance,   // 1 距离
    &hl_heart,      // 2 心率
    &hl_heart,      // 3 静息心率 (同字形, 粉色块区分)
    &hl_hrv,        // 4 HRV
    &hl_oxygen,     // 5 血氧
    &hl_breath,     // 6 呼吸
    &hl_flame,      // 7 能量
    &hl_exercise,   // 8 锻炼
    &hl_stairs,     // 9 爬楼
    &hl_temp,       // 10 腕温
    &hl_moon,       // 11 睡眠
};

// ── 生命周期 ────────────────────────────────────────────────────────────────
AppHealth::AppHealth()
{
    setAppInfo().name = "健康";
    _cache.resize(kMetricCount);
}

void AppHealth::onOpen()
{
    mclog::tagInfo(TAG, "on open");
    if (_scr) {
        lv_screen_load(_scr);   // 复用留存屏 (_requestClose 不删屏)
        _refresh();             // 应用缓存 (期间可能有新推送)
        return;
    }
    _alive = std::make_shared<bool>(true);
    _buildUi();
    lv_screen_load(_scr);
    _installSwipeGesture();
    // 桌面调试: TAB5_HEALTH_DETAIL=<idx> 打开即直开该指标详情
    // (SDL 合成长按不可靠, 见 memory 桌面模拟器条)
    if (const char* d = getenv("TAB5_HEALTH_DETAIL")) {
        const int idx = atoi(d);
        if (idx >= 0 && idx < kMetricCount) _openDetail(idx);
    }
}

void AppHealth::onRunning()
{
    // 失焦收尾 (app_calendar 模式): openApp(工具页) 不关本 app, 自己发现被切走
    if (_scr && lv_screen_active() != _scr) {
        mclog::tagInfo(TAG, "lost foreground, tearing down screen");
        _closeDetail();        // 先释放画布 3.4MB + 作废在飞历史 worker
        _removeSwipeGesture();
        lv_obj_t* scr = _scr;
        _scr = nullptr;
        _scroll = nullptr;
        _updated_lbl = nullptr;
        _val_lbl.clear();
        _unit_lbl.clear();
        _time_lbl.clear();
        lv_async_call([](void* p) {
            lv_obj_delete(static_cast<lv_obj_t*>(p));
        }, scr);
        return;
    }
    if (!_scr) return;

    const uint32_t now = GetHAL()->millis();
    // 60s 轮询 (服务端缓存毫秒级返回; 打开立即拉一次)
    if (_last_fetch_ms == 0 || now - _last_fetch_ms > 60u * 1000u) {
        _last_fetch_ms = now;
        _fetch();
    }
    // worker 有新数据 → UI 线程应用
    if (_data_ready.exchange(false)) {
        _refresh();
    }
    // 历史走势 worker 有结果 → 画图 (UI 线程)
    if (_hist_ready.exchange(false)) {
        _applyHistory();
    }
}

void AppHealth::onClose()
{
    mclog::tagInfo(TAG, "on close");
    _closeDetail();
    _removeSwipeGesture();
    if (_scr) {
        lv_obj_t* scr = _scr;
        _scr = nullptr;
        lv_async_call([](void* p) {
            lv_obj_delete(static_cast<lv_obj_t*>(p));
        }, scr);
    }
    _scroll = nullptr;
    _updated_lbl = nullptr;
    _val_lbl.clear();
    _unit_lbl.clear();
    _time_lbl.clear();
}

// ── UI ──────────────────────────────────────────────────────────────────────
void AppHealth::_buildUi()
{
    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);

    // 顶栏: 「健康」+ 更新时间
    lv_obj_t* title = lv_label_create(_scr);
    lv_label_set_text(title, "健康");
    lv_obj_set_style_text_font(title, zh30(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 14);

    _updated_lbl = lv_label_create(_scr);
    lv_label_set_text(_updated_lbl, "更新 --:--");
    lv_obj_set_style_text_font(_updated_lbl, zh20(), 0);
    lv_obj_set_style_text_color(_updated_lbl, lv_color_hex(COL_GRAY), 0);
    lv_obj_align(_updated_lbl, LV_ALIGN_TOP_RIGHT, -24, 22);

    // 滚动容器 (app_home _dash_scroll 写法)
    _scroll = lv_obj_create(_scr);
    lv_obj_set_size(_scroll, 1280, 720 - 64);
    lv_obj_set_pos(_scroll, 0, 64);
    lv_obj_set_style_bg_opa(_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_scroll, 0, 0);
    lv_obj_set_style_pad_all(_scroll, 0, 0);
    lv_obj_set_scroll_dir(_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_scroll, LV_SCROLLBAR_MODE_OFF);

    _val_lbl.assign(kMetricCount, nullptr);
    _unit_lbl.assign(kMetricCount, nullptr);
    _time_lbl.assign(kMetricCount, nullptr);

    for (int i = 0; i < kMetricCount; i++) {
        const MetricDef& m = kMetrics[i];
        const int col = i % 3, row = i / 3;
        lv_obj_t* card = lv_obj_create(_scroll);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_pos(card, CARD_X0 + col * (CARD_W + CARD_GAP_X),
                       CARD_Y0 + row * (CARD_H + CARD_GAP_Y));
        lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 20, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        // 长按 → 指标详情走势 (app_home 健康卡同款接法; 点击无动作, 滚动不受影响)
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, (void*)(intptr_t)i);
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            auto* self = static_cast<AppHealth*>(lv_event_get_user_data(e));
            lv_obj_t* card = lv_event_get_current_target_obj(e);
            const int idx = (int)(intptr_t)lv_obj_get_user_data(card);
            if (idx < 0 || idx >= kMetricCount || self->_detail_modal) return;
            flash_card_then(card, kMetrics[idx].color, [self, idx]() {
                if (!self->_detail_modal) self->_openDetail(idx);
            });
        }, LV_EVENT_LONG_PRESSED, this);

        // 彩色圆角图标块 + 预渲染白色字形 (88x88 A8 缩半显示)
        lv_obj_t* sq = gh_rect(card, m.color, 20, 18, 44, 44, 12);
        lv_obj_t* ic = lv_image_create(sq);
        lv_image_set_src(ic, kGlyphs[m.glyph]);
        lv_image_set_scale(ic, 128);   // 50%: 88 → 44
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_image_recolor(ic, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
        lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);   // 勿吞卡片长按

        // 指标名 (图标右侧; 2026-10-31 用户嫌 20px 太小 → 24px md)
        lv_obj_t* name = lv_label_create(card);
        lv_label_set_text(name, m.name);
        lv_obj_set_style_text_font(name, zh24(), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(COL_GRAY), 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 74, 26);

        // 大数值 (montserrat_38) + 单位 (占位用 ASCII "-", montserrat 无 U+2014)
        _val_lbl[i] = lv_label_create(card);
        lv_label_set_text(_val_lbl[i], "-");
        lv_obj_set_style_text_font(_val_lbl[i], &lv_font_montserrat_38, 0);
        lv_obj_set_style_text_color(_val_lbl[i], lv_color_hex(COL_WHITE), 0);
        lv_obj_align(_val_lbl[i], LV_ALIGN_BOTTOM_LEFT, 22, -14);

        _unit_lbl[i] = lv_label_create(card);
        lv_label_set_text(_unit_lbl[i], m.unit);
        lv_obj_set_style_text_font(_unit_lbl[i], zh20(), 0);
        lv_obj_set_style_text_color(_unit_lbl[i], lv_color_hex(COL_GRAY), 0);
        lv_obj_align_to(_unit_lbl[i], _val_lbl[i], LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -6);

        // 时间 (右下)
        _time_lbl[i] = lv_label_create(card);
        lv_label_set_text(_time_lbl[i], "");
        lv_obj_set_style_text_font(_time_lbl[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(_time_lbl[i], lv_color_hex(COL_DIM), 0);
        lv_obj_align(_time_lbl[i], LV_ALIGN_BOTTOM_RIGHT, -20, -22);
    }
}

void AppHealth::_refresh()
{
    if (!_scr) return;
    std::vector<MetricCache> snap;
    std::string updated;
    {
        std::lock_guard<std::mutex> lk(_mu);
        snap = _cache;
        updated = _updated_txt;
    }
    if (_updated_lbl && !updated.empty()
        && strcmp(lv_label_get_text(_updated_lbl), updated.c_str()) != 0) {
        lv_label_set_text(_updated_lbl, updated.c_str());
    }
    // 详情页顶栏当前值跟随摘要 (详情可能先于摘要打开, _applyHistory 那次未必赶上)
    if (_detail_val && _detail_idx >= 0 && _detail_idx < (int)snap.size()
        && snap[_detail_idx].has) {
        const char* v = snap[_detail_idx].value.c_str();
        if (strcmp(lv_label_get_text(_detail_val), v) != 0) {
            lv_label_set_text(_detail_val, v);
        }
    }
    for (int i = 0; i < kMetricCount && i < (int)snap.size(); i++) {
        const char* v = snap[i].has ? snap[i].value.c_str() : "-";
        if (_val_lbl[i] && strcmp(lv_label_get_text(_val_lbl[i]), v) != 0) {
            lv_label_set_text(_val_lbl[i], v);
            // 数值标签从左缘生长: 宽度变了须把单位重新排到新右缘,
            // 否则 "—"→"14,437" 后单位被盖住 (align_to 只算一次)
            if (_unit_lbl[i]) {
                lv_obj_align_to(_unit_lbl[i], _val_lbl[i],
                                LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -6);
            }
        }
        const char* t = snap[i].has ? snap[i].time.c_str() : "";
        if (_time_lbl[i] && strcmp(lv_label_get_text(_time_lbl[i]), t) != 0) {
            lv_label_set_text(_time_lbl[i], t);
        }
    }
}

// ── 数据拉取 (worker 只写缓存, 绝不碰 LVGL — app_home 模式 A) ────────────────
void AppHealth::_fetch()
{
    if (_fetch_in_flight.exchange(true)) return;   // 防重入
    GetHAL()->tryRunDetached([this]() {
        std::vector<MetricCache> out(kMetricCount);
        std::string updated = "更新 --:--";
        do {
            const std::string base =
                "http://" + GetHAL()->getConfig("svc_host", "192.168.1.200") + ":8766";
            auto resp = GetHAL()->httpGet(base + "/api/health");
            if (!resp.ok || resp.status != 200) {
                mclog::tagWarn(TAG, "fetch failed: HTTP %d", resp.status);
                break;   // 失败保持原值
            }
            nlohmann::json j;
            try {
                j = nlohmann::json::parse(resp.body);
            } catch (...) {
                mclog::tagWarn(TAG, "json parse failed");
                break;
            }
            if (!j.value("ok", false) || !j.contains("types") || !j["types"].is_object()) {
                break;
            }
            const auto& types = j["types"];
            for (int i = 0; i < kMetricCount; i++) {
                const MetricDef& m = kMetrics[i];
                if (!types.contains(m.type) || !types[m.type].is_object()) continue;
                const auto& e = types[m.type];
                // value 双形态容错 (数字 / 字符串)
                double v = 0;
                if (e.contains("value")) {
                    if (e["value"].is_number()) v = e["value"].get<double>();
                    else if (e["value"].is_string()) v = atof(e["value"].get<std::string>().c_str());
                    else continue;
                } else {
                    continue;
                }
                char buf[32];
                if (m.decimals == -2)   // 睡眠: 分钟 → "7:32" (时:分)
                    snprintf(buf, sizeof(buf), "%d:%02d",
                             (int)(v / 60), ((int)v) % 60);
                else if (m.decimals < 0) snprintf(buf, sizeof(buf), "%d", (int)llround(v));
                else snprintf(buf, sizeof(buf), "%.*f", m.decimals, v);  // 浮点必须 snprintf
                out[i].value = buf;
                // at: "2026-10-31T06:59:59+08:00" → "06:59"
                std::string at = e.value("at", std::string());
                if (at.size() >= 16) out[i].time = at.substr(11, 5);
                out[i].has = true;
            }
            std::string ua = j.value("updated_at", std::string());
            if (ua.size() >= 16) updated = "更新 " + ua.substr(11, 5);
        } while (false);

        {
            std::lock_guard<std::mutex> lk(_mu);
            // 增量合并: 本次没拿到的类型保留旧值 (聚合服务端同款语义)
            if ((int)_cache.size() < kMetricCount) _cache.resize(kMetricCount);
            for (int i = 0; i < kMetricCount; i++) {
                if (out[i].has) _cache[i] = out[i];
            }
            if (updated != "更新 --:--") _updated_txt = updated;
        }
        _data_ready.store(true);
        _fetch_in_flight.store(false);
    });
}

// ── 指标详情: 长按卡片 → 全屏每日走势 (app_stocks 走势图同款骨架) ───────────
static constexpr int DET_W = 1280, DET_H = 720 - 64;   // 走势画布 (顶栏 64px 外)

void AppHealth::_openDetail(int idx)
{
    if (!_scr) return;
    _closeDetail();
    if (idx < 0 || idx >= kMetricCount) return;
    _detail_idx = idx;
    const MetricDef& m = kMetrics[idx];
    mclog::tagInfo(TAG, "open detail: {} (idx {})", m.type, idx);

    _detail_modal = lv_obj_create(_scr);
    lv_obj_set_size(_detail_modal, 1280, 720);
    lv_obj_set_pos(_detail_modal, 0, 0);
    lv_obj_set_style_bg_color(_detail_modal, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(_detail_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_detail_modal, 0, 0);
    lv_obj_set_style_radius(_detail_modal, 0, 0);
    lv_obj_set_style_pad_all(_detail_modal, 0, 0);
    lv_obj_clear_flag(_detail_modal, LV_OBJ_FLAG_SCROLLABLE);

    // 顶栏: 指标名 (指标色) + 当前值 + 单位
    lv_obj_t* name = lv_label_create(_detail_modal);
    lv_label_set_text(name, m.name);
    lv_obj_set_style_text_font(name, zh30(), 0);
    lv_obj_set_style_text_color(name, lv_color_hex(m.color), 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 24, 12);

    std::string cur = "-";
    {
        std::lock_guard<std::mutex> lk(_mu);
        if (idx < (int)_cache.size() && _cache[idx].has) cur = _cache[idx].value;
    }
    lv_obj_t* val = lv_label_create(_detail_modal);
    lv_label_set_text(val, cur.c_str());
    lv_obj_set_style_text_font(val, &lv_font_montserrat_38, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(COL_WHITE), 0);
    lv_obj_align_to(val, name, LV_ALIGN_OUT_RIGHT_MID, 24, 4);
    _detail_val = val;   // 详情先于摘要到时, _applyHistory 补填

    lv_obj_t* unit = lv_label_create(_detail_modal);
    lv_label_set_text(unit, m.unit);
    lv_obj_set_style_text_font(unit, zh20(), 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(COL_GRAY), 0);
    lv_obj_align_to(unit, val, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -4);

    // canvas: ARGB8888 大块 — 设备端 SPIRAM_USE_MALLOC=y, malloc 自动落 PSRAM;
    // 勿用 heap_caps_malloc(MALLOC_CAP_SPIRAM) (app_stocks 2026-10-27 实锤根因)
    _detail_pixels = malloc(DET_W * DET_H * 4);
    if (_detail_pixels) {
        memset(_detail_pixels, 0, DET_W * DET_H * 4);
    } else {
        mclog::tagWarn(TAG, "detail canvas alloc failed ({} B)", DET_W * DET_H * 4);
    }
    _detail_canvas = lv_canvas_create(_detail_modal);
    lv_canvas_set_buffer(_detail_canvas, _detail_pixels, DET_W, DET_H,
                         LV_COLOR_FORMAT_ARGB8888);
    lv_obj_align(_detail_canvas, LV_ALIGN_TOP_LEFT, 0, 64);

    // 状态提示 (加载中/失败, 居中)
    _detail_status = lv_label_create(_detail_modal);
    lv_label_set_text(_detail_status, "走势加载中…");
    lv_obj_set_style_text_font(_detail_status, zh20(), 0);
    lv_obj_set_style_text_color(_detail_status, lv_color_hex(COL_GRAY), 0);
    lv_obj_align(_detail_status, LV_ALIGN_TOP_MID, 0, 64 + DET_H / 2 - 10);

    // ＋/－ 调整可视天数窗口 (app_stocks 同款左下角大按钮)
    {
        auto mk_zoom = [&](const char* txt, int x, int delta) {
            lv_obj_t* b = lv_button_create(_detail_modal);
            lv_obj_set_size(b, 88, 64);
            lv_obj_set_pos(b, x, 720 - 64 - 24);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x2C2C2E), 0);
            lv_obj_set_style_radius(b, 14, 0);
            lv_obj_add_event_cb(b, [](lv_event_t* e) {
                auto* self = static_cast<AppHealth*>(lv_event_get_user_data(e));
                self->_zoomDetail((int)(intptr_t)lv_obj_get_user_data(
                    lv_event_get_current_target_obj(e)));
            }, LV_EVENT_CLICKED, this);
            lv_obj_set_user_data(b, (void*)(intptr_t)delta);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, txt);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_36, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(COL_WHITE), 0);
            lv_obj_center(l);
        };
        mk_zoom("-", 28, -7);              // 缩小窗口 = 放大细节
        mk_zoom("+", 28 + 88 + 44, +7);    // 扩大窗口 = 看更久
    }

    // 窗口高/低 (右上角) 与 7 日均图例 (左上角) — 必须建在 canvas 之后
    // (画布不透明, LVGL 后建者在上; app_stocks 实锤: 先建会被整个盖住)
    _detail_hilo = lv_label_create(_detail_modal);
    lv_label_set_text(_detail_hilo, "");
    lv_obj_set_style_text_font(_detail_hilo, zh20(), 0);
    lv_obj_set_style_text_color(_detail_hilo, lv_color_hex(COL_GRAY), 0);
    // y=122: 避开顶部轴刻度 (顶档刻度在网格线下方 y≈94), 挤在一起读不清
    lv_obj_align(_detail_hilo, LV_ALIGN_TOP_RIGHT, -16, 122);

    _detail_leg = lv_label_create(_detail_modal);
    lv_label_set_text(_detail_leg, "");
    lv_obj_set_style_text_font(_detail_leg, zh20(), 0);
    lv_obj_set_style_text_color(_detail_leg, lv_color_hex(COL_MA7), 0);
    lv_obj_align(_detail_leg, LV_ALIGN_TOP_LEFT, 16, 66);

    lv_obj_t* close_btn = lv_button_create(_detail_modal);
    lv_obj_set_size(close_btn, 150, 48);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -16, 10);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_t* cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "关闭");
    lv_obj_set_style_text_font(cl, zh20(), 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(COL_WHITE), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(close_btn, [](lv_event_t* e) {
        static_cast<AppHealth*>(lv_event_get_user_data(e))->_closeDetail();
    }, LV_EVENT_CLICKED, this);

    // 拉历史 (worker 不碰 LVGL; _detail_type 供在飞结果比对作废)
    _detail_win = 30;
    {
        std::lock_guard<std::mutex> lk(_hmu);
        _detail_type = m.type;
        _hist.clear();
        _hist_ok = false;
        _hist_err.clear();
    }
    _hist_ready.store(false);
    _fetchHistory();
}

void AppHealth::_closeDetail()
{
    if (_detail_modal) {
        lv_obj_delete(_detail_modal);   // canvas/labels/按钮均为其子对象, 随之销毁
        _detail_modal = nullptr;
        _detail_canvas = nullptr;
        _detail_status = nullptr;
        _detail_hilo = nullptr;
        _detail_leg = nullptr;
        _detail_val = nullptr;
    }
    _detail_xt.clear();
    if (_detail_pixels) {
        free(_detail_pixels);
        _detail_pixels = nullptr;
    }
    std::lock_guard<std::mutex> lk(_hmu);
    _detail_type.clear();   // 在飞 worker 结果作废 (交接时比对失败即丢弃)
    _detail_idx = -1;
    _hist.clear();
}

void AppHealth::_fetchHistory()
{
    if (_hist_in_flight.exchange(true)) return;
    std::string want;
    {
        std::lock_guard<std::mutex> lk(_hmu);
        want = _detail_type;
    }
    if (want.empty()) {
        _hist_in_flight.store(false);
        return;
    }
    const bool spawned = GetHAL()->tryRunDetached([this, want]() {
        std::vector<DayPoint> pts;
        std::string err;
        bool ok = false;
        do {
            const std::string base =
                "http://" + GetHAL()->getConfig("svc_host", "192.168.1.200") + ":8766";
            auto resp = GetHAL()->httpGet(
                base + "/api/health/history?type=" + want + "&days=90");
            if (!resp.ok || resp.status != 200) {
                err = "HTTP " + std::to_string(resp.status);
                break;
            }
            nlohmann::json j;
            try {
                j = nlohmann::json::parse(resp.body);
            } catch (...) {
                err = "解析失败";
                break;
            }
            if (!j.value("ok", false) || !j.contains("points")
                || !j["points"].is_array()) {
                err = j.value("error", std::string("服务无数据"));
                break;
            }
            for (const auto& p : j["points"]) {
                if (!p.is_object()) continue;
                const std::string d = p.value("date", std::string());
                if (d.size() < 10) continue;
                const int yy = atoi(d.substr(0, 4).c_str());
                const int mm = atoi(d.substr(5, 2).c_str());
                const int dd = atoi(d.substr(8, 2).c_str());
                double v = 0;
                if (p.contains("value")) {
                    if (p["value"].is_number()) v = p["value"].get<double>();
                    else if (p["value"].is_string())
                        v = atof(p["value"].get<std::string>().c_str());
                    else continue;
                }
                pts.push_back({days_from_civil(yy, mm, dd), mm * 100 + dd, v});
            }
            // 缺日补 0: 首→末连续化 (柱状图日期等距)
            if (!pts.empty()) {
                std::vector<DayPoint> cont;
                cont.reserve((size_t)(pts.back().day - pts.front().day) + 1);
                size_t k = 0;
                for (int day = pts.front().day; day <= pts.back().day; day++) {
                    if (k < pts.size() && pts[k].day == day) {
                        cont.push_back(pts[k++]);
                    } else {
                        int yy, mm, dd;
                        civil_from_days(day, yy, mm, dd);
                        cont.push_back({day, mm * 100 + dd, 0.0});
                    }
                }
                pts.swap(cont);
            }
            ok = true;
        } while (false);

        {
            std::lock_guard<std::mutex> lk(_hmu);
            if (_detail_type == want) {   // 详情仍开着且没换指标才交接
                _hist = std::move(pts);
                _hist_ok = ok;
                _hist_err = err;
                _hist_ready.store(true);  // onRunning 在 UI 线程消费
            }
        }
        _hist_in_flight.store(false);
    });
    if (!spawned) {
        // 派生失败必须还旗标 + 给 UI 出口 (app_ssh 教训: 不能静默卡"加载中")
        _hist_in_flight.store(false);
        if (_detail_status) lv_label_set_text(_detail_status, "后台线程创建失败");
    }
}

void AppHealth::_applyHistory()
{
    if (!_detail_modal) return;
    bool ok = false;
    std::string err;
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(_hmu);
        ok = _hist_ok;
        err = _hist_err;
        n = _hist.size();
    }
    // 顶栏当前值补填: 详情可能先于首次摘要拉取打开 (此时 _cache 尚空)
    if (_detail_val && _detail_idx >= 0 && _detail_idx < kMetricCount) {
        std::string cur = "-";
        {
            std::lock_guard<std::mutex> lk(_mu);
            if (_detail_idx < (int)_cache.size() && _cache[_detail_idx].has)
                cur = _cache[_detail_idx].value;
        }
        if (strcmp(lv_label_get_text(_detail_val), cur.c_str()) != 0) {
            lv_label_set_text(_detail_val, cur.c_str());
        }
    }
    if (!ok) {
        if (_detail_status) {
            const std::string t = "走势获取失败  " + err;
            lv_label_set_text(_detail_status, t.c_str());
        }
        return;
    }
    if (n == 0) {
        if (_detail_status) lv_label_set_text(_detail_status, "暂无历史数据");
        return;
    }
    if (_detail_status) {
        lv_obj_add_flag(_detail_status, LV_OBJ_FLAG_HIDDEN);
        _detail_status = nullptr;
    }
    _drawDetailChart();
}

void AppHealth::_zoomDetail(int delta)
{
    if (!_detail_canvas) return;
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(_hmu);
        n = (int)_hist.size();
    }
    if (n <= 0) return;
    int w = _detail_win + delta;
    if (w < 7) w = 7;
    if (w > 90) w = 90;
    if (w > n) w = n;
    if (w == _detail_win) return;
    _detail_win = w;
    _drawDetailChart();   // 点击回调已在 LVGL 上下文
}

void AppHealth::_drawDetailChart()
{
    if (!_detail_canvas || !_detail_pixels || _detail_idx < 0) return;
    std::vector<DayPoint> h;
    {
        std::lock_guard<std::mutex> lk(_hmu);
        h = _hist;
    }
    if (h.empty()) return;
    const MetricDef& m = kMetrics[_detail_idx];

    int win = _detail_win;
    if (win > (int)h.size()) win = (int)h.size();
    if (win < 1) return;
    const int start = (int)h.size() - win;   // 窗口锚定最新一天

    // 画布内布局: 左 24 / 右 1190 (右留轴刻度) / 上 20 / 下 612 (下留日期轴)
    const int PL = 24, PR = 1190, PT = 20, PB = 612;

    double vmax = 0;
    for (int i = start; i < (int)h.size(); i++) {
        if (h[i].value > vmax) vmax = h[i].value;
    }
    vmax = vmax > 0 ? vmax * 1.02 : 1.0;
    if (m.decimals == -2) {
        // 睡眠 (分钟): 上限取 4h 的倍数 → 四档刻度全为整点小时
        const double step4 = 240.0;
        vmax = ceil(vmax / step4) * step4;
        if (vmax < step4) vmax = step4;
    } else {
        vmax = nice_ceil(vmax);
    }
    const double yscale = (double)(PB - PT) / vmax;

    canvas_draw::px_fill(_detail_pixels, DET_W, DET_H, COL_CHART_BG);
    for (int g = 0; g <= 4; g++) {   // 网格 5 档
        const int y = PB - (int)((double)(PB - PT) * g / 4.0 + 0.5);
        canvas_draw::px_line(_detail_pixels, DET_W, DET_H, PL, y, PR, y,
                             g == 0 ? COL_AXIS : COL_GRID);
    }

    // 柱: 历史柱向底色调暗, 最新一天全色突出
    const double slot = (double)(PR - PL) / win;
    int bw = (int)(slot * 0.66);
    if (bw < 2) bw = 2;
    if (bw > 160) bw = 160;   // 天数很少时柱不过宽 (单日满屏成色块)
    const uint32_t bar_past = canvas_draw::px_blend(m.color, COL_CHART_BG, 0.45f);
    double wmax = -1e18, wmin = 1e18;
    std::vector<int> cx(win);
    for (int i = 0; i < win; i++) {
        const DayPoint& p = h[start + i];
        cx[i] = PL + (int)(slot * (i + 0.5));
        const int bh = (int)(p.value * yscale + 0.5);
        if (bh > 0) {
            const uint32_t c = (start + i == (int)h.size() - 1) ? m.color : bar_past;
            canvas_draw::px_rect(_detail_pixels, DET_W, DET_H,
                                 cx[i] - bw / 2, PB - bh,
                                 cx[i] + (bw - 1) / 2, PB - 1, c);
        }
        if (p.value > wmax) wmax = p.value;
        if (p.value < wmin) wmin = p.value;
    }

    // 7 日均线 (窗口左端向更早借 6 天, 线段随柱心走)
    {
        int pvx = 0, pvy = 0;
        bool has_prev = false;
        for (int i = 0; i < win; i++) {
            const int gi = start + i;
            if (gi < 6) { has_prev = false; continue; }
            double s = 0;
            for (int k = gi - 6; k <= gi; k++) s += h[k].value;
            const int y = PB - (int)(s / 7.0 * yscale + 0.5);
            if (has_prev) {
                canvas_draw::px_line(_detail_pixels, DET_W, DET_H,
                                     pvx, pvy, cx[i], y, COL_MA7);
            }
            pvx = cx[i];
            pvy = y;
            has_prev = true;
        }
    }
    lv_obj_invalidate(_detail_canvas);

    // ── 覆盖层 label: 每次整幅重画后重建 (轴刻度 + 日期) ──
    for (lv_obj_t* l : _detail_xt) {
        if (l) lv_obj_delete(l);
    }
    _detail_xt.clear();

    for (int g = 1; g <= 4; g++) {   // y 轴刻度 (右侧; zh20 — 可能含 "万")
        char t[24];
        fmt_axis_val(vmax * g / 4.0, m.decimals, t, sizeof(t));
        const int y = PB - (int)((double)(PB - PT) * g / 4.0 + 0.5);
        // 顶档刻度下移到线下方: 线上方那格会与右上角「高/低」标注撞车
        const int ly = (g == 4) ? 64 + PT + 10 : 64 + y - 10;
        lv_obj_t* l = lv_label_create(_detail_modal);
        lv_label_set_text(l, t);
        lv_obj_set_style_text_font(l, zh20(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, PR + 8, ly);
        _detail_xt.push_back(l);
    }
    {   // x 轴日期 (纯 ASCII "M/D", 从最新端往左稀疏标)
        const int step = win <= 8 ? 1 : (win <= 30 ? 5 : (win <= 60 ? 10 : 15));
        for (int i = win - 1; i >= 0; i -= step) {
            const DayPoint& p = h[start + i];
            char t[16];
            snprintf(t, sizeof(t), "%d/%d", p.md / 100, p.md % 100);
            lv_obj_t* l = lv_label_create(_detail_modal);
            lv_label_set_text(l, t);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
            const int tw = (int)lv_txt_get_width(t, (uint32_t)strlen(t),
                                                 &lv_font_montserrat_20, 0);
            lv_obj_align(l, LV_ALIGN_TOP_LEFT, cx[i] - tw / 2, 64 + PB + 8);
            _detail_xt.push_back(l);
        }
    }

    if (_detail_hilo) {   // 窗口内高/低 (右上角)
        char hi[32], lo[32], t[96];
        fmt_full_val(wmax, m.decimals, hi, sizeof(hi));
        fmt_full_val(wmin, m.decimals, lo, sizeof(lo));
        snprintf(t, sizeof(t), "高 %s   低 %s", hi, lo);
        lv_label_set_text(_detail_hilo, t);
    }
    if (_detail_leg) {    // 图例 + 当前窗口天数 (左上角, 黄=均线色)
        char t[48];
        snprintf(t, sizeof(t), "7日均（近%d天）", win);
        lv_label_set_text(_detail_leg, t);
    }
}

// ── 上拨退出 (app_calendar 同款: GESTURE + 位移兜底) ─────────────────────────
namespace {
struct HlSwipeCtx {
    AppHealth* app;
    std::weak_ptr<bool> alive;
    int press_x = -1, press_y = -1;
};
}

void AppHealth::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new HlSwipeCtx{this, _alive};
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<HlSwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                if (!ctx->app->_scr || lv_screen_active() != ctx->app->_scr) return;
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    // 位置守卫: 只认屏幕最下缘 (y>650) 起手的上拨
                    if (ctx->press_y <= 650) return;
                    auto* defer = new HlSwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* c = static_cast<HlSwipeCtx*>(u);
                        auto a = c->alive.lock();
                        // 详情页开着: 底缘上拨关详情回卡片页, 不退出 app
                        if (a && *a && c->app->_detail_modal) {
                            c->app->_closeDetail();
                        } else if (a && *a && c->app->_scr
                                   && lv_screen_active() == c->app->_scr) {
                            c->app->_requestClose();
                        }
                        delete c;
                    }, defer);
                }
            }, LV_EVENT_GESTURE, _gesture_ctx);
            // 位移兜底 (PRESSING 不达 indev 回调, PRESSED 记起点)
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<HlSwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                const lv_event_code_t code = lv_event_get_code(e);
                if (code == LV_EVENT_PRESSED) {
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    ctx->press_x = p.x;
                    ctx->press_y = p.y;
                } else if (code == LV_EVENT_RELEASED) {
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    const int dx = p.x - ctx->press_x, dy = p.y - ctx->press_y;
                    const bool from_bottom = ctx->press_y > 650;
                    if (from_bottom && dy < -120 && abs(dy) > abs(dx) * 2) {
                        auto* defer = new HlSwipeCtx{ctx->app, ctx->alive};
                        lv_async_call([](void* u) {
                            auto* c = static_cast<HlSwipeCtx*>(u);
                            auto a = c->alive.lock();
                            if (a && *a) {
                                if (c->app->_detail_modal) {
                                    c->app->_closeDetail();   // 详情页: 关详情不退 app
                                } else {
                                    c->app->_requestClose();
                                }
                            }
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

void AppHealth::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
}
