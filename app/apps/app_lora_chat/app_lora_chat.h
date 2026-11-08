#pragma once
#include "../utils/theme/theme.h"
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
    // 配色: 实时读全局主题 (utils/theme/theme.h)
    static uint32_t C_BG()        { return 0x000000; }
    static uint32_t C_HEADER()    { return theme::cur().card[1]; }
    static uint32_t C_USER_BUB()  { return theme::cur().card[0]; }  // 自己气泡: 强调一档
    static uint32_t C_PEER_BUB()  { return theme::cur().card[2]; }
    static uint32_t C_INPUT_BG()  { return theme::cur().card[2]; }
    static uint32_t C_TEXT()      { return theme::cur().text_hi; }
    static uint32_t C_SEND()      { return theme::cur().press; }
    static uint32_t C_STATUS_OK() { return theme::cur().ok; }
    static uint32_t C_STATUS_NG() { return theme::cur().warn; }

    // ── UART (PORT.A: GPIO53=RX, GPIO54=TX) ──────────────────────────────
    static constexpr int UART_PORT_NUM = 2;   // UART_NUM_2
    static constexpr int UART_RX_GPIO  = 53;
    static constexpr int UART_TX_GPIO  = 54;
    // 2026-11-08 115200→9600: 共用线 (Hub 上与 NFC I2C 同两根) 曾间歇出现
    // 22ms 周期波形把 9B 消息打成 288B 垃圾 (与内容无关, XTAL 钉死仍复现,
    // 判定台面级 EMI/电气瞬态). 9600 每位 104µs=12 倍位宽, 瞬态只占位零头;
    // LoRa 聊天带宽毫无损失 (SF12 空口一条 3-6s 才是瓶颈). C6L 侧同步改.
    static constexpr int UART_BAUD     = 9600;
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
    lv_obj_t*   _mic_btn     = nullptr;   // 语音输入按钮 (设备端)
    lv_obj_t*   _keyboard    = nullptr;   // 屏幕虚拟键盘 (无实体键盘时弹出)
    lv_indev_t* _gesture_indev = nullptr;

    // UART / RX queue
    std::queue<std::string> _rx_queue;
    std::mutex              _rx_mutex;
    std::atomic<bool>       _uart_ok{false};
    std::atomic<bool>       _task_running{false};

    // 收发诊断 (2026-11-08): "等待 C6L" 卡住时区分「模块没电/接线错」vs
    // 「模块在说话但不是透传文本」(出厂 Meshtastic 固件 Grove 口是 GPS 角色不回数据).
    std::atomic<uint32_t>   _rx_bytes{0};      // UART 累计收字节数
    std::atomic<uint32_t>   _tx_bytes{0};      // UART 累计发字节数
    std::atomic<bool>       _rx_started{false}; // 是否收到过首包 (诊断气泡只弹一次)
    int64_t                 _last_diag_us = 0; // 等待期计数刷新节流

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
    static void _micBtn_cb(lv_event_t* e);

    static const lv_font_t* _font();
};
