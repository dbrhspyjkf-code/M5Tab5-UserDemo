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
    // ── 3s 自动退出 (2026-11-10 定稿: 唤醒后没说话 / 回答后没接话) ──
    int64_t _last_voice_ms = 0;  // 最近一次"有活动"的时刻 (状态跳转/VAD/STT 即重置)
    int _last_state = -1;        // 上一 tick 的设备状态
    bool _heard_user = false;    // 本轮 Listening 已听到用户说话 (VAD/STT) → 等回复不退出
    int64_t _stt_at_listen_start = 0;  // 进入本轮 Listening 时的 STT 基线 (检测本轮新 STT)
    bool _answered = false;      // 小智已完整回答过至少一次 (状态离开 Speaking)
    lv_indev_t* _gesture_indev = nullptr;
    lv_obj_t* _scr = nullptr;   // web 占位屏 (PLATFORM_BUILD_WEB only)

    void _installSwipeGesture();
    void _removeSwipeGesture();
};
