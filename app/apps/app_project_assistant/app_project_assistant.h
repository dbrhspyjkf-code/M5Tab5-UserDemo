#pragma once

#include <atomic>
#include <functional>
#include <lvgl.h>
#include <mooncake.h>
#include <mutex>
#include <string>
#include <vector>

class AppProjectAssistant : public mooncake::AppAbility {
public:
    AppProjectAssistant();

    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::function<void()> _close_cb;

    // ── UI ──────────────────────────────────────────────────────────────────
    lv_obj_t* _scr         = nullptr;
    lv_obj_t* _chat_scroll = nullptr;  // scrollable viewport
    lv_obj_t* _chat_panel  = nullptr;  // flex-column container inside scroll
    lv_obj_t* _status_lbl  = nullptr;  // tool progress line (below scroll)
    lv_obj_t* _input       = nullptr;  // textarea
    lv_obj_t* _input_row   = nullptr;  // parent container that holds input + buttons
    lv_obj_t* _send_btn    = nullptr;
    lv_obj_t* _voice_btn   = nullptr;  // shown only while keyboard is up
    lv_obj_t* _keyboard    = nullptr;
    lv_obj_t* _keyboard_btn = nullptr;  // toggles the on-screen keyboard

    uint32_t _last_input_click_ms = 0;

    // ── State machine (written by bg thread, read by onRunning) ──────────────
    enum class UpdateKind { None, Progress, Done, Error };
    struct PendingUpdate {
        UpdateKind kind   = UpdateKind::None;
        std::string text;                // Done/Error: final text
        std::vector<std::string> tools;  // Progress: active tool names
        std::string partial;             // Progress: partial reply so far
    };

    std::mutex           _upd_mu;
    PendingUpdate        _pending{};
    std::atomic<bool>    _dirty{false};
    std::atomic<bool>    _inflight{false};
    std::atomic<bool>    _closing{false};
    // True once we've started the xiaozhi task and it has finished its
    // background init (audio service up, MQTT/UDP channel openable). Until
    // this is true, voice input is disabled and the status line shows a
    // "starting" message.
    std::atomic<bool>    _voice_ready{false};
    std::atomic<bool>    _voice_starting{false};
    int64_t              _voice_start_ms = 0;

    // UI height constants (set in _buildUi based on keyboard state)
    static constexpr int HEADER_H      = 64;
    static constexpr int STATUS_H      = 40;
    static constexpr int INPUT_ROW_H   = 104;
    static constexpr int KEYBOARD_H    = 280;
    static constexpr int SCREEN_H      = 720;
    static constexpr int SCREEN_W      = 1280;
    static constexpr int BUBBLE_MAX_W  = 880;

    // ── Methods ──────────────────────────────────────────────────────────────
    void _buildUi();
    void _showKeyboard();
    void _hideKeyboard();
    void _addBubble(bool is_user, const std::string& text);
    void _setStatus(const std::string& text);
    void _sendMessage();
    void _startChat(const std::string& text);

    // Runs on background thread: POST then poll until done
    void _runChatRequest(const std::string& text);

    void _setPending(UpdateKind kind, const std::string& text,
                     std::vector<std::string> tools = {}, const std::string& partial = "");

    static std::string _bridgeUrl(const std::string& path);
    static std::string _stripMarkdown(const std::string& s);

    static void _back_cb(lv_event_t* e);
    static void _send_cb(lv_event_t* e);
    static void _textarea_cb(lv_event_t* e);
    static void _keyboard_cb(lv_event_t* e);
    static void _keyboard_btn_cb(lv_event_t* e);
    static void _voice_btn_cb(lv_event_t* e);
};
