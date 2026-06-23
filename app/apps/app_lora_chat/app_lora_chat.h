#pragma once
#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <queue>
#include <mutex>
#include <string>
#include <atomic>

class AppLoraChat : public mooncake::AppAbility {
public:
    AppLoraChat();
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    // ── Layout constants ──────────────────────────────────────────────────
    static constexpr int SCREEN_W    = 1280;
    static constexpr int SCREEN_H    = 720;
    static constexpr int HEADER_H    = 72;
    static constexpr int INPUT_ROW_H = 120;
    static constexpr int BUBBLE_MAX_W = 800;
    static constexpr int KB_H = 340;   // 屏幕虚拟键盘高度

    // ── Palette ───────────────────────────────────────────────────────────
    static constexpr uint32_t C_BG        = 0x081522;
    static constexpr uint32_t C_HEADER    = 0x0D1F35;
    static constexpr uint32_t C_USER_BUB  = 0x1A4A7A;
    static constexpr uint32_t C_PEER_BUB  = 0x132A43;
    static constexpr uint32_t C_INPUT_BG  = 0x102033;
    static constexpr uint32_t C_ACCENT    = 0x4FA3FF;
    static constexpr uint32_t C_TEXT      = 0xEAF3FB;
    static constexpr uint32_t C_SEND      = 0x1A6B3A;
    static constexpr uint32_t C_STATUS_OK = 0x2ECC71;
    static constexpr uint32_t C_STATUS_NG = 0xF39C12;

    // ── UART (PORT.A: GPIO53=RX, GPIO54=TX) ──────────────────────────────
    static constexpr int UART_PORT_NUM = 2;   // UART_NUM_2
    static constexpr int UART_RX_GPIO  = 53;
    static constexpr int UART_TX_GPIO  = 54;
    static constexpr int UART_BAUD     = 115200;
    static constexpr int UART_BUF_SIZE = 512;

    std::function<void()> _close_cb;

    // UI
    lv_obj_t*   _scr         = nullptr;
    lv_obj_t*   _status_lbl  = nullptr;
    lv_obj_t*   _chat_scroll = nullptr;
    lv_obj_t*   _chat_panel  = nullptr;
    lv_obj_t*   _input       = nullptr;
    lv_obj_t*   _input_row   = nullptr;   // 输入框+发送键所在行 (弹键盘时整体上移)
    lv_obj_t*   _send_btn    = nullptr;
    lv_obj_t*   _keyboard    = nullptr;   // 屏幕虚拟键盘 (无实体键盘时弹出)
    lv_indev_t* _gesture_indev = nullptr;

    // UART / RX queue
    std::queue<std::string> _rx_queue;
    std::mutex              _rx_mutex;
    std::atomic<bool>       _uart_ok{false};
    std::atomic<bool>       _task_running{false};

    // helpers
    void _buildUi();
    void _addBubble(bool is_user, const std::string& text);
    void _sendMessage();
    void _setStatus(const std::string& text, uint32_t color);
    void _showKeyboard();   // 无实体键盘时: 弹虚拟键盘 + 输入行上移
    void _hideKeyboard();   // 收起键盘 + 输入行复位

    void _uartInit();
    void _uartDeinit();
    static void _uartRxTask(void* arg);

    void _installSwipeGesture();
    void _removeSwipeGesture();
    static void _gesture_cb(lv_event_t* e);
    static void _sendBtn_cb(lv_event_t* e);

    static const lv_font_t* _font();
};
