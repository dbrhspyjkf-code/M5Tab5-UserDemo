#pragma once
#include <mooncake.h>
#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <mutex>

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

    // Weather is fetched asynchronously (home has no HA client of its own), so a
    // worker thread only writes these strings; onRunning copies them to labels.
    std::string _weather_temp;
    std::string _weather_cond;
    std::mutex  _weather_mutex;
    void _fetch_weather();

    static void _btn_event_cb(lv_event_t* e);
};
