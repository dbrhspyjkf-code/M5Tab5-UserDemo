#include "app_email_led.h"

#include <mooncake_log.h>

#ifndef PLATFORM_BUILD_DESKTOP
#include <hal/hal.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include "../app_settings/app_settings.h"
#endif

static const char* TAG = "email_led";

// ─────────────────────────────────────────────────────────────────────────────
// ESP-only 实现 (桌面模拟器无 LED 硬件, 整块编译成 no-op)
// ─────────────────────────────────────────────────────────────────────────────
#ifndef PLATFORM_BUILD_DESKTOP

// "NEW EMAIL" 的 5x7 位图, 按消息顺序排好 (N E W ' ' E M A I L).
// 位布局与「灯阵」app 的 font5x7 一致: bit4=最左列, bit0=最右列, byte0=顶行.
// E 的最右列刻意留暗, 防止 LED 光晕把 E 看成 B (与灯阵 app 同款修正).
static constexpr int MSG_LEN = 9;
static const uint8_t MSG_GLYPHS[MSG_LEN][7] = {
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11},  // N
    {0x1E,0x10,0x10,0x1C,0x10,0x10,0x1E},  // E
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},  // W
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // ' '
    {0x1E,0x10,0x10,0x1C,0x10,0x10,0x1E},  // E
    {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},  // M
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},  // A
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},  // I
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},  // L
};

// 排版 / 节奏常量
static constexpr int CHAR_W   = 5;
static constexpr int CHAR_H   = 7;
static constexpr int CHAR_GAP = 1;
static constexpr int CELL     = CHAR_W + CHAR_GAP;          // 每字符 6px
static constexpr int Y_OFF    = 0;                          // (8-7)/2, 顶部对齐 (与灯阵一致)
static constexpr int TOTAL_W  = MSG_LEN * CELL - CHAR_GAP;  // 9*6-1 = 53px

static constexpr int SCROLL_MS_PER_PX = 150;              // 滚动速度
static constexpr int SCROLL_RANGE     = TOTAL_W + 40;    // 完全滚出 40 宽屏幕需要的像素

// 静态成员定义
AppEmailLed*       AppEmailLed::s_instance = nullptr;
std::atomic<bool>  AppEmailLed::s_app_owns_porta{false};

#endif  // !PLATFORM_BUILD_DESKTOP

// ─────────────────────────────────────────────────────────────────────────────
// 构造 / 析构 / 生命周期
// ─────────────────────────────────────────────────────────────────────────────
AppEmailLed::AppEmailLed() {}

AppEmailLed::~AppEmailLed()
{
#ifndef PLATFORM_BUILD_DESKTOP
    if (s_instance == this) s_instance = nullptr;
#endif
}

void AppEmailLed::onCreate()
{
#ifndef PLATFORM_BUILD_DESKTOP
    s_instance = this;
#endif
    mclog::tagInfo(TAG, "email-led notifier created");
}

void AppEmailLed::onResume() {}

void AppEmailLed::onRunning()
{
#ifndef PLATFORM_BUILD_DESKTOP
    int64_t now_us = esp_timer_get_time();

    // 每 60s 主动刷新一次未读数 (异步, 任意线程安全). 不依赖 home 在前台轮询.
    if (_last_poll_us == 0 || now_us - _last_poll_us > 60LL * 1000 * 1000) {
        _last_poll_us = now_us;
        AppSettings::fetchEmail();
    }

    bool app_owns = s_app_owns_porta.load();
    int  unread   = AppSettings::email_unread_total.load();
    bool want     = (!app_owns && unread > 0);

    if (!want) {
        if (_active) _standDown();
        return;
    }

    // 需要显示但还没拿到硬件 → 尝试 init (app 可能刚释放完)
    if (!_active) {
        if (_stripInit() != ESP_OK) return;  // 拿不到, 下一 tick 再试
        _active = true;
        _last_frame_us = 0;
        mclog::tagInfo(TAG, "unread={}, taking over LED", unread);
    }

    // 限流 ~33ms/帧 (滚动 150ms/px, 足够顺滑且少占 RMT 总线)
    if (_last_frame_us != 0 && now_us - _last_frame_us < 33 * 1000) return;
    _last_frame_us = now_us;

    _renderFrame(now_us / 1000);
#endif
}

void AppEmailLed::onPause()
{
#ifndef PLATFORM_BUILD_DESKTOP
    if (_active) _standDown();
#endif
}

void AppEmailLed::onDestroy()
{
#ifndef PLATFORM_BUILD_DESKTOP
    if (_active) _standDown();
    if (s_instance == this) s_instance = nullptr;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// app 交接接口
// ─────────────────────────────────────────────────────────────────────────────
void AppEmailLed::setPortAOwnedByApp(bool owned)
{
#ifndef PLATFORM_BUILD_DESKTOP
    s_app_owns_porta.store(owned);
    // 立即把硬件让出去, 这样 app 的 _stripInit / _uartInit 不会撞 RMT/GPIO.
    if (owned && s_instance) s_instance->_standDown();
#else
    (void)owned;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// LED 驱动 (ESP-only)
// ─────────────────────────────────────────────────────────────────────────────
#ifndef PLATFORM_BUILD_DESKTOP

esp_err_t AppEmailLed::_stripInit()
{
    std::lock_guard<std::mutex> lk(_strip_mu);
    if (_strip) return ESP_OK;

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num         = DIN_GPIO;
    strip_cfg.max_leds               = N;
    strip_cfg.led_model              = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz     = 10 * 1000 * 1000;  // 10 MHz (与灯阵一致)
    rmt_cfg.mem_block_symbols = 96;

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &_strip);
    if (err != ESP_OK) {
        // 常见于 app 还没完全释放 RMT/GPIO; 不算硬错, 下一 tick 重试.
        ESP_LOGW(TAG, "led_strip init failed (will retry): %s", esp_err_to_name(err));
        _strip = nullptr;
        return err;
    }
    led_strip_clear(_strip);
    return ESP_OK;
}

void AppEmailLed::_stripDeinit()
{
    std::lock_guard<std::mutex> lk(_strip_mu);
    if (_strip) {
        led_strip_del(_strip);
        _strip = nullptr;
    }
}

void AppEmailLed::_standDown()
{
    {
        std::lock_guard<std::mutex> lk(_strip_mu);
        if (_strip) {
            led_strip_clear(_strip);
            led_strip_refresh(_strip);
        }
    }
    _stripDeinit();
    _active = false;
}

// 与「灯阵」app 完全相同的走线映射: 5 块 8x8 串成 40x8 横排.
// 面板物理安装逆时针偏转 90°, 顺时针补偿: 逻辑 (lx,y) → 物理 (row=lx, col=7-y).
int AppEmailLed::_xy2i(int x, int y)
{
    int panel = x / PANEL;
    int lx    = x % PANEL;
    return panel * (PANEL * PANEL) + lx * PANEL + (PANEL - 1 - y);
}

void AppEmailLed::_setPixelXY(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (!_strip || x < 0 || x >= W || y < 0 || y >= H) return;
    r = (r * _brightness) >> 8;
    g = (g * _brightness) >> 8;
    b = (b * _brightness) >> 8;
    led_strip_set_pixel(_strip, _xy2i(x, y), r, g, b);
}

// 间歇滚动: 滚一遍 "NEW EMAIL" (从右进、整体滚出左屏) → 暂停 2.5s 熄屏 → 循环.
void AppEmailLed::_renderFrame(int64_t now_ms)
{
    constexpr int SCROLL_DUR = SCROLL_RANGE * SCROLL_MS_PER_PX;  // 滚完一遍耗时
    constexpr int PAUSE_MS   = 2500;
    constexpr int CYCLE      = SCROLL_DUR + PAUSE_MS;

    {
        std::lock_guard<std::mutex> lk(_strip_mu);
        if (!_strip) return;
        led_strip_clear(_strip);

        int cycle_t = (int)(now_ms % CYCLE);
        if (cycle_t < SCROLL_DUR) {
            int scroll_px = cycle_t / SCROLL_MS_PER_PX;
            int x_start   = W - scroll_px;  // 第 0 字符左边缘 x

            for (int ci = 0; ci < MSG_LEN; ci++) {
                int cx = x_start + ci * CELL;
                if (cx + CHAR_W <= 0) continue;  // 完全在左屏外
                if (cx >= W) break;              // 完全在右屏外
                const uint8_t* bmp = MSG_GLYPHS[ci];
                for (int col = 0; col < CHAR_W; col++) {
                    int x = cx + col;
                    if (x < 0 || x >= W) continue;
                    for (int row = 0; row < CHAR_H; row++) {
                        if (bmp[row] & (1 << (4 - col))) {
                            _setPixelXY(x, Y_OFF + row, 0, 0, 255);  // 蓝色
                        }
                    }
                }
            }
        }
        // 暂停段: 不画任何像素 (上面已 clear), 屏幕熄灭

        led_strip_refresh(_strip);
    }
}

#endif  // !PLATFORM_BUILD_DESKTOP
