#include "app_unit_puzzle.h"
#include "../app_email_led/app_email_led.h"

#include <hal/hal.h>
#include <mooncake_log.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <cmath>
#include <cstdio>
#include <functional>
// lv_timer_t struct is in lv_timer_private.h — don't include user code there.
// Use lv_timer_get_user_data() public API instead.

#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#endif

// 用 lv_font_montserrat_8 (8px 高度, ASCII) 替代 unscii_8 (项目 sdkconfig 没 enable).
// 字符实际高度 6-7px, 我们把它垂直居中放在 8x8 LED 上 (顶部 + 1px padding).
// montserrat_8 在 lvgl.h 里已 declare, 直接 extern 即可.
LV_FONT_DECLARE(lv_font_montserrat_8);

// =============================================================================
// 5x7 LED 字体 (本文件内置, 不调 lvgl font API, 避开 PSRAM 读 glyph + RMT 抢 bus)
// =============================================================================
// 格式: 每字符 7 字节, 每字节 1 行 (bit 0 = 左列, bit 4 = 右列, bit 5..6 = 0).
//      第 0 字节 = 顶部行, 第 6 字节 = 底部行.
// 只内置 4 个常用字符 (H/M/5/O), 其他 91 字符占位 (全 0 = 空白), 后续要加再补.
//
// 崩溃历史: v3/v4 用 lv_font_montserrat_8 在 _renderText 取 glyph bitmap 时
// 0x4811606a load fault. 原因疑为 RMT 写 LED 期间 PSRAM 字形数据被覆盖.
// v6 起改用本表, glyph 在 rodata (.text), XIP 自 PSRAM 段已验证可读.

// Bit layout: bit4=col0(left) … bit0=col4(right). Row0=top, Row6=bottom.
// Lowercase letters (0x61-0x7A) are rendered as uppercase in _drawPattern case 8.
static const uint8_t font5x7[95][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x20 ' '
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04},  // 0x21 '!'
    {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},  // 0x22 '"'
    {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00},  // 0x23 '#'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x24 '$'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x25 '%'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x26 '&'
    {0x04,0x04,0x00,0x00,0x00,0x00,0x00},  // 0x27 '\''
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02},  // 0x28 '('
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08},  // 0x29 ')'
    {0x00,0x11,0x0A,0x1F,0x0A,0x11,0x00},  // 0x2A '*'
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},  // 0x2B '+'
    {0x00,0x00,0x00,0x00,0x04,0x04,0x08},  // 0x2C ','
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},  // 0x2D '-'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x04},  // 0x2E '.'
    {0x02,0x02,0x04,0x04,0x08,0x08,0x10},  // 0x2F '/'
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},  // 0x30 '0'
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},  // 0x31 '1'
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},  // 0x32 '2'
    {0x0E,0x01,0x01,0x0E,0x01,0x01,0x0E},  // 0x33 '3'
    {0x11,0x11,0x11,0x1F,0x01,0x01,0x01},  // 0x34 '4'
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},  // 0x35 '5'
    {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},  // 0x36 '6'
    {0x1F,0x01,0x02,0x04,0x04,0x04,0x04},  // 0x37 '7'
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},  // 0x38 '8'
    {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E},  // 0x39 '9'
    {0x00,0x04,0x00,0x00,0x04,0x00,0x00},  // 0x3A ':'
    {0x00,0x04,0x00,0x00,0x04,0x04,0x08},  // 0x3B ';'
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02},  // 0x3C '<'
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},  // 0x3D '='
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08},  // 0x3E '>'
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},  // 0x3F '?'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x40 '@'
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},  // 0x41 'A'
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},  // 0x42 'B'
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},  // 0x43 'C'
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},  // 0x44 'D'
    {0x1E,0x10,0x10,0x1C,0x10,0x10,0x1E},  // 0x45 'E' (最右列全暗, 防光晕看成 B)
    {0x1E,0x10,0x10,0x1C,0x10,0x10,0x10},  // 0x46 'F'
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},  // 0x47 'G'
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},  // 0x48 'H'
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},  // 0x49 'I'
    {0x07,0x01,0x01,0x01,0x11,0x11,0x0E},  // 0x4A 'J'
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},  // 0x4B 'K'
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},  // 0x4C 'L'
    {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},  // 0x4D 'M'
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11},  // 0x4E 'N'
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},  // 0x4F 'O'
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},  // 0x50 'P'
    {0x0E,0x11,0x11,0x11,0x13,0x0E,0x03},  // 0x51 'Q'
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},  // 0x52 'R'
    {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},  // 0x53 'S'
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},  // 0x54 'T'
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},  // 0x55 'U'
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},  // 0x56 'V'
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},  // 0x57 'W'
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},  // 0x58 'X'
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04},  // 0x59 'Y'
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},  // 0x5A 'Z'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x5B '['
    {0x10,0x08,0x08,0x04,0x02,0x02,0x01},  // 0x5C '\'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x5D ']'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x5E '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},  // 0x5F '_'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x60 '`'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x61 'a' (用大写代替, 见渲染代码)
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x62 'b'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x63 'c'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x64 'd'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x65 'e'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x66 'f'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x67 'g'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x68 'h'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x69 'i'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x6A 'j'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x6B 'k'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x6C 'l'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x6D 'm'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x6E 'n'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x6F 'o'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x70 'p'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x71 'q'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x72 'r'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x73 's'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x74 't'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x75 'u'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x76 'v'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x77 'w'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x78 'x'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x79 'y'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x7A 'z'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x7B '{'
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04},  // 0x7C '|'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x7D '}'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 0x7E '~'
};

static const char* TAG = "puzzle";

// 小智 puhui 30px cbin 字体, GB2312 ~3500 字, XIP from flash.
// 跟 app_settings 工具页用同一个字体, 这样按钮上的中文 (红/绿/蓝/跑马/...)
// 不会回退到 montserrat 的 .notdef 方框.
static const lv_font_t* zh_font_30()
{
#ifndef PLATFORM_BUILD_DESKTOP
    extern const uint8_t font_puhui_common_30_4_bin_start[] asm("_binary_font_puhui_common_30_4_bin_start");
    static lv_font_t* f = nullptr;
    if (!f) f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
#else
    return &lv_font_montserrat_20;
#endif
}

// =============================================================================
// AppAbility 生命周期
// =============================================================================

AppUnitPuzzle::AppUnitPuzzle()
{
    setAppInfo().name = "灯阵";
}

void AppUnitPuzzle::onCreate()
{
    mclog::tagInfo(TAG, "on create");
    // 不在 onCreate 自动 open(). 历史 bug: 这里 open() 会被 mooncake 状态机
    // 立即触发, 导致开机后直接进入灯阵界面. 正确行为是只在工具页 tile
    // 主动 openApp() 时才启动灯阵. (home 之所以 open() 是因为它需要
    // 开机后立即显示, 但灯阵不应该.)
}

void AppUnitPuzzle::onOpen()
{
    mclog::tagInfo(TAG, "on open");

    // 独占 PORT A: 让后台邮件通知器先释放 RMT/GPIO53, 再由本 app 接管.
    AppEmailLed::setPortAOwnedByApp(true);

    _init_err = _stripInit();
    if (_init_err != ESP_OK) {
        mclog::tagError(TAG, "led_strip init failed: %s", esp_err_to_name(_init_err));
    }

    _buildUi();
    _installSwipeGesture();

    _led_on.store(true);
    _pattern.store(0);
    _diag_mode.store(false);

    _anim_timer = lv_timer_create(_animTick, 30, this);
    // 诊断信息每秒刷新一次 (RMT 状态 + 帧计数)
    _diag_timer = lv_timer_create(_diagTick, 1000, this);

    ESP_LOGI(TAG, "onOpen done, led_on=%d pattern=%d init_err=%s",
             (int)_led_on.load(), _pattern.load(), esp_err_to_name(_init_err));
}

void AppUnitPuzzle::onRunning()
{
    // 动画由 lv_timer 驱动, 这里无事可做
}

void AppUnitPuzzle::onClose()
{
    mclog::tagInfo(TAG, "on close");

    if (_anim_timer) {
        lv_timer_delete(_anim_timer);
        _anim_timer = nullptr;
    }
    if (_diag_timer) {
        lv_timer_delete(_diag_timer);
        _diag_timer = nullptr;
    }

    // 如果文字输入弹窗仍然打开, 先关掉 (避免悬空指针)
    if (_text_ui[0]) {
        auto* hal = GetHAL();
        if (hal && hal->lvKbGroup && _text_ta)
            lv_group_remove_obj(_text_ta);
        _text_ta = nullptr;
        lv_obj_delete(_text_ui[0]);
        _text_ui[0] = nullptr;
    }

    _removeSwipeGesture();

    // 关灯 (即使 strip init 失败也要安全)
    {
        std::lock_guard<std::mutex> lk(_strip_mu);
        if (_strip) {
            led_strip_clear(_strip);
            led_strip_refresh(_strip);
        }
    }

    _stripDeinit();

    // 把 PORT A 交还给后台邮件通知器 (下一 tick 若有未读会自动接管显示).
    AppEmailLed::setPortAOwnedByApp(false);

    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }

    if (_close_cb) _close_cb();
}

// 退出灯阵的唯一出口 (swipe-up + 返回按钮都经此). 因为 onClose 不会触发
// (见 .h 说明), 退出收尾必须在这里做: 停掉动画定时器、关灯、释放 RMT, 然后
// 把 PORT A 交还后台邮件通知器 —— 否则 setPortAOwnedByApp(true) 永不复位,
// worker 永久让位, 退出后再有未读也不会亮 LED.
void AppUnitPuzzle::_requestClose()
{
    _led_on.store(false);  // 让 _animTick 立即停止画 LED

    if (_anim_timer) { lv_timer_delete(_anim_timer); _anim_timer = nullptr; }
    if (_diag_timer) { lv_timer_delete(_diag_timer); _diag_timer = nullptr; }

    {
        std::lock_guard<std::mutex> lk(_strip_mu);
        if (_strip) {
            led_strip_clear(_strip);
            led_strip_refresh(_strip);
        }
    }
    _stripDeinit();

    AppEmailLed::setPortAOwnedByApp(false);

    if (_close_cb) _close_cb();
}

// =============================================================================
// LED strip 驱动封装
// =============================================================================

esp_err_t AppUnitPuzzle::_stripInit()
{
    std::lock_guard<std::mutex> lk(_strip_mu);

    if (_strip) return ESP_OK;

    ESP_LOGI(TAG, "_stripInit: GPIO=%d, N=%d, model=WS2812, fmt=GRB", (int)DIN_GPIO, N);

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num         = DIN_GPIO;
    strip_cfg.max_leds               = N;
    strip_cfg.led_model              = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz     = 10 * 1000 * 1000;  // 10 MHz (项目惯例)
    // 64 LED ≈ 1536 RMT symbols, 默认 48 太少, 显式给 96 保险
    rmt_cfg.mem_block_symbols = 96;

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "led_strip handle OK, _strip=%p", (void*)_strip);

    led_strip_clear(_strip);
    ESP_LOGI(TAG, "led_strip_clear done");
    return ESP_OK;
}

void AppUnitPuzzle::_enterDiagMode()
{
    _diag_mode.store(true);
    _led_on.store(false);
    // 释放 RMT channel, 让我们能直接控制 GPIO
    _stripDeinit();
    // 配置 GPIO 为普通 output
    gpio_reset_pin(DIN_GPIO);
    gpio_set_direction(DIN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DIN_GPIO, 0);
    ESP_LOGW(TAG, "DIAG MODE: GPIO %d released from RMT, toggling 1Hz", (int)DIN_GPIO);
}

void AppUnitPuzzle::_exitDiagMode()
{
    _diag_mode.store(false);
    gpio_reset_pin(DIN_GPIO);  // 释放回 high-Z, 让 led_strip 重新接管
    _init_err = _stripInit();
    _led_on.store(true);
    _pattern.store(0);
    _frames_sent.store(0);
    ESP_LOGW(TAG, "DIAG MODE: exit, RMT re-init err=%s", esp_err_to_name(_init_err));
}

void AppUnitPuzzle::_stripDeinit()
{
    std::lock_guard<std::mutex> lk(_strip_mu);
    if (_strip) {
        led_strip_del(_strip);
        _strip = nullptr;
    }
}

void AppUnitPuzzle::_setPixel(int i, uint8_t r, uint8_t g, uint8_t b)
{
    if (!_strip || i < 0 || i >= N) return;
    // 全局亮度缩放
    r = (r * _brightness) >> 8;
    g = (g * _brightness) >> 8;
    b = (b * _brightness) >> 8;
    led_strip_set_pixel(_strip, i, r, g, b);
}

void AppUnitPuzzle::_setPixelXY(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    _setPixel(_xy2i(x, y), r, g, b);
}

void AppUnitPuzzle::_clear()
{
    if (!_strip) return;
    led_strip_clear(_strip);
}

void AppUnitPuzzle::_show()
{
    if (!_strip) return;
    led_strip_refresh(_strip);
}

// 5 块 8x8 串成 40x8 横排 (DIN 在最左, 数据左→右). 单块走线是「逐行同向」
// (progressive/raster: 每行左→右), 不是蛇形. 现在横向正常显示 (不旋转):
// 逻辑 (x,y), x∈[0,40) 横向, y∈[0,8) 纵向 →
//   panel = x / 8 (第几块, 0=最左), lx = x % 8 (块内列),
//   块内 index = y*8 + lx, 全局 index = panel*64 + 块内 index.
// 若文字上下颠倒/左右镜像, 调这里 (y→7-y 或 lx→7-lx).
int AppUnitPuzzle::_xy2i(int x, int y)
{
    int panel = x / PANEL;          // 第几块面板
    int lx    = x % PANEL;          // 块内逻辑列 (0..7)
    // 面板物理安装方向逆时针偏转 90°, 顺时针补偿: 逻辑 (lx,y) → 物理 (row=lx, col=7-y)
    return panel * (PANEL * PANEL) + lx * PANEL + (PANEL - 1 - y);
}

// =============================================================================
// UI
// =============================================================================
//
// 给每个按钮分配一个 {this, idx} descriptor. 堆分配由按钮生命周期自动释放
// (在 _scr 删除时由 onClose 触发, 而 button 是 _scr 的子节点会跟着删除).
// LVGL lv_obj_set_user_data 只能存一个 void*, 所以把 descriptor 指针塞进去.

struct BtnDesc { AppUnitPuzzle* app; int idx; };
// 亮度 +/− 按钮的回调参数: app + delta (步长, 负数减小)
struct BrightDelta { AppUnitPuzzle* app; int delta; };

void AppUnitPuzzle::_pattern_btn_cb(lv_event_t* e)
{
    auto* desc = static_cast<BtnDesc*>(lv_event_get_user_data(e));
    if (!desc || !desc->app) return;
    AppUnitPuzzle* app = desc->app;

    if (desc->idx == -1) {
        // 开关
        bool was_on = app->_led_on.load();
        app->_led_on.store(!was_on);
        if (was_on) {
            app->_clear();
            app->_show();
            if (app->_status_lbl) lv_label_set_text(app->_status_lbl, "已关闭");
        } else {
            app->_pattern.store(0);
            if (app->_status_lbl) lv_label_set_text(app->_status_lbl, "已开启");
        }
    } else {
        if (desc->idx == 8) {
            // 文字模式: 弹出输入框让用户输入
            app->_showTextInput();
            return;
        }
        app->_pattern.store(desc->idx);
        if (app->_status_lbl) {
            char buf[32];
            snprintf(buf, sizeof(buf), "图案 %d", desc->idx);
            lv_label_set_text(app->_status_lbl, buf);
        }
    }
}

void AppUnitPuzzle::_buildUi()
{
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x0D1B2A), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t* title = lv_label_create(_scr);
    lv_label_set_text(title, "Unit-Puzzle 8x8 WS2812");
    lv_obj_set_style_text_color(title, lv_color_hex(0x4FA3FF), 0);
    lv_obj_set_style_text_font(title, zh_font_30(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    // 返回按钮 (左上角): 退出灯阵, 回到工具页. 上拨手势也能退出 (见 _installSwipeGesture).
    lv_obj_t* back_btn = lv_obj_create(_scr);
    lv_obj_set_size(back_btn, 150, 70);
    lv_obj_set_pos(back_btn, 20, 12);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(back_btn, 16, 0);
    lv_obj_set_style_border_width(back_btn, 2, 0);
    lv_obj_set_style_border_color(back_btn, lv_color_hex(0x6A8FB5), 0);
    lv_obj_clear_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);

    lv_obj_t* back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "← 返回");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(back_lbl, zh_font_30(), 0);
    lv_obj_set_align(back_lbl, LV_ALIGN_CENTER);

    // 点击在 LVGL 线程上, 但 _requestClose 会触发 onClose 删除 _scr (本按钮的父节点),
    // 同步删会 use-after-free. 用 lv_async_call 延后到事件处理完再关.
    lv_obj_add_event_cb(back_btn, [](lv_event_t* e) {
        auto* app = static_cast<AppUnitPuzzle*>(lv_event_get_user_data(e));
        if (!app) return;
        lv_async_call([](void* u) {
            static_cast<AppUnitPuzzle*>(u)->_requestClose();
        }, app);
    }, LV_EVENT_CLICKED, this);

    // 状态行
    _status_lbl = lv_label_create(_scr);
    lv_label_set_text(_status_lbl, "已开启");
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(0xEAF3FB), 0);
    lv_obj_set_style_text_font(_status_lbl, zh_font_30(), 0);
    lv_obj_align(_status_lbl, LV_ALIGN_TOP_MID, 0, 48);

    // 诊断信息 (RMT 状态/帧计数/diag mode)
    _diag_lbl = lv_label_create(_scr);
    lv_label_set_text(_diag_lbl, "...");
    lv_obj_set_style_text_color(_diag_lbl, lv_color_hex(0xFFB300), 0);
    lv_obj_set_style_text_font(_diag_lbl, zh_font_30(), 0);
    lv_obj_set_style_text_align(_diag_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_diag_lbl, 1200);
    lv_obj_align(_diag_lbl, LV_ALIGN_TOP_MID, 0, 78);
    _updateDiag();  // 立即填充

    // 9 个图案按钮 (3 行 × 3 列), idx 8 是"文字" (输入文字后在 LED 上滚动显示)
    static const char* labels[9] = {
        "红", "绿", "蓝", "白",
        "跑马", "心跳", "笑脸", "彩虹",
        "文字",
    };
    static const uint32_t colors[9] = {
        0xFF3344, 0x33FF66, 0x3388FF, 0xFFEEAA,
        0x9C27B0, 0xE91E63, 0xFFB300, 0x00BCD4,
        0x4FA3FF,  // 文字按钮: 跟主题强调色一致 (蓝)
    };

    // 3x3 网格: BTN_W=260, GAP=30, 起点 x=(1280-3*260-2*30)/2 = 220
    const int GRID_X0 = 220;
    const int GRID_Y0 = 110;
    const int BTN_W   = 260;
    const int BTN_H   = 100;
    const int GAP_X   = 30;
    const int GAP_Y   = 20;

    for (int i = 0; i < 9; i++) {
        int col = i % 3;
        int row = i / 3;
        int x = GRID_X0 + col * (BTN_W + GAP_X);
        int y = GRID_Y0 + row * (BTN_H + GAP_Y);

        lv_obj_t* btn = lv_obj_create(_scr);
        lv_obj_set_size(btn, BTN_W, BTN_H);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_bg_color(btn, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lbl, zh_font_30(), 0);
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);

        // 堆分配的 descriptor: 生命周期跟随 _scr (button 是 _scr 的孩子, 关闭时一起删)
        auto* desc = new BtnDesc{this, i};
        lv_obj_add_event_cb(btn, _pattern_btn_cb, LV_EVENT_CLICKED, desc);
    }

    // idx=8 "文字"模式: 点击后弹出 _showTextInput() 输入框,
    // 确认后文字存入 _text_buf, _drawPattern case 8 滚动显示.

    // 亮度调节按钮: − 和 + (放在 GPIO 诊断按钮的左边, 避开被覆盖)
    //   diag 起点 x=750, 所以 +/− 必须在 x<750
    //   +: x=610, 60x80  (范围 610-670)
    //   −: x=680, 60x80  (范围 680-740)  跟 diag (750-) 留 10px 间隔
    //   顺序 (从左到右): +, −, GPIO 诊断, 电源
    auto make_bright_btn = [&](int x, const char* sym, int delta) {
        lv_obj_t* b = lv_obj_create(_scr);
        lv_obj_set_size(b, 60, 80);
        lv_obj_set_pos(b, x, 600);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A3A), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x6A8FB5), 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        // 按下变深
        lv_obj_set_style_bg_color(b, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);

        lv_obj_t* lbl = lv_label_create(b);
        lv_label_set_text(lbl, sym);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lbl, zh_font_30(), 0);
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);

        // 回调里直接读 app + 改 brightness (UI timer 单线程, 不需要 atomic)
        auto* payload = new BrightDelta{this, delta};
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            auto* p = static_cast<BrightDelta*>(lv_event_get_user_data(e));
            if (!p || !p->app) return;
            int v = p->app->_brightness + p->delta;
            if (v < 0)   v = 0;
            if (v > 255) v = 255;
            p->app->_brightness = (uint8_t)v;
        }, LV_EVENT_CLICKED, payload);
    };
    make_bright_btn(610, "+", +32);
    make_bright_btn(680, "−", -32);

    // 开关按钮 (idx = -1)
    lv_obj_t* power_btn = lv_obj_create(_scr);
    lv_obj_set_size(power_btn, 200, 80);
    lv_obj_set_pos(power_btn, 1000, 600);
    lv_obj_set_style_bg_color(power_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(power_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(power_btn, 16, 0);
    lv_obj_set_style_border_width(power_btn, 2, 0);
    lv_obj_set_style_border_color(power_btn, lv_color_hex(0xFF4444), 0);
    lv_obj_clear_flag(power_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(power_btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* power_lbl = lv_label_create(power_btn);
    // 不带 LV_SYMBOL_POWER (U+F011 FontAwesome 私有区) — puhui 30 画不出, 会 load fault.
    // 改用纯中文 "开关" 走 puhui 30 字体, 跟其他按钮风格一致.
    lv_label_set_text(power_lbl, "开关");
    lv_obj_set_style_text_color(power_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(power_lbl, zh_font_30(), 0);
    lv_obj_set_align(power_lbl, LV_ALIGN_CENTER);

    auto* power_desc = new BtnDesc{this, -1};
    lv_obj_add_event_cb(power_btn, _pattern_btn_cb, LV_EVENT_CLICKED, power_desc);

    // 诊断按钮: 进入/退出 GPIO toggle 测试模式
    lv_obj_t* diag_btn = lv_obj_create(_scr);
    lv_obj_set_size(diag_btn, 240, 80);
    lv_obj_set_pos(diag_btn, 1000 - 250, 600);  // 紧贴 power 按钮左边
    lv_obj_set_style_bg_color(diag_btn, lv_color_hex(0x4A3A00), 0);
    lv_obj_set_style_bg_opa(diag_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(diag_btn, 16, 0);
    lv_obj_set_style_border_width(diag_btn, 2, 0);
    lv_obj_set_style_border_color(diag_btn, lv_color_hex(0xFFB300), 0);
    lv_obj_clear_flag(diag_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(diag_btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* diag_lbl_btn = lv_label_create(diag_btn);
    // 不带 LV_SYMBOL_SETTINGS (U+F013) 同上原因, 改用纯中文 "GPIO 测试".
    lv_label_set_text(diag_lbl_btn, "GPIO 测试");
    lv_obj_set_style_text_color(diag_lbl_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(diag_lbl_btn, zh_font_30(), 0);
    lv_obj_set_align(diag_lbl_btn, LV_ALIGN_CENTER);

    auto* diag_desc = new BtnDesc{this, -2};
    lv_obj_add_event_cb(diag_btn, [](lv_event_t* e) {
        auto* d = static_cast<BtnDesc*>(lv_event_get_user_data(e));
        if (!d || !d->app) return;
        AppUnitPuzzle* a = d->app;
        if (a->_diag_mode.load()) {
            a->_exitDiagMode();
        } else {
            a->_enterDiagMode();
        }
    }, LV_EVENT_CLICKED, diag_desc);

    lv_screen_load(_scr);
}

// =============================================================================
// 动画 tick — 每 30ms 重画一帧
// =============================================================================

void AppUnitPuzzle::_animTick(lv_timer_t* t)
{
    auto* self = static_cast<AppUnitPuzzle*>(lv_timer_get_user_data(t));
    if (!self) return;
    if (!self->_led_on.load()) return;
    if (!self->_strip) return;
    self->_drawPattern();
}

// HSV → RGB (h: 0-360, s/v: 0-255)
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b)
{
    uint8_t region = h / 60;
    uint8_t rem    = (h % 60) * 255 / 60;
    uint8_t p      = (v * (255 - s)) / 255;
    uint8_t q      = (v * (255 - ((s * rem) / 255))) / 255;
    uint8_t t      = (v * (255 - ((s * (255 - rem)) / 255))) / 255;
    switch (region) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

void AppUnitPuzzle::_drawPattern()
{
    static int64_t t0 = esp_timer_get_time();
    int64_t now  = (esp_timer_get_time() - t0) / 1000;
    int phase    = (int)(now % 100000);  // 防溢出

    _clear();
    int p = _pattern.load();

    switch (p) {
    case 0: { // 全亮红
        for (int i = 0; i < N; ++i) _setPixel(i, 255, 0, 0);
        break;
    }
    case 1: { // 全亮绿
        for (int i = 0; i < N; ++i) _setPixel(i, 0, 255, 0);
        break;
    }
    case 2: { // 全亮蓝
        for (int i = 0; i < N; ++i) _setPixel(i, 0, 0, 255);
        break;
    }
    case 3: { // 全亮白 (亮度限制, 防过流)
        for (int i = 0; i < N; ++i) _setPixel(i, 200, 200, 200);
        break;
    }
    case 4: { // 跑马灯 (1 颗绿 + 尾迹)
        int pos = (phase / 80) % N;
        for (int i = 0; i < N; ++i) {
            int dist = (i - pos + N) % N;
            if (dist == 0)        _setPixel(i, 0, 255, 0);
            else if (dist <= 2)   _setPixel(i, 0, 100, 0);
            else if (dist <= 4)   _setPixel(i, 0, 40, 0);
        }
        break;
    }
    case 5: { // 心跳 (呼吸红, 1.5s 一个心跳: 收缩-舒张-暂停)
        int period = 1500;
        int t_in   = phase % period;
        float s = 0.0f;
        if (t_in < 200)       s = t_in / 200.0f;                                // 0→1
        else if (t_in < 500)  s = 1.0f - (t_in - 200) / 300.0f;                 // 1→0
        else if (t_in < 700)  s = (t_in - 500) / 200.0f * 0.6f;                // 0→0.6
        else if (t_in < 1000) s = 0.6f * (1.0f - (t_in - 700) / 300.0f);       // 0.6→0
        // 剩下 500ms 全 0 (暂停)
        uint8_t v = (uint8_t)(s * 255);
        for (int i = 0; i < N; ++i) _setPixel(i, v, 0, 0);
        break;
    }
    case 6: { // 笑脸 (边框 + 眼 + 嘴)
        // 边框
        for (int y = 0; y < H; ++y) {
            _setPixelXY(0, y, 255, 200, 0);
            _setPixelXY(W-1, y, 255, 200, 0);
        }
        for (int x = 0; x < W; ++x) {
            _setPixelXY(x, 0, 255, 200, 0);
            _setPixelXY(x, H-1, 255, 200, 0);
        }
        // 眼睛
        _setPixelXY(2, 2, 0, 0, 0);
        _setPixelXY(5, 2, 0, 0, 0);
        // 嘴
        _setPixelXY(2, 4, 0, 0, 0);
        _setPixelXY(3, 5, 0, 0, 0);
        _setPixelXY(4, 5, 0, 0, 0);
        _setPixelXY(5, 4, 0, 0, 0);
        break;
    }
    case 7: { // 彩虹斜对角 (滚动)
        int offset = (phase / 40) % 16;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                uint16_t h = (uint16_t)((x + y + offset) * 16) % 360;
                uint8_t r, g, b;
                hsv_to_rgb(h, 220, 220, r, g, b);
                _setPixelXY(x, y, r, g, b);
            }
        }
        break;
    }
    case 8: { // 文字滚动 — 5x7 内置位图, 水平滚动显示用户输入文字
        const char* text = _text_buf;
        int len = (int)strlen(text);
        if (len == 0) break;  // 无文字时黑屏 (提示用户先输入)

        constexpr int CHAR_W = 5;
        constexpr int CHAR_H = 7;
        constexpr int CHAR_GAP = 1;          // 字符间距 1px
        constexpr int CELL    = CHAR_W + CHAR_GAP;  // 每字符占 6px
        constexpr int Y_OFF   = (H - CHAR_H) / 2;   // 8-7=1, 垂直居中

        int total_w = len * CELL - CHAR_GAP;  // 末字符后不加间距
        int x_start;
        if (total_w <= W) {
            // 放得下: 静态居中, 不滚动 (单字符/短词一眼看清)
            x_start = (W - total_w) / 2;
        } else {
            // 放不下: 从右侧进入, 150ms 滚动 1px, 滚过总宽后循环
            int scroll_px = (phase / 150) % (total_w + W);
            x_start = W - scroll_px;  // 第0字符的左边 x
        }

        for (int ci = 0; ci < len; ci++) {
            char c = text[ci];
            // 小写转大写
            if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
            if (c < 0x20 || c > 0x7E) c = '?';
            const uint8_t* bmp = font5x7[c - 0x20];

            int cx = x_start + ci * CELL;
            if (cx + CHAR_W <= 0) continue;  // 完全在左边屏外
            if (cx >= W) break;              // 完全在右边屏外

            for (int col = 0; col < CHAR_W; col++) {
                int x = cx + col;
                if (x < 0 || x >= W) continue;
                for (int row = 0; row < CHAR_H; row++) {
                    if (bmp[row] & (1 << (4 - col))) {
                        int y = Y_OFF + row;
                        if (y >= 0 && y < H)
                            _setPixelXY(x, y, 0, 255, 200);  // 青绿色
                    }
                }
            }
        }
        break;
    }
    default: break;
    }

    _show();
}

// ── 文字滚动渲染 (case 8) ─────────────────────────────────────────────────
// 把 ASCII 字符串用 lv_font_montserrat_8 渲染到 8x8 LED.
// montserrat_8 实际字符高度 6-7px (line_height 10), 垂直居中 + 顶部 1px padding.
// 字符宽度通常 4-6px, 字符间加 1px 间隔; 总宽 <= 8 时静态显示,
// 总宽 > 8 时按 phase 水平滚动 (左移).
void AppUnitPuzzle::_renderText(const char* s, int n, int phase,
                                uint8_t r, uint8_t g, uint8_t b)
{
    if (!s || n <= 0) return;

    // 第一遍: 算出每个字符的 advance width (box_w) 和总宽
    int char_w[16] = {0};
    int char_h[16] = {0};
    int total_w = 0;
    const int GAP = 1;  // 字符间距
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c < 0x20 || c > 0x7E) c = '?';  // 非 ASCII 替换成 ?
        lv_font_glyph_dsc_t dsc;
        if (lv_font_get_glyph_dsc(&lv_font_montserrat_8, &dsc, (uint32_t)c, 0) != false) {
            char_w[i] = dsc.box_w;
            char_h[i] = dsc.box_h;
        } else {
            char_w[i] = 0;
            char_h[i] = 0;
        }
        if (i > 0) total_w += GAP;
        total_w += char_w[i];
    }

    // 计算水平偏移: phase 每 8 帧滚动 1 像素, 滚一圈 = total_w + 8 (空白)
    int scroll_period = (total_w + W) * 8;  // 帧数
    if (scroll_period <= 0) scroll_period = 1;
    int x_off = (phase / 8) % scroll_period;
    if (x_off >= total_w + W) x_off = total_w + W - 1;

    // 第二遍: 把每个字符的位图按 (x_off + 累加宽度) 画到 8x8
    int acc_w = 0;
    for (int i = 0; i < n; i++) {
        int draw_x = acc_w - x_off;  // 在 8x8 LED 上的起始列 (可负)
        acc_w += char_w[i] + GAP;
        if (draw_x + char_w[i] <= 0) continue;  // 完全在左
        if (draw_x >= W) break;                  // 完全在右

        // 取字形位图 (bpp=4 抗锯齿, 每个像素占 1 字节 alpha, box_w * box_h 字节)
        char c = s[i];
        if (c < 0x20 || c > 0x7E) c = '?';
        lv_font_glyph_dsc_t dsc;
        if (lv_font_get_glyph_dsc(&lv_font_montserrat_8, &dsc, (uint32_t)c, 0) == false) continue;
        const uint8_t* bmp = (const uint8_t*)lv_font_get_glyph_bitmap(&dsc, nullptr);
        if (!bmp) continue;

        int clip_left  = draw_x < 0 ? -draw_x : 0;
        int clip_right = draw_x + char_w[i] > W ? (W - draw_x) : char_w[i];
        // 垂直居中: 把 char_h 行放在 8 行 LED 中间
        int y_off = (8 - char_h[i]) / 2;
        for (int col = clip_left; col < clip_right; col++) {
            for (int row = 0; row < char_h[i]; row++) {
                uint8_t alpha = bmp[row * char_w[i] + col];  // 0-255
                if (alpha > 96) {  // 阈值, 减少蒙糊点
                    int y = y_off + row;
                    if (y >= 0 && y < 8) {
                        _setPixelXY(draw_x + col, y, r, g, b);
                    }
                }
            }
        }
    }
}

// =============================================================================
// 文字输入弹窗
// =============================================================================

void AppUnitPuzzle::_showTextInput()
{
    // 如果已经打开就不重复创建
    if (_text_ui[0]) return;

    // 全屏半透明遮罩
    lv_obj_t* overlay = lv_obj_create(_scr);
    lv_obj_set_size(overlay, 1280, 800);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    _text_ui[0] = overlay;

    // 对话框主体 (放上方, 给底部虚拟键盘腾空间)
    lv_obj_t* box = lv_obj_create(overlay);
    lv_obj_set_size(box, 900, 320);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1A2A3A), 0);
    lv_obj_set_style_radius(box, 20, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x4FA3FF), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t* title = lv_label_create(box);
    lv_label_set_text(title, "输入文字 (LED 滚动显示, 最多 16 字符)");
    lv_obj_set_style_text_color(title, lv_color_hex(0x4FA3FF), 0);
    lv_obj_set_style_text_font(title, zh_font_30(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    // 输入框
    lv_obj_t* ta = lv_textarea_create(box);
    lv_obj_set_size(ta, 820, 100);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 80);
    lv_textarea_set_max_length(ta, 16);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "e.g. HELLO WORLD");
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);
    if (_text_buf[0]) lv_textarea_set_text(ta, _text_buf);
    _text_ta = ta;

    // 确认按钮
    lv_obj_t* ok_btn = lv_obj_create(box);
    lv_obj_set_size(ok_btn, 200, 80);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 80, -20);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x1B5E20), 0);
    lv_obj_set_style_radius(ok_btn, 12, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_add_flag(ok_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ok_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, "确认");
    lv_obj_set_style_text_font(ok_lbl, zh_font_30(), 0);
    lv_obj_set_style_text_color(ok_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_align(ok_lbl, LV_ALIGN_CENTER);
    _text_ui[2] = ok_btn;

    // 取消按钮
    lv_obj_t* cancel_btn = lv_obj_create(box);
    lv_obj_set_size(cancel_btn, 200, 80);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -80, -20);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x6A1B1B), 0);
    lv_obj_set_style_radius(cancel_btn, 12, 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cancel_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "取消");
    lv_obj_set_style_text_font(cancel_lbl, zh_font_30(), 0);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_align(cancel_lbl, LV_ALIGN_CENTER);

    // 屏幕虚拟键盘 (默认隐藏, 点输入框时弹出). 放屏幕底部, 全宽.
    lv_obj_t* kb = lv_keyboard_create(overlay);
    lv_obj_set_size(kb, 1280, 360);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    // 把 textarea 加入物理键盘 group (USB 实体键盘也能用)
    auto* hal = GetHAL();
    if (hal && hal->lvKbGroup) {
        lv_group_add_obj(hal->lvKbGroup, ta);
        lv_group_focus_obj(ta);
    }

    // 点输入框 → 弹出虚拟键盘; 键盘自带的关闭(✕)/确认(✓) → 收起键盘.
    lv_obj_add_event_cb(ta, [](lv_event_t* e) {
        lv_obj_t* keyboard = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
        if (keyboard) lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, kb);

    lv_obj_add_event_cb(kb, [](lv_event_t* e) {
        lv_obj_t* keyboard = static_cast<lv_obj_t*>(lv_event_get_target(e));
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, [](lv_event_t* e) {
        lv_obj_t* keyboard = static_cast<lv_obj_t*>(lv_event_get_target(e));
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CANCEL, nullptr);

    struct Ctx { AppUnitPuzzle* app; lv_obj_t* overlay; lv_obj_t* ta; };
    auto* ctx = new Ctx{this, overlay, ta};

    lv_obj_add_event_cb(ok_btn, [](lv_event_t* e) {
        auto* c = static_cast<Ctx*>(lv_event_get_user_data(e));
        if (!c) return;
        const char* text = lv_textarea_get_text(c->ta);
        if (text) {
            strncpy(c->app->_text_buf, text, sizeof(c->app->_text_buf) - 1);
            c->app->_text_buf[sizeof(c->app->_text_buf) - 1] = '\0';
        }
        c->app->_text_scroll = 0;
        c->app->_pattern.store(8);
        if (c->app->_status_lbl) lv_label_set_text(c->app->_status_lbl, "文字滚动");
        auto* hal = GetHAL();
        if (hal && hal->lvKbGroup) lv_group_remove_obj(c->ta);
        c->app->_text_ta  = nullptr;
        c->app->_text_ui[0] = nullptr;
        c->app->_text_ui[2] = nullptr;
        lv_obj_delete(c->overlay);
        delete c;
    }, LV_EVENT_CLICKED, ctx);

    lv_obj_add_event_cb(cancel_btn, [](lv_event_t* e) {
        auto* c = static_cast<Ctx*>(lv_event_get_user_data(e));
        if (!c) return;
        auto* hal = GetHAL();
        if (hal && hal->lvKbGroup) lv_group_remove_obj(c->ta);
        c->app->_text_ta  = nullptr;
        c->app->_text_ui[0] = nullptr;
        c->app->_text_ui[2] = nullptr;
        lv_obj_delete(c->overlay);
        delete c;
    }, LV_EVENT_CLICKED, ctx);
}

// =============================================================================
// 诊断模式 — 1Hz GPIO toggle, 帮助定位 PORT A Signal 真身
// =============================================================================
void AppUnitPuzzle::_diagTick(lv_timer_t* t)
{
    auto* self = static_cast<AppUnitPuzzle*>(lv_timer_get_user_data(t));
    if (!self) return;

    // 更新 UI 诊断信息
    self->_updateDiag();

    // diag mode: toggle GPIO
    if (self->_diag_mode.load()) {
        static bool high = false;
        high = !high;
        gpio_set_level(DIN_GPIO, high ? 1 : 0);
    }
}

void AppUnitPuzzle::_updateDiag()
{
    if (!_diag_lbl) return;
    char buf[200];
    if (_init_err != ESP_OK) {
        snprintf(buf, sizeof(buf),
                 "RMT init FAILED: %s\n"
                 "GPIO=%d  N=%d  WS2812 GRB\n"
                 "→ 可能是 RMT channel 全被占用或参数错",
                 esp_err_to_name(_init_err), (int)DIN_GPIO, N);
    } else if (_diag_mode.load()) {
        snprintf(buf, sizeof(buf),
                 "DIAG: GPIO %d 1Hz toggle\n"
                 "→ 用万用表/LED 测哪个 GPIO 有信号\n"
                 "→ 即 PORT A Signal 真身",
                 (int)DIN_GPIO);
    } else {
        snprintf(buf, sizeof(buf),
                 "RMT OK  GPIO=%d  N=%d  WS2812 GRB",
                 (int)DIN_GPIO, N);
    }
    lv_label_set_text(_diag_lbl, buf);
}

// =============================================================================
// Swipe-up 退出 (与 Claude app / 小智 一致: 复用已存在的 pointer indev,
// 不另起虚拟 indev. 在任意位置滑动都触发)
// =============================================================================
static lv_indev_t* s_gesture_indev = nullptr;

void AppUnitPuzzle::_gesture_event_cb(lv_event_t* e)
{
    lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
    if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
        auto* app = static_cast<AppUnitPuzzle*>(lv_event_get_user_data(e));
        if (app) {
            // lv_async_call 切回 LVGL 线程再调 close, 安全
            AppUnitPuzzle* a = app;
            lv_async_call([](void* u) {
                static_cast<AppUnitPuzzle*>(u)->_requestClose();
            }, a);
        }
    }
}

void AppUnitPuzzle::_installSwipeGesture()
{
    // 找到系统已有的 pointer indev, 注册手势回调 (与 Claude app 同款)
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, _gesture_event_cb, LV_EVENT_GESTURE, this);
            s_gesture_indev = indev;
            return;
        }
        indev = lv_indev_get_next(indev);
    }
    mclog::tagWarn(TAG, "no pointer indev found, swipe-up disabled");
}

void AppUnitPuzzle::_removeSwipeGesture()
{
    if (s_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(s_gesture_indev, _gesture_event_cb, this);
        s_gesture_indev = nullptr;
    }
}