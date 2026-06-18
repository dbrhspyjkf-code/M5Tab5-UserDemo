#include "app_home.h"
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_lvgl.h>
#include <hal/hal.h>
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <xiaozhi_ctl.h>
#include "../app_voice_input/app_voice_input.h"
#include "../app_ha/ha_weather.h"

static const std::string _tag = "app-home";

// Full-coverage Chinese font (same as the Claude/HA apps) — GB2312 common
// ~3500 glyphs, so dialog labels never render as tofu boxes. Replaces the old
// subset font_zh_36 (~135 chars, which boxed e.g. 其/他). On device it's a cbin
// blob used in-place from flash; on the desktop sim we fall back to the linked
// 20px C-array font.
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

    // Helper: make a status-bar item tappable. tag routes the click:
    // 1 = battery → power dialog, 2 = wifi → network dialog,
    // 3 = gear → quick settings, 4 = weather → weather detail.
    auto make_tappable = [&](lv_obj_t* item, int tag) {
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(item, 18);  // enlarge touch target
        lv_obj_set_user_data(item, (void*)(intptr_t)tag);
        lv_obj_add_event_cb(item, _statusClick_cb, LV_EVENT_CLICKED, this);
    };

    // Tap the center weather block → detailed weather card.
    make_tappable(wbox, 4);

    _status_wifi = lv_label_create(sbar);
    lv_obj_set_style_text_font(_status_wifi, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_status_wifi, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(_status_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(_status_wifi, LV_ALIGN_RIGHT_MID, -250, 0);
    make_tappable(_status_wifi, 2);

    // Quick-settings gear — to the left of the battery (volume + brightness).
    _status_tools = lv_label_create(sbar);
    lv_obj_set_style_text_font(_status_tools, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_status_tools, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(_status_tools, LV_SYMBOL_SETTINGS);
    lv_obj_align(_status_tools, LV_ALIGN_RIGHT_MID, -180, 0);
    make_tappable(_status_tools, 3);

    _status_batt = lv_label_create(sbar);
    lv_obj_set_style_text_font(_status_batt, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_status_batt, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(_status_batt, LV_SYMBOL_BATTERY_FULL " --%");
    lv_obj_align(_status_batt, LV_ALIGN_RIGHT_MID, -28, 0);
    make_tappable(_status_batt, 1);

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

    // Weather-detail popup: worker finished → drop the text into its label.
    if (_wx_body && _wx_ready.exchange(false)) {
        std::lock_guard<std::mutex> lk(_weather_mutex);
        lv_label_set_text(_wx_body, _wx_text.c_str());
    }
}

void AppHome::_fetch_weather()
{
    // Home has no HA client, so read the HA weather entity directly via the REST
    // API (GET /api/states/<entity> with the shared token). The worker only
    // writes strings — it must not touch LVGL — onRunning copies them to labels.
    std::string url = "http://" + GetHAL()->getConfig("ha_host", "192.168.1.133")
                    + ":8123/api/states/" + ha_weather::ENTITY;
    GetHAL()->tryRunDetached([this, url]() {
        auto resp = GetHAL()->httpGet(url, {{"Authorization", std::string("Bearer ") + ha_weather::TOKEN}});
        if (!resp.ok) return;
        try {
            auto j = nlohmann::json::parse(resp.body);
            std::string st = j.value("state", "");
            if (st.empty() || st == "unavailable" || st == "unknown") return;
            auto attrs = j.value("attributes", nlohmann::json::object());
            std::string temp;
            if (attrs.contains("temperature")) {
                // temperature may be a JSON number (e.g. 28.1) — render whole degrees.
                double t = attrs["temperature"].is_number()
                         ? attrs["temperature"].get<double>()
                         : std::atof(attrs["temperature"].get<std::string>().c_str());
                temp = std::to_string((int)(t + (t >= 0 ? 0.5 : -0.5)));
            }
            std::lock_guard<std::mutex> lk(_weather_mutex);
            if (!temp.empty()) _weather_temp = temp + "°C";
            _weather_cond = ha_weather::condZh(st);
        } catch (...) {}
    });
}

void AppHome::onClose()
{
    mclog::tagInfo(_tag, "on close");
    // The modal lives on lv_layer_top(), not on _scr, so delete it explicitly.
    if (_modal) {
        lv_obj_delete(_modal);
        _modal = nullptr;
    }
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
    _status_tools = nullptr;
    _qs_vol_lbl = _qs_brt_lbl = nullptr;
    _net_ssid = _net_pass = _net_host = _net_ssid_dd = _net_kb = _net_status = nullptr;
    _btn_data.clear();
}

void AppHome::_btn_event_cb(lv_event_t* e)
{
    auto* bd = static_cast<BtnData*>(lv_event_get_user_data(e));
    bd->home->_child_app_id = bd->app_id;
    mooncake::GetMooncake().openApp(bd->app_id);
}

// ════════════════════════════════════════════════════════════════════════════
//  Status-bar quick-access popups (power / quick settings / network)
// ════════════════════════════════════════════════════════════════════════════

namespace {
constexpr uint32_t M_BG     = 0x132840;  // card background (deep blue)
constexpr uint32_t M_ACCENT = 0x4FA3FF;
constexpr uint32_t M_LBL    = 0xBFD3E6;
constexpr uint32_t M_BTN    = 0x2E5A8F;
constexpr uint32_t M_SAVE   = 0x2E8B57;
constexpr uint32_t M_DANGER = 0xC0392B;
constexpr uint32_t M_WARN   = 0xD08B1E;

// Styled pill button. cb is a captureless lambda; `self` is passed as user_data.
lv_obj_t* mk_btn(lv_obj_t* parent, const char* text, uint32_t color, int w, int h,
                 lv_event_cb_t cb, void* self)
{
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, self);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, zh_font_lg(), 0);
    return b;
}
}  // namespace

void AppHome::_closeModal()
{
    if (!_modal) return;
    lv_obj_delete_async(_modal);  // safe to call from within the modal's own event
    _modal      = nullptr;
    _qs_vol_lbl = _qs_brt_lbl = nullptr;
    _net_ssid = _net_pass = _net_host = _net_svc = _net_ssid_dd = _net_kb = _net_status = nullptr;
    _wx_body  = nullptr;
}

lv_obj_t* AppHome::_openModal(int card_w, int card_h, const char* title)
{
    // Full-screen dimming backdrop on the top layer (covers status bar + buttons).
    _modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_modal, 1280, 720);
    lv_obj_set_pos(_modal, 0, 0);
    lv_obj_set_style_bg_color(_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_modal, 0, 0);
    lv_obj_set_style_radius(_modal, 0, 0);
    lv_obj_set_style_pad_all(_modal, 0, 0);
    lv_obj_clear_flag(_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_modal, LV_OBJ_FLAG_CLICKABLE);
    // Tap outside the card → dismiss.
    lv_obj_add_event_cb(_modal, _modalBg_cb, LV_EVENT_CLICKED, this);

    // Centered card. CLICKABLE so taps on it are absorbed (don't reach backdrop).
    lv_obj_t* card = lv_obj_create(_modal);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(M_BG), 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2E5A8F), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_text_color(t, lv_color_hex(M_ACCENT), 0);
    lv_obj_set_style_text_font(t, zh_font_lg(), 0);
    return card;
}

// ─── Weather detail popup (weather tap) ─────────────────────────────────────
void AppHome::_openWeatherDialog()
{
    lv_obj_t* card = _openModal(680, 500, "天气详情");
    // Slightly translucent so the home wallpaper shows through.
    lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);
    // Anchor near the top, just under the 56px status bar (not vertically centered).
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 64);

    _wx_body = lv_label_create(card);
    lv_obj_set_pos(_wx_body, 32, 74);
    lv_obj_set_width(_wx_body, 616);
    lv_obj_set_style_text_color(_wx_body, lv_color_hex(M_LBL), 0);
    lv_obj_set_style_text_font(_wx_body, zh_font_lg(), 0);
    lv_obj_set_style_text_line_space(_wx_body, 12, 0);
    lv_label_set_text(_wx_body, "加载中…");

    _wx_ready = false;
    _fetchWeatherDetail();
}

void AppHome::_fetchWeatherDetail()
{
    std::string base = "http://" + GetHAL()->getConfig("ha_host", "192.168.1.133") + ":8123";
    std::string tok  = std::string("Bearer ") + ha_weather::TOKEN;
    GetHAL()->tryRunDetached([this, base, tok]() {
        auto dir8 = [](double b) -> const char* {
            static const char* D[] = {"北","东北","东","东南","南","西南","西","西北"};
            int i = (int)((b + 22.5) / 45.0) & 7;
            return D[i];
        };
        std::string out;
        // ── Current conditions ──
        try {
            auto resp = GetHAL()->httpGet(base + "/api/states/" + ha_weather::ENTITY,
                                          {{"Authorization", tok}});
            if (resp.ok) {
                auto j = nlohmann::json::parse(resp.body);
                std::string st = j.value("state", "");
                auto a = j.value("attributes", nlohmann::json::object());
                auto num = [&](const char* k, double def = 0) {
                    return a.contains(k) && a[k].is_number() ? a[k].get<double>() : def;
                };
                char line[160];
                out += ha_weather::condZh(st) + "   " +
                       std::to_string((int)(num("temperature") + 0.5)) + "°C\n";
                // Pack three fields per line to keep the card short. met.no via HA
                // reports wind in mph / pressure in inHg — convert to km/h / hPa.
                snprintf(line, sizeof(line), "湿度 %d%%   露点 %d°C   紫外线 %.1f\n",
                         (int)num("humidity"), (int)(num("dew_point") + 0.5), num("uv_index"));
                out += line;
                snprintf(line, sizeof(line), "云量 %d%%   风 %s%dkm/h   气压 %dhPa\n",
                         (int)num("cloud_coverage"), dir8(num("wind_bearing")),
                         (int)(num("wind_speed") * 1.60934 + 0.5),
                         (int)(num("pressure") * 33.8639 + 0.5));
                out += line;
            }
        } catch (...) {}

        // ── Daily forecast ──
        try {
            auto resp = GetHAL()->httpPost(
                base + "/api/services/weather/get_forecasts?return_response",
                std::string("{\"entity_id\":\"") + ha_weather::ENTITY + "\",\"type\":\"daily\"}",
                {{"Authorization", tok}, {"Content-Type", "application/json"}});
            if (resp.ok) {
                auto j = nlohmann::json::parse(resp.body);
                auto fc = j["service_response"][ha_weather::ENTITY]["forecast"];
                if (fc.is_array() && !fc.empty()) {
                    out += "未来几天\n";
                    int n = 0;
                    for (auto& f : fc) {
                        if (n >= 6) break;
                        std::string dt = f.value("datetime", "");      // YYYY-MM-DD...
                        std::string md = dt.size() >= 10 ? dt.substr(5, 2) + "/" + dt.substr(8, 2) : dt;
                        std::string cond = ha_weather::condZh(f.value("condition", ""));
                        int hi = f.contains("temperature") && f["temperature"].is_number()
                                 ? (int)(f["temperature"].get<double>() + 0.5) : 0;
                        int lo = f.contains("templow") && f["templow"].is_number()
                                 ? (int)(f["templow"].get<double>() + 0.5) : 0;
                        char cell[64];
                        snprintf(cell, sizeof(cell), "%s %s %d~%d°", md.c_str(), cond.c_str(), lo, hi);
                        out += cell;
                        // Two days per line.
                        out += (n % 2 == 1) ? "\n" : "    ";
                        n++;
                    }
                    if (n % 2 == 1) out += "\n";
                }
            }
        } catch (...) {}

        if (out.empty()) out = "天气获取失败";
        {
            std::lock_guard<std::mutex> lk(_weather_mutex);
            _wx_text = out;
        }
        _wx_ready = true;
    });
}

// ─── Power dialog (battery tap) ─────────────────────────────────────────────
void AppHome::_openPowerDialog()
{
    lv_obj_t* card = _openModal(560, 300, "电源选项");

    lv_obj_t* q = lv_label_create(card);
    lv_label_set_text(q, "是否关机？");
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_text_color(q, lv_color_hex(M_LBL), 0);
    lv_obj_set_style_text_font(q, zh_font_lg(), 0);

    const int BW = 150, BH = 64, GAP = 24;
    int total = BW * 3 + GAP * 2;
    int x0 = (560 - total) / 2;

    lv_obj_t* b_off = mk_btn(card, "关机", M_DANGER, BW, BH,
        [](lv_event_t* e) {
            auto* s = static_cast<AppHome*>(lv_event_get_user_data(e));
            s->_closeModal();
            GetHAL()->powerOff();
        }, this);
    lv_obj_align(b_off, LV_ALIGN_BOTTOM_LEFT, x0, -32);

    lv_obj_t* b_reb = mk_btn(card, "重启", M_WARN, BW, BH,
        [](lv_event_t* e) {
            auto* s = static_cast<AppHome*>(lv_event_get_user_data(e));
            s->_closeModal();
            GetHAL()->reboot();
        }, this);
    lv_obj_align(b_reb, LV_ALIGN_BOTTOM_LEFT, x0 + BW + GAP, -32);

    lv_obj_t* b_cancel = mk_btn(card, "取消", M_BTN, BW, BH,
        [](lv_event_t* e) {
            static_cast<AppHome*>(lv_event_get_user_data(e))->_closeModal();
        }, this);
    lv_obj_align(b_cancel, LV_ALIGN_BOTTOM_LEFT, x0 + 2 * (BW + GAP), -32);
}

// ─── Quick-settings dialog (gear tap): volume + brightness ──────────────────
void AppHome::_openQuickSettings()
{
    lv_obj_t* card = _openModal(680, 340, "音量与亮度");

    const int LBL_X = 40, SLD_X = 180, SLD_W = 380, SLD_H = 44;
    const int VAL_X = SLD_X + SLD_W + 18;

    auto mk_row = [&](const char* name, int yy, int min_v, int max_v, int cur_v,
                      lv_event_cb_t cb, lv_obj_t** out_val) -> lv_obj_t* {
        lv_obj_t* l = lv_label_create(card);
        lv_label_set_text(l, name);
        lv_obj_set_pos(l, LBL_X, yy + 6);
        lv_obj_set_style_text_color(l, lv_color_hex(M_LBL), 0);
        lv_obj_set_style_text_font(l, zh_font_lg(), 0);

        lv_obj_t* sl = lv_slider_create(card);
        lv_obj_set_pos(sl, SLD_X, yy + 6);
        lv_obj_set_size(sl, SLD_W, SLD_H);
        lv_slider_set_range(sl, min_v, max_v);
        lv_slider_set_value(sl, cur_v, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sl, lv_color_hex(0x1A3050), LV_PART_MAIN);
        lv_obj_set_style_radius(sl, SLD_H / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, lv_color_hex(M_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_radius(sl, SLD_H / 2, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        lv_obj_set_style_radius(sl, SLD_H / 2, LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, 4, LV_PART_KNOB);
        lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, this);

        lv_obj_t* v = lv_label_create(card);
        char buf[8]; snprintf(buf, sizeof(buf), "%d%%", cur_v);
        lv_label_set_text(v, buf);
        lv_obj_set_pos(v, VAL_X, yy + 8);
        lv_obj_set_style_text_color(v, lv_color_hex(M_ACCENT), 0);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_28, 0);
        *out_val = v;
        return sl;
    };

    int vol = xiaozhi_get_speaker_volume();
    int brt = GetHAL()->getDisplayBrightness();
    mk_row("音量", 86,  0, 100, vol, _qsVol_cb, &_qs_vol_lbl);
    mk_row("亮度", 158, 10, 100, brt, _qsBrt_cb, &_qs_brt_lbl);

    lv_obj_t* done = mk_btn(card, "完成", M_SAVE, 200, 60,
        [](lv_event_t* e) {
            static_cast<AppHome*>(lv_event_get_user_data(e))->_closeModal();
        }, this);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -28);
}

void AppHome::_qsVol_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    int v = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
    xiaozhi_set_speaker_volume(v);
    char buf[8]; snprintf(buf, sizeof(buf), "%d%%", v);
    if (self->_qs_vol_lbl) lv_label_set_text(self->_qs_vol_lbl, buf);
}

void AppHome::_qsBrt_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    int v = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
    GetHAL()->setDisplayBrightness((uint8_t)v);
    char buf[8]; snprintf(buf, sizeof(buf), "%d%%", v);
    if (self->_qs_brt_lbl) lv_label_set_text(self->_qs_brt_lbl, buf);
}

// ─── Network dialog (WiFi tap): WiFi + HA server ────────────────────────────
void AppHome::_openNetworkDialog()
{
    auto* hal = GetHAL();
    lv_obj_t* card = _openModal(820, 606, "网络设置");

    const int LBL_X = 36, FLD_X = 250, FLD_W = 520, ROW_H = 66;
    int y = 86;

    auto mk_label = [&](const char* text, int yy) {
        lv_obj_t* l = lv_label_create(card);
        lv_label_set_text(l, text);
        lv_obj_set_pos(l, LBL_X, yy + 12);
        lv_obj_set_style_text_color(l, lv_color_hex(M_LBL), 0);
        lv_obj_set_style_text_font(l, zh_font_lg(), 0);
    };
    auto mk_ta = [&](int yy, int w, bool password) -> lv_obj_t* {
        lv_obj_t* ta = lv_textarea_create(card);
        lv_obj_set_pos(ta, FLD_X, yy);
        lv_obj_set_size(ta, w, ROW_H - 10);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_password_mode(ta, password);
        lv_obj_set_style_text_font(ta, zh_font_lg(), 0);
        lv_obj_add_event_cb(ta, _netTa_cb, LV_EVENT_ALL, this);
        return ta;
    };

    // WiFi SSID + 扫描
    mk_label("WiFi 名称", y);
    _net_ssid = mk_ta(y, FLD_W - 140, false);
    lv_textarea_set_text(_net_ssid, hal->getConfig("wifi_ssid", "ChinaNet-11G").c_str());
    lv_obj_t* scan = mk_btn(card, "扫描", M_BTN, 124, ROW_H - 10,
        [](lv_event_t* e) {
            static_cast<AppHome*>(lv_event_get_user_data(e))->_doNetworkScan();
        }, this);
    lv_obj_set_pos(scan, FLD_X + FLD_W - 124, y);
    y += ROW_H;

    // Scan results dropdown
    mk_label("搜索结果", y);
    _net_ssid_dd = lv_dropdown_create(card);
    lv_obj_set_pos(_net_ssid_dd, FLD_X, y);
    lv_obj_set_size(_net_ssid_dd, FLD_W, ROW_H - 10);
    lv_dropdown_set_options(_net_ssid_dd, "(点扫描)");
    lv_obj_set_style_text_font(_net_ssid_dd, zh_font_lg(), 0);
    lv_obj_add_event_cb(_net_ssid_dd, [](lv_event_t* e) {
        auto* s = static_cast<AppHome*>(lv_event_get_user_data(e));
        char buf[64] = {0};
        lv_dropdown_get_selected_str(s->_net_ssid_dd, buf, sizeof(buf));
        if (buf[0] && buf[0] != '(') lv_textarea_set_text(s->_net_ssid, buf);
    }, LV_EVENT_VALUE_CHANGED, this);
    y += ROW_H;

    // WiFi password
    mk_label("WiFi 密码", y);
    _net_pass = mk_ta(y, FLD_W, true);
    lv_textarea_set_text(_net_pass, hal->getConfig("wifi_pass", "").c_str());
    y += ROW_H;

    // HA server host (smart-home devices, :8123)
    mk_label("HA 服务器", y);
    _net_host = mk_ta(y, FLD_W, false);
    lv_textarea_set_text(_net_host, hal->getConfig("ha_host", "192.168.1.133").c_str());
    y += ROW_H;

    // Other services host (weather :8766 + Claude bridge :8770), runs on the Mac
    mk_label("其他服务器", y);
    _net_svc = mk_ta(y, FLD_W, false);
    lv_textarea_set_text(_net_svc, hal->getConfig("svc_host", "192.168.1.142").c_str());
    y += ROW_H + 6;

    // Save + status
    lv_obj_t* save = mk_btn(card, "保存并重启", M_SAVE, 260, 60,
        [](lv_event_t* e) {
            static_cast<AppHome*>(lv_event_get_user_data(e))->_doNetworkSave();
        }, this);
    lv_obj_set_pos(save, FLD_X, y);

    _net_status = lv_label_create(card);
    lv_label_set_text(_net_status, "");
    lv_obj_set_pos(_net_status, FLD_X + 280, y + 16);
    lv_obj_set_style_text_color(_net_status, lv_color_hex(0xFFD166), 0);
    lv_obj_set_style_text_font(_net_status, zh_font_lg(), 0);

    // On-screen keyboard — child of the backdrop so it floats above the card.
    _net_kb = lv_keyboard_create(_modal);
    lv_obj_set_size(_net_kb, 1280, 300);
    lv_obj_align(_net_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(_net_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_net_kb, _netKb_cb, LV_EVENT_ALL, this);
}

void AppHome::_doNetworkScan()
{
    if (!_net_status) return;
    lv_label_set_text(_net_status, "扫描中…");
    auto aps = GetHAL()->wifiScan();
    if (aps.empty()) {
        lv_dropdown_set_options(_net_ssid_dd, "(未发现网络)");
        lv_label_set_text(_net_status, "未发现网络");
        return;
    }
    std::string opts;
    for (size_t i = 0; i < aps.size(); i++) {
        if (i) opts += "\n";
        opts += aps[i].ssid;
    }
    lv_dropdown_set_options(_net_ssid_dd, opts.c_str());
    lv_dropdown_set_selected(_net_ssid_dd, 0);
    lv_textarea_set_text(_net_ssid, aps[0].ssid.c_str());
    lv_label_set_text(_net_status, "");
}

void AppHome::_doNetworkSave()
{
    auto* hal = GetHAL();
    std::string ssid = lv_textarea_get_text(_net_ssid);
    std::string pass = lv_textarea_get_text(_net_pass);
    std::string host = lv_textarea_get_text(_net_host);
    std::string svc  = lv_textarea_get_text(_net_svc);
    if (ssid.empty()) {
        lv_label_set_text(_net_status, "请填写 WiFi 名称");
        return;
    }
    hal->setConfig("wifi_ssid", ssid);
    hal->setConfig("wifi_pass", pass);
    if (!host.empty()) hal->setConfig("ha_host", host);
    if (!svc.empty())  hal->setConfig("svc_host", svc);
    lv_label_set_text(_net_status, "已保存，正在重启…");
    mclog::tagInfo(_tag, "network saved ssid={} ha_host={} svc_host={}, rebooting", ssid, host, svc);
    hal->delay(800);
    hal->reboot();
}

void AppHome::_netTa_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (!self->_net_kb) return;
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(self->_net_kb, ta);
        lv_obj_clear_flag(self->_net_kb, LV_OBJ_FLAG_HIDDEN);
        AppVoiceInput::requestVoiceInput(ta, 300);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(self->_net_kb, LV_OBJ_FLAG_HIDDEN);
        AppVoiceInput::dismissMicButton();
    }
}

void AppHome::_netKb_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    lv_event_code_t code = lv_event_get_code(e);
    if ((code == LV_EVENT_READY || code == LV_EVENT_CANCEL) && self->_net_kb) {
        lv_obj_add_flag(self->_net_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

// ─── Status-bar click routing + backdrop dismiss ────────────────────────────
void AppHome::_statusClick_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    if (self->_modal) return;  // one popup at a time
    lv_obj_t* item = (lv_obj_t*)lv_event_get_target(e);
    intptr_t tag = (intptr_t)lv_obj_get_user_data(item);
    if      (tag == 1) self->_openPowerDialog();
    else if (tag == 2) self->_openNetworkDialog();
    else if (tag == 3) self->_openQuickSettings();
    else if (tag == 4) self->_openWeatherDialog();
}

void AppHome::_modalBg_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    // Only a tap on the backdrop itself (not bubbled from a child) dismisses.
    if ((lv_obj_t*)lv_event_get_target(e) == self->_modal) {
        self->_closeModal();
    }
}
