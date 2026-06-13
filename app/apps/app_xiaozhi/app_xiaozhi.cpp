#include "app_xiaozhi.h"
#include "xiaozhi_ctl.h"

#include <mooncake_log.h>

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
    xiaozhi_activate_screen();
}

void AppXiaoZhi::onRunning()
{
    // xiaozhi manages itself in its own FreeRTOS task
}

void AppXiaoZhi::onClose()
{
    mclog::tagInfo(TAG, "close — restore home screen");
    if (_close_cb) {
        _close_cb();
    }
}
