/**
 * @file app_settings.h
 * @brief On-screen settings: volume/brightness sliders, WiFi, HA server address.
 *        Saves to NVS via the HAL config store and reboots to apply WiFi/HA changes.
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

    lv_indev_t* _gesture_indev = nullptr;
    lv_obj_t* _scr        = nullptr;
    lv_obj_t* _vol_slider  = nullptr;
    lv_obj_t* _vol_lbl     = nullptr;
    lv_obj_t* _brt_slider  = nullptr;
    lv_obj_t* _brt_lbl     = nullptr;
    lv_obj_t* _ssid_ta    = nullptr;
    lv_obj_t* _pass_ta    = nullptr;
    lv_obj_t* _host_ta    = nullptr;
    lv_obj_t* _ssid_dd    = nullptr;  // scan-result dropdown
    lv_obj_t* _ss_dd      = nullptr;  // screensaver idle-timeout dropdown
    lv_obj_t* _kb         = nullptr;  // shared on-screen keyboard
    lv_obj_t* _status_lbl = nullptr;

    void _buildUi();
    void _doScan();
    void _save();
    void _installSwipeGesture();
    void _removeSwipeGesture();

    static void _ta_event_cb(lv_event_t* e);
    static void _kb_event_cb(lv_event_t* e);
    static void _scan_btn_cb(lv_event_t* e);
    static void _dd_event_cb(lv_event_t* e);
    static void _save_btn_cb(lv_event_t* e);
    static void _vol_slider_cb(lv_event_t* e);
    static void _brt_slider_cb(lv_event_t* e);
};
