#include "app_settings.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <string>

static const std::string _tag = "app-settings";

// Chinese font defined in app_ha/view/font_zh_36.c
extern lv_font_t font_zh_36;

// Defaults mirror the compile-time fallbacks in hal_wifi.cpp / app_ha.cpp so the
// fields show the currently-effective values.
static constexpr const char* DEF_SSID = "ChinaNet-11G";
static constexpr const char* DEF_HOST = "192.168.1.142";

AppSettings::AppSettings()
{
    setAppInfo().name = "设置";
}

void AppSettings::onCreate() {}

void AppSettings::onOpen()
{
    mclog::tagInfo(_tag, "open");
    _buildUi();
}

void AppSettings::onRunning() {}

void AppSettings::onClose()
{
    mclog::tagInfo(_tag, "close");
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    if (_close_cb) _close_cb();
}

// ─── UI ───────────────────────────────────────────────────────────────────────
void AppSettings::_buildUi()
{
    auto* hal = GetHAL();

    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x0D1B2A), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Back button (top-left)
    lv_obj_t* back = lv_obj_create(_scr);
    lv_obj_set_size(back, 120, 56);
    lv_obj_set_pos(back, 24, 20);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x1E3A5F), 0);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, _back_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "返回");
    lv_obj_center(back_lbl);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(back_lbl, &font_zh_36, 0);

    // Title
    lv_obj_t* title = lv_label_create(_scr);
    lv_label_set_text(title, "设置");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &font_zh_36, 0);

    // Layout columns for the form (top half; keyboard occupies bottom half)
    const int LBL_X = 40;
    const int FLD_X = 300;
    const int FLD_W = 560;
    int y = 110;
    const int ROW_H = 64;

    auto make_label = [&](const char* text, int yy) {
        lv_obj_t* l = lv_label_create(_scr);
        lv_label_set_text(l, text);
        lv_obj_set_pos(l, LBL_X, yy + 12);
        lv_obj_set_style_text_color(l, lv_color_hex(0xBFD3E6), 0);
        lv_obj_set_style_text_font(l, &font_zh_36, 0);
    };

    auto make_ta = [&](int yy, bool password) {
        lv_obj_t* ta = lv_textarea_create(_scr);
        lv_obj_set_pos(ta, FLD_X, yy);
        lv_obj_set_size(ta, FLD_W, ROW_H - 8);
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
    lv_obj_set_size(scan_btn, 130, ROW_H - 8);
    lv_obj_set_pos(scan_btn, FLD_X + FLD_W - 130, y);
    lv_obj_set_style_bg_color(scan_btn, lv_color_hex(0x2E5A8F), 0);
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

    // Scan-result dropdown (filled after a scan)
    make_label("搜索结果", y);
    _ssid_dd = lv_dropdown_create(_scr);
    lv_obj_set_pos(_ssid_dd, FLD_X, y);
    lv_obj_set_size(_ssid_dd, FLD_W, ROW_H - 8);
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

    // Save button
    lv_obj_t* save_btn = lv_obj_create(_scr);
    lv_obj_set_size(save_btn, 280, 60);
    lv_obj_set_pos(save_btn, FLD_X, y + 6);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x2E8B57), 0);
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

    // Shared on-screen keyboard (bottom half, hidden until a field is focused)
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
    // Auto-fill SSID field with the strongest result.
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

    lv_label_set_text(_status_lbl, "已保存，正在重启…");
    mclog::tagInfo(_tag, "saved ssid={} host={}, rebooting", ssid, host);

    // Give LVGL one frame to paint the status, then reboot to apply.
    hal->delay(800);
    hal->reboot();
}

// ─── Event callbacks ──────────────────────────────────────────────────────────
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

void AppSettings::_back_btn_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->close();
}
