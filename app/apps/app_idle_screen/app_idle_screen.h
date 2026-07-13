#pragma once
#include <mooncake.h>
#include <lvgl.h>

class AppHome;

/**
 * Idle screensaver — always-on WorkerAbility (installed once at boot, not a
 * user-launchable app). Watches LVGL's global input-inactivity timer; after
 * IDLE_MS with no touch anywhere on the device, covers the screen with a
 * full-screen clock + date + weather overlay on lv_layer_top(). Any touch on
 * the overlay dismisses it immediately.
 */
class AppIdleScreen : public mooncake::WorkerAbility {
public:
    // AppHome already fetches+caches weather for its own status bar; reuse
    // that instead of running a second HTTP poller.
    void setHomeApp(AppHome* home) { _home = home; }

    void onCreate()  override;
    void onResume()  override;
    void onRunning() override;
    void onPause()   override;

private:
    static constexpr uint32_t IDLE_MS      = 4 * 60 * 1000;  // 4min of no touch → show
    static constexpr uint32_t MOVE_MS      = 5000;   // reposition every 5s (burn-in guard)

    void _show();
    void _hide();
    void _updateClock();
    void _reposition();

    AppHome* _home = nullptr;

    lv_obj_t* _overlay     = nullptr;
    lv_obj_t* _group       = nullptr;  // time+date+weather move together
    lv_obj_t* _time_lbl    = nullptr;
    lv_obj_t* _date_lbl    = nullptr;
    lv_obj_t* _weather_lbl = nullptr;
    uint32_t  _last_clock_update_ms = 0;
    uint32_t  _last_move_ms         = 0;
    int       _pos_index            = 0;
};
