#include "app_settings.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <string>
#include <xiaozhi_ctl.h>
#include "screensaver/screensaver.h"

static const std::string _tag = "app-settings";

// Chinese font defined in app_ha/view/font_zh_36.c
extern lv_font_t font_zh_36;

static constexpr const char* DEF_SSID = "ChinaNet-11G";
static constexpr const char* DEF_HOST = "192.168.1.142";

// Screensaver idle-timeout presets (seconds), aligned with the dropdown options
// "关闭 / 30秒 / 1分钟 / 3分钟 / 5分钟 / 10分钟". 0 = disabled.
static constexpr int SS_PRESET_SECS[] = {0, 30, 60, 180, 300, 600};

// Theme colors matching the home/HA deep-blue palette
static constexpr uint32_t C_BG      = 0x0D1B2A;
static constexpr uint32_t C_ACCENT  = 0x4FA3FF;
static constexpr uint32_t C_LBL     = 0xBFD3E6;
static constexpr uint32_t C_CARD    = 0x132840;
static constexpr uint32_t C_BTN     = 0x2E5A8F;
static constexpr uint32_t C_SAVE    = 0x2E8B57;

AppSettings::AppSettings()
{
    setAppInfo().name = "设置";
}

void AppSettings::onCreate() {}

void AppSettings::onOpen()
{
    mclog::tagInfo(_tag, "open");
    _buildUi();
    _installSwipeGesture();
}

void AppSettings::onRunning() {}

void AppSettings::onClose()
{
    mclog::tagInfo(_tag, "close");
    _removeSwipeGesture();
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _vol_slider = _brt_slider = _vol_lbl = _brt_lbl = nullptr;
    _ssid_ta = _pass_ta = _host_ta = _ssid_dd = _ss_dd = _kb = _status_lbl = nullptr;
    if (_close_cb) _close_cb();
}

// ─── Swipe-up gesture ─────────────────────────────────────────────────────────
void AppSettings::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                // Ignore the touch that's only meant to wake the screensaver.
                if (screensaver::isActive()) return;
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    lv_async_call([](void* udata) {
                        auto* app = static_cast<AppSettings*>(udata);
                        if (app) app->close();
                    }, lv_event_get_user_data(e));
                }
            }, LV_EVENT_GESTURE, this);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppSettings::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, this);
        _gesture_indev = nullptr;
    }
}

// ─── UI ───────────────────────────────────────────────────────────────────────
void AppSettings::_buildUi()
{
    auto* hal = GetHAL();

    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* title = lv_label_create(_scr);
    lv_label_set_text(title, "设置");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(title, &font_zh_36, 0);

    // Layout constants
    const int LBL_X  = 40;
    const int SLD_X  = 260;
    const int SLD_W  = 800;
    const int FLD_X  = 300;
    const int FLD_W  = 560;
    int y             = 86;
    const int ROW_H   = 72;
    const int SLD_H   = 50;   // touch-friendly slider height

    // Helper: row label
    auto make_label = [&](const char* text, int yy) {
        lv_obj_t* l = lv_label_create(_scr);
        lv_label_set_text(l, text);
        lv_obj_set_pos(l, LBL_X, yy + 16);
        lv_obj_set_style_text_color(l, lv_color_hex(C_LBL), 0);
        lv_obj_set_style_text_font(l, &font_zh_36, 0);
    };

    // Helper: value label (right of slider)
    auto make_val_lbl = [&](const char* init_text, int yy) -> lv_obj_t* {
        lv_obj_t* l = lv_label_create(_scr);
        lv_label_set_text(l, init_text);
        lv_obj_set_pos(l, SLD_X + SLD_W + 18, yy + 12);
        lv_obj_set_style_text_color(l, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        return l;
    };

    // Helper: slider
    auto make_slider = [&](int yy, int min_v, int max_v, int cur_v) -> lv_obj_t* {
        lv_obj_t* sl = lv_slider_create(_scr);
        lv_obj_set_pos(sl, SLD_X, yy + (ROW_H - SLD_H) / 2);
        lv_obj_set_size(sl, SLD_W, SLD_H);
        lv_slider_set_range(sl, min_v, max_v);
        lv_slider_set_value(sl, cur_v, LV_ANIM_OFF);
        // Track
        lv_obj_set_style_bg_color(sl, lv_color_hex(0x1A3050), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(sl, SLD_H / 2, LV_PART_MAIN);
        // Filled portion
        lv_obj_set_style_bg_color(sl, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_radius(sl, SLD_H / 2, LV_PART_INDICATOR);
        // Knob
        lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        lv_obj_set_style_radius(sl, SLD_H / 2, LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, 4, LV_PART_KNOB);
        return sl;
    };

    // ── Volume slider ──────────────────────────────────────────────────────
    int cur_vol = xiaozhi_get_speaker_volume();
    make_label("音量", y);
    _vol_slider = make_slider(y, 0, 100, cur_vol);
    char vbuf[8]; snprintf(vbuf, sizeof(vbuf), "%d%%", cur_vol);
    _vol_lbl = make_val_lbl(vbuf, y);
    lv_obj_add_event_cb(_vol_slider, _vol_slider_cb, LV_EVENT_VALUE_CHANGED, this);
    y += ROW_H;

    // ── Brightness slider ──────────────────────────────────────────────────
    int cur_brt = hal->getDisplayBrightness();
    make_label("亮度", y);
    _brt_slider = make_slider(y, 10, 100, cur_brt);
    char bbuf[8]; snprintf(bbuf, sizeof(bbuf), "%d%%", cur_brt);
    _brt_lbl = make_val_lbl(bbuf, y);
    lv_obj_add_event_cb(_brt_slider, _brt_slider_cb, LV_EVENT_VALUE_CHANGED, this);
    y += ROW_H;

    // Divider
    lv_obj_t* div = lv_obj_create(_scr);
    lv_obj_set_pos(div, LBL_X, y + 4);
    lv_obj_set_size(div, 1280 - LBL_X * 2, 2);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x1E3A5F), 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_clear_flag(div, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    y += 20;

    // ── WiFi / HA fields ───────────────────────────────────────────────────
    auto make_ta = [&](int yy, bool password) {
        lv_obj_t* ta = lv_textarea_create(_scr);
        lv_obj_set_pos(ta, FLD_X, yy);
        lv_obj_set_size(ta, FLD_W, ROW_H - 12);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_password_mode(ta, password);
        lv_obj_set_style_text_font(ta, &font_zh_36, 0);
        lv_obj_add_event_cb(ta, _ta_event_cb, LV_EVENT_ALL, this);
        return ta;
    };

    // WiFi SSID + scan button
    make_label("WiFi 名称", y);
    _ssid_ta = make_ta(y, false);
    lv_obj_set_width(_ssid_ta, FLD_W - 150);
    lv_textarea_set_text(_ssid_ta, hal->getConfig("wifi_ssid", DEF_SSID).c_str());

    lv_obj_t* scan_btn = lv_obj_create(_scr);
    lv_obj_set_size(scan_btn, 130, ROW_H - 12);
    lv_obj_set_pos(scan_btn, FLD_X + FLD_W - 130, y);
    lv_obj_set_style_bg_color(scan_btn, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_radius(scan_btn, 12, 0);
    lv_obj_set_style_border_width(scan_btn, 0, 0);
    lv_obj_clear_flag(scan_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scan_btn, _scan_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, "扫描");
    lv_obj_center(scan_lbl);
    lv_obj_set_style_text_color(scan_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(scan_lbl, &font_zh_36, 0);
    y += ROW_H;

    // Scan-result dropdown
    make_label("搜索结果", y);
    _ssid_dd = lv_dropdown_create(_scr);
    lv_obj_set_pos(_ssid_dd, FLD_X, y);
    lv_obj_set_size(_ssid_dd, FLD_W, ROW_H - 12);
    lv_dropdown_set_options(_ssid_dd, "(点扫描)");
    lv_obj_set_style_text_font(_ssid_dd, &font_zh_36, 0);
    lv_obj_add_event_cb(_ssid_dd, _dd_event_cb, LV_EVENT_VALUE_CHANGED, this);
    y += ROW_H;

    // WiFi password
    make_label("WiFi 密码", y);
    _pass_ta = make_ta(y, true);
    lv_textarea_set_text(_pass_ta, hal->getConfig("wifi_pass", "").c_str());
    y += ROW_H;

    // HA server host
    make_label("HA 服务器 IP", y);
    _host_ta = make_ta(y, false);
    lv_textarea_set_text(_host_ta, hal->getConfig("ha_host", DEF_HOST).c_str());
    y += ROW_H;

    // ── Screensaver idle timeout ─────────────────────────────────────────────
    // After this idle time, any app screen (home / 智能家居 / 小智 / 设置) is
    // covered by the daily Bing wallpaper. "关闭" disables it.
    make_label("屏保时间", y);
    _ss_dd = lv_dropdown_create(_scr);
    lv_obj_set_pos(_ss_dd, FLD_X, y);
    lv_obj_set_size(_ss_dd, FLD_W, ROW_H - 12);
    // ASCII option labels on purpose: the dropdown's popup list is a separate
    // widget that uses the default (montserrat) font, which has no Chinese
    // glyphs, so Chinese options would render as boxes. ASCII renders in any
    // font. SS_PRESET_SECS stays the source of truth for the actual seconds.
    lv_dropdown_set_options(_ss_dd, "OFF\n30s\n1 min\n3 min\n5 min\n10 min");
    lv_obj_set_style_text_font(_ss_dd, &lv_font_montserrat_28, 0);
    {
        // Select the option matching the stored seconds (default 60 → 1分钟).
        int cur = 60;
        try { cur = std::stoi(hal->getConfig("ss_idle_s", "60")); } catch (...) {}
        int idx = 2;  // fallback: 1分钟
        for (int i = 0; i < (int)(sizeof(SS_PRESET_SECS) / sizeof(SS_PRESET_SECS[0])); i++)
            if (SS_PRESET_SECS[i] == cur) { idx = i; break; }
        lv_dropdown_set_selected(_ss_dd, idx);
    }
    y += ROW_H;

    // Save button
    lv_obj_t* save_btn = lv_obj_create(_scr);
    lv_obj_set_size(save_btn, 280, 60);
    lv_obj_set_pos(save_btn, FLD_X, y + 6);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(C_SAVE), 0);
    lv_obj_set_style_radius(save_btn, 14, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_clear_flag(save_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(save_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(save_btn, _save_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "保存并重启");
    lv_obj_center(save_lbl);
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(save_lbl, &font_zh_36, 0);

    // Status line
    _status_lbl = lv_label_create(_scr);
    lv_label_set_text(_status_lbl, "");
    lv_obj_set_pos(_status_lbl, FLD_X + 300, y + 18);
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(0xFFD166), 0);
    lv_obj_set_style_text_font(_status_lbl, &font_zh_36, 0);

    // Keyboard (bottom half, hidden until a field is focused)
    _kb = lv_keyboard_create(_scr);
    lv_obj_set_size(_kb, 1280, 300);
    lv_obj_align(_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_kb, _kb_event_cb, LV_EVENT_ALL, this);

    lv_screen_load(_scr);
}

// ─── WiFi scan ────────────────────────────────────────────────────────────────
void AppSettings::_doScan()
{
    lv_label_set_text(_status_lbl, "扫描中…");
    auto aps = GetHAL()->wifiScan();
    if (aps.empty()) {
        lv_dropdown_set_options(_ssid_dd, "(未发现网络)");
        lv_label_set_text(_status_lbl, "未发现网络，可手动输入");
        return;
    }
    std::string opts;
    for (size_t i = 0; i < aps.size(); i++) {
        if (i) opts += "\n";
        opts += aps[i].ssid;
    }
    lv_dropdown_set_options(_ssid_dd, opts.c_str());
    lv_dropdown_set_selected(_ssid_dd, 0);
    lv_textarea_set_text(_ssid_ta, aps[0].ssid.c_str());
    lv_label_set_text(_status_lbl, "");
}

// ─── Save ─────────────────────────────────────────────────────────────────────
void AppSettings::_save()
{
    auto* hal = GetHAL();
    std::string ssid = lv_textarea_get_text(_ssid_ta);
    std::string pass = lv_textarea_get_text(_pass_ta);
    std::string host = lv_textarea_get_text(_host_ta);

    if (ssid.empty()) {
        lv_label_set_text(_status_lbl, "请填写 WiFi 名称");
        return;
    }

    hal->setConfig("wifi_ssid", ssid);
    hal->setConfig("wifi_pass", pass);
    if (!host.empty()) hal->setConfig("ha_host", host);

    // Screensaver idle timeout (seconds; 0 = off). Applied on reboot.
    if (_ss_dd) {
        int idx = lv_dropdown_get_selected(_ss_dd);
        int n   = (int)(sizeof(SS_PRESET_SECS) / sizeof(SS_PRESET_SECS[0]));
        if (idx < 0 || idx >= n) idx = 2;
        hal->setConfig("ss_idle_s", std::to_string(SS_PRESET_SECS[idx]));
    }

    lv_label_set_text(_status_lbl, "已保存，正在重启…");
    mclog::tagInfo(_tag, "saved ssid={} host={}, rebooting", ssid, host);

    hal->delay(800);
    hal->reboot();
}

// ─── Event callbacks ──────────────────────────────────────────────────────────
void AppSettings::_vol_slider_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    int v = lv_slider_get_value(self->_vol_slider);
    xiaozhi_set_speaker_volume(v);
    char buf[8]; snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(self->_vol_lbl, buf);
}

void AppSettings::_brt_slider_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    int v = lv_slider_get_value(self->_brt_slider);
    GetHAL()->setDisplayBrightness((uint8_t)v);
    char buf[8]; snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(self->_brt_lbl, buf);
}

void AppSettings::_ta_event_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(self->_kb, ta);
        lv_obj_clear_flag(self->_kb, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(self->_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppSettings::_kb_event_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(self->_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppSettings::_scan_btn_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_doScan();
}

void AppSettings::_dd_event_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    char buf[64] = {0};
    lv_dropdown_get_selected_str(self->_ssid_dd, buf, sizeof(buf));
    if (buf[0] && buf[0] != '(') {
        lv_textarea_set_text(self->_ssid_ta, buf);
    }
}

void AppSettings::_save_btn_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_save();
}
