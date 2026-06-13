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
    mclog::tagInfo(TAG, "starting xiaozhi task");
    xiaozhi_start_task();
}

void AppXiaoZhi::onOpen()
{
    mclog::tagInfo(TAG, "open — activate xiaozhi screen");
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
