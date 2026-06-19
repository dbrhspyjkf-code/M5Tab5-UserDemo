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

    // Input-row layout constants (shared by _buildUi / _showKeyboard /
    // _hideKeyboard so the keyboard button can be re-positioned from
    // anywhere — the local `constexpr` it used to live in _buildUi was
    // out of scope in the helpers, hence the linker errors).
    //
    //   no keyboard:  [8][input W ][20][kb=80][32][send=164][8]  → W = 968
    //   keyboard up:  [8][input W'][20][mic=80][20][kb=80][32][send=164][8]  → W' = 868
    static constexpr int SIDE_GAP      = 8;
    static constexpr int BTN_GAP       = 20;
    static constexpr int SEND_GAP      = 32;  // wider gap so Send isn't accidentally hit
    static constexpr int KB_W          = 80;
    static constexpr int MIC_W         = 80;
    static constexpr int SEND_W        = 164;
    static constexpr int INPUT_W       = SCREEN_W - 2*SIDE_GAP - KB_W - SEND_W
                                                 - BTN_GAP - SEND_GAP;  // 968
    static constexpr int INPUT_W_MIC   = INPUT_W - MIC_W - BTN_GAP;       // 868

    // Visual centre bias for the keyboard button. The Send button (164 px) is
    // much wider than the kb button (80 px), so a geometrically-centred kb
    // (gaps of 26/26 between kb and its neighbours) looks pulled right of
    // centre next to the broad Send. Shifting the kb left by this many px
    // makes the visual gap-to-send bigger than the gap-to-input/voice, which
    // reads as more balanced. Both _buildUi (initial) and _showKeyboard /
    // _hideKeyboard use the same bias.
    static constexpr int KB_LEFT_BIAS  = 12;

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

    // Swipe-up-to-exit gesture (mirrors app_xiaozhi / app_ha). Pointer-indev
    // listener so the gesture fires regardless of which LVGL object the user
    // touches (chat area, header, etc.). The back button in the header was
    // removed in favour of this gesture.
    lv_indev_t* _gesture_indev = nullptr;
    void _addSwipeGesture();
    void _removeSwipeGesture();

    static std::string _bridgeUrl(const std::string& path);
    static std::string _stripMarkdown(const std::string& s);

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
