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

    // 就地重建主页屏幕（主题切换用）：删旧屏重跑 onOpen。
    void rebuild();

    // Inject the "open email page" callback. The status-bar mail icon needs
    // to (1) open the AppSettings app (if not yet open) and (2) jump straight
    // to its email sub-page — coupling that to AppSettings would create a
    // circular header dependency, so the installer wires the lambda here.
    using OpenEmailCb = std::function<void()>;
    void setOpenEmailHandler(OpenEmailCb cb) { _open_email = std::move(cb); }

    // Cached weather (refreshed every 15 min by AppHome's own status bar) —
    // read by AppIdleScreen so it doesn't need its own fetch/cache/timer.
    void getWeather(std::string& temp, std::string& cond)
    {
        std::lock_guard<std::mutex> lk(_weather_mutex);
        temp = _weather_temp;
        cond = _weather_cond;
    }

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

    // Hero card on the home screen (orange, video-style, 2026-10-26). It
    // replaces the old top status bar: clock / date / lunar / weather lines
    // plus the wifi / mail / gear / battery icon row at the card bottom.
    lv_obj_t* _status_time    = nullptr;
    lv_obj_t* _status_date    = nullptr;
    lv_obj_t* _status_lunar   = nullptr;  // 农历 line under the date
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

    // Home dashboard, all fed by the hermes service on svc_host:8766. Right
    // column above the app buttons: 3 video-style list rows (GLM / DeepSeek /
    // Apple 提醒日程 via /agenda) + a wide OrangePi card (/system/status,
    // 3x2 module grid with threshold-colored bars, 参考 Mac 的
    // orange-pi.widget). Same pattern as the weather: a detached worker
    // writes strings/struct, onRunning copies them to widgets.
    lv_obj_t*  _dash_glm = nullptr;
    lv_obj_t*  _dash_ds  = nullptr;
    lv_obj_t*  _dash_ag1 = nullptr;
    lv_obj_t*  _dash_ag2 = nullptr;
    uint32_t   _last_dash_ms = 0;
    std::string _dash_glm_txt, _dash_ds_txt, _dash_ag1_txt, _dash_ag2_txt;

    struct OpiModule { lv_obj_t* val = nullptr; lv_obj_t* bar = nullptr; };
    OpiModule  _opi_mod[5] = {};    // 负载 / CPU / 内存 / 交换 / 磁盘
    lv_obj_t*  _opi_uptime = nullptr;
    lv_obj_t*  _opi_ip     = nullptr;
    lv_obj_t*  _opi_temp   = nullptr;  // "48℃"
    lv_obj_t*  _opi_fan    = nullptr;  // "风扇 1200" / "被动散热"（温度格）
    lv_obj_t*  _opi_dot    = nullptr;  // header 状态点

    struct OpiData {
        bool ok = false;
        std::string ip, uptime;
        double load1 = 0, load5 = 0, load15 = 0, cpu = 0;
        double mem_pct = 0, swap_pct = 0, disk_pct = 0, temp = 0;
        int mem_used_a = 0, mem_tot_a = 0;  // GB×10, 保留 1 位小数
        int disk_used = 0, disk_tot = 0;    // GB 整数
        int fan_rpm = -1;                   // -1 = 无风扇数据(被动)
    };
    OpiData    _opi;                  // guarded by _dash_mutex
    std::mutex  _dash_mutex;
    void _fetch_dashboard();
    void _updateOpiWidgets();  // 把 _opi 刷进右缘 OPi 卡; 调用方需已持有 _dash_mutex

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
    lv_obj_t* _qs_theme_lbl = nullptr; // quick-settings theme-cycle button
    lv_obj_t* _net_ssid   = nullptr;  // network dialog: SSID textarea
    lv_obj_t* _net_pass   = nullptr;  // network dialog: password textarea
    lv_obj_t* _net_host   = nullptr;  // network dialog: server host textarea (writes both ha_host + svc_host)
    lv_obj_t* _net_reachy = nullptr;  // network dialog: Reachy 主机 textarea (NVS reachy_host)
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
    static void _qsTheme_cb(lv_event_t* e);   // theme::cycle() + rebuild
    void _refreshThemeBtn();                   // 按钮文字 ← 当前主题名
    static void _netTa_cb(lv_event_t* e);
    static void _netKb_cb(lv_event_t* e);
};
