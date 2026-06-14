/**
 * @file app_settings.h
 * @brief On-screen settings: WiFi (scan + password) and HA server address.
 *        Saves to NVS via the HAL config store and reboots to apply.
 */
#pragma once
#include <mooncake.h>
#include <smooth_lvgl.h>
#include <functional>

class AppSettings : public mooncake::AppAbility {
public:
    AppSettings();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

    // Called when the user backs out, so the launcher can restore its screen.
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

private:
    std::function<void()> _close_cb;

    lv_obj_t* _scr        = nullptr;
    lv_obj_t* _ssid_ta    = nullptr;
    lv_obj_t* _pass_ta    = nullptr;
    lv_obj_t* _host_ta    = nullptr;
    lv_obj_t* _ssid_dd    = nullptr;  // scan-result dropdown
    lv_obj_t* _kb         = nullptr;  // shared on-screen keyboard
    lv_obj_t* _status_lbl = nullptr;

    void _buildUi();
    void _doScan();
    void _save();

    static void _ta_event_cb(lv_event_t* e);
    static void _kb_event_cb(lv_event_t* e);
    static void _scan_btn_cb(lv_event_t* e);
    static void _dd_event_cb(lv_event_t* e);
    static void _save_btn_cb(lv_event_t* e);
    static void _back_btn_cb(lv_event_t* e);
};
