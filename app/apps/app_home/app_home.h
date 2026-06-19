#pragma once
#include <mooncake.h>
#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

class AppHome : public mooncake::AppAbility {
public:
    struct AppEntry {
        std::string name;
        int         app_id;
    };

    AppHome();

    // Called from app_installer after all apps are installed
    void addApp(const std::string& name, int app_id);

    // Called by child app's close callback — reloads home screen before the
    // child app destroys its own screen objects.
    void restoreScreen();

    // Inject the "open email page" callback. The status-bar mail icon needs
    // to (1) open the AppSettings app (if not yet open) and (2) jump straight
    // to its email sub-page — coupling that to AppSettings would create a
    // circular header dependency, so the installer wires the lambda here.
    using OpenEmailCb = std::function<void()>;
    void setOpenEmailHandler(OpenEmailCb cb) { _open_email = std::move(cb); }

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    std::vector<AppEntry> _apps;
    lv_obj_t*  _scr          = nullptr;
    int        _child_app_id = -1;

    // Parallel array to _apps — stable pointers needed for LVGL event user_data
    struct BtnData { AppHome* home; int app_id; };
    std::vector<BtnData> _btn_data;

    // Top system status bar (clock / WiFi / battery). The per-app status bars
    // (xiaozhi, HA) were removed; status now lives only on the home screen.
    lv_obj_t* _status_time    = nullptr;
    lv_obj_t* _status_date    = nullptr;
    lv_obj_t* _status_wtemp   = nullptr;  // weather temperature (latin font, has °)
    lv_obj_t* _status_wcond   = nullptr;  // weather condition (zh font)
    lv_obj_t* _status_wifi    = nullptr;
    lv_obj_t* _status_batt    = nullptr;
    uint32_t  _last_status_ms = 0;
    uint32_t  _last_batt_ms    = 0;
    uint32_t  _last_weather_ms = 0;

    lv_obj_t* _status_tools   = nullptr;  // gear/quick-settings icon (left of battery)
    lv_obj_t* _status_email   = nullptr;  // mail icon (left of wifi), only visible when unread>0
    OpenEmailCb _open_email;              // tap handler: jump into settings email sub-page
    uint32_t  _last_email_ms  = 0;        // throttle for the 60s email poll

    // Weather is fetched asynchronously (home has no HA client of its own), so a
    // worker thread only writes these strings; onRunning copies them to labels.
    std::string _weather_temp;
    std::string _weather_cond;
    std::mutex  _weather_mutex;
    void _fetch_weather();

    // Weather detail popup (tap the status-bar weather). A worker fetches the
    // full HA weather entity + daily forecast into _wx_text; onRunning copies it
    // into the _wx_body label once _wx_ready flips.
    lv_obj_t*         _wx_body  = nullptr;
    std::string       _wx_text;
    std::atomic<bool> _wx_ready{false};
    void _openWeatherDialog();
    void _fetchWeatherDetail();

    static void _btn_event_cb(lv_event_t* e);

    // ── Status-bar quick-access popups ──────────────────────────────────────────
    // A modal popup lives on lv_layer_top() so it covers the whole screen. Only
    // one is open at a time; _modal is its backdrop (nullptr when none).
    lv_obj_t* _modal      = nullptr;
    lv_obj_t* _qs_vol_lbl = nullptr;  // quick-settings volume %
    lv_obj_t* _qs_brt_lbl = nullptr;  // quick-settings brightness %
    lv_obj_t* _net_ssid   = nullptr;  // network dialog: SSID textarea
    lv_obj_t* _net_pass   = nullptr;  // network dialog: password textarea
    lv_obj_t* _net_host   = nullptr;  // network dialog: HA host textarea
    lv_obj_t* _net_svc    = nullptr;  // network dialog: 其他服务器 (天气/Claude) textarea
    lv_obj_t* _net_ssid_dd = nullptr; // network dialog: scan-result dropdown
    lv_obj_t* _net_kb     = nullptr;  // network dialog: on-screen keyboard
    lv_obj_t* _net_status = nullptr;  // network dialog: status line

    void _closeModal();
    lv_obj_t* _openModal(int card_w, int card_h, const char* title);
    void _openPowerDialog();
    void _openQuickSettings();
    void _openNetworkDialog();
    void _doNetworkScan();
    void _doNetworkSave();

    static void _statusClick_cb(lv_event_t* e);  // routes by user_data tag
    static void _modalBg_cb(lv_event_t* e);
    static void _qsVol_cb(lv_event_t* e);
    static void _qsBrt_cb(lv_event_t* e);
    static void _netTa_cb(lv_event_t* e);
    static void _netKb_cb(lv_event_t* e);
};
