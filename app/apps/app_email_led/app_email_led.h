#pragma once

#include <mooncake.h>

/**
 * AppEmailLed — PORT A 仲裁转发 (类名沿用历史).
 *
 * 「NEW EMAIL」LED 矩阵播放已于 2026-11-08 随灯阵硬件功能一并移除;
 * 后台邮件轮询 (原每 2 分钟 fetchEmail) 已于 2026-11-12 按用户指示移除 —
 * 未读邮件由主页状态栏 (120s) 与工具页自行轮询, 本类不再注册
 * WorkerAbility 常驻实例 (app_installer.h), 只剩下面两个静态接口.
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
};
