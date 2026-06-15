#include "app_home.h"
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_lvgl.h>
#include <hal/hal.h>
#include <ctime>
#include <cstdio>
#include <nlohmann/json.hpp>

static const std::string _tag = "app-home";

// Chinese font defined in app_ha/view/font_zh_36.c
extern lv_font_t font_zh_36;

// 28px Chinese font (same visual size as lv_font_montserrat_28) for the status bar.
extern lv_font_t font_zh_28;

// Bold Chinese font (STHeiti Medium, 40px) for the home buttons — defined in
// app/apps/app_home/font_zh_bold.c (glyphs: 智能家居小设置 + ASCII).
extern lv_font_t font_zh_bold;

// Home background image (1280x720 RGB565) defined in app/assets/images/home_bg.c
// (LVGL image descriptors use a plain top-level symbol — same name in C and C++.)
extern const lv_image_dsc_t home_bg;

AppHome::AppHome()
{
    setAppInfo().name = "AppHome";
}

void AppHome::addApp(const std::string& name, int app_id)
{
    _apps.push_back({name, app_id});
}

void AppHome::restoreScreen()
{
    if (_scr) {
        lv_screen_load(_scr);
    }
    _child_app_id = -1;
}

void AppHome::onCreate()
{
    open();  // home auto-opens immediately after install
}

void AppHome::onOpen()
{
    mclog::tagInfo(_tag, "on open");

    // Create a dedicated screen so other apps can own their own screens
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x0D1B2A), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Full-screen background image (created first → stays at the bottom layer).
    lv_obj_t* bg = lv_image_create(_scr);
    lv_image_set_src(bg, &home_bg);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_CLICKABLE);

    // Stable storage for button user_data pointers — must not reallocate after reserve
    _btn_data.clear();
    _btn_data.reserve(_apps.size());

    // Buttons laid out in a single row along the bottom of the screen.
    const int n = (int)_apps.size();
    static constexpr int BTN_H      = 96;
    static constexpr int BTN_GAP    = 30;
    static constexpr int SIDE_PAD   = 60;   // left/right margin
    static constexpr int BOTTOM_PAD = 36;   // gap from the bottom edge

    int avail_w = 1280 - 2 * SIDE_PAD - (n - 1) * BTN_GAP;
    int btn_w   = n > 0 ? avail_w / n : avail_w;
    int total_w = n * btn_w + (n - 1) * BTN_GAP;
    int start_x = (1280 - total_w) / 2;
    int y       = 720 - BTN_H - BOTTOM_PAD;

    for (int i = 0; i < n; i++) {
        _btn_data.push_back({this, _apps[i].app_id});
        BtnData& bd = _btn_data.back();

        lv_obj_t* btn = lv_obj_create(_scr);
        lv_obj_set_size(btn, btn_w, BTN_H);
        lv_obj_set_pos(btn, start_x + i * (btn_w + BTN_GAP), y);
        // Electric-blue gradient pill matching the background artwork's palette.
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E6FC2), 0);          // top: electric blue
        lv_obj_set_style_bg_grad_color(btn, lv_color_hex(0x0A2A54), 0);     // bottom: deep navy
        lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
        // Pressed: brighten the whole gradient.
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A9BE8), LV_STATE_PRESSED);
        lv_obj_set_style_bg_grad_color(btn, lv_color_hex(0x14467E), LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 22, 0);
        // Cyan neon edge + soft outer glow.
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x5BD5FF), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_80, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x2E8BE6), 0);
        lv_obj_set_style_shadow_width(btn, 18, 0);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, _apps[i].name.c_str());
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lbl, &font_zh_bold, 0);  // bold
        // Subtle dark text shadow for legibility over the bright gradient.
        lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, 0);

        lv_obj_add_event_cb(btn, _btn_event_cb, LV_EVENT_CLICKED, &bd);
    }

    // ── Top system status bar (clock / WiFi / battery) ──
    lv_obj_t* sbar = lv_obj_create(_scr);
    lv_obj_set_pos(sbar, 0, 0);
    lv_obj_set_size(sbar, 1280, 56);
    lv_obj_set_style_bg_color(sbar, lv_color_hex(0x07111E), 0);
    lv_obj_set_style_bg_opa(sbar, LV_OPA_60, 0);   // semi-transparent over the artwork
    lv_obj_set_style_border_width(sbar, 0, 0);
    lv_obj_set_style_radius(sbar, 0, 0);
    lv_obj_set_style_pad_all(sbar, 0, 0);
    lv_obj_clear_flag(sbar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    _status_time = lv_label_create(sbar);
    lv_obj_set_style_text_font(_status_time, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_status_time, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(_status_time, "--:--");
    lv_obj_align(_status_time, LV_ALIGN_LEFT_MID, 28, 0);

    // Date — to the right of the clock, same size as time (28px Chinese font).
    _status_date = lv_label_create(sbar);
    lv_obj_set_style_text_font(_status_date, &font_zh_28, 0);
    lv_obj_set_style_text_color(_status_date, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(_status_date, "");
    lv_obj_align(_status_date, LV_ALIGN_LEFT_MID, 138, 0);

    // Weather (center): temperature (latin font, has °) + condition (zh font).
    lv_obj_t* wbox = lv_obj_create(sbar);
    lv_obj_set_size(wbox, LV_SIZE_CONTENT, 56);
    lv_obj_set_style_bg_opa(wbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wbox, 0, 0);
    lv_obj_set_style_pad_all(wbox, 0, 0);
    lv_obj_set_style_pad_column(wbox, 10, 0);
    lv_obj_set_flex_flow(wbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wbox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(wbox, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_align(wbox, LV_ALIGN_CENTER, 0, 0);

    _status_wtemp = lv_label_create(wbox);
    lv_obj_set_style_text_font(_status_wtemp, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_status_wtemp, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(_status_wtemp, "");

    _status_wcond = lv_label_create(wbox);
    lv_obj_set_style_text_font(_status_wcond, &font_zh_28, 0);
    lv_obj_set_style_text_color(_status_wcond, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(_status_wcond, "");

    _status_wifi = lv_label_create(sbar);
    lv_obj_set_style_text_font(_status_wifi, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_status_wifi, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(_status_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(_status_wifi, LV_ALIGN_RIGHT_MID, -150, 0);

    _status_batt = lv_label_create(sbar);
    lv_obj_set_style_text_font(_status_batt, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_status_batt, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(_status_batt, LV_SYMBOL_BATTERY_FULL " --%");
    lv_obj_align(_status_batt, LV_ALIGN_RIGHT_MID, -28, 0);

    _last_status_ms = 0;  // force first refresh on next onRunning
    _last_batt_ms   = 0;

    lv_screen_load(_scr);
}

void AppHome::onRunning()
{
    // restoreScreen() is called via callback before a child app closes.
    // Here we just refresh the top status bar (clock / WiFi / battery).
    if (!_status_time) return;

    uint32_t now = GetHAL()->millis();
    if (_last_status_ms != 0 && now - _last_status_ms < 1000) return;
    _last_status_ms = now;

    // Clock
    time_t t = time(nullptr);
    struct tm lt = {};
    localtime_r(&t, &lt);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    lv_label_set_text(_status_time, buf);

    // Date (Chinese)
    static const char* const wd[] = {"日", "一", "二", "三", "四", "五", "六"};
    char dbuf[40];
    snprintf(dbuf, sizeof(dbuf), "%d月%d日 周%s", lt.tm_mon + 1, lt.tm_mday, wd[lt.tm_wday]);
    lv_label_set_text(_status_date, dbuf);

    // WiFi
    bool wifi = GetHAL()->isWifiConnected();
    lv_obj_set_style_text_color(_status_wifi, lv_color_hex(wifi ? 0x4FA3FF : 0xFF4444), 0);

    // Battery — read the (slower) power monitor about every 10 s
    if (_last_batt_ms == 0 || now - _last_batt_ms > 10000) {
        _last_batt_ms = now;
        auto* hal = GetHAL();
        hal->updatePowerMonitorData();
        float v = hal->powerMonitorData.busVoltage;
        int pct = -1;
        if (v > 0.5f) {                       // NP-F550 7.4V pack: 8.4V=100%, 6.0V=0%
            if      (v >= 8.4f) pct = 100;
            else if (v <= 6.0f) pct = 0;
            else                pct = (int)((v - 6.0f) / 2.4f * 100.0f);
        }
        const char* icon = pct < 0   ? LV_SYMBOL_BATTERY_EMPTY
                         : pct >= 80  ? LV_SYMBOL_BATTERY_FULL
                         : pct >= 55  ? LV_SYMBOL_BATTERY_3
                         : pct >= 30  ? LV_SYMBOL_BATTERY_2
                         : pct >= 10  ? LV_SYMBOL_BATTERY_1
                         :              LV_SYMBOL_BATTERY_EMPTY;
        char bb[24];
        if (pct >= 0) snprintf(bb, sizeof(bb), "%s %d%%", icon, pct);
        else          snprintf(bb, sizeof(bb), "%s --%%", icon);
        lv_label_set_text(_status_batt, bb);
        uint32_t c = pct < 0  ? 0x8FA8C4
                   : pct < 20 ? 0xFF4444
                   : pct < 40 ? 0xF39C12
                   :            0x4FA3FF;
        lv_obj_set_style_text_color(_status_batt, lv_color_hex(c), 0);
    }

    // Weather — refetch every 15 min; copy the latest cached text each tick.
    if (_last_weather_ms == 0 || now - _last_weather_ms > 15 * 60 * 1000) {
        _last_weather_ms = now;
        _fetch_weather();
    }
    {
        std::lock_guard<std::mutex> lk(_weather_mutex);
        if (!_weather_temp.empty()) lv_label_set_text(_status_wtemp, _weather_temp.c_str());
        if (!_weather_cond.empty()) lv_label_set_text(_status_wcond, _weather_cond.c_str());
    }
}

void AppHome::_fetch_weather()
{
    // Home has no HA client; pull the weather endpoint directly. ha_host comes
    // from NVS (same key the HA app uses). The worker only writes strings — it
    // must not touch LVGL — and onRunning copies them to the labels.
    std::string host = GetHAL()->getConfig("ha_host", "192.168.1.142");
    std::string url  = "http://" + host + ":8766/weather?city=Guangzhou";
    GetHAL()->tryRunDetached([this, url]() {
        auto resp = GetHAL()->httpGet(url);
        if (!resp.ok) return;
        try {
            auto j = nlohmann::json::parse(resp.body);
            if (!j.value("ok", false)) return;
            std::string temp = j.value("temp_c", "");
            std::string cond = j.value("condition", "");
            std::lock_guard<std::mutex> lk(_weather_mutex);
            if (!temp.empty()) _weather_temp = temp + "°C";
            _weather_cond = cond;
        } catch (...) {}
    });
}

void AppHome::onClose()
{
    mclog::tagInfo(_tag, "on close");
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _status_time  = nullptr;
    _status_date  = nullptr;
    _status_wtemp = nullptr;
    _status_wcond = nullptr;
    _status_wifi  = nullptr;
    _status_batt  = nullptr;
    _btn_data.clear();
}

void AppHome::_btn_event_cb(lv_event_t* e)
{
    auto* bd = static_cast<BtnData*>(lv_event_get_user_data(e));
    bd->home->_child_app_id = bd->app_id;
    mooncake::GetMooncake().openApp(bd->app_id);
}
