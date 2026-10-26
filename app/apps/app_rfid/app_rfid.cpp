#include "app_rfid.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <smooth_lvgl.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

static const char* TAG = "rfid";

#ifndef PLATFORM_BUILD_DESKTOP
#include <esp_timer.h>
#include <esp_heap_caps.h>                      // 后台任务周期性堆监控
#include <driver/gpio.h>                          // GPIO53/54 bitbang
#include <rom/ets_sys.h>                          // esp_rom_delay_us
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nlohmann/json.hpp>                    // 解析 HA /api/states
#include <string>
#include <vector>
#include "../app_email_led/app_email_led.h"       // setPortAOwnedByApp / retirePortA (PORT A 仲裁)
#include "../app_ha/ha_weather.h"                 // ha_weather::token() / baseUrl()
#include "../utils/audio/audio.h"                 // audio::play_tone (嘀卡提示音)
#endif

// ── 中文字体 (与 app_stocks 同一套: 30px 内嵌 cbin, 20px assets 分区, 桌面回退 C 数组) ──
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#include "../utils/assets/assets_partition.h"
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
static const lv_font_t* zh_font_lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
static const lv_font_t* zh_font_sm()
{
    // 20px 全字库放 assets 分区 (pack_assets.py), mmap 原址使用
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_sm() { return &font_puhui_20_4; }
#endif

#ifndef PLATFORM_BUILD_DESKTOP
// ── 软 I2C (bitbang) 底层 ────────────────────────────────────────────────────
// P4 的两个硬件 I2C 控制器分别被内部总线 (GPIO31/32) 和键盘 (GPIO0/1) 占满,
// PORT A (GPIO53/54) 只能用 GPIO 模拟. 开漏输出: 写 1 = 释放 (靠上拉拉高),
// 写 0 = 拉低; 读回即总线电平. ~100kHz.
static constexpr gpio_num_t BB_SDA = GPIO_NUM_53;
static constexpr gpio_num_t BB_SCL = GPIO_NUM_54;

static inline void bb_delay() { esp_rom_delay_us(10); }  // ~50kHz, 留足上拉上升时间
static inline void bb_idle()  { gpio_set_level(BB_SDA, 1); gpio_set_level(BB_SCL, 1); }

// 释放 SCL 后等待从机放手 (clock stretching), 带超时
static bool bb_wait_scl_high()
{
    for (int i = 0; i < 200; i++) {
        if (gpio_get_level(BB_SCL)) return true;
        esp_rom_delay_us(1);
    }
    return false;
}

static void bb_start()
{
    gpio_set_level(BB_SDA, 1);
    gpio_set_level(BB_SCL, 1);
    bb_delay();
    gpio_set_level(BB_SDA, 0);
    bb_delay();
    gpio_set_level(BB_SCL, 0);
    bb_delay();
}

static void bb_stop()
{
    gpio_set_level(BB_SDA, 0);
    bb_delay();
    gpio_set_level(BB_SCL, 1);
    bb_wait_scl_high();
    bb_delay();
    gpio_set_level(BB_SDA, 1);
    bb_delay();
}

// 写 1 字节, 返回 true = 收到 ACK
static bool bb_write(uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        gpio_set_level(BB_SDA, (b & 0x80) ? 1 : 0);
        bb_delay();
        gpio_set_level(BB_SCL, 1);
        bb_wait_scl_high();
        bb_delay();
        gpio_set_level(BB_SCL, 0);
        b <<= 1;
    }
    gpio_set_level(BB_SDA, 1);   // 释放, 读 ACK
    bb_delay();
    gpio_set_level(BB_SCL, 1);
    bb_wait_scl_high();
    bb_delay();
    bool ack = (gpio_get_level(BB_SDA) == 0);
    gpio_set_level(BB_SCL, 0);
    bb_delay();
    return ack;
}

// 读 1 字节, ack=true 回 ACK (还要继续读), false 回 NACK (最后一字节)
static uint8_t bb_read(bool ack)
{
    uint8_t b = 0;
    gpio_set_level(BB_SDA, 1);
    for (int i = 0; i < 8; i++) {
        b <<= 1;
        gpio_set_level(BB_SCL, 1);
        bb_wait_scl_high();
        bb_delay();
        if (gpio_get_level(BB_SDA)) b |= 1;
        gpio_set_level(BB_SCL, 0);
        bb_delay();
    }
    gpio_set_level(BB_SDA, ack ? 0 : 1);
    bb_delay();
    gpio_set_level(BB_SCL, 1);
    bb_wait_scl_high();
    bb_delay();
    gpio_set_level(BB_SCL, 0);
    gpio_set_level(BB_SDA, 1);
    bb_delay();
    return b;
}
#endif  // !PLATFORM_BUILD_DESKTOP

// ════════════════════════════════════════════════════════════════════════════
//  生命周期
// ════════════════════════════════════════════════════════════════════════════
AppRfid::AppRfid()
{
    setAppInfo().name = "RFID Reader";
#ifndef PLATFORM_BUILD_DESKTOP
    s_instance = this;
#endif
}

void AppRfid::onCreate()
{
    mclog::tagInfo(TAG, "created");
#ifndef PLATFORM_BUILD_DESKTOP
    _bgStart();  // 开机即启动后台「嘀卡关灯」寻卡任务 (与 UI 互斥用硬件)
#endif
}

void AppRfid::onOpen()
{
    mclog::tagInfo(TAG, "opened");
    if (_scr) _destroyUi();  // 幂等: 清掉上次遗留的 UI
    _app_open.store(true);
    _buildUi();
    _hwInit();
    // 寻卡轮询挂在 LVGL 线程, 与 UI 同线程, 无需额外锁
    _poll_timer = lv_timer_create(_pollTick, 400, this);
}

void AppRfid::onRunning()
{
    // mooncake openApp(其他) 不会触发本 app onClose (见 app_stocks 同款注释).
    // 一旦 LVGL 活动屏不再是本 app 的 _scr (上拨返回/唤醒词/屏保...), 立刻收尾.
    bool foreground = (_scr != nullptr && lv_screen_active() == _scr);
    if (!foreground && _app_open.load()) {
        mclog::tagInfo(TAG, "lost foreground, releasing PORT A");
        _teardownHardware();
        _destroyUi();
    }
}

void AppRfid::onClose()
{
    // 安全网 (不一定会被触发, 见 _requestClose 注释)
    mclog::tagInfo(TAG, "closed");
    _teardownHardware();
    _destroyUi();
}

void AppRfid::_requestClose()
{
    // 设备选择页还开着时, 上拨先退选择页, 不退出整个 app
    if (_picker_page) { _closePicker(); return; }
    _teardownHardware();
    // 不在这里 _destroyUi(): _close_cb 是 openApp(工具页), 异步生效, 此刻 _scr
    // 仍是活动屏, 删掉会悬空 LVGL active-screen 指针 → 布局时崩. 与 app_stocks
    // 完全一致的处理, 留给下次 onOpen 顶部的幂等 _destroyUi 收割.
    if (_close_cb) _close_cb();
}

void AppRfid::_teardownHardware()
{
    _app_open.store(false);
#ifndef PLATFORM_BUILD_DESKTOP
    // 释放两个 GPIO 回高阻输入, 然后通知后台寻卡任务重新接管初始化
    gpio_reset_pin(BB_SDA);
    gpio_reset_pin(BB_SCL);
    s_bg_reinit.store(true);
    s_ui_owns.store(false);
    AppEmailLed::setPortAOwnedByApp(false);  // 无害: PORT A 已 retire, 邮件 LED 不会再来
#endif
    _reader_ok    = false;
    _card_present = false;
}

// ════════════════════════════════════════════════════════════════════════════
//  硬件初始化
// ════════════════════════════════════════════════════════════════════════════
void AppRfid::_hwInit()
{
#ifndef PLATFORM_BUILD_DESKTOP
    // 先接管: 让后台寻卡任务停手 + 邮件 LED 释放 GPIO53 (RMT), 再把引脚配成开漏.
    // 延迟 60ms 等后台可能在途的一笔 I2C 交易收尾, 避免中途改引脚方向.
    s_ui_owns.store(true);
    AppEmailLed::setPortAOwnedByApp(true);
    vTaskDelay(pdMS_TO_TICKS(60));

    // 软 I2C 初始化: SDA=53, SCL=54, 开漏 + 内部上拉 (Unit NFC 板上还有外部上拉)
    gpio_reset_pin(BB_SDA);
    gpio_reset_pin(BB_SCL);
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << BB_SDA) | (1ULL << BB_SCL);
    io.mode         = GPIO_MODE_INPUT_OUTPUT_OD;  // 开漏, 可读回
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io);
    bb_idle();

    // 探测 0x50 (ST25R3916)
    bb_start();
    bool ack = bb_write((uint8_t)(NFC_I2C_ADDR << 1));
    bb_stop();
    if (!ack) {
        mclog::tagWarn(TAG, "no ACK from 0x{:02X}", NFC_I2C_ADDR);
        _setStatus("● 未检测到读卡器", false);
        _setMsg("请把 Unit NFC 插到 PORT A (红色 Grove 口)", C_DIM);
        return;
    }

    if (!_stInit()) {
        _setStatus("● 读卡器初始化失败", false);
        return;
    }
    if (!_stConfigNfcA()) {
        _setStatus("● RF 场开启失败", false);
        return;
    }
    _reader_ok = true;
    _setStatus("● 读卡器就绪", true);
    mclog::tagInfo(TAG, "ST25R3916 ready");
#else
    _reader_ok = false;
    _setStatus("● 仅 Tab5 真机可用", false);
#endif
}

// ════════════════════════════════════════════════════════════════════════════
//  UI
// ════════════════════════════════════════════════════════════════════════════
void AppRfid::_buildUi()
{
    LvglLockGuard lock;

    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header ─────────────────────────────────────────────────────────────
    lv_obj_t* header = lv_obj_create(_scr);
    lv_obj_set_size(header, SCREEN_W, HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "RFID Reader");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);

    _status_lbl = lv_label_create(header);
    lv_label_set_text(_status_lbl, "● 初始化...");
    lv_obj_set_style_text_font(_status_lbl, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(_status_lbl, LV_ALIGN_RIGHT_MID, -24, 0);

    // ── 左面板: 卡片信息 ─────────────────────────────────────────────────────
    lv_obj_t* left = lv_obj_create(_scr);
    lv_obj_set_size(left, 560, 596);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 24, HEADER_H + 20);
    lv_obj_set_style_bg_color(left, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_radius(left, 16, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 24, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lt = lv_label_create(left);
    lv_label_set_text(lt, "卡片信息");
    lv_obj_set_style_text_font(lt, zh_font_sm(), 0);
    lv_obj_set_style_text_color(lt, lv_color_hex(C_DIM), 0);
    lv_obj_align(lt, LV_ALIGN_TOP_LEFT, 0, 0);

    _hint_lbl = lv_label_create(left);
    lv_label_set_text(_hint_lbl, "将卡片靠近 Unit NFC 读卡器");
    lv_obj_set_style_text_font(_hint_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_hint_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(_hint_lbl, LV_ALIGN_CENTER, 0, 0);

    _uid_lbl = lv_label_create(left);
    lv_label_set_text(_uid_lbl, "");
    lv_obj_set_style_text_font(_uid_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_uid_lbl, lv_color_hex(C_ACCENT), 0);
    lv_obj_align(_uid_lbl, LV_ALIGN_TOP_LEFT, 0, 60);

    _type_lbl = lv_label_create(left);
    lv_label_set_text(_type_lbl, "");
    lv_obj_set_style_text_font(_type_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_type_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(_type_lbl, LV_ALIGN_TOP_LEFT, 0, 130);

    _proto_lbl = lv_label_create(left);
    lv_label_set_text(_proto_lbl, "");
    lv_obj_set_style_text_font(_proto_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_proto_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(_proto_lbl, LV_ALIGN_TOP_LEFT, 0, 200);

    // ── 嘀卡关灯配置: 放卡后点按钮把该卡加入触发卡列表 (后台服务用) ──────────
    lv_obj_t* off_btn = lv_button_create(left);
    lv_obj_set_size(off_btn, 200, 56);
    lv_obj_align(off_btn, LV_ALIGN_TOP_LEFT, 0, 440);
    lv_obj_set_style_bg_color(off_btn, lv_color_hex(0x8E44AD), 0);
    lv_obj_set_style_radius(off_btn, 10, 0);
    lv_obj_t* off_btn_l = lv_label_create(off_btn);
    lv_label_set_text(off_btn_l, "设为关灯卡");
    lv_obj_set_style_text_font(off_btn_l, zh_font_sm(), 0);
    lv_obj_set_style_text_color(off_btn_l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(off_btn_l);
    lv_obj_add_event_cb(off_btn, _setOffCard_cb, LV_EVENT_CLICKED, this);

    lv_obj_t* off_clr = lv_button_create(left);
    lv_obj_set_size(off_clr, 200, 56);
    lv_obj_align(off_clr, LV_ALIGN_TOP_LEFT, 0, 506);
    lv_obj_set_style_bg_color(off_clr, lv_color_hex(0x5A7186), 0);
    lv_obj_set_style_radius(off_clr, 10, 0);
    lv_obj_t* off_clr_l = lv_label_create(off_clr);
    lv_label_set_text(off_clr_l, "清空关灯卡");
    lv_obj_set_style_text_font(off_clr_l, zh_font_sm(), 0);
    lv_obj_set_style_text_color(off_clr_l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(off_clr_l);
    lv_obj_add_event_cb(off_clr, _clearOffCard_cb, LV_EVENT_CLICKED, this);

    _offuid_lbl = lv_label_create(left);
    lv_obj_set_style_text_font(_offuid_lbl, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_offuid_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(_offuid_lbl, LV_ALIGN_TOP_LEFT, 216, 452);
    lv_label_set_long_mode(_offuid_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_offuid_lbl, 296);
    _updateOffUidLabel();

    // ── 右面板: 块读写 ───────────────────────────────────────────────────────
    lv_obj_t* right = lv_obj_create(_scr);
    lv_obj_set_size(right, 648, 596);
    lv_obj_align(right, LV_ALIGN_TOP_LEFT, 608, HEADER_H + 20);
    lv_obj_set_style_bg_color(right, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_radius(right, 16, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 24, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* rt = lv_label_create(right);
    lv_label_set_text(rt, "数据读写 (MIFARE Classic 默认密钥 FF×6)");
    lv_obj_set_style_text_font(rt, zh_font_sm(), 0);
    lv_obj_set_style_text_color(rt, lv_color_hex(C_DIM), 0);
    lv_obj_align(rt, LV_ALIGN_TOP_LEFT, 0, 0);

    // 块地址选择: − [块 4 (扇区 1)] +
    auto mk_step_btn = [&](const char* txt, lv_event_cb_t cb, int x) {
        lv_obj_t* b = lv_button_create(right);
        lv_obj_set_size(b, 56, 56);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, 48);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
    };
    mk_step_btn("-", _blockMinus_cb, 0);
    mk_step_btn("+", _blockPlus_cb, 340);

    _block_lbl = lv_label_create(right);
    lv_obj_set_style_text_font(_block_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_block_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(_block_lbl, LV_ALIGN_TOP_LEFT, 72, 58);
    _updateBlockLabel();

    // 数据输入框
    _data_ta = lv_textarea_create(right);
    lv_obj_set_size(_data_ta, 600, 64);
    lv_obj_align(_data_ta, LV_ALIGN_TOP_LEFT, 0, 128);
    lv_textarea_set_one_line(_data_ta, true);
    lv_textarea_set_max_length(_data_ta, 16);
    lv_textarea_set_placeholder_text(_data_ta, "写入内容 (最多 16 字符)");
    lv_obj_set_style_text_font(_data_ta, &lv_font_montserrat_20, 0);

    // 物理 USB 键盘也能输入 (对象删除时 LVGL 会自动移出 group)
    auto* hal = GetHAL();
    if (hal && hal->lvKbGroup) lv_group_add_obj(hal->lvKbGroup, _data_ta);

    // 按钮行: 读取 / 写入 / 清空
    auto mk_btn = [&](const char* txt, lv_event_cb_t cb, int x, uint32_t color) {
        lv_obj_t* b = lv_button_create(right);
        lv_obj_set_size(b, 188, 56);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, 216);
        lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, zh_font_sm(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
    };
    mk_btn("读取", _readBtn_cb,  0,   C_ACCENT);
    mk_btn("写入", _writeBtn_cb, 206, 0x27AE60);
    mk_btn("清空", _clearBtn_cb, 412, 0x5A7186);

    // 关灯设备选择页入口 (从 HA 拉实体勾选, 覆盖内置默认五件套)
    lv_obj_t* pk_btn = lv_button_create(right);
    lv_obj_set_size(pk_btn, 296, 56);
    lv_obj_align(pk_btn, LV_ALIGN_TOP_LEFT, 0, 500);
    lv_obj_set_style_bg_color(pk_btn, lv_color_hex(0x8E44AD), 0);
    lv_obj_set_style_radius(pk_btn, 10, 0);
    lv_obj_t* pk_l = lv_label_create(pk_btn);
    lv_label_set_text(pk_l, "选择关灯设备");
    lv_obj_set_style_text_font(pk_l, zh_font_sm(), 0);
    lv_obj_set_style_text_color(pk_l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(pk_l);
    lv_obj_add_event_cb(pk_btn, _pickerBtn_cb, LV_EVENT_CLICKED, this);

    // 读回数据的 hex / ascii 显示
    _hex_lbl = lv_label_create(right);
    lv_label_set_text(_hex_lbl, "--");
    lv_obj_set_style_text_font(_hex_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_hex_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(_hex_lbl, LV_ALIGN_TOP_LEFT, 0, 300);

    // 操作结果提示
    _msg_lbl = lv_label_create(right);
    lv_label_set_text(_msg_lbl, "");
    lv_obj_set_style_text_font(_msg_lbl, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_msg_lbl, lv_color_hex(C_DIM), 0);
    lv_label_set_long_mode(_msg_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_msg_lbl, 600);
    lv_obj_align(_msg_lbl, LV_ALIGN_TOP_LEFT, 0, 400);

    // ── 虚拟键盘 (默认隐藏, 点输入框弹出) ────────────────────────────────────
    _kb = lv_keyboard_create(_scr);
    lv_obj_set_size(_kb, SCREEN_W, 340);
    lv_obj_align(_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(_kb, _data_ta);
    lv_obj_add_flag(_kb, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(_data_ta, [](lv_event_t* e) {
        auto* self = static_cast<AppRfid*>(lv_event_get_user_data(e));
        if (self->_kb) lv_obj_clear_flag(self->_kb, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_kb, [](lv_event_t* e) {
        lv_obj_add_flag(lv_event_get_target_obj(e), LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(_kb, [](lv_event_t* e) {
        lv_obj_add_flag(lv_event_get_target_obj(e), LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CANCEL, nullptr);

    lv_screen_load(_scr);
    _installSwipeGesture();
}

void AppRfid::_destroyUi()
{
    LvglLockGuard lock;
    *_alive = false;  // 在途手势回调从此 no-op (与 app_stocks 相同)
    _removeSwipeGesture();
    if (_poll_timer) { lv_timer_delete(_poll_timer); _poll_timer = nullptr; }
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _status_lbl = _hint_lbl = _uid_lbl = _type_lbl = _proto_lbl = nullptr;
    _block_lbl = _data_ta = _hex_lbl = _msg_lbl = _kb = nullptr;
    _offuid_lbl = nullptr;
    _picker_page = _picker_list = _picker_status = nullptr;  // 随 _scr 一起被删
    _alive = std::make_shared<bool>(true);  // 下次 _buildUi 用新的
}

// Swipe-up-to-exit, 与 app_stocks 完全相同的 SwipeCtx/alive-flag 模式:
// user_data 是堆上 SwipeCtx (app + weak_ptr<bool>), 不依赖
// lv_indev_remove_event_cb_with_user_data 赢得与在途事件的竞态.
namespace {
struct SwipeCtx {
    AppRfid* app;
    std::weak_ptr<bool> alive;
};
}  // namespace

void AppRfid::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new SwipeCtx{this, _alive};
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    // 延迟到下一轮再拆 UI, 不能在 indev 分发里同步做
                    auto* app_ctx = new SwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* c = static_cast<SwipeCtx*>(u);
                        auto a = c->alive.lock();
                        if (a && *a) c->app->_requestClose();
                        delete c;
                    }, app_ctx);
                }
            }, LV_EVENT_GESTURE, _gesture_ctx);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppRfid::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
    // _gesture_ctx 故意不释放 — 见分配处的注释.
}

// ════════════════════════════════════════════════════════════════════════════
//  UI 辅助
// ════════════════════════════════════════════════════════════════════════════
void AppRfid::_setStatus(const char* text, bool ok)
{
    if (!_status_lbl) return;
    lv_label_set_text(_status_lbl, text);
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(ok ? C_OK : C_ERR), 0);
}

void AppRfid::_setMsg(const char* text, uint32_t color)
{
    if (!_msg_lbl) return;
    lv_label_set_text(_msg_lbl, text);
    lv_obj_set_style_text_color(_msg_lbl, lv_color_hex(color), 0);
}

void AppRfid::_updateBlockLabel()
{
    if (!_block_lbl) return;
    lv_label_set_text_fmt(_block_lbl, "块 %d  (扇区 %d)", _block, _block / 4);
}

void AppRfid::_clearCardInfo()
{
    if (_hint_lbl)  lv_obj_clear_flag(_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    if (_uid_lbl)   lv_label_set_text(_uid_lbl, "");
    if (_type_lbl)  lv_label_set_text(_type_lbl, "");
    if (_proto_lbl) lv_label_set_text(_proto_lbl, "");
}

void AppRfid::_showCardInfo()
{
    if (_hint_lbl) lv_obj_add_flag(_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    if (_uid_lbl) {
        char buf[40];
        int off = 0;
        for (int i = 0; i < _uid_len; i++)
            off += snprintf(buf + off, sizeof(buf) - off, "%s%02X", i ? " " : "", _uid[i]);
        lv_label_set_text(_uid_lbl, buf);
    }
    if (_type_lbl) lv_label_set_text(_type_lbl, _cardTypeName());
    if (_proto_lbl) {
        lv_label_set_text_fmt(_proto_lbl, "ATQA %02X %02X   SAK %02X",
                              _atqa[0], _atqa[1], _sak);
    }
}

const char* AppRfid::_cardTypeName()
{
    switch (_sak) {
        case 0x08: return "MIFARE Classic 1K";
        case 0x18: return "MIFARE Classic 4K";
        case 0x09: return "MIFARE Mini";
        case 0x00:
        case 0x04: return "MIFARE Ultralight / NTAG";
        case 0x20: return "MIFARE DESFire";
        default:   return "ISO14443-3A 卡";
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  按钮回调
// ════════════════════════════════════════════════════════════════════════════
void AppRfid::_blockMinus_cb(lv_event_t* e)
{
    auto* self = static_cast<AppRfid*>(lv_event_get_user_data(e));
    if (self->_block > 0) self->_block--;
    self->_updateBlockLabel();
}

void AppRfid::_blockPlus_cb(lv_event_t* e)
{
    auto* self = static_cast<AppRfid*>(lv_event_get_user_data(e));
    if (self->_block < 63) self->_block++;
    self->_updateBlockLabel();
}

void AppRfid::_readBtn_cb(lv_event_t* e)
{
    static_cast<AppRfid*>(lv_event_get_user_data(e))->_readBlockNow();
}

void AppRfid::_writeBtn_cb(lv_event_t* e)
{
    static_cast<AppRfid*>(lv_event_get_user_data(e))->_writeBlockNow();
}

void AppRfid::_clearBtn_cb(lv_event_t* e)
{
    auto* self = static_cast<AppRfid*>(lv_event_get_user_data(e));
    if (self->_data_ta) lv_textarea_set_text(self->_data_ta, "");
    if (self->_hex_lbl) lv_label_set_text(self->_hex_lbl, "--");
    self->_setMsg("", C_DIM);
}

void AppRfid::_setOffCard_cb(lv_event_t* e)
{
    static_cast<AppRfid*>(lv_event_get_user_data(e))->_saveOffCard();
}

void AppRfid::_clearOffCard_cb(lv_event_t* e)
{
    static_cast<AppRfid*>(lv_event_get_user_data(e))->_clearOffCards();
}

// ── 「嘀卡关灯」配置 (多卡列表, NVS "rfid_off_uids" 逗号分隔) ─────────────────
namespace {
// 读取关灯卡 UID 列表; 兼容旧的单卡键 "rfid_off_uid"
std::vector<std::string> off_uid_list()
{
    std::vector<std::string> out;
    if (!GetHAL()) return out;
    std::string list = GetHAL()->getConfig("rfid_off_uids", "");
    if (list.empty()) {  // 兼容 v1 的单卡键
        std::string legacy = GetHAL()->getConfig("rfid_off_uid", "");
        if (!legacy.empty()) list = legacy;
    }
    size_t pos = 0;
    while (pos < list.size()) {
        size_t comma = list.find(',', pos);
        std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos
                                                                       : comma - pos);
        pos = (comma == std::string::npos) ? list.size() : comma + 1;
        while (!item.empty() && item.front() == ' ') item.erase(0, 1);
        while (!item.empty() && item.back() == ' ')  item.pop_back();
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

void off_uid_save(const std::vector<std::string>& list)
{
    std::string joined;
    for (const auto& u : list) {
        if (!joined.empty()) joined += ',';
        joined += u;
    }
    GetHAL()->setConfig("rfid_off_uids", joined);
}

// 默认关灯实体 (与 M5Dial 同款, 同一个家). NVS "rfid_off_ents" 为空时生效;
// 用户在选择页勾过后以 NVS 为准, 特殊值 "none" 表示显式一个不选.
// (键名必须 ≤15 字符 — NVS 上限, 超长的键 set/get 会静默失败!)
const char* OFF_DEFAULT_ENTITIES =
    "switch.xiaomi_cn_945886502_w1_on_p_2_1,"    // 餐厅灯
    "switch.xiaomi_cn_945769368_w1_on_p_2_1,"    // 书台灯
    "switch.xiaomi_cn_2143401838_w2_on_p_3_1,"   // 筒灯
    "switch.xiaomi_cn_2143401838_w2_on_p_2_1,"   // 吸顶灯
    "fan.dmaker_cn_740412216_p5c_s_2_fan";       // 风扇

// 当前生效的关灯实体列表 (选择页勾选状态与后台关灯共用同一份)
std::vector<std::string> off_entity_list()
{
    std::vector<std::string> out;
    std::string raw = GetHAL() ? GetHAL()->getConfig("rfid_off_ents", "") : "";
    if (raw == "none") return out;               // 选择页里显式全不选
    if (raw.empty()) raw = OFF_DEFAULT_ENTITIES; // 从未配置 → 默认五件套
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t comma = raw.find(',', pos);
        std::string item = raw.substr(pos, comma == std::string::npos ? std::string::npos
                                                                      : comma - pos);
        pos = (comma == std::string::npos) ? raw.size() : comma + 1;
        while (!item.empty() && item.front() == ' ') item.erase(0, 1);
        while (!item.empty() && item.back() == ' ')  item.pop_back();
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

bool off_entity_contains(const std::vector<std::string>& list, const std::string& eid)
{
    for (const auto& e : list) if (e == eid) return true;
    return false;
}
}  // namespace

void AppRfid::_saveOffCard()
{
#ifndef PLATFORM_BUILD_DESKTOP
    if (!_card_present) { _setMsg("未检测到卡片, 请先放卡", C_ERR); return; }
    char hex[24] = {0};
    int off = 0;
    for (int i = 0; i < _uid_len; i++) off += snprintf(hex + off, sizeof(hex) - off, "%02X", _uid[i]);

    auto list = off_uid_list();
    for (const auto& u : list) {
        if (u == hex) {
            _setMsg("这张卡已经在关灯卡列表里", C_DIM);
            return;
        }
    }
    list.push_back(hex);
    off_uid_save(list);
    _updateOffUidLabel();
    _setMsg("已加入关灯卡, 后台嘀这张卡即关灯", C_OK);
    mclog::tagInfo(TAG, "off-card added: {} (total {})", hex, list.size());
#else
    _setMsg("仅 Tab5 真机可用", C_ERR);
#endif
}

void AppRfid::_clearOffCards()
{
#ifndef PLATFORM_BUILD_DESKTOP
    GetHAL()->setConfig("rfid_off_uids", "");
    GetHAL()->setConfig("rfid_off_uid", "");  // 旧单卡键一并清掉
    _updateOffUidLabel();
    _setMsg("关灯卡列表已清空", C_OK);
    mclog::tagInfo(TAG, "off-card list cleared");
#else
    _setMsg("仅 Tab5 真机可用", C_ERR);
#endif
}

void AppRfid::_updateOffUidLabel()
{
    if (!_offuid_lbl) return;
    auto list = off_uid_list();
    if (list.empty()) {
        lv_label_set_text(_offuid_lbl, "关灯卡: 未设置");
        return;
    }
    std::string text = "关灯卡 " + std::to_string(list.size()) + " 张:";
    for (const auto& u : list) {
        text += "\n";
        for (size_t i = 0; i + 1 < u.size(); i += 2) {
            text += u.substr(i, 2);
            text += ' ';
        }
        if (!text.empty() && text.back() == ' ') text.pop_back();
    }
    lv_label_set_text(_offuid_lbl, text.c_str());
}

// ── 关灯设备选择页 ───────────────────────────────────────────────────────────
void AppRfid::_pickerBtn_cb(lv_event_t* e)
{
    static_cast<AppRfid*>(lv_event_get_user_data(e))->_openPicker();
}

void AppRfid::_openPicker()
{
    if (_picker_page) return;
    if (_kb) lv_obj_add_flag(_kb, LV_OBJ_FLAG_HIDDEN);  // 别挡列表

    _picker_page = lv_obj_create(_scr);
    lv_obj_set_size(_picker_page, SCREEN_W, SCREEN_H);
    lv_obj_align(_picker_page, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_picker_page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(_picker_page, 0, 0);
    lv_obj_set_style_radius(_picker_page, 0, 0);
    lv_obj_clear_flag(_picker_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(_picker_page);
    lv_label_set_text(title, "选择关灯设备  (勾选即保存)");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 20);

    // 显式返回按钮 — 列表滚动会吃掉上拨手势 (LVGL 滚动物件消耗 drag 后
    // 不再产生 GESTURE 事件), 不能只靠 swipe-up 退出本页.
    lv_obj_t* back = lv_button_create(_picker_page);
    lv_obj_set_size(back, 140, 56);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -24, 10);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x5A7186), 0);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_t* back_l = lv_label_create(back);
    lv_label_set_text(back_l, "返回");
    lv_obj_set_style_text_font(back_l, zh_font_sm(), 0);
    lv_obj_set_style_text_color(back_l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(back_l);
    lv_obj_add_event_cb(back, [](lv_event_t* e) {
        static_cast<AppRfid*>(lv_event_get_user_data(e))->_closePicker();
    }, LV_EVENT_CLICKED, this);

    _picker_status = lv_label_create(_picker_page);
    lv_label_set_text(_picker_status, "正在从 HA 拉取设备列表...");
    lv_obj_set_style_text_font(_picker_status, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_picker_status, lv_color_hex(C_DIM), 0);
    lv_obj_align(_picker_status, LV_ALIGN_TOP_LEFT, 24, 76);

    // 滚动容器: 每行一个 lv_checkbox
    _picker_list = lv_obj_create(_picker_page);
    lv_obj_set_size(_picker_list, SCREEN_W - 48, SCREEN_H - 130);
    lv_obj_align(_picker_list, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_set_style_bg_color(_picker_list, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_radius(_picker_list, 16, 0);
    lv_obj_set_style_border_width(_picker_list, 0, 0);
    lv_obj_set_flex_flow(_picker_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(_picker_list, 16, 0);
    lv_obj_set_style_pad_row(_picker_list, 12, 0);

    _fetchPickerEntities();
}

void AppRfid::_closePicker()
{
    if (!_picker_page) return;
    lv_obj_delete(_picker_page);
    _picker_page = _picker_list = _picker_status = nullptr;
    _picker_entities.clear();
    _picker_updated.store(false);
    _picker_error.store(false);
    _picker_fetching.store(false);  // 在途 worker 的结果落进空 page 也会被 flag 检查挡住
}

void AppRfid::_fetchPickerEntities()
{
#ifndef PLATFORM_BUILD_DESKTOP
    if (_picker_fetching.exchange(true)) return;

    std::string host  = GetHAL()->getConfig("ha_host", "");
    std::string token = ha_weather::token();
    if (host.empty() || token.empty()) {
        mclog::tagWarn(TAG, "picker: ha_host/ha_token 未配置");
        _picker_fetching.store(false);
        _picker_error.store(true);
        return;
    }
    std::string url = ha_weather::baseUrl(host) + "/api/states";

    GetHAL()->tryRunDetached([this, url, token]() {
        auto resp = GetHAL()->httpGet(url, {{"Authorization", "Bearer " + token}});
        if (!resp.ok) {
            mclog::tagWarn(TAG, "picker: /api/states failed status={}", resp.status);
            _picker_fetching.store(false);
            _picker_error.store(true);
            return;
        }
        try {
            auto arr = nlohmann::json::parse(resp.body);
            auto effective = off_entity_list();  // 当前生效清单 → 预勾选
            std::vector<PickerEntity> ents;
            for (auto& item : arr) {
                std::string eid = item.value("entity_id", "");
                std::string dom = eid.substr(0, eid.find('.'));
                if (dom != "switch" && dom != "light" && dom != "fan") continue;
                PickerEntity pe;
                pe.entity_id = eid;
                pe.name = item["attributes"].value("friendly_name", eid);
                pe.checked = off_entity_contains(effective, eid);
                ents.push_back(std::move(pe));
            }
            std::sort(ents.begin(), ents.end(),
                      [](const PickerEntity& a, const PickerEntity& b) { return a.name < b.name; });
            _picker_entities = std::move(ents);
            _picker_fetching.store(false);
            _picker_updated.store(true);
            mclog::tagInfo(TAG, "picker: {} switch/light/fan entities", _picker_entities.size());
        } catch (const std::exception& e) {
            mclog::tagWarn(TAG, "picker: parse failed: {}", e.what());
            _picker_fetching.store(false);
            _picker_error.store(true);
        }
    });
#else
    _picker_error.store(true);
#endif
}

void AppRfid::_rebuildPickerList()
{
    if (!_picker_list) return;
    lv_obj_clean(_picker_list);

    lv_label_set_text_fmt(_picker_status, "勾选嘀卡时要关闭的设备 (共 %d 个)",
                          (int)_picker_entities.size());

    for (size_t i = 0; i < _picker_entities.size(); i++) {
        auto& pe = _picker_entities[i];
        lv_obj_t* cb = lv_checkbox_create(_picker_list);
        std::string line = pe.name + "   " + pe.entity_id;
        lv_checkbox_set_text(cb, line.c_str());
        lv_obj_set_style_text_font(cb, zh_font_lg(), 0);  // 30px, 触摸行高随之撑大
        lv_obj_set_style_text_color(cb, lv_color_hex(C_TEXT), 0);
        if (pe.checked) lv_obj_add_state(cb, LV_STATE_CHECKED);
        lv_obj_set_user_data(cb, (void*)i);  // 行索引跟对象走, event user_data 留给 app
        lv_obj_add_event_cb(cb, _pickerItem_cb, LV_EVENT_VALUE_CHANGED, this);
    }
}

void AppRfid::_pickerItem_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRfid*>(lv_event_get_user_data(e));
    lv_obj_t* cb = lv_event_get_target_obj(e);
    size_t idx = (size_t)lv_obj_get_user_data(cb);
    if (idx >= app->_picker_entities.size()) return;
    app->_picker_entities[idx].checked = lv_obj_has_state(cb, LV_STATE_CHECKED);
    app->_savePickedEntities();
}

void AppRfid::_savePickedEntities()
{
    std::string joined;
    for (const auto& pe : _picker_entities) {
        if (!pe.checked) continue;
        if (!joined.empty()) joined += ',';
        joined += pe.entity_id;
    }
    // 全不选写 "none" 占位, 否则空串会被当成"从未配置"而回退默认五件套
    GetHAL()->setConfig("rfid_off_ents", joined.empty() ? "none" : joined);
    mclog::tagInfo(TAG, "picker saved: {}", joined.empty() ? "(none)" : joined);
}

void AppRfid::_readBlockNow()
{
#ifndef PLATFORM_BUILD_DESKTOP
    if (!_reader_ok)    { _setMsg("读卡器未连接", C_ERR); return; }
    if (!_card_present) { _setMsg("未检测到卡片, 请先放卡", C_ERR); return; }
    if (!_wakeAndSelect()) { _setMsg("卡片激活失败, 请重新放卡", C_ERR); return; }

    uint8_t buf[16] = {0};
    if (!_cardReadBlock((uint8_t)_block, buf)) {
        _stHaltA();
        _setMsg("读取失败 (认证失败或无权限)", C_ERR);
        return;
    }
    char hex[64]; int off = 0;
    for (int i = 0; i < 16; i++)
        off += snprintf(hex + off, sizeof(hex) - off, "%02X ", buf[i]);
    char ascii[20];
    for (int i = 0; i < 16; i++)
        ascii[i] = (buf[i] >= 0x20 && buf[i] < 0x7F) ? (char)buf[i] : '.';
    ascii[16] = '\0';
    _stHaltA();  // 读完回 HALT, 保持轮询检测稳定
    if (_hex_lbl) lv_label_set_text_fmt(_hex_lbl, "%s\n%s", hex, ascii);
    if (_data_ta) lv_textarea_set_text(_data_ta, ascii);
    _setMsg("读取成功", C_OK);
#else
    _setMsg("仅 Tab5 真机可用", C_ERR);
#endif
}

void AppRfid::_writeBlockNow()
{
#ifndef PLATFORM_BUILD_DESKTOP
    if (!_reader_ok)    { _setMsg("读卡器未连接", C_ERR); return; }
    if (!_card_present) { _setMsg("未检测到卡片, 请先放卡", C_ERR); return; }
    if (_block == 0) {
        _setMsg("块 0 是厂商块 (UID), 禁止写入", C_ERR);
        return;
    }
    if (_classic && _block % 4 == 3) {
        _setMsg("这是扇区尾块 (存密钥), 写入会锁死扇区, 已拒绝", C_ERR);
        return;
    }

    uint8_t buf[16] = {0};
    const char* text = _data_ta ? lv_textarea_get_text(_data_ta) : "";
    size_t n = strlen(text);
    if (n > 16) n = 16;
    memcpy(buf, text, n);

    if (!_wakeAndSelect()) { _setMsg("卡片激活失败, 请重新放卡", C_ERR); return; }
    if (!_cardWriteBlock((uint8_t)_block, buf)) {
        _stHaltA();
        _setMsg("写入失败 (认证失败或卡片拒绝)", C_ERR);
        return;
    }
    _setMsg("写入成功, 已回读校验", C_OK);
    _readBlockNow();  // 回读显示 (内部会重新激活并 HLTA)
#else
    _setMsg("仅 Tab5 真机可用", C_ERR);
#endif
}

// ════════════════════════════════════════════════════════════════════════════
//  寻卡轮询 (LVGL 线程, 400ms)
// ════════════════════════════════════════════════════════════════════════════
void AppRfid::_pollTick(lv_timer_t* t)
{
#ifndef PLATFORM_BUILD_DESKTOP
    auto* self = static_cast<AppRfid*>(lv_timer_get_user_data(t));
    if (!self->_app_open.load()) return;

    // 设备选择页的 worker 回填 (与 app_settings 的 fx/email cache 同一模式)
    if (self->_picker_updated.exchange(false) && self->_picker_page) self->_rebuildPickerList();
    if (self->_picker_error.exchange(false) && self->_picker_page) {
        lv_label_set_text(self->_picker_status, "拉取失败: 检查 HA 地址/Token (见日志)");
    }

    // 读卡器未就绪时自动重试探测 (每 ~2s), 支持热插拔 / 上电时序晚于 app 打开
    if (!self->_reader_ok) {
        static int retry_div = 0;
        if (++retry_div >= 5) {
            retry_div = 0;
            self->_hwInit();
        }
        return;
    }

    // 每轮: WUPA (唤醒 IDLE/HALT 卡) → 防冲突选卡 → 处理后 HLTA 回 HALT 态,
    // 保证卡贴住不动时每轮都能稳定重选 (ACTIVE 态的卡不应答 REQA/WUPA).
    uint8_t atqa[2] = {0};
    if (self->_stReqWupa(atqa, true)) {
        uint8_t uid[10] = {0};
        int ulen = 0;
        uint8_t sak = 0;
        if (self->_stSelectCard(uid, &ulen, &sak)) {
            self->_absent_streak = 0;
            self->_card_active = true;
            bool changed = !self->_card_present || ulen != self->_uid_len ||
                           memcmp(uid, self->_uid, ulen) != 0;
            if (changed) {
                memcpy(self->_uid, uid, ulen);
                self->_uid_len = ulen;
                self->_sak = sak;
                self->_atqa[0] = atqa[0];
                self->_atqa[1] = atqa[1];
                self->_classic = (sak == 0x08) || (sak == 0x18) || (sak == 0x09);
                self->_card_present = true;
                self->_encrypted = false;
                mclog::tagInfo(TAG, "card detected, uid_len={} sak=0x{:02X}", ulen, sak);
                self->_showCardInfo();
                self->_readBlockNow();  // 新卡自动读当前块
            }
            self->_stHaltA();  // 回 HALT, 下轮 WUPA 重唤醒
        }
    } else if (self->_card_present && ++self->_absent_streak >= 2) {  // 连续 2 次失败才判移除
        self->_card_present = false;
        self->_absent_streak = 0;
        self->_encrypted = false;
        self->_card_active = false;
        mclog::tagInfo(TAG, "card removed");
        self->_clearCardInfo();
        self->_setMsg("卡片已移开", C_DIM);
    }
#else
    (void)t;
#endif
}

// ════════════════════════════════════════════════════════════════════════════
//  ST25R3916 驱动 (软 I2C, ESP only)
//  移植自 M5Stack M5Unit-NFC (unit_ST25R3916*), 精简为单卡 NFC-A 读写
// ════════════════════════════════════════════════════════════════════════════
#ifndef PLATFORM_BUILD_DESKTOP

#include <bitset>
#include <esp_random.h>

namespace st25 {

// ── 寄存器 (Space-A) ─────────────────────────────────────────────────────────
static constexpr uint8_t REG_IO_CONFIG_1   = 0x00;
static constexpr uint8_t REG_IO_CONFIG_2   = 0x01;
static constexpr uint8_t REG_OP_CONTROL    = 0x02;
static constexpr uint8_t REG_MODE_DEF      = 0x03;
static constexpr uint8_t REG_BITRATE_DEF   = 0x04;
static constexpr uint8_t REG_ISO14443A_SET = 0x05;
static constexpr uint8_t REG_NFCIP1_PT_DEF = 0x08;
static constexpr uint8_t REG_AUX_DEF       = 0x0A;
static constexpr uint8_t REG_RX_CONFIG_1   = 0x0B;
static constexpr uint8_t REG_RX_CONFIG_2   = 0x0C;
static constexpr uint8_t REG_RX_CONFIG_3   = 0x0D;
static constexpr uint8_t REG_RX_CONFIG_4   = 0x0E;
static constexpr uint8_t REG_NRT_1         = 0x10;
static constexpr uint8_t REG_NRT_2         = 0x11;
static constexpr uint8_t REG_MAIN_IRQ      = 0x1A;
static constexpr uint8_t REG_TIMER_NFC_IRQ = 0x1B;
static constexpr uint8_t REG_FIFO_STATUS_1 = 0x1E;
static constexpr uint8_t REG_NTX_1         = 0x22;
static constexpr uint8_t REG_NTX_2         = 0x23;
static constexpr uint8_t REG_ANT_TUNE_1    = 0x26;
static constexpr uint8_t REG_ANT_TUNE_2    = 0x27;
static constexpr uint8_t REG_TX_DRIVER     = 0x28;
static constexpr uint8_t REG_PT_MODULATION = 0x29;
static constexpr uint8_t REG_EFD_ACT_THD   = 0x2A;
static constexpr uint8_t REG_EFD_DEACT_THD = 0x2B;
static constexpr uint8_t REG_AUX_DISPLAY   = 0x31;
static constexpr uint8_t REG_IC_IDENTITY   = 0x3F;

// Space-B (前缀 0xFB)
static constexpr uint8_t REGB_EMD_SUPPRESSION = 0x05;
static constexpr uint8_t REGB_CORRELATOR_1    = 0x0C;
static constexpr uint8_t REGB_CORRELATOR_2    = 0x0D;
static constexpr uint8_t REGB_RESISTIVE_AM    = 0x2A;
static constexpr uint8_t REGB_OVERSHOOT_1     = 0x30;
static constexpr uint8_t REGB_OVERSHOOT_2     = 0x31;
static constexpr uint8_t REGB_UNDERSHOOT_1    = 0x32;
static constexpr uint8_t REGB_UNDERSHOOT_2    = 0x33;

// 操作码 / 直接命令
static constexpr uint8_t OP_READ_REG   = 0x40;
static constexpr uint8_t OP_LOAD_FIFO  = 0x80;
static constexpr uint8_t OP_READ_FIFO  = 0x9F;
static constexpr uint8_t OP_SPACE_B    = 0xFB;
static constexpr uint8_t CMD_SET_DEFAULT        = 0xC1;
static constexpr uint8_t CMD_STOP_ALL           = 0xC2;
static constexpr uint8_t CMD_TX_WITH_CRC        = 0xC4;
static constexpr uint8_t CMD_TX_WITHOUT_CRC     = 0xC5;
static constexpr uint8_t CMD_TX_REQA            = 0xC6;
static constexpr uint8_t CMD_TX_WUPA            = 0xC7;
static constexpr uint8_t CMD_NFC_INITIAL_FIELD_ON = 0xC8;
static constexpr uint8_t CMD_TEST_ACCESS        = 0xFC;
static constexpr uint8_t CMD_RESET_RX_GAIN      = 0xD5;
static constexpr uint8_t CMD_ADJUST_REGULATORS  = 0xD6;
static constexpr uint8_t CMD_CLEAR_FIFO         = 0xDB;

// 位定义
static constexpr uint8_t OPCTL_EN    = 0x80;  // REG_OP_CONTROL
static constexpr uint8_t OPCTL_RX_EN = 0x40;
static constexpr uint8_t OPCTL_TX_EN = 0x08;
static constexpr uint8_t AUX_NO_CRC_RX = 0x80;  // REG_AUX_DEF
static constexpr uint8_t AUX_DIS_CORR  = 0x04;
static constexpr uint8_t ISOA_NO_TX_PAR = 0x80;  // REG_ISO14443A_SET
static constexpr uint8_t ISOA_ANTCL     = 0x01;
static constexpr uint8_t IRQ_RXE = 0x10;  // REG_MAIN_IRQ
static constexpr uint8_t IRQ_COL = 0x04;
static constexpr uint8_t IRQ_NRE = 0x40;  // REG_TIMER_NFC_IRQ
static constexpr uint8_t AUXD_OSC_OK = 0x10;  // REG_AUX_DISPLAY

static constexpr uint8_t  ACK_NIBBLE = 0x0A;
static constexpr uint32_t FC_HZ = 13560000;

// ── Crypto1 (移植自 nfc/crypto/mifare_classic_crypto1.hpp) ───────────────────
class Crypto1 {
public:
    void init(uint64_t key48)
    {
        _state.reset();
        for (int i = 0; i < 48; ++i) {
            int byte_index = i >> 3;
            int bit_index  = i & 0x07;
            int reversed   = (byte_index << 3) + (bit_index ^ 7);
            _state[i]      = (key48 >> reversed) & 1ULL;
        }
    }

    uint32_t inject(uint32_t uid, uint32_t nt, bool encrypted = false)
    {
        return step32(uid ^ nt, encrypted);
    }

    bool step_with(bool in, bool enc = false)
    {
        bool z = filter();
        step();
        bool ext = in ^ (enc ? z : false);
        _state[0] = _state[0] ^ ext;
        return z;
    }

    uint8_t step8(uint8_t in, bool enc = false)
    {
        uint8_t v = 0;
        for (int i = 0; i < 8; ++i) v |= (uint8_t)step_with((in >> i) & 1, enc) << i;
        return v;
    }

    uint32_t step32(uint32_t in, bool enc = false)
    {
        uint32_t v = 0;
        for (uint32_t i = 0; i < 32; ++i) {
            bool t = step_with((in >> (i ^ 24)) & 1u, enc);
            v |= (uint32_t)t << (24 ^ i);
        }
        return v;
    }

    // 加密 Nr(MSB 先) + Ar(LSB 先), 返回 8 字节的加密奇偶位
    uint8_t encrypt(uint8_t buf[8], uint32_t nr, uint32_t ar)
    {
        uint8_t parity = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t v = (uint8_t)(nr >> ((i ^ 0x03) << 3));
            buf[i]    = step8(v) ^ v;
            parity |= (uint8_t)((filter() ^ oddParity(v)) & 1) << i;
        }
        for (int pos = 4; pos < 8; ++pos) {
            int i       = pos - 4;
            uint8_t v   = (uint8_t)(ar >> (i << 3));
            uint8_t ks  = step8(0x00);
            buf[pos]    = ks ^ v;
            parity |= (uint8_t)((filter() ^ oddParity(v)) & 1) << pos;
        }
        return parity;
    }

    // 加密任意长度缓冲 (≤32), 返回奇偶位
    uint32_t encrypt(uint8_t* out, const uint8_t* in, uint8_t in_len)
    {
        uint32_t parity = 0;
        for (uint8_t i = 0; i < in_len; ++i) {
            uint8_t ks = step8(0);
            out[i]     = in[i] ^ ks;
            parity |= ((uint32_t)(filter() ^ oddParity(in[i])) & 1) << i;
        }
        return parity;
    }

    bool filter() const
    {
        const auto& s = _state;
        bool b5 = fb(s[6], s[4], s[2], s[0]);
        bool a4 = fa(s[14], s[12], s[10], s[8]);
        bool b3 = fb(s[22], s[20], s[18], s[16]);
        bool b2 = fb(s[30], s[28], s[26], s[24]);
        bool a1 = fa(s[38], s[36], s[34], s[32]);
        return fc(a1, b2, b3, a4, b5);
    }

    static bool fa(bool a, bool b, bool c, bool d) { return ((a || b) ^ (a && d)) ^ (c && ((a ^ b) || d)); }
    static bool fb(bool a, bool b, bool c, bool d) { return ((a && b) || c) ^ ((a ^ b) && (c || d)); }
    static bool fc(bool a, bool b, bool c, bool d, bool e)
    {
        return (a || ((b || e) && (d ^ e))) ^ ((a ^ (b && d)) && ((c ^ d) || (b && e)));
    }

private:
    bool step()
    {
        bool out = _state[47];
        bool fbv = _state[4] ^ _state[5] ^ _state[6] ^ _state[8] ^ _state[12] ^ _state[18] ^
                   _state[20] ^ _state[22] ^ _state[23] ^ _state[28] ^ _state[30] ^ _state[32] ^
                   _state[33] ^ _state[35] ^ _state[37] ^ _state[38] ^ _state[42] ^ _state[47];
        _state <<= 1;
        _state[0] = fbv;
        return out;
    }
    static uint8_t oddParity(uint8_t v)
    {
        v ^= v >> 4;
        v ^= v >> 2;
        v ^= v >> 1;
        return (v & 1) ? 0 : 1;
    }
    std::bitset<48> _state;
};

// MIFARE Classic prng 后继值 (FibonacciLFSR_Right<32,16,14,13,11>)
static void suc23(uint32_t seed_le, uint32_t& suc2, uint32_t& suc3)
{
    uint32_t s = seed_le;
    auto step1 = [&]() {
        bool out = s & 1;
        bool fbv = ((s >> 16) ^ (s >> 18) ^ (s >> 19) ^ (s >> 21)) & 1;
        s = (s >> 1) | ((uint32_t)fbv << 31);
        return out;
    };
    auto next32 = [&]() {
        uint32_t v = 0;
        for (int i = 0; i < 32; ++i) v |= (uint32_t)step1() << i;
        return v;
    };
    next32();
    next32();
    suc2 = next32();
    suc3 = next32();
}

static inline uint32_t be32(const uint8_t a[4])
{
    return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
}

static inline uint32_t bswap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

// 给加密数据追加奇偶位 (每字节 1 位, 共 9n 位)
static void appendParity(uint8_t* out, const uint8_t* in, uint32_t in_len, uint32_t parity)
{
    uint32_t bitpos = 0;
    for (uint32_t i = 0; i < in_len; ++i) {
        uint8_t v = in[i];
        for (int k = 0; k < 8; ++k) {
            if ((v >> k) & 1) out[bitpos >> 3] |= (1u << (bitpos & 7));
            ++bitpos;
        }
        if ((parity >> i) & 1) out[bitpos >> 3] |= (1u << (bitpos & 7));
        ++bitpos;
    }
}

}  // namespace st25

// Crypto1 密钥流状态: 认证成功后必须跨函数延续 (认证→读/写共享同一会话)
static st25::Crypto1 s_crypto;

// ── 低层寄存器访问 ────────────────────────────────────────────────────────────
bool AppRfid::_stWriteA(uint8_t reg, uint8_t val)
{
    bb_start();
    bool ok = bb_write((uint8_t)(NFC_I2C_ADDR << 1)) && bb_write(reg) && bb_write(val);
    bb_stop();
    return ok;
}

bool AppRfid::_stReadA(uint8_t reg, uint8_t* out, size_t n)
{
    bb_start();
    if (!bb_write((uint8_t)(NFC_I2C_ADDR << 1)) || !bb_write((uint8_t)(st25::OP_READ_REG | reg))) {
        bb_stop();
        return false;
    }
    bb_start();
    if (!bb_write((uint8_t)((NFC_I2C_ADDR << 1) | 1))) { bb_stop(); return false; }
    for (size_t i = 0; i < n; i++) out[i] = bb_read(i + 1 < n);
    bb_stop();
    return true;
}

uint8_t AppRfid::_stReadA8(uint8_t reg)
{
    uint8_t v = 0;
    _stReadA(reg, &v, 1);
    return v;
}

bool AppRfid::_stWriteB(uint8_t reg, uint8_t val)
{
    bb_start();
    bool ok = bb_write((uint8_t)(NFC_I2C_ADDR << 1)) && bb_write(st25::OP_SPACE_B) &&
              bb_write(reg) && bb_write(val);
    bb_stop();
    return ok;
}

bool AppRfid::_stCmd(uint8_t cmd)
{
    bb_start();
    bool ok = bb_write((uint8_t)(NFC_I2C_ADDR << 1)) && bb_write(cmd);
    bb_stop();
    return ok;
}

bool AppRfid::_stModifyA(uint8_t reg, uint8_t set_mask, uint8_t clear_mask)
{
    uint8_t v = _stReadA8(reg);
    uint8_t w = (v & ~clear_mask) | set_mask;
    return (w == v) || _stWriteA(reg, w);
}

void AppRfid::_stClearIrqs()
{
    uint8_t dummy[4];
    _stReadA(st25::REG_MAIN_IRQ, dummy, 4);  // 读 0x1A-0x1D 即清除中断
}

bool AppRfid::_stFifoLoad(const uint8_t* data, size_t n)
{
    bb_start();
    bool ok = bb_write((uint8_t)(NFC_I2C_ADDR << 1)) && bb_write(st25::OP_LOAD_FIFO);
    for (size_t i = 0; ok && i < n; i++) ok = bb_write(data[i]);
    bb_stop();
    return ok;
}

bool AppRfid::_stFifoRead(uint8_t* out, size_t n)
{
    bb_start();
    if (!bb_write((uint8_t)(NFC_I2C_ADDR << 1)) || !bb_write(st25::OP_READ_FIFO)) {
        bb_stop();
        return false;
    }
    bb_start();
    if (!bb_write((uint8_t)((NFC_I2C_ADDR << 1) | 1))) { bb_stop(); return false; }
    for (size_t i = 0; i < n; i++) out[i] = bb_read(i + 1 < n);
    bb_stop();
    return true;
}

bool AppRfid::_stFifoSize(uint16_t& bytes, uint8_t& bits)
{
    uint8_t s[2] = {0, 0};
    if (!_stReadA(st25::REG_FIFO_STATUS_1, s, 2)) return false;
    uint16_t v = ((uint16_t)s[0] << 8) | s[1];
    bytes = (v >> 8) | ((v & 0x00C0) << 2);
    bits  = (v >> 1) & 0x07;
    return true;
}

// 等待接收结束 (RXE) 或无响应超时 (NRE); collision 输出碰撞标志
bool AppRfid::_stWaitRxe(uint32_t timeout_ms, bool* collision)
{
    if (collision) *collision = false;
    for (uint32_t i = 0; i < timeout_ms * 4; i++) {
        uint8_t irq[2];
        _stReadA(st25::REG_MAIN_IRQ, irq, 2);  // 0x1A + 0x1B
        if (irq[1] & st25::IRQ_NRE) return false;                 // 无响应超时
        if (irq[0] & st25::IRQ_COL) { if (collision) *collision = true; return true; }
        if (irq[0] & st25::IRQ_RXE) return true;
        esp_rom_delay_us(250);
    }
    return false;
}

void AppRfid::_stWriteNtx(uint16_t bytes, uint8_t bits)
{
    uint16_t v = (uint16_t)(((bytes & 0x01FF) << 3) | (bits & 0x07));
    _stWriteA(st25::REG_NTX_1, (uint8_t)(v >> 8));
    _stWriteA(st25::REG_NTX_2, (uint8_t)(v & 0xFF));
}

void AppRfid::_stWriteNrt(uint32_t ms)
{
    // nrt_step=0 (64/fc 步进): nrt = ceil(ms*1000*fc / (64*1e6))
    uint64_t nrt = ((uint64_t)ms * 1000 * st25::FC_HZ + 64ull * 1000000 - 1) / (64ull * 1000000);
    if (nrt < 1) nrt = 1;
    if (nrt > 0xFFFF) nrt = 0xFFFF;
    _stWriteA(st25::REG_NRT_1, (uint8_t)(nrt >> 8));
    _stWriteA(st25::REG_NRT_2, (uint8_t)(nrt & 0xFF));
}

// ── 初始化 (M5Unit-NFC begin() 精简版, 无 IRQ 引脚, 轮询中断寄存器) ──────────
bool AppRfid::_stInit()
{
    GetHAL()->delay(50);  // 等芯片上电稳定

    // 芯片识别 (重试 5 次)
    bool detected = false;
    for (int i = 0; i < 5 && !detected; i++) {
        uint8_t v = _stReadA8(st25::REG_IC_IDENTITY);
        uint8_t type = (v >> 3) & 0x1F, rev = v & 0x07;
        mclog::tagInfo(TAG, "IC identity: 0x{:02X} (type 0x{:02X} rev {})", v, type, rev);
        detected = (type == 0x05 && rev != 0);
        if (!detected) GetHAL()->delay(20);
    }
    if (!detected) {
        mclog::tagWarn(TAG, "ST25R3916 not detected");
        _setMsg("ST25R3916 识别失败", C_ERR);
        return false;
    }

    // 防御性复位: 停掉上次遗留的 RF 活动
    _stCmd(st25::CMD_STOP_ALL);
    _stClearBitsA(st25::REG_OP_CONTROL, st25::OPCTL_TX_EN | st25::OPCTL_RX_EN);
    GetHAL()->delay(2);

    if (!_stCmd(st25::CMD_SET_DEFAULT)) return false;

    // 过热保护解除: FC / 04 / 10
    {
        bb_start();
        bool ok = bb_write((uint8_t)(NFC_I2C_ADDR << 1)) && bb_write(st25::CMD_TEST_ACCESS) &&
                  bb_write(0x04) && bb_write(0x10);
        bb_stop();
        if (!ok) return false;
    }

    // IO 配置: 总线 ~50kHz → i2c_thd=0; 5V 供电 → 不设 sup3v; io_drv_lvl
    if (!_stWriteA(st25::REG_IO_CONFIG_1, 0x00) || !_stWriteA(st25::REG_IO_CONFIG_2, 0x04))
        return false;

    _stWriteA(st25::REG_TX_DRIVER, 13 << 4);        // AM 调制强度 13
    _stSetBitsA(st25::REG_IO_CONFIG_1, 0x07);       // MCU_CLK 关闭
    _stWriteB(st25::REGB_RESISTIVE_AM, 0x80);       // 最小 non-overlap
    _stSetBitsA(st25::REG_IO_CONFIG_2, 0x20);       // aat_en
    _stWriteB(st25::REGB_RESISTIVE_AM, 0x00);       // 常规 non-overlap

    _stWriteA(st25::REG_EFD_ACT_THD, 0x13);         // 外部场检测阈值
    _stWriteA(st25::REG_EFD_DEACT_THD, 0x02);
    _stModifyA(st25::REG_NFCIP1_PT_DEF, 0x05 << 4, 0xF0);  // FDT
    _stWriteA(st25::REG_PT_MODULATION, 0x5F);
    _stWriteB(st25::REGB_EMD_SUPPRESSION, 0x40);
    _stWriteA(st25::REG_ANT_TUNE_1, 0x82);
    _stWriteA(st25::REG_ANT_TUNE_2, 0x82);

    _stSetBitsA(st25::REG_OP_CONTROL, 0x03);        // 自动外部场检测
    _stCmd(st25::CMD_CLEAR_FIFO);

    // 开振荡器并等待稳定
    if ((_stReadA8(st25::REG_OP_CONTROL) & st25::OPCTL_EN) == 0) {
        _stClearIrqs();
        _stSetBitsA(st25::REG_OP_CONTROL, st25::OPCTL_EN);
        bool osc = false;
        for (int i = 0; i < 100 && !osc; i++) {     // 最长 ~50ms
            osc = (_stReadA8(st25::REG_AUX_DISPLAY) & st25::AUXD_OSC_OK) != 0;
            if (!osc) esp_rom_delay_us(500);
        }
        if (!osc) {
            mclog::tagWarn(TAG, "oscillator failed");
            _setMsg("振荡器启动失败", C_ERR);
            return false;
        }
    }

    _stCmd(st25::CMD_ADJUST_REGULATORS);
    GetHAL()->delay(5);
    return true;
}

// 配置 ISO14443A 106k 并打开 RF 场 (configure_nfc_a 精简版)
bool AppRfid::_stConfigNfcA()
{
    _stCmd(st25::CMD_STOP_ALL);
    _stClearBitsA(st25::REG_OP_CONTROL, 0x04);      // 关闭 wakeup 模式

    if (!_stWriteA(st25::REG_MODE_DEF, 0x08 | 0x01) ||   // ISO14443A + nfc_ar8_auto
        !_stWriteA(st25::REG_BITRATE_DEF, 0x00) ||       // 106k/106k
        !_stWriteA(st25::REG_ISO14443A_SET, 0x00))
        return false;

    _stClearBitsA(st25::REG_AUX_DEF, st25::AUX_DIS_CORR);
    _stWriteB(st25::REGB_OVERSHOOT_1, 0x40);
    _stWriteB(st25::REGB_OVERSHOOT_2, 0x03);
    _stWriteB(st25::REGB_UNDERSHOOT_1, 0x40);
    _stWriteB(st25::REGB_UNDERSHOOT_2, 0x03);
    _stWriteB(st25::REGB_CORRELATOR_1, 0x47);
    _stWriteB(st25::REGB_CORRELATOR_2, 0x00);

    _stWriteA(st25::REG_RX_CONFIG_1, 0x08);         // z_600k
    _stWriteA(st25::REG_RX_CONFIG_2, 0x20 | 0x08 | 0x04 | 0x01);  // sqm_dyn|agc_en|agc_m|agc6_3
    _stWriteA(st25::REG_RX_CONFIG_3, 0xD8);         // 稳定优先的增益
    _stWriteA(st25::REG_RX_CONFIG_4, 0x22);
    _stCmd(st25::CMD_RESET_RX_GAIN);

    // 开 RF 场
    _stCmd(st25::CMD_NFC_INITIAL_FIELD_ON);
    GetHAL()->delay(5);
    return _stSetBitsA(st25::REG_OP_CONTROL, st25::OPCTL_TX_EN | st25::OPCTL_RX_EN);
}

// 普通收发 (硬件自动 CRC)
bool AppRfid::_stTransceive(const uint8_t* tx, size_t tx_n, uint8_t* rx, size_t rx_cap,
                            size_t* rx_n, uint8_t* rx_last_bits, uint32_t timeout_ms)
{
    _stWriteNrt(timeout_ms);
    _stWriteA(st25::REG_ISO14443A_SET, 0x00);
    _stClearBitsA(st25::REG_AUX_DEF, st25::AUX_NO_CRC_RX);
    _stClearIrqs();
    _stCmd(st25::CMD_CLEAR_FIFO);
    _stFifoLoad(tx, tx_n);
    _stWriteNtx((uint16_t)tx_n, 0);
    _stCmd(st25::CMD_TX_WITH_CRC);

    if (!_stWaitRxe(timeout_ms)) return false;

    uint16_t bytes = 0;
    uint8_t bits = 0;
    _stFifoSize(bytes, bits);
    size_t n = bytes < rx_cap ? bytes : rx_cap;
    if (n && !_stFifoRead(rx, n)) return false;
    if (rx_n) *rx_n = n;
    if (rx_last_bits) *rx_last_bits = bits;
    return n > 0;
}

bool AppRfid::_stReqWupa(uint8_t atqa[2], bool wupa)
{
    _stWriteNrt(4);
    _stWriteA(st25::REG_ISO14443A_SET, st25::ISOA_ANTCL);
    _stSetBitsA(st25::REG_AUX_DEF, st25::AUX_NO_CRC_RX);  // ATQA 应答无 CRC
    _stClearIrqs();
    _stCmd(st25::CMD_CLEAR_FIFO);
    _stCmd(wupa ? st25::CMD_TX_WUPA : st25::CMD_TX_REQA);

    if (!_stWaitRxe(6)) return false;

    uint16_t bytes = 0;
    uint8_t bits = 0;
    _stFifoSize(bytes, bits);
    if (bytes < 2) return false;
    return _stFifoRead(atqa, 2);
}

// HLTA: 选中的卡回 HALT 态, 下一轮用 WUPA 重唤醒. 卡不应答, 忽略结果.
void AppRfid::_stHaltA()
{
    uint8_t frame[2] = {0x50, 0x00};
    uint8_t rx[4] = {0};
    size_t rn = 0;
    _stTransceive(frame, 2, rx, sizeof(rx), &rn, nullptr, 5);
    _encrypted   = false;  // Crypto1 会话随 HALT 失效
    _card_active = false;
}

// 手动读写前确保卡已激活: 已 ACTIVE 直接用, 否则 WUPA + 重选并核对 UID
bool AppRfid::_wakeAndSelect()
{
    if (_card_active) return true;
    uint8_t atqa[2] = {0};
    if (!_stReqWupa(atqa, true)) return false;
    uint8_t uid[10] = {0};
    int ulen = 0;
    uint8_t sak = 0;
    if (!_stSelectCard(uid, &ulen, &sak)) return false;
    if (ulen != _uid_len || memcmp(uid, _uid, ulen) != 0) return false;
    _card_active = true;
    return true;
}

// 防冲突 (单卡, 无碰撞循环) + 选卡, 支持 1~3 级级联
bool AppRfid::_stSelectCard(uint8_t* uid, int* uid_len, uint8_t* sak)
{
    uint8_t sel = 0x93;
    int off = 0;
    for (int level = 0; level < 3; level++) {
        // ANTICOLL: [SELn, 0x20], 无 CRC
        _stWriteNrt(8);
        _stWriteA(st25::REG_ISO14443A_SET, st25::ISOA_ANTCL);
        _stClearBitsA(st25::REG_AUX_DEF, st25::AUX_NO_CRC_RX);
        _stClearIrqs();
        _stCmd(st25::CMD_CLEAR_FIFO);
        uint8_t frame[2] = {sel, 0x20};
        _stFifoLoad(frame, 2);
        _stWriteNtx(2, 0);
        _stCmd(st25::CMD_TX_WITHOUT_CRC);

        bool collision = false;
        if (!_stWaitRxe(10, &collision) || collision) return false;  // 单卡假设

        uint8_t rbuf[5] = {0};
        uint16_t bytes = 0;
        uint8_t bits = 0;
        _stFifoSize(bytes, bits);
        if (bytes != 5 || !_stFifoRead(rbuf, 5)) return false;
        if ((uint8_t)(rbuf[0] ^ rbuf[1] ^ rbuf[2] ^ rbuf[3]) != rbuf[4]) return false;  // BCC

        // SELECT: [SELn, 0x70, uid4, bcc], 带 CRC
        uint8_t sframe[7] = {sel, 0x70, rbuf[0], rbuf[1], rbuf[2], rbuf[3], rbuf[4]};
        uint8_t srx[4] = {0};
        size_t sn = 0;
        if (!_stTransceive(sframe, 7, srx, 3, &sn, nullptr, 8) || sn < 1) return false;

        if (rbuf[0] == 0x88) {           // 级联标志
            memcpy(uid + off, rbuf + 1, 3);
            off += 3;
            sel += 2;                    // 0x93 → 0x95 → 0x97
        } else {
            memcpy(uid + off, rbuf, 4);
            off += 4;
            *uid_len = off;
            *sak = srx[0];
            return true;
        }
    }
    return false;
}

// ── MIFARE Classic: Crypto1 三次握手认证 (默认 KeyA FF×6) ────────────────────
bool AppRfid::_mfAuthKeyA(uint8_t block)
{
    _encrypted = false;

    // 1) 明文发送 AUTH, 收卡方随机数 Nt
    uint8_t auth_frame[2] = {0x60, block};
    uint8_t rb[4] = {0};
    size_t rlen = 0;
    if (!_stTransceive(auth_frame, 2, rb, 4, &rlen, nullptr, 4) || rlen != 4) {
        mclog::tagWarn(TAG, "auth step1 failed");
        return false;
    }
    esp_rom_delay_us(87);  // AUTH → AB 至少 86.4us

    // 2) Crypto1 初始化, 发加密的 (Nr, Ar)
    const uint64_t key48 = 0xFFFFFFFFFFFFULL;  // 默认 KeyA
    const uint8_t* tail4 = _uid + _uid_len - 4;
    uint32_t u32 = st25::be32(tail4);
    uint32_t nt  = st25::be32(rb);
    uint32_t nr  = esp_random();
    uint32_t ar = 0, suc3 = 0;

    s_crypto.init(key48);
    s_crypto.inject(u32, nt, false);
    st25::suc23(st25::bswap32(nt), ar, suc3);

    uint8_t ab[8] = {0};
    uint8_t parity = s_crypto.encrypt(ab, nr, ar);
    uint8_t bitstream[9] = {0};
    st25::appendParity(bitstream, ab, 8, parity);

    _stCmd(st25::CMD_RESET_RX_GAIN);
    _stWriteNrt(16);
    _stWriteA(st25::REG_ISO14443A_SET, st25::ISOA_NO_TX_PAR);
    _stSetBitsA(st25::REG_AUX_DEF, st25::AUX_NO_CRC_RX);
    _stClearIrqs();
    _stCmd(st25::CMD_CLEAR_FIFO);
    _stFifoLoad(bitstream, sizeof(bitstream));
    _stWriteNtx(9, 0);
    _stCmd(st25::CMD_TX_WITHOUT_CRC);

    if (!_stWaitRxe(20)) {
        mclog::tagWarn(TAG, "auth step2 timeout");
        return false;
    }

    // 3) 收并解密 At, 校验 == suc3
    uint8_t ba[4] = {0};
    uint16_t bytes = 0;
    uint8_t bits = 0;
    _stFifoSize(bytes, bits);
    if (bytes != 4 || !_stFifoRead(ba, 4)) return false;

    uint8_t at2[4];
    for (int i = 0; i < 4; i++) at2[i] = ba[i] ^ s_crypto.step8(0);
    uint32_t at32 = (uint32_t)at2[0] | ((uint32_t)at2[1] << 8) | ((uint32_t)at2[2] << 16) |
                    ((uint32_t)at2[3] << 24);
    _encrypted = (at32 == suc3);
    if (!_encrypted) mclog::tagWarn(TAG, "auth failed (At mismatch, key not FF×6?)");
    return _encrypted;
}

// 加密发送: 明文+CRC → Crypto1 加密 → 追加奇偶位 → 9n 位无 CRC 发送
bool AppRfid::_mfSendEncrypted(const uint8_t* tx, size_t tx_n)
{
    uint8_t tmp[20] = {0};
    memcpy(tmp, tx, tx_n);
    uint8_t crc[2];
    _crcA(tx, tx_n, crc);
    tmp[tx_n]     = crc[0];
    tmp[tx_n + 1] = crc[1];
    size_t total = tx_n + 2;

    uint8_t enc[20] = {0};
    uint32_t parity = s_crypto.encrypt(enc, tmp, (uint8_t)total);

    uint16_t total_bits = (uint16_t)(9 * total);
    uint8_t bitstream[24] = {0};
    st25::appendParity(bitstream, enc, total, parity);

    _stWriteA(st25::REG_ISO14443A_SET, st25::ISOA_NO_TX_PAR);
    _stClearIrqs();
    _stCmd(st25::CMD_CLEAR_FIFO);
    _stFifoLoad(bitstream, (total_bits + 7) >> 3);
    _stWriteNtx((uint16_t)(total_bits >> 3), (uint8_t)(total_bits & 0x07));
    return _stCmd(st25::CMD_TX_WITHOUT_CRC);
}

bool AppRfid::_mfTransceiveEncrypted(uint8_t* rx, size_t* rx_n, size_t rx_cap,
                                     const uint8_t* tx, size_t tx_n,
                                     uint32_t timeout_ms, bool include_crc, bool decrypt)
{
    if (include_crc) _stSetBitsA(st25::REG_AUX_DEF, st25::AUX_NO_CRC_RX);
    else             _stClearBitsA(st25::REG_AUX_DEF, st25::AUX_NO_CRC_RX);
    if (!_mfSendEncrypted(tx, tx_n)) return false;

    size_t rlen = rx_cap + (include_crc ? 2 : 0);
    if (!_stWaitRxe(timeout_ms + 6)) return false;

    uint16_t bytes = 0;
    uint8_t bits = 0;
    _stFifoSize(bytes, bits);
    if (bytes != rlen) return false;
    uint8_t rbuf[20] = {0};
    if (!_stFifoRead(rbuf, rlen)) return false;

    if (decrypt) {
        if (bytes == 1) {  // 4-bit ACK
            uint8_t ret = rbuf[0] & 0x0F, res = 0;
            res |= (uint8_t)(s_crypto.step_with(0) ^ ((ret >> 0) & 1)) << 0;
            res |= (uint8_t)(s_crypto.step_with(0) ^ ((ret >> 1) & 1)) << 1;
            res |= (uint8_t)(s_crypto.step_with(0) ^ ((ret >> 2) & 1)) << 2;
            res |= (uint8_t)(s_crypto.step_with(0) ^ ((ret >> 3) & 1)) << 3;
            rbuf[0] = res;
            if (res != st25::ACK_NIBBLE) return false;
        } else {
            for (size_t i = 0; i < bytes; i++) rbuf[i] ^= s_crypto.step8(0);
        }
    }

    if (include_crc) {
        uint8_t crc[2];
        _crcA(rbuf, rx_cap, crc);
        if (crc[0] != rbuf[rlen - 2] || crc[1] != rbuf[rlen - 1]) return false;
    }
    memcpy(rx, rbuf, rx_cap);
    if (rx_n) *rx_n = rx_cap;
    return true;
}

// ── 块读写 (Classic 走加密; Ultralight/NTAG 走明文) ──────────────────────────
bool AppRfid::_cardReadBlock(uint8_t block, uint8_t out[16])
{
    uint8_t cmd[2] = {0x30, block};
    if (_classic) {
        if (!_encrypted && !_mfAuthKeyA(block)) return false;
        size_t n = 0;
        return _mfTransceiveEncrypted(out, &n, 16, cmd, 2, 12, true, true);
    }
    size_t n = 0;
    return _stTransceive(cmd, 2, out, 16, &n, nullptr, 12) && n == 16;
}

bool AppRfid::_cardWriteBlock(uint8_t block, const uint8_t data[16])
{
    if (_classic) {
        if (!_encrypted && !_mfAuthKeyA(block)) return false;
        uint8_t cmd[2] = {0xA0, block};
        uint8_t rx[1];
        size_t n = 0;
        if (!_mfTransceiveEncrypted(rx, &n, 1, cmd, 2, 5, false, true) || rx[0] != st25::ACK_NIBBLE)
            return false;
        n = 0;
        if (!_mfTransceiveEncrypted(rx, &n, 1, data, 16, 10, false, true) || rx[0] != st25::ACK_NIBBLE)
            return false;
        return true;
    }
    // Ultralight/NTAG: 0xA2 页写, 每次 4 字节
    for (int p = 0; p < 4; p++) {
        uint8_t cmd[6] = {(uint8_t)0xA2, (uint8_t)(block + p),
                          data[p * 4], data[p * 4 + 1], data[p * 4 + 2], data[p * 4 + 3]};
        uint8_t rx[2] = {0};
        size_t n = 0;
        uint8_t lb = 0;
        if (!_stTransceive(cmd, 6, rx, 1, &n, &lb, 10) || (rx[0] & 0x0F) != st25::ACK_NIBBLE)
            return false;
    }
    return true;
}

// ISO14443-3 CRC_A (与 M5Utility CRC16(0xC6C6,0x1021,ref,ref,0) 等价: init 0x6363)
void AppRfid::_crcA(const uint8_t* data, size_t n, uint8_t out[2])
{
    uint16_t crc = 0x6363;
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)(data[i] ^ (crc & 0xFF));
        ch ^= (uint8_t)(ch << 4);
        crc = (crc >> 8) ^ ((uint16_t)ch << 8) ^ ((uint16_t)ch << 3) ^ ((uint16_t)ch >> 4);
    }
    out[0] = (uint8_t)(crc & 0xFF);
    out[1] = (uint8_t)(crc >> 8);
}

// ════════════════════════════════════════════════════════════════════════════
//  后台「嘀卡关灯」服务
//  与 M5Dial-UserDemo 的 rfid_service 同一思路: 开机常驻任务拥有读卡器,
//  卡进场 (缺席→在场沿) 蜂鸣一声; UID 匹配 NVS "rfid_off_uids" 时对实体列表
//  逐个调 HA turn_off. 打开 RFID Reader UI 时经 s_ui_owns 让位, 关闭后接管.
// ════════════════════════════════════════════════════════════════════════════

AppRfid* AppRfid::s_instance = nullptr;
std::atomic<bool> AppRfid::s_ui_owns{false};
std::atomic<bool> AppRfid::s_bg_reinit{false};

void AppRfid::_bgStart()
{
    AppEmailLed::retirePortA();  // 后台寻卡与 RMT 灯带物理互斥, PORT A 专属 NFC
    xTaskCreate(_bgTaskEntry, "rfid_bg", 6 * 1024, this, 4, nullptr);
}

void AppRfid::_bgTaskEntry(void* arg)
{
    static_cast<AppRfid*>(arg)->_bgLoop();
    vTaskDelete(nullptr);
}

// 后台路径的硬件初始化: 与 _hwInit 相同的引脚/芯片流程, 但不碰任何 UI 对象
bool AppRfid::_bgHwInit()
{
    gpio_reset_pin(BB_SDA);
    gpio_reset_pin(BB_SCL);
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << BB_SDA) | (1ULL << BB_SCL);
    io.mode         = GPIO_MODE_INPUT_OUTPUT_OD;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io);
    bb_idle();

    bb_start();
    bool ack = bb_write((uint8_t)(NFC_I2C_ADDR << 1));
    bb_stop();
    if (!ack) return false;
    if (!_stInit()) return false;
    if (!_stConfigNfcA()) return false;
    return true;
}

// 匹配成功: 蜂鸣 + 对实体列表逐个 HA turn_off (HTTP 放 detached worker)
void AppRfid::_bgTurnOffAll()
{
    audio::play_tone(4000, 0.08);
    vTaskDelay(pdMS_TO_TICKS(120));
    audio::play_tone(5200, 0.12);

    std::string host  = GetHAL()->getConfig("ha_host", "");
    std::string token = ha_weather::token();
    if (host.empty() || token.empty()) {
        mclog::tagWarn(TAG, "bg off: ha_host/ha_token 未配置 (设置 app 里填)");
        return;
    }
    std::string url   = ha_weather::baseUrl(host);
    auto entities = off_entity_list();  // 选择页勾选结果 (或默认五件套)
    if (entities.empty()) {
        mclog::tagWarn(TAG, "bg off: 实体清单为空 (选择页全不选)");
        return;
    }

    GetHAL()->tryRunDetached([url, token, entities]() {
        for (const auto& eid : entities) {
            std::string domain = eid.substr(0, eid.find('.'));
            std::string body   = "{\"entity_id\":\"" + eid + "\"}";
            auto resp = GetHAL()->httpPost(url + "/api/services/" + domain + "/turn_off",
                                           body,
                                           {{"Authorization", "Bearer " + token},
                                            {"Content-Type", "application/json"}});
            mclog::tagInfo(TAG, "bg off {} -> {}", eid, resp.ok ? "ok" : "FAILED");
        }
    });
}

void AppRfid::_bgLoop()
{
    vTaskDelay(pdMS_TO_TICKS(3000));  // 等系统启动稳定 (WiFi/GPIO 不急着用)

    bool hw_ready = false;
    bool present  = false;
    int  absent   = 0;
    uint8_t cur_uid[10] = {0};
    int cur_len = 0;
    int health_div = 0;  // 每 ~10min 打一次堆水位 (长期运行碎化排查用)

    for (;;) {
        if (++health_div >= 2000) {  // 2000 × ~300ms ≈ 10min
            health_div = 0;
            mclog::tagInfo(TAG, "bg health: heap={} min={} largest8bit={}",
                           heap_caps_get_free_size(MALLOC_CAP_8BIT),
                           heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
        // UI 打开期间完全停手 (引脚方向都被 UI 重配过)
        if (s_ui_owns.load()) {
            hw_ready = false;
            present  = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s_bg_reinit.exchange(false)) hw_ready = false;

        if (!hw_ready) {
            hw_ready = _bgHwInit();
            if (!hw_ready) {
                vTaskDelay(pdMS_TO_TICKS(2000));  // Unit 未插/上电晚, 慢速重试
                continue;
            }
            _reader_ok = true;  // 供 UI 打开时参考 (UI 会自行重查)
            mclog::tagInfo(TAG, "bg reader ready");
        }

        // 与 UI 轮询同一套路: WUPA → 选卡 → HLTA, 每 ~300ms 一轮
        bool found = false;
        uint8_t atqa[2] = {0};
        if (_stReqWupa(atqa, true)) {
            uint8_t uid[10] = {0};
            int ulen = 0;
            uint8_t sak = 0;
            if (_stSelectCard(uid, &ulen, &sak)) {
                found = true;
                absent = 0;
                _card_active = true;

                bool is_new = !present || ulen != cur_len || memcmp(uid, cur_uid, ulen) != 0;
                if (is_new) {
                    present = true;
                    memcpy(cur_uid, uid, ulen);
                    cur_len = ulen;

                    char hex[24] = {0};
                    int off = 0;
                    for (int i = 0; i < ulen; i++)
                        off += snprintf(hex + off, sizeof(hex) - off, "%02X", uid[i]);
                    mclog::tagInfo(TAG, "bg card: {}", hex);
                    audio::play_tone(4000, 0.08);  // 嘀

                    bool matched = false;
                    for (const auto& u : off_uid_list()) {
                        if (u == hex) { matched = true; break; }
                    }
                    if (matched) {
                        mclog::tagInfo(TAG, "off-card matched, turning off lights");
                        _bgTurnOffAll();
                    }
                }
                _stHaltA();
            }
        }
        if (!found && present && ++absent >= 2) {
            present = false;
            mclog::tagInfo(TAG, "bg card removed");
        }

        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

#endif  // !PLATFORM_BUILD_DESKTOP
