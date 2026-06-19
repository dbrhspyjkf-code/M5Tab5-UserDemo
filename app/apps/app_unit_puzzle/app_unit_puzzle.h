#pragma once

#include <mooncake.h>
#include <lvgl.h>
#include <led_strip.h>
#include <driver/gpio.h>   // gpio_num_t / GPIO_NUM_53 used by DIN_GPIO
#include <esp_err.h>       // esp_err_t / ESP_OK used by _init_err and _stripInit decl
#include <atomic>
#include <functional>
#include <mutex>

/**
 * AppUnitPuzzle — 8x8 WS2812 LED 矩阵 (M5Stack Unit-Puzzle U193) 测试 App.
 *
 * 接线: Grove → Tab5 PORT A
 *   - Signal (DIN, 黄) → GPIO53 (Tab5 PORT.A 的 Signal 引脚, 与 EXT_I2C SDA 复用)
 *   - 5V 由 PI4IOE1 P2 (EXT5V_EN) 控制, 上电默认开, 无需额外操作
 *
 * 8 种预设图案 + 1 个开关按钮, 全部跑在 lvgl 的 FreeRTOS 任务里 (lv_timer).
 * LED 的实际刷新由 led_strip 组件的 RMT 异步完成, 不阻塞 UI.
 *
 * 退出: swipe-up 手势 (与 Claude app / 小智一致, 见 _installSwipeGesture).
 */
class AppUnitPuzzle : public mooncake::AppAbility {
public:
    AppUnitPuzzle();

    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    static constexpr int W = 8;
    static constexpr int H = 8;
    static constexpr int N = W * H;  // 64 LEDs
    // PORT A Signal 真身: GPIO53 (Tab5 的 M5Stack UiFlow/PORT.A 引脚定义)
    // 注意: 与 EXT_I2C SDA 复用 (BSP_EXT_I2C_SDA). 用作 WS2812 DIN 时
    // RMT 接管此 pin, I2C bus 不可用.
    static constexpr gpio_num_t DIN_GPIO = GPIO_NUM_53;

    // ── State ──────────────────────────────────────────────────────────────
    std::function<void()> _close_cb;
    led_strip_handle_t    _strip = nullptr;
    lv_obj_t*             _scr = nullptr;
    lv_obj_t*             _status_lbl = nullptr;   // 顶部诊断状态条
    lv_obj_t*             _diag_lbl = nullptr;    // 详细诊断信息 (RMT 状态/像素计数)
    uint8_t               _brightness = 48;  // ~19%, 长时间工作更凉
    std::atomic<bool>     _led_on{false};    // 总开关
    std::atomic<int>      _pattern{0};       // 当前播放的图案索引
    std::atomic<bool>     _diag_mode{false}; // true → GPIO toggle 测试模式
    std::atomic<int>      _frames_sent{0};   // 累计发送到 strip 的帧数
    lv_timer_t*           _anim_timer = nullptr;
    lv_timer_t*           _diag_timer = nullptr;
    std::mutex            _strip_mu;         // 保护 _strip 的销毁/使用竞态
    esp_err_t             _init_err = ESP_OK; // 记录 RMT init 结果

    // ── 文字图案 (pattern=8) 的输入区与缓冲区 ──────────────────────────────
    char        _text_buf[17] = "";  // 用户在 textarea 输入的 ASCII, 最多 16 字符
    int         _text_scroll = 0;    // 水平滚动偏移 (像素)
    lv_obj_t*   _text_ta     = nullptr;  // textarea 控件
    lv_obj_t*   _text_ui[3]  = {nullptr, nullptr, nullptr};  // [label, ta, ok_btn] 一起显隐

    // ── Methods ────────────────────────────────────────────────────────────
    void _buildUi();
    void _installSwipeGesture();
    void _removeSwipeGesture();

    // Public-ish helper for the swipe-up gesture: ask the framework to close
    // this app (runs the close callback installed by app_installer.h, which
    // restores the home screen).
    void _requestClose() { if (_close_cb) _close_cb(); }

    // LED 驱动
    esp_err_t _stripInit();
    void _stripDeinit();
    void _setPixel(int i, uint8_t r, uint8_t g, uint8_t b);
    void _setPixelXY(int x, int y, uint8_t r, uint8_t g, uint8_t b);
    void _clear();
    void _show();
    static int _xy2i(int x, int y);  // 蛇形走线 (默认 row-major 蛇形)

    // 图案 (每帧重画一帧, 由 lv_timer 驱动)
    void _drawPattern();

    // 把 ASCII 字符串渲染到 8x8 LED: 每个字符取 unscii_8 字形位图,
    // 按从左到右排进 8x8 缓冲区, 总宽 >8 时按 phase 滚动.
    void _renderText(const char* s, int n, int phase,
                     uint8_t r, uint8_t g, uint8_t b);

    // 诊断模式: GPIO toggle 1Hz, 帮助用户用万用表/LED 二极管定位 PORT A Signal
    void _enterDiagMode();
    void _exitDiagMode();
    static void _diagTick(lv_timer_t* t);

    // 诊断 UI 更新
    void _updateDiag();

    // 图案回调 (按 _pattern 当前值分发)
    static void _animTick(lv_timer_t* t);
    static void _pattern_btn_cb(lv_event_t* e);
    static void _gesture_event_cb(lv_event_t* e);
};