#include "app_email_led.h"

#include <mooncake_log.h>

#ifndef PLATFORM_BUILD_DESKTOP
#include <esp_timer.h>
#include "../app_settings/app_settings.h"
#include "../app_rfid/app_rfid.h"  // requestPortA/releasePortA (PORT A 仲裁转发)
#endif

static const char* TAG = "email_led";

// ─────────────────────────────────────────────────────────────────────────────
// 构造 / 析构 / 生命周期
// ─────────────────────────────────────────────────────────────────────────────
AppEmailLed::AppEmailLed() {}

AppEmailLed::~AppEmailLed() {}

void AppEmailLed::onCreate()
{
    mclog::tagInfo(TAG, "email-led notifier created");
}

void AppEmailLed::onResume() {}

void AppEmailLed::onRunning()
{
#ifndef PLATFORM_BUILD_DESKTOP
    // 每 2 分钟主动刷新一次未读数 (异步, 任意线程安全). 不依赖 home 在前台轮询.
    int64_t now_us = esp_timer_get_time();
    if (_last_poll_us == 0 || now_us - _last_poll_us > 120LL * 1000 * 1000) {
        _last_poll_us = now_us;
        AppSettings::fetchEmail();
    }
#endif
}

void AppEmailLed::onPause() {}

void AppEmailLed::onDestroy() {}

// ─────────────────────────────────────────────────────────────────────────────
// app 交接接口 (PORT A 仲裁转发, 类名沿用历史)
// ─────────────────────────────────────────────────────────────────────────────
void AppEmailLed::setPortAOwnedByApp(bool owned)
{
#ifndef PLATFORM_BUILD_DESKTOP
    // 转发给后台寻卡任务 (PORT A 待机时的默认主人):
    // true  = 等它确认停手后才返回, 抢占方随后才配 UART, 不会互相踩;
    // false = 触发它重走初始化 (gpio_reset_pin 清引脚残留) 恢复嘀卡.
    if (owned) AppRfid::requestPortA();
    else       AppRfid::releasePortA();
#else
    (void)owned;
#endif
}

void AppEmailLed::retirePortA()
{
#ifndef PLATFORM_BUILD_DESKTOP
    // 2026-11-08 LED 矩阵播放随灯阵硬件功能移除后, 本类不再驱动 GPIO;
    // 接口保留给 AppRfid 启动路径调用 (历史语义: PORT A 专属后台 NFC).
    mclog::tagInfo(TAG, "PORT A retired (owned by background RFID service)");
#endif
}
