#pragma once

#include <mooncake.h>
#include <atomic>

#ifndef PLATFORM_BUILD_DESKTOP
#include <led_strip.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <mutex>
#endif

/**
 * AppEmailLed — 后台未读邮件 LED 通知 (WorkerAbility, 开机常驻).
 *
 * 当 AppSettings::email_unread_total > 0 时, 在 PORT A 的 8x8 WS2812 矩阵
 * (Unit-Puzzle, 与「灯阵」app 同一块) 上间歇滚动显示蓝色 "NEW EMAIL".
 *
 * 不是用户可点的 app: 在 app_installer.h 里用 createAbility + resumeWorkerAbility
 * 注册一次 (和 AppVoiceInput 一致).
 *
 * GPIO53 (PORT A Signal) 同时被「灯阵」app (RMT/WS2812) 和「LoRa 聊天」app
 * (UART) 抢用. 这两个 app 打开时必须独占该引脚, 所以它们在 onOpen 里调
 * setPortAOwnedByApp(true) 让本通知器立即释放硬件, 在 onClose 里调
 * setPortAOwnedByApp(false) 把控制权还回来. mooncake 单线程协作调度,
 * 因此这套交接没有真正的数据竞争.
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

    // 「灯阵」/「LoRa」app 在 onOpen(true 之前自行 init 硬件) / onClose(false) 调用.
    // true: 标记 PORT A 归 app 所有, 并立即释放本通知器持有的 led_strip.
    // false: 解除标记, 下一 tick 若仍有未读会自动重新接管.
    static void setPortAOwnedByApp(bool owned);

private:
#ifndef PLATFORM_BUILD_DESKTOP
    // 5 块 8x8 串成 40x8 横排 (与「灯阵」app 同一条灯带, DIN 进最左块).
    static constexpr int        PANEL  = 8;
    static constexpr int        PANELS = 5;
    static constexpr int        W = PANEL * PANELS;   // 40
    static constexpr int        H = PANEL;            // 8
    static constexpr int        N = W * H;            // 320
    static constexpr gpio_num_t DIN_GPIO = GPIO_NUM_53;

    static AppEmailLed*        s_instance;
    static std::atomic<bool>   s_app_owns_porta;

    led_strip_handle_t _strip      = nullptr;
    std::mutex         _strip_mu;
    uint8_t            _brightness = 48;     // 与「灯阵」app 一致, ~19%, 夜间不刺眼
    bool               _active     = false;  // 当前是否持有并在驱动 strip
    int64_t            _last_poll_us  = 0;    // 上次 fetchEmail 的时间戳
    int64_t            _last_frame_us = 0;    // 上次刷帧的时间戳 (限流)

    esp_err_t _stripInit();
    void      _stripDeinit();
    void      _setPixelXY(int x, int y, uint8_t r, uint8_t g, uint8_t b);
    static int _xy2i(int x, int y);
    void      _renderFrame(int64_t now_ms);  // 间歇滚动 "NEW EMAIL"
    void      _standDown();                   // 清屏 + 释放硬件
#endif
};
