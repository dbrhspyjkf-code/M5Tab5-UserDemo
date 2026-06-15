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
    lv_indev_t* _gesture_indev = nullptr;

    void _installSwipeGesture();
    void _removeSwipeGesture();
};
