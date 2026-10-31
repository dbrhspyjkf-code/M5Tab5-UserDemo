#include "app_health.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
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
// 20px (指标名/单位/时间)
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
};
static constexpr int kMetricCount = (int)(sizeof(kMetrics) / sizeof(kMetrics[0]));

// 深色苹果风调色
static constexpr uint32_t COL_BG     = 0x000000;  // 屏底
static constexpr uint32_t COL_CARD   = 0x1C1C1E;  // 卡底
static constexpr uint32_t COL_WHITE  = 0xFFFFFF;  // 大数值
static constexpr uint32_t COL_GRAY   = 0x98989F;  // 指标名/单位
static constexpr uint32_t COL_DIM    = 0x6C6C70;  // 时间

// 布局: 顶栏 64 + 网格区 656; 3 列卡 402x138 (2026-10-31 用户指定一行 3 列,
// 11 卡 4 行一屏全显, 无需滚动 — 底缘上拨手势零冲突)
static constexpr int CARD_W = 402, CARD_H = 138;
static constexpr int CARD_X0 = 21, CARD_Y0 = 8;
static constexpr int CARD_GAP_X = 16, CARD_GAP_Y = 16;

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
}

void AppHealth::onRunning()
{
    // 失焦收尾 (app_calendar 模式): openApp(工具页) 不关本 app, 自己发现被切走
    if (_scr && lv_screen_active() != _scr) {
        mclog::tagInfo(TAG, "lost foreground, tearing down screen");
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
}

void AppHealth::onClose()
{
    mclog::tagInfo(TAG, "on close");
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
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

        // 彩色圆角图标块 + 预渲染白色字形 (88x88 A8 缩半显示)
        lv_obj_t* sq = gh_rect(card, m.color, 20, 18, 44, 44, 12);
        lv_obj_t* ic = lv_image_create(sq);
        lv_image_set_src(ic, kGlyphs[m.glyph]);
        lv_image_set_scale(ic, 128);   // 50%: 88 → 44
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_image_recolor(ic, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);

        // 指标名 (图标右侧)
        lv_obj_t* name = lv_label_create(card);
        lv_label_set_text(name, m.name);
        lv_obj_set_style_text_font(name, zh20(), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(COL_GRAY), 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 76, 30);

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
                if (m.decimals < 0) snprintf(buf, sizeof(buf), "%d", (int)llround(v));
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
                        if (a && *a && c->app->_scr
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

void AppHealth::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
}
