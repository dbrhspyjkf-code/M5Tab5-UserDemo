#pragma once
#include <mooncake.h>
#include <lvgl.h>
#include <functional>

/**
 * AppXiaoZhi - Mooncake AppAbility wrapper around xiaozhi-esp32's Application.
 *
 * Lifecycle:
 *  onCreate   : (noop — Application::Initialize() runs once in app_main)
 *  onOpen     : load xiaozhi's LVGL screen; ensure Application is initialized
 *  onRunning  : noop — xiaozhi runs in its own FreeRTOS task
 *  onClose    : restore AppHome screen via close callback
 */
class AppXiaoZhi : public mooncake::AppAbility {
public:
    AppXiaoZhi();

    /** Called by AppHome before it restores its screen. */
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    std::function<void()> _close_cb;
    bool _initialized = false;
    // ── 3s 无用户语音自动退出 (2026-11-10 定稿) ──
    int64_t _last_voice_ms = 0;  // 最近一次"有语音活动"的时刻 (状态跳转或 VAD 听到人声)
    int _last_state = -1;        // 上一 tick 的设备状态 (跳转即视为有活动)
    lv_indev_t* _gesture_indev = nullptr;
    lv_obj_t* _scr = nullptr;   // web 占位屏 (PLATFORM_BUILD_WEB only)
    // 唤醒时正在显示的画面 (设备路径): 模糊背景显示它, 关闭时也回到它;
    // 为空时兜底走 _close_cb (home->restoreScreen 回主页)
    lv_obj_t* _return_scr = nullptr;

    void _installSwipeGesture();
    void _removeSwipeGesture();
};
