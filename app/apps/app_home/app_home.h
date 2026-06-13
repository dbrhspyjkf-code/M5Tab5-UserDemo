#pragma once
#include <mooncake.h>
#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>

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

    static void _btn_event_cb(lv_event_t* e);
};
