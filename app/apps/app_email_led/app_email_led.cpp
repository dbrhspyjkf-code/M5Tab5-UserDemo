#include "app_email_led.h"

#include <mooncake_log.h>

#ifndef PLATFORM_BUILD_DESKTOP
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

void AppEmailLed::onRunning() {}

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
    // 2026-11-12 起后台邮件轮询也移除 (用户明确邮件不再需要 LED 功能),
    // 不再注册 WorkerAbility 实例, 本类只剩这两个静态转发接口.
    // 接口保留给 AppRfid 启动路径调用 (历史语义: PORT A 专属后台 NFC).
    mclog::tagInfo(TAG, "PORT A retired (owned by background RFID service)");
#endif
}
