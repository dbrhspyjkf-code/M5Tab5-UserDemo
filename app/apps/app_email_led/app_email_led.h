#pragma once

#include <mooncake.h>

#ifndef PLATFORM_BUILD_DESKTOP
#include <cstdint>
#endif

/**
 * AppEmailLed — 后台邮件状态轮询 + PORT A 仲裁转发 (WorkerAbility, 开机常驻).
 *
 * 每 2 分钟调 AppSettings::fetchEmail() 刷新未读数 (工具页邮件子页等消费).
 * 「NEW EMAIL」LED 矩阵播放已于 2026-11-08 随灯阵硬件功能一并移除,
 * 类名与文件名沿用历史.
 *
 * 不是用户可点的 app: 在 app_installer.h 里用 createAbility + resumeWorkerAbility
 * 注册一次 (和 AppVoiceInput 一致).
 *
 * GPIO53/54 (PORT A) 由「LoRa 聊天」等前台 app 与后台 NFC 寻卡共用: app 在
 * onOpen 里调 setPortAOwnedByApp(true) 请求独占 (内部转发 AppRfid::
 * requestPortA), onClose 里 setPortAOwnedByApp(false) 归还. mooncake 单线程
 * 协作调度, 因此这套交接没有真正的数据竞争.
 */
class AppEmailLed : public mooncake::WorkerAbility {
public:
    AppEmailLed();
    ~AppEmailLed() override;

    void onCreate()  override;
    void onResume()  override;
    void onRunning() override;
    void onPause()   override;
    void onDestroy() override;

    // 前台 app 在 onOpen(true 之前自行 init 硬件) / onClose(false) 调用.
    // true:  转发 AppRfid::requestPortA() —— 等后台寻卡任务确认停手后才返回.
    // false: 转发 AppRfid::releasePortA(), 后台寻卡重新接管 PORT A.
    static void setPortAOwnedByApp(bool owned);

    // RFID 后台服务 (嘀卡关灯) 常驻占用 PORT A 后调用一次 (历史接口, 现仅记日志).
    static void retirePortA();

private:
#ifndef PLATFORM_BUILD_DESKTOP
    int64_t _last_poll_us = 0;    // 上次 fetchEmail 的时间戳
#endif
};
