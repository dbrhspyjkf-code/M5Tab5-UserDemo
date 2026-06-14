#include "app_xiaozhi.h"
#include "xiaozhi_ctl.h"

#include <mooncake_log.h>
#include <hal/hal.h>
#include <esp_timer.h>

static const char* TAG = "AppXiaoZhi";
static constexpr int PILL_W      = 200;
static constexpr int PILL_H      = 40;
static constexpr int PILL_BOTTOM = 12;

static int active_display_width()
{
    lv_display_t* disp = lv_display_get_default();
    return disp ? lv_display_get_horizontal_resolution(disp) : 1280;
}

static int active_display_height()
{
    lv_display_t* disp = lv_display_get_default();
    return disp ? lv_display_get_vertical_resolution(disp) : 720;
}

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
    _installExitButton();
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
    _removeExitButton();
    if (_close_cb) {
        _close_cb();
    }
}

void AppXiaoZhi::_installExitButton()
{
    _removeExitButton();

    lv_obj_t* scr = lv_screen_active();
    if (!scr) return;

    const int w = active_display_width();
    const int h = active_display_height();
    _exit_button = lv_obj_create(scr);
    lv_obj_set_size(_exit_button, PILL_W, PILL_H);
    lv_obj_set_pos(_exit_button, (w - PILL_W) / 2, h - PILL_H - PILL_BOTTOM);
    lv_obj_set_style_bg_color(_exit_button, lv_color_hex(0x0A1520), 0);
    lv_obj_set_style_bg_opa(_exit_button, LV_OPA_70, 0);
    lv_obj_set_style_radius(_exit_button, PILL_H / 2, 0);
    lv_obj_set_style_border_width(_exit_button, 0, 0);
    lv_obj_clear_flag(_exit_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_exit_button, LV_OBJ_FLAG_CLICKABLE);

    // White indicator bar centered inside pill
    lv_obj_t* bar = lv_obj_create(_exit_button);
    lv_obj_set_size(bar, 80, 5);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_80, 0);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_center(bar);

    lv_obj_add_event_cb(_exit_button, [](lv_event_t* e) {
        lv_event_stop_bubbling(e);
        auto* app = static_cast<AppXiaoZhi*>(lv_event_get_user_data(e));
        if (app) app->close();
    }, LV_EVENT_CLICKED, this);
    lv_obj_move_to_index(_exit_button, -1);
}

void AppXiaoZhi::_removeExitButton()
{
    if (_exit_button) {
        lv_obj_delete(_exit_button);
        _exit_button = nullptr;
    }
}
