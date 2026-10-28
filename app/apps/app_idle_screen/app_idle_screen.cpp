#include "app_idle_screen.h"
#include "../app_home/app_home.h"
#include "../app_voice_input/app_voice_input.h"
#include "../utils/datetime/lunar_calendar.h"
#include <hal/hal.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>

// Full-coverage Chinese font (same blob already linked in for the HA/Claude
// screens) — GB2312 common ~3500 glyphs, no tofu boxes. On device it's a
// cbin blob used in-place from flash; on the desktop sim, the linked 20px
// C-array font.
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
static const lv_font_t* zh_font_lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
#endif

static constexpr int SCREEN_W = 1280;
static constexpr int SCREEN_H = 720;
static constexpr uint32_t C_BG   = 0x000000;
static constexpr uint32_t C_TEXT = 0xEAF3FB;
static constexpr uint32_t C_DIM  = 0x8FA8C4;

// Group box the clock/date/weather move around inside — kept well clear of
// the screen edges (burn-in guard: OLED/LCD panels showing the same static
// bright text in the same spot for hours can leave a faint ghost image).
static constexpr int GROUP_W = 600;
static constexpr int GROUP_H = 170;
static constexpr int MARGIN  = 60;
static const struct { int x, y; } POSITIONS[] = {
    {(SCREEN_W - GROUP_W) / 2, (SCREEN_H - GROUP_H) / 2},  // center
    {MARGIN,                    MARGIN},                     // top-left
    {SCREEN_W - GROUP_W - MARGIN, MARGIN},                    // top-right
    {MARGIN,                    SCREEN_H - GROUP_H - MARGIN}, // bottom-left
    {SCREEN_W - GROUP_W - MARGIN, SCREEN_H - GROUP_H - MARGIN}, // bottom-right
};
static constexpr int POSITION_COUNT = sizeof(POSITIONS) / sizeof(POSITIONS[0]);

void AppIdleScreen::onCreate() {}

void AppIdleScreen::onResume() {}

void AppIdleScreen::dismiss()
{
    lv_display_trigger_activity(NULL);
    _hide();
    _undim();
}

void AppIdleScreen::onPause()
{
    _hide();
    _undim();
}

void AppIdleScreen::onRunning()
{
    // 播放抑制(收音机后台播放): 不弹时钟遮盖(会盖住别的 app), 但闲置熄屏
    // 仍然允许 —— 听歌熄屏更合理也更省电 (2026-10-28 语义调整)
    if (_suppressed) {
        if (_overlay) _hide();
        _refreshConfig();
        uint32_t idle_ms = lv_display_get_inactive_time(nullptr);
        if (idle_ms < _idle_timeout_ms) {
            _undim();
            return;
        }
        if (!_dimmed) _dim();
        return;
    }

    // Don't cover an in-progress voice-input session (recording / result overlay).
    if (AppVoiceInput::isMicActive()) {
        if (_overlay) _hide();
        _undim();
        return;
    }

    _refreshConfig();

    uint32_t idle_ms = lv_display_get_inactive_time(nullptr);

    // Any touch resets LVGL's inactivity timer for the whole display —
    // including a tap on the overlay itself. A small idle_ms here means
    // "something was touched since we showed it": hide the overlay / restore
    // brightness. The overlay's own click handler already hides it instantly;
    // this is just a safety net in case that somehow didn't fire.
    if (idle_ms < _idle_timeout_ms) {
        if (_overlay) _hide();
        _undim();
        return;
    }

    // 闲置超时: 勾选屏保 → 全屏时钟遮盖; 不勾 → 不弹任何界面, 亮度降到 5%
    if (_ss_enabled) {
        if (!_overlay) _show();
        else _updateClock();
    } else {
        if (!_dimmed) _dim();
    }
}

void AppIdleScreen::setSuppressed(bool s)
{
    _suppressed = s;
    if (s && _overlay) _hide();
    if (s) _undim();
}

void AppIdleScreen::_refreshConfig()
{
    uint32_t now = GetHAL()->millis();
    if (_last_cfg_ms != 0 && now - _last_cfg_ms < CFG_MS) return;
    _last_cfg_ms = now;
    _ss_enabled  = GetHAL()->getConfig("screensaver_en", "1") != "0";
    int m = atoi(GetHAL()->getConfig("screensaver_min", "5").c_str());
    if (m != 2 && m != 5 && m != 10) m = 5;   // 只认设置弹窗里的三档
    _idle_timeout_ms = (uint32_t)m * 60 * 1000;
#ifdef PLATFORM_BUILD_DESKTOP
    // 桌面调试 (同 TAB5_THEME/TAB5_PAGE 模式): TAB5_IDLE_MS=毫秒 强制闲置超时,
    // TAB5_SS_EN=0 强制关屏保走降亮度分支, 免等 2 分钟
    if (const char* e = getenv("TAB5_IDLE_MS")) {
        if (atoi(e) > 0) _idle_timeout_ms = (uint32_t)atoi(e);
    }
    if (const char* e = getenv("TAB5_SS_EN")) _ss_enabled = atoi(e) != 0;
#endif
}

void AppIdleScreen::_dim()
{
    _pre_dim_brt = GetHAL()->getDisplayBrightness();
    _dimmed = true;
    // 2026-10-28 真机实测: 背光驱动在低占空比段有视觉下限, 5%(gamma 后
    // ~1.1% duty)依旧亮 — 闲置降亮直接熄屏(duty 0), 触摸恢复原亮度
    GetHAL()->setDisplayBrightness(0);
}

void AppIdleScreen::_undim()
{
    if (!_dimmed) return;
    _dimmed = false;
    GetHAL()->setDisplayBrightness(_pre_dim_brt);
}

void AppIdleScreen::_show()
{
    if (_overlay) return;

    _overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_overlay, 0, 0);
    lv_obj_set_style_bg_color(_overlay, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    lv_obj_set_style_radius(_overlay, 0, 0);
    lv_obj_set_style_pad_all(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(_overlay);

    // Time/date/weather live inside _group so they move together — repositioning
    // just moves this one container, not three separately-aligned labels.
    _group = lv_obj_create(_overlay);
    lv_obj_set_size(_group, GROUP_W, GROUP_H);
    lv_obj_set_style_bg_opa(_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_group, 0, 0);
    lv_obj_set_style_pad_all(_group, 0, 0);
    lv_obj_clear_flag(_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_group, LV_OBJ_FLAG_CLICKABLE);  // clicks pass through to _overlay

    _time_lbl = lv_label_create(_group);
    lv_obj_set_style_text_font(_time_lbl, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(_time_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(_time_lbl, LV_ALIGN_TOP_MID, 0, 0);

    _date_lbl = lv_label_create(_group);
    lv_obj_set_style_text_font(_date_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_date_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(_date_lbl, LV_ALIGN_TOP_MID, 0, 70);

    _weather_lbl = lv_label_create(_group);
    lv_obj_set_style_text_font(_weather_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_weather_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(_weather_lbl, LV_ALIGN_TOP_MID, 0, 116);

    // Touch anywhere on the overlay dismisses it immediately (PRESSED, not
    // CLICKED, so it reacts on touch-down rather than waiting for release).
    lv_obj_add_event_cb(_overlay, [](lv_event_t* e) {
        static_cast<AppIdleScreen*>(lv_event_get_user_data(e))->_hide();
    }, LV_EVENT_PRESSED, this);

    _pos_index = 0;
    lv_obj_set_pos(_group, POSITIONS[0].x, POSITIONS[0].y);
    _last_clock_update_ms = 0;  // force immediate first render
    _last_move_ms = GetHAL()->millis();
    _updateClock();
}

void AppIdleScreen::_hide()
{
    if (!_overlay) return;
    lv_obj_delete(_overlay);
    _overlay = _group = _time_lbl = _date_lbl = _weather_lbl = nullptr;
}

void AppIdleScreen::_reposition()
{
    _pos_index = (_pos_index + 1) % POSITION_COUNT;
    lv_obj_set_pos(_group, POSITIONS[_pos_index].x, POSITIONS[_pos_index].y);
}

void AppIdleScreen::_updateClock()
{
    uint32_t now = GetHAL()->millis();
    if (_last_clock_update_ms != 0 && now - _last_clock_update_ms < 1000) return;
    _last_clock_update_ms = now;

    if (now - _last_move_ms >= MOVE_MS) {
        _last_move_ms = now;
        _reposition();
    }

    time_t t = time(nullptr);
    struct tm lt = {};
    localtime_r(&t, &lt);

    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    lv_label_set_text(_time_lbl, tbuf);

    static const char* const wd[] = {"日", "一", "二", "三", "四", "五", "六"};
    char dbuf[64];
    int n = snprintf(dbuf, sizeof(dbuf), "%d月%d日 周%s", lt.tm_mon + 1, lt.tm_mday, wd[lt.tm_wday]);
    char lbuf[24];
    if (n > 0 && lunar::format_lunar_date(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lbuf, sizeof(lbuf)))
        snprintf(dbuf + n, sizeof(dbuf) - n, " 农历%s", lbuf);
    lv_label_set_text(_date_lbl, dbuf);

    std::string temp, cond;
    if (_home) _home->getWeather(temp, cond);
    if (!temp.empty() || !cond.empty()) {
        char wbuf[48];
        snprintf(wbuf, sizeof(wbuf), "%s  %s", cond.c_str(), temp.c_str());
        lv_label_set_text(_weather_lbl, wbuf);
    } else {
        lv_label_set_text(_weather_lbl, "");
    }
}
