#include "app_lora_chat.h"
#include "../utils/theme/theme.h"
#include "../app_email_led/app_email_led.h"
#include <hal/hal.h>
#include <mooncake_log.h>

#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../app_voice_input/app_voice_input.h"
#else
#include <thread>
#include <chrono>
extern "C" const lv_font_t font_puhui_20_4;
#endif

static const char* TAG = "app-lora-chat";

// ── Font helper (same pattern as project_assistant) ───────────────────────
const lv_font_t* AppLoraChat::_font()
{
#ifndef PLATFORM_BUILD_DESKTOP
    extern const uint8_t font_puhui_common_30_4_bin_start[]
        asm("_binary_font_puhui_common_30_4_bin_start");
    static lv_font_t* f = nullptr;
    if (!f) f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
#else
    return &font_puhui_20_4;
#endif
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

AppLoraChat::AppLoraChat()
{
    setAppInfo().name = "LoRaChat";
}

void AppLoraChat::onCreate()
{
}

void AppLoraChat::onOpen()
{
    mclog::tagInfo(TAG, "open");

    // 防重入守卫 (同 AppSettings): mooncake 重开不触发 onClose, 不拆旧屏
    // 会整屏泄漏. 先做与 onClose 相同的 UI/外设收尾再重建.
    if (_scr) {
        if (GetHAL()->lvKbGroup && _input) lv_group_remove_obj(_input);
        GetHAL()->onEnterPressed = nullptr;
        _removeSwipeGesture();
        _uartDeinit();
        AppEmailLed::setPortAOwnedByApp(false);
        AppVoiceInput::cancelVoiceText();   // 防悬挂回调写已删 textarea
        AppVoiceInput::dismissMicButton();
        lv_obj_delete(_scr);
        _scr = nullptr;
        _status_lbl = _chat_scroll = _chat_panel = _input = _send_btn = nullptr;
        _mic_btn = nullptr;
        _input_row = _keyboard = nullptr;
    }

    _scr = lv_obj_create(nullptr);
    lv_obj_set_size(_scr, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG()), 0);
    lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(_scr);

    _buildUi();
    _installSwipeGesture();

    // 独占 PORT A (GPIO53 当 UART 用): 先让后台邮件通知器释放 RMT.
    AppEmailLed::setPortAOwnedByApp(true);
    _uartInit();

    // Enter key → send
    GetHAL()->onEnterPressed = [this]() { _sendMessage(); };
    if (GetHAL()->lvKbGroup && _input) {
        lv_group_add_obj(GetHAL()->lvKbGroup, _input);
    }
}

void AppLoraChat::onRunning()
{
    // 等待期诊断 (2026-11-08): 每 2s 刷新收发计数. RX 恒 0 = 模块没回任何数据
    // (没电/接线反/出厂固件不走透传); RX 在涨 = 模块在说话, 看首包诊断气泡.
    if (_uart_ok && !_rx_started.load()) {
        int32_t now_ms = lv_tick_get();
        if (now_ms - _last_diag_us > 2000) {
            _last_diag_us = now_ms;
            char buf[64];
            snprintf(buf, sizeof(buf), "等待 C6L… RX:%uB TX:%uB",
                     (unsigned)_rx_bytes.load(), (unsigned)_tx_bytes.load());
            _setStatus(buf, C_STATUS_NG());
        }
    }

    // Drain incoming LoRa messages from the UART RX queue
    std::queue<std::string> pending;
    {
        std::lock_guard<std::mutex> lk(_rx_mutex);
        std::swap(pending, _rx_queue);
    }
    while (!pending.empty()) {
        auto msg = std::move(pending.front());
        pending.pop();

        // Parse optional RSSI suffix: "hello world|RSSI:-80"
        std::string display = msg;
        auto sep = msg.rfind("|RSSI:");
        if (sep != std::string::npos) {
            display = msg.substr(0, sep);
            std::string rssi_str = "RSSI " + msg.substr(sep + 1);
            _setStatus(rssi_str, C_STATUS_OK());
        }
        if (!display.empty()) {
            _addBubble(false, display);
        }
    }
}

void AppLoraChat::onClose()
{
    mclog::tagInfo(TAG, "close");

    GetHAL()->onEnterPressed = nullptr;
    if (GetHAL()->lvKbGroup && _input) {
        lv_group_remove_obj(_input);
    }

    _removeSwipeGesture();
    _uartDeinit();

    // 把 PORT A 交还给后台邮件通知器.
    AppEmailLed::setPortAOwnedByApp(false);
    AppVoiceInput::cancelVoiceText();
    AppVoiceInput::dismissMicButton();

    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _status_lbl = _chat_scroll = _chat_panel = _input = _send_btn = nullptr;
    _mic_btn = nullptr;
    _input_row = _keyboard = nullptr;
}

// ── UI ────────────────────────────────────────────────────────────────────

void AppLoraChat::_buildUi()
{
    const int chat_h = SCREEN_H - HEADER_H - INPUT_ROW_H;

    // Header
    lv_obj_t* header = lv_obj_create(_scr);
    lv_obj_set_size(header, SCREEN_W, HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(C_HEADER()), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "LoRa Chat");
    lv_obj_set_style_text_font(title, _font(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme::cur().accent), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);

    _status_lbl = lv_label_create(header);
    lv_label_set_text(_status_lbl, "初始化中...");
    lv_obj_set_style_text_font(_status_lbl, _font(), 0);
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_STATUS_NG()), 0);
    lv_obj_align(_status_lbl, LV_ALIGN_RIGHT_MID, -24, 0);

    // Chat scroll area
    _chat_scroll = lv_obj_create(_scr);
    lv_obj_set_size(_chat_scroll, SCREEN_W, chat_h);
    lv_obj_align(_chat_scroll, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_bg_color(_chat_scroll, lv_color_hex(C_BG()), 0);
    lv_obj_set_style_border_width(_chat_scroll, 0, 0);
    lv_obj_set_style_pad_all(_chat_scroll, 0, 0);
    lv_obj_set_scroll_dir(_chat_scroll, LV_DIR_VER);
    lv_obj_clear_flag(_chat_scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);

    _chat_panel = lv_obj_create(_chat_scroll);
    lv_obj_set_width(_chat_panel, SCREEN_W);
    lv_obj_set_height(_chat_panel, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(_chat_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_chat_panel, 0, 0);
    lv_obj_set_style_pad_all(_chat_panel, 12, 0);
    lv_obj_set_style_pad_row(_chat_panel, 10, 0);
    lv_obj_clear_flag(_chat_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(_chat_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_chat_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_chat_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Input row
    lv_obj_t* input_row = lv_obj_create(_scr);
    _input_row = input_row;
    lv_obj_set_size(input_row, SCREEN_W, INPUT_ROW_H);
    lv_obj_align(input_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(input_row, lv_color_hex(C_INPUT_BG()), 0);
    lv_obj_set_style_border_width(input_row, 0, 0);
    lv_obj_set_style_pad_all(input_row, 8, 0);
    lv_obj_clear_flag(input_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(input_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Textarea
    _input = lv_textarea_create(input_row);
    lv_obj_set_size(_input, SCREEN_W - 180, 90);
    lv_textarea_set_placeholder_text(_input, "输入消息...");
    lv_textarea_set_one_line(_input, true);
    lv_obj_set_style_text_font(_input, _font(), 0);
    lv_obj_set_style_text_font(_input, _font(), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(_input, lv_color_hex(theme::cur().card[0]), 0);
    lv_obj_set_style_border_width(_input, 0, 0);
    lv_obj_set_style_radius(_input, 12, 0);
    lv_obj_set_style_text_color(_input, lv_color_hex(C_TEXT()), 0);
    lv_obj_set_style_pad_hor(_input, 16, 0);
    lv_obj_set_flex_grow(_input, 1);

    // 点输入框: 没接实体键盘才弹屏幕虚拟键盘 (接了实体键盘直接打字, 不挡屏).
    lv_obj_add_event_cb(_input, [](lv_event_t* e) {
        static_cast<AppLoraChat*>(lv_event_get_user_data(e))->_showKeyboard();
    }, LV_EVENT_CLICKED, this);

    // 语音输入按钮 (2026-11-08): 大麦克风模式, 识别文字回调填入输入框 ——
    // 中文输入的主路径 (LoRa 链路是透明字节流, UTF-8 中文直接过).
#ifndef PLATFORM_BUILD_DESKTOP
    _mic_btn = lv_button_create(input_row);
    lv_obj_set_size(_mic_btn, 130, 90);
    lv_obj_set_style_bg_color(_mic_btn, lv_color_hex(C_SEND()), 0);
    lv_obj_set_style_radius(_mic_btn, 12, 0);
    lv_obj_set_style_border_width(_mic_btn, 0, 0);
    lv_obj_set_style_margin_left(_mic_btn, 12, 0);
    lv_obj_add_event_cb(_mic_btn, _micBtn_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* mic_lbl = lv_label_create(_mic_btn);
    lv_label_set_text(mic_lbl, "语音");
    lv_obj_set_style_text_font(mic_lbl, _font(), 0);
    lv_obj_set_style_text_color(mic_lbl, lv_color_hex(C_TEXT()), 0);
    lv_obj_center(mic_lbl);
#endif

    // Send button
    _send_btn = lv_button_create(input_row);
    lv_obj_set_size(_send_btn, 140, 90);
    lv_obj_set_style_bg_color(_send_btn, lv_color_hex(C_SEND()), 0);
    lv_obj_set_style_radius(_send_btn, 12, 0);
    lv_obj_set_style_border_width(_send_btn, 0, 0);
    lv_obj_set_style_margin_left(_send_btn, 12, 0);
    lv_obj_add_event_cb(_send_btn, _sendBtn_cb, LV_EVENT_CLICKED, this);

    lv_obj_t* send_lbl = lv_label_create(_send_btn);
    lv_label_set_text(send_lbl, "发送");
    lv_obj_set_style_text_font(send_lbl, _font(), 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_hex(C_TEXT()), 0);
    lv_obj_center(send_lbl);

    // 屏幕虚拟键盘 (默认隐藏, 点输入框且无实体键盘时弹出). 绑定到 _input.
    _keyboard = lv_keyboard_create(_scr);
    lv_obj_set_size(_keyboard, SCREEN_W, KB_H);
    lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(_keyboard, _input);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    // 键盘自带 ✓ (READY) / ✗ (CANCEL) → 收起键盘 + 输入行复位.
    lv_obj_add_event_cb(_keyboard, [](lv_event_t* e) {
        static_cast<AppLoraChat*>(lv_event_get_user_data(e))->_hideKeyboard();
    }, LV_EVENT_READY, this);
    lv_obj_add_event_cb(_keyboard, [](lv_event_t* e) {
        static_cast<AppLoraChat*>(lv_event_get_user_data(e))->_hideKeyboard();
    }, LV_EVENT_CANCEL, this);

    // Hint
    _addBubble(false, "等待连接 C6L 模块...\n" \
               "接线: PORT.A → Unit C6L Grove\n" \
               "状态栏 RX 恒 0 = 模块没回数据\n" \
               "上拨退出");
}

void AppLoraChat::_addBubble(bool is_user, const std::string& text)
{
    if (!_chat_panel) return;

    lv_obj_t* row = lv_obj_create(_chat_panel);
    lv_obj_set_width(row, SCREEN_W - 24);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        is_user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, BUBBLE_MAX_W);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(is_user ? C_USER_BUB() : C_PEER_BUB()), 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_pad_all(bubble, 18, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, text.c_str());
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, BUBBLE_MAX_W - 36);
    lv_obj_set_style_text_font(lbl, _font(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT()), 0);

    lv_obj_update_layout(_chat_panel);
    lv_obj_scroll_to_y(_chat_scroll, LV_COORD_MAX, LV_ANIM_ON);
}

void AppLoraChat::_setStatus(const std::string& text, uint32_t color)
{
    if (!_status_lbl) return;
    lv_label_set_text(_status_lbl, text.c_str());
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(color), 0);
}

// 点输入框: 接了 USB 实体键盘就直接打字 (不弹屏幕键盘, 不挡屏); 没接才弹虚拟键盘
// 并把输入行 (输入框+发送键) 整体上移到键盘上方, 这样输入框可见、发送键也能点.
void AppLoraChat::_showKeyboard()
{
    if (!_keyboard || !_input_row) return;
    if (GetHAL()->usbKeyboardDetect()) return;  // 有实体键盘, 无需屏幕键盘
    lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_keyboard);
    lv_obj_align(_input_row, LV_ALIGN_BOTTOM_MID, 0, -KB_H);  // 抬到键盘正上方
}

void AppLoraChat::_hideKeyboard()
{
    if (!_keyboard || !_input_row) return;
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_input_row, LV_ALIGN_BOTTOM_MID, 0, 0);       // 复位到底部
}

void AppLoraChat::_sendMessage()
{
    if (!_input) return;
    const char* txt = lv_textarea_get_text(_input);
    if (!txt || txt[0] == '\0') return;

    std::string msg(txt);
    lv_textarea_set_text(_input, "");

    _addBubble(true, msg);

#ifndef PLATFORM_BUILD_DESKTOP
    if (_uart_ok) {
        std::string line = msg + "\n";
        // 排障仪表 (2026-11-08): C6L 侧曾收到与发送内容不符的 255B 周期垃圾,
        // 实际写上 UART 的字节逐一可见, 供两端逐字节对照定案
        char hex[3 * 96 + 1] = {0};
        int off = 0;
        for (size_t k = 0; k < line.size() && off < (int)sizeof(hex) - 4; k++)
            off += snprintf(hex + off, sizeof(hex) - off, "%02X ",
                            (unsigned char)line[k]);
        mclog::tagInfo(TAG, "tx {}B: {}", line.size(), hex);
        uart_write_bytes((uart_port_t)UART_PORT_NUM, line.c_str(), line.size());
        _tx_bytes += line.size();
    } else {
        _addBubble(false, "[C6L 未连接]");
    }
#else
    // Desktop sim: echo back for testing
    {
        std::lock_guard<std::mutex> lk(_rx_mutex);
        _rx_queue.push("[echo] " + msg);
    }
#endif
}

// ── UART ─────────────────────────────────────────────────────────────────

void AppLoraChat::_uartInit()
{
#ifndef PLATFORM_BUILD_DESKTOP
    mclog::tagInfo(TAG, "uart init: TX=%d RX=%d baud=%d", UART_TX_GPIO, UART_RX_GPIO, UART_BAUD);

    uart_config_t cfg = {};
    cfg.baud_rate  = UART_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    // 2026-11-08: 时钟源钉死 XTAL —— DEFAULT(PLL 派生)期间 C6L 曾收到字节被
    // ~32x 拉长的周期性垃圾 (9B 消息变 288B), 疑发送窗口时钟树被动; XTAL 是
    // 唯一不受任何调压/切频路径影响的基准.
    cfg.source_clk = UART_SCLK_XTAL;

    esp_err_t err;
    err = uart_driver_install((uart_port_t)UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, nullptr, 0);
    if (err != ESP_OK) {
        mclog::tagError(TAG, "uart_driver_install failed: %d", err);
        _setStatus("UART 安装失败", C_STATUS_NG());
        return;
    }
    err = uart_param_config((uart_port_t)UART_PORT_NUM, &cfg);
    if (err != ESP_OK) {
        mclog::tagError(TAG, "uart_param_config failed: %d", err);
        uart_driver_delete((uart_port_t)UART_PORT_NUM);
        _setStatus("UART 配置失败", C_STATUS_NG());
        return;
    }
    err = uart_set_pin((uart_port_t)UART_PORT_NUM, UART_TX_GPIO, UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        mclog::tagError(TAG, "uart_set_pin failed: %d", err);
        uart_driver_delete((uart_port_t)UART_PORT_NUM);
        _setStatus("UART 引脚失败", C_STATUS_NG());
        return;
    }

    _uart_ok = true;
    _task_running = true;
    xTaskCreate(_uartRxTask, "lora_rx", 3072, this, 5, nullptr);
    _setStatus("等待 C6L...", C_STATUS_NG());
    mclog::tagInfo(TAG, "uart ready");
#else
    _setStatus("模拟模式 (echo)", C_STATUS_OK());
#endif
}

void AppLoraChat::_uartDeinit()
{
#ifndef PLATFORM_BUILD_DESKTOP
    _task_running = false;
    _uart_ok = false;
    vTaskDelay(50 / portTICK_PERIOD_MS);
    uart_driver_delete((uart_port_t)UART_PORT_NUM);
    mclog::tagInfo(TAG, "uart deinit done");
#endif
}

void AppLoraChat::_uartRxTask(void* arg)
{
#ifndef PLATFORM_BUILD_DESKTOP
    auto* self = static_cast<AppLoraChat*>(arg);
    std::string line_buf;
    line_buf.reserve(256);
    uint8_t ch;

    // 首包快照: 前 24 字节十六进制回显到聊天区+日志 —— 不建立连接也能识别模块
    // 在说什么 (透传文本 / Meshtastic protobuf 0x94 0xC3 开头 / NMEA '$GP' /
    // 乱码=波特率不符). 出厂 Meshtastic 固件 Grove 口是 GPS 角色, 恒静默.
    uint8_t snap[24] = {0};
    int  snap_n      = 0;
    uint32_t snap_last_ms = 0;

    auto flush_snap = [&]() {
        if (snap_n <= 0) return;
        char hex[8 * 24 + 8] = {0};
        int off = 0;
        for (int i = 0; i < snap_n; i++)
            off += snprintf(hex + off, sizeof(hex) - off, "%02X ", snap[i]);
        mclog::tagInfo(TAG, "first rx {}B: {}", snap_n, hex);
        {
            std::lock_guard<std::mutex> lk(self->_rx_mutex);
            self->_rx_queue.push(std::string("[首包 ") + std::to_string(snap_n)
                                 + "B] " + hex);
        }
        self->_rx_started.store(true);
        snap_n = 0;
    };

    while (self->_task_running) {
        int n = uart_read_bytes((uart_port_t)UART_PORT_NUM, &ch, 1,
                                20 / portTICK_PERIOD_MS);
        if (n <= 0) {
            // 字节流断档 100ms 就把已收的快照先回显 (单字节 ping 也能看到)
            if (snap_n > 0 && lv_tick_elaps(snap_last_ms) > 100) flush_snap();
            continue;
        }

        self->_rx_bytes += n;
        if (!self->_rx_started.load()) {
            snap[snap_n++] = ch;
            snap_last_ms   = lv_tick_get();
            if (snap_n >= (int)sizeof(snap)) flush_snap();
        }

        if (ch == '\n' || ch == '\r') {
            if (!line_buf.empty()) {
                std::lock_guard<std::mutex> lk(self->_rx_mutex);
                self->_rx_queue.push(std::move(line_buf));
                line_buf.clear();
                line_buf.reserve(256);
            }
        } else if (line_buf.size() < 512) {
            line_buf += (char)ch;
        }
    }
    vTaskDelete(nullptr);
#endif
}

// ── Swipe-up gesture ─────────────────────────────────────────────────────

static lv_point_t s_gesture_press{0, 0};   // PRESSED 起点 (底部起手判定)

void AppLoraChat::_gesture_cb(lv_event_t* e)
{
    lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(dev, &s_gesture_press);
        return;
    }
    if (code != LV_EVENT_GESTURE) return;
    if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
        auto* self = static_cast<AppLoraChat*>(lv_event_get_user_data(e));
        // 双守卫 (2026-10-27 "所有 app 上拨都退工具页" 实锤): indev 级手势
        // 广播给所有已注册的回调 —— ① 只在本 app 前台; ② 只认屏幕最下缘
        // (y>650) 起手的上拨, 其余位置归滚动.
        if (!self || !self->_scr || lv_screen_active() != self->_scr) return;
        if (s_gesture_press.y <= 650) return;
        // 退出走 _close_cb → openApp(工具页), mooncake 不触发 onClose, 所以
        // 在这里收尾: 卸载 UART 释放 GPIO53, 再把 PORT A 交还后台邮件通知器
        // (否则 setPortAOwnedByApp(true) 永不复位, worker 永久让位).
        self->_uartDeinit();
        AppEmailLed::setPortAOwnedByApp(false);
        AppVoiceInput::cancelVoiceText();
        AppVoiceInput::dismissMicButton();
        if (self->_close_cb) self->_close_cb();
    }
}

void AppLoraChat::_installSwipeGesture()
{
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, _gesture_cb, LV_EVENT_ALL, this);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppLoraChat::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, _gesture_cb, this);
        _gesture_indev = nullptr;
    }
}

// ── Static callbacks ──────────────────────────────────────────────────────

void AppLoraChat::_sendBtn_cb(lv_event_t* e)
{
    static_cast<AppLoraChat*>(lv_event_get_user_data(e))->_sendMessage();
}

// 语音输入 (2026-11-08): 大麦克风模式 + 回调交付. 用 requestVoiceText 而非
// requestMicInput(textarea) —— LoRa Chat 退出时会拆 UI, 回调在交付时校验
// 屏幕归属, 退出路径再 cancelVoiceText, 杜绝向已删 textarea 写入.
// STT 结果尾部常带非 UTF-8 脏字节 (实测 '你好\x80\x82'), 清洗后再入框 ——
// 脏字节随消息上 UART 是 C6L 侧乱码的变量之一, 必须排除.
static std::string lora_utf8_sanitize(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        int len;
        if ((c & 0x80) == 0x00) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else { i++; continue; }                  // 非法起始字节: 丢
        if (i + (size_t)len > in.size()) break;  // 截断尾序列: 丢
        bool ok = true;
        for (int k = 1; k < len; k++) {
            if (((unsigned char)in[i + k] & 0xC0) != 0x80) { ok = false; break; }
        }
        // 过短编码 (C0/C1) 与 UTF-16 代理区不是合法 UTF-8
        if (ok && len == 2 && c < 0xC2) ok = false;
        if (ok && len == 3) {
            uint32_t cp = ((uint32_t)c & 0x0F) << 12
                        | ((uint32_t)(unsigned char)in[i + 1] & 0x3F) << 6
                        | ((uint32_t)(unsigned char)in[i + 2] & 0x3F);
            if (cp >= 0xD800 && cp <= 0xDFFF) ok = false;
        }
        if (!ok) { i++; continue; }
        out.append(in, i, (size_t)len);
        i += (size_t)len;
    }
    return out;
}

void AppLoraChat::_micBtn_cb(lv_event_t* e)
{
#ifndef PLATFORM_BUILD_DESKTOP
    auto* self = static_cast<AppLoraChat*>(lv_event_get_user_data(e));
    AppVoiceInput::requestVoiceText([self](const std::string& text) {
        if (!text.empty() && self && self->_input && self->_scr
            && lv_screen_active() == self->_scr) {
            std::string clean = lora_utf8_sanitize(text);
            if (!clean.empty()) lv_textarea_add_text(self->_input, clean.c_str());
        }
    });
#endif
}
