#pragma once
#include <mooncake.h>
#include <lvgl.h>

class AppHome;

/**
 * Idle screensaver — always-on WorkerAbility (installed once at boot, not a
 * user-launchable app). Watches LVGL's global input-inactivity timer; after
 * the configured idle timeout with no touch anywhere on the device:
 *  - screensaver enabled  → full-screen clock + date + weather overlay on
 *    lv_layer_top(), any touch dismisses it;
 *  - screensaver disabled → no overlay, backlight goes off instead (any
 *    touch restores the user's brightness). 真机实测背光驱动低占空比有
 *    视觉下限, PWM 调不到近黑, 所以闲置直接 duty 0 熄屏。
 * Config (checkbox + idle timeout) lives in the home gear dialog, persisted
 * to NVS keys screensaver_en / screensaver_min; polled every CFG_MS here.
 */
class AppIdleScreen : public mooncake::WorkerAbility {
public:
    // AppHome already fetches+caches weather for its own status bar; reuse
    // that instead of running a second HTTP poller.
    void setHomeApp(AppHome* home) { _home = home; }
    void dismiss();
    // 独占模式: 收音机等流媒体 app 打开期间抑制屏保(不弹时钟遮盖 + 省其轮询)
    void setSuppressed(bool s);
private:
    bool _suppressed = false;
public:

    void onCreate()  override;
    void onResume()  override;
    void onRunning() override;
    void onPause()   override;

private:
    static constexpr uint32_t MOVE_MS      = 5000;   // reposition every 5s (burn-in guard)
    static constexpr uint32_t CFG_MS       = 3000;   // poll NVS config every 3s

    void _show();
    void _hide();
    void _updateClock();
    void _reposition();
    void _refreshConfig();
    void _dim();
    void _undim();

    AppHome* _home = nullptr;

    lv_obj_t* _overlay     = nullptr;
    lv_obj_t* _group       = nullptr;  // time+date+weather move together
    lv_obj_t* _time_lbl    = nullptr;
    lv_obj_t* _date_lbl    = nullptr;
    lv_obj_t* _weather_lbl = nullptr;
    uint32_t  _last_clock_update_ms = 0;
    uint32_t  _last_move_ms         = 0;
    int       _pos_index            = 0;

    // ── NVS-backed config (screensaver_en / screensaver_min) ──
    uint32_t  _idle_timeout_ms = 5 * 60 * 1000;  // 闲置超时 (2/5/10 分钟)
    bool      _ss_enabled      = true;           // 勾选=闲置进屏保; 不勾=降亮度
    bool      _dimmed          = false;          // 已处于 5% 亮度
    uint8_t   _pre_dim_brt     = 100;            // 降亮前的用户亮度 (恢复用)
    uint32_t  _last_cfg_ms     = 0;
};
