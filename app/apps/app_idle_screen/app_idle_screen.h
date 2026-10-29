#pragma once
#include <mooncake.h>
#include <lvgl.h>
#include <atomic>
#include <mutex>

class AppHome;

/**
 * Idle screensaver — always-on WorkerAbility (installed once at boot, not a
 * user-launchable app). Watches LVGL's global input-inactivity timer; after
 * the configured idle timeout with no touch anywhere on the device:
 *  - screensaver enabled  → full-screen photo slideshow (hermes
 *    /screensaver/photo.rgb565, one photo per minute; first-failure falls
 *    back to the old clock+date+weather overlay), any touch dismisses it;
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
    static constexpr uint32_t PHOTO_MS     = 60 * 1000;  // 照片每分钟换一张

    void _show();
    void _hide();
    void _updateClock();
    void _reposition();
    void _refreshConfig();
    void _dim();
    void _undim();

    // ── 照片屏保 (2026-10-29): hermes /screensaver/photo.rgb565 ──
    // 双缓冲 ping-pong: worker 下载到 _pb_next → LVGL 线程应用换下 _pb_cur。
    // 缓冲布局 [lv_image_dsc_t][1280*720*2 像素] (app_player 封面同款);
    // 服务端 PIL 已转 RGB565, 设备零解码。首张连败(服务未重启/空目录)回退
    // 时钟模式。
    void _photoTick();            // LVGL 线程: 应用/到点换图/失败回退/补拉
    void _fetchPhotoAsync(int idx);
    void _applyNextPhoto();
    void _buildClockContent();    // 时钟内容(回退模式; 原屏保 UI)
    lv_obj_t* _photo_img = nullptr;
    lv_obj_t* _ph_scrim = nullptr;     // 右下角时间日期底衬 (半透明黑圆角)
    lv_obj_t* _ph_time_lbl = nullptr;  // "HH:MM" montserrat_44
    lv_obj_t* _ph_week_lbl = nullptr;  // "周三 农历xx" zh 30px (行2)
    uint32_t  _ph_clock_ms = 0;        // 照片页时间刷新节流
    void _updatePhotoClock();
    void*     _pb_cur   = nullptr;
    void*     _pb_next  = nullptr;
    std::atomic<bool>    _pb_ready{false};
    std::atomic<bool>    _pb_fail{false};
    std::atomic<bool>    _fetching{false};
    std::atomic<uint32_t> _ss_gen{0};   // 会话代: show/hide 递增, 在途 worker 据此丢弃
    std::mutex _pm;                     // _pb_* 发布/释放互斥(与 worker)
    bool      _clock_mode  = false;
    int       _pidx        = 0;
    uint32_t  _last_photo_ms  = 0;
    uint32_t  _photo_retry_ms = 0;
    int       _photo_fails    = 0;

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
