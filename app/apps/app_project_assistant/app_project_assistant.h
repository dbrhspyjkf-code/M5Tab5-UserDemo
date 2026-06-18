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
    lv_obj_t* _scr          = nullptr;
    lv_obj_t* _chat_scroll  = nullptr;  // scrollable viewport
    lv_obj_t* _chat_panel   = nullptr;  // flex-column container inside scroll
    lv_obj_t* _status_lbl   = nullptr;  // tool progress line (below scroll)
    lv_obj_t* _input        = nullptr;  // textarea
    lv_obj_t* _input_row    = nullptr;  // parent container that holds input + buttons
    lv_obj_t* _send_btn     = nullptr;
    lv_obj_t* _voice_btn    = nullptr;  // shown only while keyboard is up
    lv_obj_t* _keyboard     = nullptr;
    lv_obj_t* _keyboard_btn = nullptr;  // toggles the on-screen keyboard
    lv_obj_t* _clear_btn    = nullptr;  // clear chat history (header right)

    // In-flight streaming bubble: the label inside the currently-building
    // Claude reply. Non-null while a request is in-progress and at least one
    // partial chunk has arrived. Owned by _chat_panel (auto-deleted with screen).
    lv_obj_t* _streaming_lbl = nullptr;

    uint32_t _last_input_click_ms = 0;

    // ── Chat history ─────────────────────────────────────────────────────────
    struct ChatMsg { bool is_user; std::string text; };
    std::vector<ChatMsg> _history;  // persisted across open/close; saved to SPIFFS

    // ── State machine (written by bg thread, read by onRunning) ──────────────
    enum class UpdateKind { None, Progress, Done, Error, PermissionRequired };
    struct PendingUpdate {
        UpdateKind kind   = UpdateKind::None;
        std::string text;                // Done/Error: final text
        std::vector<std::string> tools;  // Progress: active tool names
        std::string partial;             // Progress: partial reply so far
        // PermissionRequired
        std::string perm_req_id;
        std::string perm_tool_name;
        std::string perm_tool_input;
    };

    std::mutex           _upd_mu;
    PendingUpdate        _pending{};
    std::atomic<bool>    _dirty{false};
    std::atomic<bool>    _inflight{false};
    std::atomic<bool>    _closing{false};
    std::atomic<bool>    _voice_ready{false};
    std::atomic<bool>    _voice_starting{false};
    int64_t              _voice_start_ms = 0;

    // Permission request state (set from poll thread, cleared on approve/deny)
    std::string          _perm_req_id;
    lv_obj_t*            _perm_overlay = nullptr;
    std::atomic<bool>    _perm_action_pending{false};
    std::atomic<bool>    _perm_approved{false};

    // UI height constants
    static constexpr int HEADER_H      = 64;
    static constexpr int STATUS_H      = 40;
    static constexpr int INPUT_ROW_H   = 104;
    static constexpr int KEYBOARD_H    = 280;
    static constexpr int SCREEN_H      = 720;
    static constexpr int SCREEN_W      = 1280;
    static constexpr int BUBBLE_MAX_W  = 880;
    static constexpr int MAX_HISTORY   = 50;

    // ── Methods ──────────────────────────────────────────────────────────────
    void _buildUi();
    void _showKeyboard();
    void _hideKeyboard();

    // Chat bubbles
    void _addBubble(bool is_user, const std::string& text, bool save = true);
    void _addStreamingBubble();
    void _updateStreamingBubble(const std::string& text);
    void _finalizeStreaming(const std::string& text);
    void _clearStreamingBubble();

    void _setStatus(const std::string& text);
    void _clearChat();

    // History persistence (SPIFFS, no-op on desktop)
    void _saveHistory();
    void _loadHistory();

    // Network
    void _sendMessage();
    void _startChat(const std::string& text);
    void _runChatRequest(const std::string& text);

    void _setPending(UpdateKind kind, const std::string& text,
                     std::vector<std::string> tools = {}, const std::string& partial = "");
    void _setPendingPerm(const std::string& req_id,
                         const std::string& tool_name,
                         const std::string& tool_input);

    // Permission approve/deny card
    void _showPermissionCard(const std::string& tool_name, const std::string& tool_input);
    void _hidePermissionCard();

    static std::string _bridgeUrl(const std::string& path);
    static std::string _stripMarkdown(const std::string& s);

    static void _back_cb(lv_event_t* e);
    static void _send_cb(lv_event_t* e);
    static void _clear_cb(lv_event_t* e);
    static void _approve_cb(lv_event_t* e);
    static void _deny_cb(lv_event_t* e);
    static void _scr_click_cb(lv_event_t* e);
    static void _textarea_cb(lv_event_t* e);
    static void _keyboard_cb(lv_event_t* e);
    static void _keyboard_btn_cb(lv_event_t* e);
    static void _voice_btn_cb(lv_event_t* e);
};
