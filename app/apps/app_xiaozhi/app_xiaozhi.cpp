#include "app_xiaozhi.h"
#include "xiaozhi_ctl.h"

#include <mooncake_log.h>
#include <hal/hal.h>
#include <esp_timer.h>

static const char* TAG = "AppXiaoZhi";

AppXiaoZhi::AppXiaoZhi()
{
    setAppInfo().name = "小智";
}

void AppXiaoZhi::onCreate()
{
    // Lazy init: do NOT start xiaozhi here. Starting at install time means any
    // failure in xiaozhi init crashes the device into a boot loop before the
    // launcher is usable. We start it on first open instead.
}

void AppXiaoZhi::onOpen()
{
    mclog::tagInfo(TAG, "open — start (if needed) + activate xiaozhi screen");
    // Spawn the xiaozhi task on first open (guarded internally so repeated
    // opens don't spawn it again).
    xiaozhi_start_task();
    // Resume the audio pipeline if a previous close suspended it (no-op on the
    // very first open / while already running).
    xiaozhi_resume();
    xiaozhi_activate_screen();
    _installSwipeGesture();
}

void AppXiaoZhi::onRunning()
{
    // Update battery reading every 10 s for the xiaozhi info bar.
    static int64_t s_last_batt_ms = 0;
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_batt_ms > 10000) {
        s_last_batt_ms = now_ms;
        auto* hal = GetHAL();
        if (hal) {
            hal->updatePowerMonitorData();
            float v = hal->powerMonitorData.busVoltage;
            // NP-F550 7.4 V pack: 8.4 V = 100 %, 6.0 V = 0 %
            int pct = -1;
            if (v > 0.5f) {
                if      (v >= 8.4f) pct = 100;
                else if (v <= 6.0f) pct = 0;
                else pct = (int)((v - 6.0f) / 2.4f * 100.0f);
            }
            xiaozhi_set_battery_percent(pct);
        }
    }
}

void AppXiaoZhi::onClose()
{
    mclog::tagInfo(TAG, "close — suspend xiaozhi + restore home screen");
    // Fully suspend the audio pipeline so it stops listening/talking in the
    // background and frees its task stacks + DMA. Otherwise a heavy app like HA
    // could exhaust internal RAM and crash/reboot the device.
    xiaozhi_suspend();
    _removeSwipeGesture();
    if (_close_cb) {
        _close_cb();
    }
}

void AppXiaoZhi::_installSwipeGesture()
{
    _removeSwipeGesture();

    // Register on the pointer indev directly so the gesture fires regardless of
    // which LVGL object the user touches (chat area, emotion area, etc.).
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    lv_async_call([](void* udata) {
                        auto* app = static_cast<AppXiaoZhi*>(udata);
                        if (app) app->close();
                    }, lv_event_get_user_data(e));
                }
            }, LV_EVENT_GESTURE, this);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppXiaoZhi::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, this);
        _gesture_indev = nullptr;
    }
}
