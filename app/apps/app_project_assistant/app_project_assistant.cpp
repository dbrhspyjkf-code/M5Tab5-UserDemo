#include "app_project_assistant.h"

#include <hal/hal.h>
#include <mooncake_log.h>
#include <nlohmann/json.hpp>
#include "../app_voice_input/app_voice_input.h"
#include "xiaozhi_ctl.h"
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <thread>
#include <chrono>
#endif
#include <esp_timer.h>
#include <regex>  // (kept for callers; _stripMarkdown no longer uses it)
using json = nlohmann::json;

static const std::string TAG = "app-claude";
LV_IMAGE_DECLARE(key_icon);
#include "mic_icon.h"
LV_IMAGE_DECLARE(claude_logo);

namespace {

// ── Palette ────────────────────────────────────────────────────────────────
constexpr uint32_t C_BG       = 0x081522;
constexpr uint32_t C_HEADER   = 0x0D1F35;
constexpr uint32_t C_USER_BUB = 0x1A4A7A;  // user bubble (right)
constexpr uint32_t C_BOT_BUB  = 0x132A43;  // Claude bubble (left)
constexpr uint32_t C_STATUS   = 0x0D1F35;
constexpr uint32_t C_INPUT_BG = 0x102033;
constexpr uint32_t C_ACCENT   = 0x4FA3FF;
constexpr uint32_t C_TEXT     = 0xEAF3FB;
constexpr uint32_t C_MUTED    = 0x6B8DAD;
constexpr uint32_t C_SEND     = 0x1A6B3A;

constexpr uint32_t DOUBLE_CLICK_MS = 350;

// ── Chat font ──────────────────────────────────────────────────────────────
#ifndef PLATFORM_BUILD_DESKTOP
// font_puhui_common_30_4 is the same font xiaozhi uses (loaded as a cbin
// binary blob, embedded by xiaozhi_core's CMakeLists). It has full GB2312
// common ~3500 glyph coverage — unlike font_puhui_basic_30_4 which is a
// subset and renders most chars as tofu. The cbin data lives in flash and
// is XIP'd at render time, so there's no big RAM copy (only a small
// lv_font_t header in heap).
//
// We declare the binary's start symbol here (linked from xiaozhi_core's
// EMBED_FILES) and lazy-init one cbin_font per process. Safe to call
// multiple times — the cbin data is shared; only the lv_font_t header is
// per-instance.
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");

static const lv_font_t* zh_font()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
#else
// Desktop sim: the tab5 puhui 20px C-array font is linked in (see desktop
// CMakeLists APP_LAYER_SRCS) and has full CJK coverage, so Chinese renders
// instead of tofu boxes.
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font() { return &font_puhui_20_4; }
#endif

const lv_font_t* chat_font() { return zh_font(); }
const lv_font_t* input_font() { return zh_font(); }

}  // namespace

// ── Lifecycle ─────────────────────────────────────────────────────────────

AppProjectAssistant::AppProjectAssistant()
{
    setAppInfo().name = "Claude";
}

void AppProjectAssistant::onCreate() {
    _loadHistory();
}

void AppProjectAssistant::onOpen()
{
    mclog::tagInfo(TAG, "open");
    _closing = false;
    _buildUi();
    // Replay persisted history into the freshly-built chat panel
    for (auto& msg : _history) {
        _addBubble(msg.is_user, msg.text, false);
    }
    lv_screen_load(_scr);

    // Physical-keyboard Enter → send (bypasses the LVGL key-event
    // pipeline so the textarea class default doesn't add a '\n' first).
    // Cleared in onClose.
    if (GetHAL()) {
        GetHAL()->onEnterPressed = [this]() { _sendMessage(); };
    }

    // If xiaozhi has never been started (user has never opened the 小智 app),
    // bring it up in the background so voice input works on the first try.
    // Don't block here — poll readiness in onRunning() and show a "starting"
    // status line until MQTT/UDP is openable.
    if (!xiaozhi_is_initialized()) {
        mclog::tagInfo(TAG, "xiaozhi not initialized — starting in background");
        xiaozhi_start_task();
        _voice_starting.store(true);
        _voice_start_ms = esp_timer_get_time() / 1000;
        _setStatus("Starting voice service...");
    } else {
        mclog::tagInfo(TAG, "xiaozhi already initialized");
        _voice_ready.store(true);
    }
}

void AppProjectAssistant::onRunning()
{
    // Poll xiaozhi startup so the status line clears when the voice service
    // is ready. We don't block onOpen — the audio service init + AFE model
    // load + MQTT connect takes 5-30 s and would freeze the UI.
    if (_voice_starting.load() && !_voice_ready.load()) {
        if (xiaozhi_is_initialized()) {
            mclog::tagInfo(TAG, "xiaozhi ready after %lld ms",
                           (long long)((esp_timer_get_time() / 1000) - _voice_start_ms));
            _voice_ready.store(true);
            _voice_starting.store(false);
            _setStatus("");
        } else {
            int64_t elapsed = (esp_timer_get_time() / 1000) - _voice_start_ms;
            // Refresh the status every ~5 s so the user sees it's alive
            static int64_t s_last_status_ms = 0;
            if (elapsed - s_last_status_ms > 5000) {
                s_last_status_ms = elapsed;
                char buf[64];
                snprintf(buf, sizeof(buf), "Starting voice service... (%llds)",
                         (long long)(elapsed / 1000));
                _setStatus(buf);
            }
        }
    }

    if (!_scr || !_dirty.exchange(false)) return;

    PendingUpdate upd;
    {
        std::lock_guard<std::mutex> lk(_upd_mu);
        upd = _pending;
        _pending.kind = UpdateKind::None;
    }

    switch (upd.kind) {
    case UpdateKind::Progress: {
        // Show partial reply growing in a streaming bubble
        if (!upd.partial.empty()) {
            if (!_streaming_lbl) _addStreamingBubble();
            _updateStreamingBubble(_stripMarkdown(upd.partial));
        }
        // Status bar: tool names (may be Chinese) or generic "Thinking..."
        std::string s;
        if (!upd.tools.empty()) {
            s = upd.tools[0];
            if (upd.tools.size() > 1) s += " +" + std::to_string(upd.tools.size() - 1);
        } else if (upd.partial.empty()) {
            s = "Thinking...";
        }
        if (!s.empty()) _setStatus(s);
        break;
    }
    case UpdateKind::Done:
        _setStatus("");
        _inflight = false;
        _finalizeStreaming(_stripMarkdown(upd.text));
        break;
    case UpdateKind::Error:
        _setStatus("");
        _inflight = false;
        _clearStreamingBubble();
        _addBubble(false, "⚠ " + upd.text);
        break;
    case UpdateKind::PermissionRequired:
        if (!_perm_overlay) {
            _perm_req_id = upd.perm_req_id;
            _showPermissionCard(upd.perm_tool_name, upd.perm_tool_input);
        }
        break;
    default:
        break;
    }
}

void AppProjectAssistant::onClose()
{
    mclog::tagInfo(TAG, "close");
    _closing = true;
    _saveHistory();
    // Detach the swipe-up listener before the screen is torn down.
    _removeSwipeGesture();
    // Release the physical-keyboard "Enter pressed" hook so subsequent
    // apps don't accidentally trigger _sendMessage on their own key events.
    if (GetHAL()) GetHAL()->onEnterPressed = nullptr;
    // The voice-input big mic lives on lv_layer_top(), not on _scr, so it
    // would survive the screen delete. Tear it down explicitly.
    AppVoiceInput::dismissMicButton();
    // Remove textarea from the physical keyboard group before deleting the screen.
    // (LVGL 9 also removes objects automatically on deletion, but being explicit
    // prevents a brief window where the dangling pointer could be dereferenced.)
    if (GetHAL()->lvKbGroup && _input) {
        lv_group_remove_obj(_input);
    }
    // _streaming_lbl and _perm_overlay live inside _scr; nullify before delete.
    _streaming_lbl = nullptr;
    _perm_overlay = nullptr;
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _chat_scroll = _chat_panel = _status_lbl = nullptr;
    _input = _input_row = _send_btn = _voice_btn = _keyboard = _keyboard_btn = _clear_btn = nullptr;
    if (_close_cb) _close_cb();
}

// ── UI build ──────────────────────────────────────────────────────────────

void AppProjectAssistant::_buildUi()
{
    // Root screen
    _scr = lv_obj_create(NULL);
    lv_obj_set_size(_scr, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(_scr, 0, 0);
    lv_obj_set_style_pad_all(_scr, 0, 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header ──────────────────────────────────────────────────────────────
    lv_obj_t* hdr = lv_obj_create(_scr);
    lv_obj_set_size(hdr, SCREEN_W, HEADER_H);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    // Back button removed — exit via swipe-up gesture (see _addSwipeGesture).
    // The header now hosts only the title + Claude logo.

    lv_obj_t* title = lv_label_create(hdr);
    lv_label_set_text(title, "Claude");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    // Claude logo to the left of the title text
    lv_obj_t* logo = lv_image_create(hdr);
    lv_image_set_src(logo, &claude_logo);
    lv_obj_align_to(logo, title, LV_ALIGN_OUT_LEFT_MID, -12, 0);

    // Clear chat history button (trash icon, top-right corner)
    _clear_btn = lv_btn_create(hdr);
    lv_obj_set_size(_clear_btn, 72, 48);
    lv_obj_align(_clear_btn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(_clear_btn, lv_color_hex(0x2A1515), 0);
    lv_obj_set_style_bg_color(_clear_btn, lv_color_hex(0x1A0808), LV_STATE_PRESSED);
    lv_obj_set_style_radius(_clear_btn, 10, 0);
    lv_obj_set_style_border_width(_clear_btn, 0, 0);
    lv_obj_add_event_cb(_clear_btn, _clear_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* clear_icon = lv_label_create(_clear_btn);
    lv_label_set_text(clear_icon, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_font(clear_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(clear_icon, lv_color_hex(0xFF6B6B), 0);
    lv_obj_center(clear_icon);

    // ── Chat scroll area ─────────────────────────────────────────────────────
    int chat_h = SCREEN_H - HEADER_H - STATUS_H - INPUT_ROW_H;

    _chat_scroll = lv_obj_create(_scr);
    lv_obj_set_size(_chat_scroll, SCREEN_W, chat_h);
    lv_obj_align(_chat_scroll, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_bg_color(_chat_scroll, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(_chat_scroll, 0, 0);
    lv_obj_set_style_pad_all(_chat_scroll, 0, 0);
    lv_obj_set_scroll_dir(_chat_scroll, LV_DIR_VER);
    lv_obj_clear_flag(_chat_scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);

    // Flex-column panel inside scroll
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
    lv_obj_set_flex_align(_chat_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // ── Status bar ───────────────────────────────────────────────────────────
    lv_obj_t* status_row = lv_obj_create(_scr);
    lv_obj_set_size(status_row, SCREEN_W, STATUS_H);
    lv_obj_align(status_row, LV_ALIGN_TOP_MID, 0, HEADER_H + chat_h);
    lv_obj_set_style_bg_color(status_row, lv_color_hex(C_STATUS), 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_pad_hor(status_row, 20, 0);
    lv_obj_set_style_pad_ver(status_row, 0, 0);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);

    _status_lbl = lv_label_create(status_row);
    lv_label_set_text(_status_lbl, "");
    lv_label_set_long_mode(_status_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_status_lbl, SCREEN_W - 40);
    lv_obj_set_style_text_font(_status_lbl, zh_font(), 0);  // supports Chinese tool names
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_MUTED), 0);
    lv_obj_align(_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    // ── Input row ────────────────────────────────────────────────────────────
    _input_row = lv_obj_create(_scr);
    lv_obj_set_size(_input_row, SCREEN_W, INPUT_ROW_H);
    lv_obj_align(_input_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_input_row, lv_color_hex(C_INPUT_BG), 0);
    lv_obj_set_style_border_width(_input_row, 0, 0);
    lv_obj_set_style_pad_all(_input_row, 8, 0);
    lv_obj_clear_flag(_input_row, LV_OBJ_FLAG_SCROLLABLE);

    // Layout (no keyboard): [8][input W][20][kb=80][32][send=164][8] = 1280 → W=968
    // Layout (keyboard up): [8][input W'][20][mic=80][20][kb=80][32][send=164][8] = 1280 → W'=868
    // The wider gap before Send reduces mis-taps. The 🎤 button only appears
    // when the keyboard is up (voice input is gated through the keyboard UI).
    // All gaps/widths are class constants (see header) so the kb button can be
    // re-positioned from _showKeyboard / _hideKeyboard too.

    _input = lv_textarea_create(_input_row);
    lv_obj_set_size(_input, INPUT_W, 80);
    lv_obj_align(_input, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_placeholder_text(_input, "Tap to type or voice...");
    lv_textarea_set_one_line(_input, false);
    lv_obj_set_style_text_font(_input, input_font(), 0);
    // Textarea placeholder uses its own part — also set the font there so the
    // placeholder text isn't rendered with a default font.
    lv_obj_set_style_text_font(_input, input_font(), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(_input, lv_color_hex(0x1A3A5C), 0);
    lv_obj_set_style_border_width(_input, 0, 0);
    lv_obj_set_style_radius(_input, 12, 0);
    lv_obj_add_event_cb(_input, _textarea_cb, LV_EVENT_ALL, this);

    // Voice button — only visible while the on-screen keyboard is up. Triggers
    // big-mic recording (tap-anywhere-to-stop overlay) via AppVoiceInput.
    _voice_btn = lv_btn_create(_input_row);
    lv_obj_set_size(_voice_btn, MIC_W, 80);
    // Positioned where it will sit when the keyboard pops up: between input and
    // keyboard button. When the keyboard is down the input is wider.
    const int voice_x = SIDE_GAP + INPUT_W_MIC + BTN_GAP;
    lv_obj_align(_voice_btn, LV_ALIGN_LEFT_MID, voice_x, 0);
    lv_obj_set_style_bg_color(_voice_btn, lv_color_hex(0xC0392B), 0);
    lv_obj_set_style_bg_color(_voice_btn, lv_color_hex(0x8E2A20), LV_STATE_PRESSED);
    lv_obj_set_style_radius(_voice_btn, 12, 0);
    lv_obj_set_style_border_width(_voice_btn, 0, 0);
    lv_obj_add_event_cb(_voice_btn, _voice_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* mic_img = lv_image_create(_voice_btn);
    lv_image_set_src(mic_img, &mic_icon);
    lv_obj_center(mic_img);
    lv_obj_add_flag(_voice_btn, LV_OBJ_FLAG_HIDDEN);  // shown by _showKeyboard

    // Keyboard toggle — left of Send. The voice button is to its left when
    // shown; the wider SEND_GAP reduces mis-taps on Send.
    _keyboard_btn = lv_btn_create(_input_row);
    lv_obj_set_size(_keyboard_btn, KB_W, 80);
    // Initial position: centred between input's right edge and Send's left
    // edge, then shifted left by KB_LEFT_BIAS to compensate for the much
    // wider Send button (see header for the why). _showKeyboard /
    // _hideKeyboard apply the same shift when the voice button shows/hides.
    lv_obj_align(_keyboard_btn, LV_ALIGN_LEFT_MID,
                 (SIDE_GAP + INPUT_W + (SCREEN_W - SIDE_GAP - SEND_W)) / 2 - KB_W / 2
                 - KB_LEFT_BIAS, 0);
    lv_obj_set_style_bg_color(_keyboard_btn, lv_color_hex(0x1A3A5C), 0);
    lv_obj_set_style_bg_color(_keyboard_btn, lv_color_hex(0x0E2240), LV_STATE_PRESSED);
    lv_obj_set_style_radius(_keyboard_btn, 12, 0);
    lv_obj_set_style_border_width(_keyboard_btn, 0, 0);
    lv_obj_add_event_cb(_keyboard_btn, _keyboard_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* kb_img = lv_image_create(_keyboard_btn);
    lv_image_set_src(kb_img, &key_icon);
    lv_obj_set_style_image_recolor(kb_img, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(kb_img);

    _send_btn = lv_btn_create(_input_row);
    lv_obj_set_size(_send_btn, SEND_W, 80);
    lv_obj_align(_send_btn, LV_ALIGN_RIGHT_MID, -SIDE_GAP, 0);
    lv_obj_set_style_bg_color(_send_btn, lv_color_hex(C_SEND), 0);
    lv_obj_set_style_bg_color(_send_btn, lv_color_hex(0x0F4525), LV_STATE_PRESSED);
    lv_obj_set_style_radius(_send_btn, 12, 0);
    lv_obj_set_style_border_width(_send_btn, 0, 0);
    lv_obj_add_event_cb(_send_btn, _send_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* send_lbl = lv_label_create(_send_btn);
    lv_label_set_text(send_lbl, "Send");
    lv_obj_set_style_text_font(send_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(send_lbl);

    // ── On-screen keyboard (hidden; not used when physical keyboard present) ──
    _keyboard = lv_keyboard_create(_scr);
    lv_obj_set_size(_keyboard, SCREEN_W, KEYBOARD_H);
    lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(_keyboard, _input);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_keyboard, _keyboard_cb, LV_EVENT_ALL, this);

    // ── Physical keyboard group ───────────────────────────────────────────────
    // Register the textarea with the HAL keyboard group so physical key events
    // are routed here. The group persists across app open/close cycles; we add
    // on open and remove on close.
    if (GetHAL()->lvKbGroup) {
        lv_group_add_obj(GetHAL()->lvKbGroup, _input);
        lv_group_focus_obj(_input);
    }

    // Tap anywhere on screen (chat area, header, status bar) dismisses keyboard.
    // Events propagate to _scr from children; we filter out clicks inside the
    // keyboard or input row so typing isn't interrupted.
    lv_obj_add_event_cb(_scr, _scr_click_cb, LV_EVENT_CLICKED, this);

    // Swipe-up-from-bottom = exit (replaces the header back button).
    _addSwipeGesture();
}

// ── Keyboard helpers ──────────────────────────────────────────────────────

void AppProjectAssistant::_showKeyboard()
{
    if (!_keyboard || !_input) return;
    lv_keyboard_set_textarea(_keyboard, _input);
    lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    // Shrink chat scroll to make room
    if (_chat_scroll)
        lv_obj_set_height(_chat_scroll, SCREEN_H - HEADER_H - STATUS_H - INPUT_ROW_H - KEYBOARD_H);
    // Slide the input row up above the keyboard so the user can still see what
    // they typed and have access to the Send / keyboard buttons.
    if (_input_row)
        lv_obj_align(_input_row, LV_ALIGN_BOTTOM_MID, 0, -KEYBOARD_H);
    // Show the voice button and shrink the input + shift the keyboard button
    // right to keep it centred between the voice button and Send. All sizes /
    // gaps are class constants (see header) so the geometry stays consistent
    // with _buildUi.
    if (_voice_btn)
        lv_obj_clear_flag(_voice_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(_input, INPUT_W_MIC, 80);
    if (_keyboard_btn) {
        // Centre the keyboard button between the voice button and Send, then
        // shift left by KB_LEFT_BIAS so the kb reads as visually balanced
        // (Send is much wider than kb — see header for the why).
        // voice_end = SIDE_GAP + INPUT_W_MIC + BTN_GAP + MIC_W    → 976
        // send_x    = SCREEN_W - SIDE_GAP - SEND_W                → 1108
        int voice_end = SIDE_GAP + INPUT_W_MIC + BTN_GAP + MIC_W;
        int send_x    = SCREEN_W - SIDE_GAP - SEND_W;
        int kb_x      = (voice_end + send_x) / 2 - KB_W / 2 - KB_LEFT_BIAS;
        lv_obj_align(_keyboard_btn, LV_ALIGN_LEFT_MID, kb_x, 0);
    }
}

// ─── Swipe-up-to-exit gesture ────────────────────────────────────────────────
// Replaces the header back button. Register on the pointer indev directly
// so the gesture fires regardless of which LVGL object the user touches
// (chat bubble, header, keyboard, etc.) — same pattern as app_xiaozhi.
void AppProjectAssistant::_addSwipeGesture()
{
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    lv_async_call([](void* ud) {
                        auto* self = static_cast<AppProjectAssistant*>(ud);
                        if (self) self->close();
                    }, lv_event_get_user_data(e));
                }
            }, LV_EVENT_GESTURE, this);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppProjectAssistant::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, this);
        _gesture_indev = nullptr;
    }
}

void AppProjectAssistant::_hideKeyboard()
{
    if (!_keyboard) return;
    // Reentrancy guard: lv_obj_add_flag(HIDDEN) below synchronously triggers
    // a focus-group change, which sends LV_EVENT_DEFOCUSED to the textarea.
    // _textarea_cb handles DEFOCUSED by calling _hideKeyboard() again — without
    // this guard that recurses infinitely and overflows the stack. On reentry
    // the HIDDEN flag is already set, so we bail out immediately.
    if (lv_obj_has_flag(_keyboard, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    AppVoiceInput::dismissMicButton();
    if (_chat_scroll)
        lv_obj_set_height(_chat_scroll, SCREEN_H - HEADER_H - STATUS_H - INPUT_ROW_H);
    // Slide the input row back to the bottom.
    if (_input_row)
        lv_obj_align(_input_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    // Hide the voice button and restore the wider input + original kb button
    // position (centred between the input's right edge and Send's left edge).
    if (_voice_btn)
        lv_obj_add_flag(_voice_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(_input, INPUT_W, 80);
    if (_keyboard_btn) {
        // Centre the kb button between input's right edge and Send's left
        // edge, then shift left by KB_LEFT_BIAS for visual balance (Send
        // is much wider than kb — see header for the why).
        // input_end = SIDE_GAP + INPUT_W          → 976
        // send_x    = SCREEN_W - SIDE_GAP - SEND_W → 1108
        int input_end = SIDE_GAP + INPUT_W;
        int send_x    = SCREEN_W - SIDE_GAP - SEND_W;
        int kb_x      = (input_end + send_x) / 2 - KB_W / 2 - KB_LEFT_BIAS;
        lv_obj_align(_keyboard_btn, LV_ALIGN_LEFT_MID, kb_x, 0);
    }
}

// ── Chat bubble ────────────────────────────────────────────────────────────

void AppProjectAssistant::_addBubble(bool is_user, const std::string& text, bool save)
{
    if (!_chat_panel) return;

    // Row container (full width, transparent)
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

    // Bubble
    lv_obj_t* bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, BUBBLE_MAX_W);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(is_user ? C_USER_BUB : C_BOT_BUB), 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_pad_all(bubble, 18, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, text.c_str());
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, BUBBLE_MAX_W - 36);
    lv_obj_set_style_text_font(lbl, chat_font(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);

    // Scroll to bottom after adding
    lv_obj_update_layout(_chat_panel);
    lv_obj_scroll_to_y(_chat_scroll, LV_COORD_MAX, LV_ANIM_ON);

    if (save) {
        _history.push_back({is_user, text});
        if (_history.size() > MAX_HISTORY) _history.erase(_history.begin());
    }
}

void AppProjectAssistant::_setStatus(const std::string& text)
{
    if (_status_lbl) lv_label_set_text(_status_lbl, text.c_str());
}

// ── Streaming bubble ───────────────────────────────────────────────────────

void AppProjectAssistant::_addStreamingBubble()
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
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, BUBBLE_MAX_W);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(C_BOT_BUB), 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_pad_all(bubble, 18, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, "...");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, BUBBLE_MAX_W - 36);
    lv_obj_set_style_text_font(lbl, chat_font(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_MUTED), 0);  // muted until real text

    lv_obj_update_layout(_chat_panel);
    lv_obj_scroll_to_y(_chat_scroll, LV_COORD_MAX, LV_ANIM_ON);

    _streaming_lbl = lbl;
}

void AppProjectAssistant::_updateStreamingBubble(const std::string& text)
{
    if (!_streaming_lbl) return;
    lv_label_set_text(_streaming_lbl, text.c_str());
    lv_obj_set_style_text_color(_streaming_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_update_layout(_chat_panel);
    lv_obj_scroll_to_y(_chat_scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

void AppProjectAssistant::_finalizeStreaming(const std::string& text)
{
    if (_streaming_lbl) {
        lv_label_set_text(_streaming_lbl, text.c_str());
        lv_obj_set_style_text_color(_streaming_lbl, lv_color_hex(C_TEXT), 0);
        lv_obj_update_layout(_chat_panel);
        lv_obj_scroll_to_y(_chat_scroll, LV_COORD_MAX, LV_ANIM_ON);
        _history.push_back({false, text});
        if (_history.size() > MAX_HISTORY) _history.erase(_history.begin());
        _streaming_lbl = nullptr;
    } else {
        _addBubble(false, text);
    }
}

void AppProjectAssistant::_clearStreamingBubble()
{
    if (!_streaming_lbl) return;
    lv_obj_t* bubble = lv_obj_get_parent(_streaming_lbl);
    if (bubble) {
        lv_obj_t* row = lv_obj_get_parent(bubble);
        if (row && row != _chat_panel) lv_obj_delete(row);
    }
    _streaming_lbl = nullptr;
}

void AppProjectAssistant::_clearChat()
{
    _history.clear();
    if (_chat_panel) lv_obj_clean(_chat_panel);
    _streaming_lbl = nullptr;
    _saveHistory();
}

// ── History persistence (SPIFFS) ───────────────────────────────────────────

void AppProjectAssistant::_saveHistory()
{
#ifndef PLATFORM_BUILD_DESKTOP
    auto j = nlohmann::json::array();
    for (auto& m : _history) j.push_back({{"u", m.is_user}, {"t", m.text}});
    std::string s = j.dump();
    FILE* f = fopen("/spiffs/claude_chat.json", "w");
    if (f) { fwrite(s.c_str(), 1, s.size(), f); fclose(f); }
#endif
}

void AppProjectAssistant::_loadHistory()
{
    _history.clear();
#ifndef PLATFORM_BUILD_DESKTOP
    FILE* f = fopen("/spiffs/claude_chat.json", "r");
    if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) { fclose(f); return; }
    std::string s(sz, '\0');
    fread(&s[0], 1, sz, f);
    fclose(f);
    try {
        auto j = nlohmann::json::parse(s);
        if (!j.is_array()) return;
        for (auto& e : j) {
            bool u = e.value("u", true);
            std::string t = e.value("t", std::string(""));
            if (!t.empty()) _history.push_back({u, t});
        }
    } catch (...) {}
#endif
}

// ── Send / network ─────────────────────────────────────────────────────────

void AppProjectAssistant::_sendMessage()
{
    if (!_input || _inflight.load()) return;
    const char* raw = lv_textarea_get_text(_input);
    if (!raw || raw[0] == '\0') return;

    std::string text(raw);
    _hideKeyboard();
    lv_textarea_set_text(_input, "");
    _addBubble(true, text);
    _startChat(text);
}

void AppProjectAssistant::_startChat(const std::string& text)
{
    if (_inflight.exchange(true)) return;
    _setStatus("Sending...");

    bool ok = GetHAL()->tryRunDetached([this, text]() {
        _runChatRequest(text);
    });

    if (!ok) {
        _inflight = false;
        _setPending(UpdateKind::Error, "Network thread unavailable");
    }
}

void AppProjectAssistant::_runChatRequest(const std::string& text)
{
    // POST /api/chat
    std::string req_id;
    try {
        json body = {{"text", text}, {"chatId", "tab5"}};
        auto resp = GetHAL()->httpPost(
            _bridgeUrl("/api/chat"), body.dump(),
            {{"Content-Type", "application/json"}});
        if (!resp.ok) {
            _setPending(UpdateKind::Error, "Connection failed " + std::to_string(resp.status));
            _inflight = false;
            return;
        }
        auto parsed = json::parse(resp.body);
        req_id = parsed.value("requestId", "");
    } catch (const std::exception& e) {
        _setPending(UpdateKind::Error, e.what());
        _inflight = false;
        return;
    }

    if (req_id.empty()) {
        _setPending(UpdateKind::Error, "Invalid requestId");
        _inflight = false;
        return;
    }

    // Poll /api/result/:reqId until done
    const std::string poll_url = _bridgeUrl("/api/result/" + req_id + "?timeout=8");
    int attempts = 0;
    bool perm_shown = false;
    std::string current_perm_id;

    while (!_closing.load() && attempts < 60) {
        try {
            auto resp = GetHAL()->httpGet(poll_url);
            if (!resp.ok) {
                _setPending(UpdateKind::Error, "Poll failed " + std::to_string(resp.status));
                break;
            }
            auto parsed = json::parse(resp.body);
            bool done     = parsed.value("done", false);
            bool perm_req = parsed.value("permissionRequired", false);

            if (done) {
                std::string reply = parsed.value("text", "");
                _setPending(UpdateKind::Done, reply);
                _inflight = false;
                return;
            }

            if (perm_req) {
                std::string perm_id = parsed.value("permissionRequestId", "");

                // Check if user has already tapped Allow/Deny for this permission
                if (_perm_action_pending.load() && current_perm_id == perm_id) {
                    bool approved = _perm_approved.load();
                    _perm_action_pending.store(false);
                    current_perm_id.clear();
                    perm_shown = false;
                    json approve_body = {{"approved", approved}};
                    GetHAL()->httpPost(
                        _bridgeUrl("/api/approve/" + perm_id),
                        approve_body.dump(),
                        {{"Content-Type", "application/json"}});
                    _setPending(UpdateKind::Progress, "", {});  // show "Thinking..."
                    continue;
                }

                // Show the permission card the first time we see this perm_id
                if (!perm_shown || current_perm_id != perm_id) {
                    current_perm_id = perm_id;
                    perm_shown = true;
                    _setPendingPerm(perm_id,
                        parsed.value("permissionToolName", ""),
                        parsed.value("permissionToolInput", ""));
                }

                // Bridge returns immediately when perm pending — throttle polling
#ifndef PLATFORM_BUILD_DESKTOP
                vTaskDelay(pdMS_TO_TICKS(300));
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
#endif
                continue;  // don't count toward attempt limit
            }

            // Normal progress — reset perm state
            perm_shown = false;
            current_perm_id.clear();
            ++attempts;

            std::vector<std::string> tools;
            if (parsed.contains("tools") && parsed["tools"].is_array()) {
                for (auto& t : parsed["tools"]) tools.push_back(t.get<std::string>());
            }
            std::string partial = parsed.value("partial", "");
            _setPending(UpdateKind::Progress, "", std::move(tools), partial);

        } catch (const std::exception& e) {
            _setPending(UpdateKind::Error, e.what());
            break;
        }
    }

    if (attempts >= 60) _setPending(UpdateKind::Error, "Request timed out");
    _inflight = false;
}

// ── Helpers ────────────────────────────────────────────────────────────────

void AppProjectAssistant::_setPending(UpdateKind kind, const std::string& text,
                                       std::vector<std::string> tools,
                                       const std::string& partial)
{
    {
        std::lock_guard<std::mutex> lk(_upd_mu);
        _pending.kind    = kind;
        _pending.text    = text;
        _pending.tools   = std::move(tools);
        _pending.partial = partial;
    }
    _dirty = true;
}

void AppProjectAssistant::_setPendingPerm(const std::string& req_id,
                                           const std::string& tool_name,
                                           const std::string& tool_input)
{
    {
        std::lock_guard<std::mutex> lk(_upd_mu);
        _pending.kind           = UpdateKind::PermissionRequired;
        _pending.perm_req_id    = req_id;
        _pending.perm_tool_name = tool_name;
        _pending.perm_tool_input = tool_input;
    }
    _dirty = true;
}

// ── Permission card ────────────────────────────────────────────────────────

void AppProjectAssistant::_showPermissionCard(
    const std::string& tool_name, const std::string& tool_input)
{
    if (!_scr || _perm_overlay) return;

    // Semi-transparent dark overlay covering full screen
    _perm_overlay = lv_obj_create(_scr);
    lv_obj_set_size(_perm_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_perm_overlay, 0, 0);
    lv_obj_set_style_bg_color(_perm_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_perm_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(_perm_overlay, 0, 0);
    lv_obj_set_style_pad_all(_perm_overlay, 0, 0);
    lv_obj_clear_flag(_perm_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(_perm_overlay, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_perm_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_perm_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Card
    constexpr int CARD_W    = 840;
    constexpr int CARD_PAD  = 32;
    constexpr int INNER_W   = CARD_W - 2 * CARD_PAD;

    lv_obj_t* card = lv_obj_create(_perm_overlay);
    lv_obj_set_width(card, CARD_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0D1F35), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xF0A030), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, CARD_PAD, 0);
    lv_obj_set_style_pad_row(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Title
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "⚠  Permission Required");
    lv_obj_set_style_text_font(title, zh_font(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF0A030), 0);

    // Tool name
    lv_obj_t* tool_lbl = lv_label_create(card);
    lv_label_set_text(tool_lbl, ("Tool:  " + tool_name).c_str());
    lv_obj_set_width(tool_lbl, INNER_W);
    lv_obj_set_style_text_font(tool_lbl, zh_font(), 0);
    lv_obj_set_style_text_color(tool_lbl, lv_color_hex(C_TEXT), 0);

    // Tool input code block (if present)
    if (!tool_input.empty()) {
        lv_obj_t* box = lv_obj_create(card);
        lv_obj_set_width(box, INNER_W);
        lv_obj_set_height(box, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x081522), 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, 10, 0);
        lv_obj_set_style_pad_all(box, 16, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        std::string disp = tool_input.size() > 400
            ? tool_input.substr(0, 397) + "..." : tool_input;
        lv_obj_t* inp_lbl = lv_label_create(box);
        lv_label_set_text(inp_lbl, disp.c_str());
        lv_label_set_long_mode(inp_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(inp_lbl, INNER_W - 32);
        lv_obj_set_style_text_font(inp_lbl, zh_font(), 0);
        lv_obj_set_style_text_color(inp_lbl, lv_color_hex(0xB0C8E0), 0);
    }

    // Button row: [Deny]  [Allow]
    lv_obj_t* btn_row = lv_obj_create(card);
    lv_obj_set_width(btn_row, INNER_W);
    lv_obj_set_height(btn_row, 80);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    constexpr int BTN_W = (INNER_W - 24) / 2;

    lv_obj_t* deny = lv_btn_create(btn_row);
    lv_obj_set_size(deny, BTN_W, 72);
    lv_obj_set_style_bg_color(deny, lv_color_hex(0x5C1A1A), 0);
    lv_obj_set_style_bg_color(deny, lv_color_hex(0x3A0F0F), LV_STATE_PRESSED);
    lv_obj_set_style_radius(deny, 14, 0);
    lv_obj_set_style_border_width(deny, 0, 0);
    lv_obj_add_event_cb(deny, _deny_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* deny_lbl = lv_label_create(deny);
    lv_label_set_text(deny_lbl, "✗  Deny");
    lv_obj_set_style_text_font(deny_lbl, zh_font(), 0);
    lv_obj_set_style_text_color(deny_lbl, lv_color_hex(0xFF8080), 0);
    lv_obj_center(deny_lbl);

    lv_obj_t* allow = lv_btn_create(btn_row);
    lv_obj_set_size(allow, BTN_W, 72);
    lv_obj_set_style_bg_color(allow, lv_color_hex(0x1A5C2A), 0);
    lv_obj_set_style_bg_color(allow, lv_color_hex(0x0F3A1A), LV_STATE_PRESSED);
    lv_obj_set_style_radius(allow, 14, 0);
    lv_obj_set_style_border_width(allow, 0, 0);
    lv_obj_add_event_cb(allow, _approve_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* allow_lbl = lv_label_create(allow);
    lv_label_set_text(allow_lbl, "✓  Allow");
    lv_obj_set_style_text_font(allow_lbl, zh_font(), 0);
    lv_obj_set_style_text_color(allow_lbl, lv_color_hex(0x80FF80), 0);
    lv_obj_center(allow_lbl);
}

void AppProjectAssistant::_hidePermissionCard()
{
    if (_perm_overlay) {
        lv_obj_delete(_perm_overlay);
        _perm_overlay = nullptr;
    }
    _perm_req_id.clear();
}

std::string AppProjectAssistant::_bridgeUrl(const std::string& path)
{
    // The Claude bridge runs on the Mac (Hermes host), NOT on the Home
    // Assistant box. HA moved to .133 while Hermes services (Claude bridge
    // :8770, weather, Sonos) stayed on the Mac (.142) — so use "svc_host"
    // here, not "ha_host" (which now points at HA).
    std::string host = GetHAL()->getConfig("svc_host", "192.168.1.142");
    return "http://" + host + ":8770" + path;
}

std::string AppProjectAssistant::_stripMarkdown(const std::string& s)
{
    // Manual markdown stripping. AVOID std::regex here — the 4 chained
    // regex_replace calls were Stack protection fault'ing on long STT text:
    // re_bold's `\*{1,3}([^*]+)\*{1,3}` does DFS backtracking even when no
    // `*` is present (tries each of the 3 prefix lengths × every position)
    // and the std::pair<sub_match,sub_match> stack frames on each call
    // overflowed the 4 KB main task stack.
    std::string r = s;
    auto strip_paired = [&](const char* delim, size_t dlen) {
        // Greedy: find first delim, then the next one; erase both, keep content.
        // If no closing delim found, leave the opening one in place and stop.
        size_t pos = 0;
        while (pos + dlen * 2 <= r.size()) {
            size_t open = r.find(delim, pos);
            if (open == std::string::npos) break;
            size_t close = r.find(delim, open + dlen);
            if (close == std::string::npos) break;
            r.erase(close, dlen);
            r.erase(open, dlen);
            // Don't advance pos — content stays in place, can contain more
            // paired marks (rare but cheap to handle).
        }
    };
    // Order: longest first so ***…*** doesn't get half-stripped to *…* / *…*.
    strip_paired("***", 3);
    strip_paired("**", 2);
    strip_paired("*", 1);
    strip_paired("`", 1);
    // Heading `## ` and blockquote `> ` at line start. Cheap, no stack.
    std::string out;
    out.reserve(r.size());
    size_t i = 0;
    while (i < r.size()) {
        bool at_line_start = (i == 0 || r[i-1] == '\n');
        if (at_line_start) {
            // Strip leading ## (1-6) + space
            size_t j = i;
            int h = 0;
            while (j < r.size() && r[j] == '#' && h < 6) { j++; h++; }
            if (h >= 1 && j < r.size() && r[j] == ' ') { i = j + 1; continue; }
            // Strip leading > + space
            if (r[i] == '>') {
                size_t k = i + 1;
                if (k < r.size() && r[k] == ' ') i = k + 1;
                else if (k == r.size() || r[k] == '\n') i = k;
                else { out.push_back(r[i++]); continue; }
            }
        }
        out.push_back(r[i++]);
    }
    return out;
}

// ── Event callbacks ────────────────────────────────────────────────────────
// (Back button removed — exit via swipe-up gesture, see _addSwipeGesture.)

void AppProjectAssistant::_clear_cb(lv_event_t* e)
{
    static_cast<AppProjectAssistant*>(lv_event_get_user_data(e))->_clearChat();
}

void AppProjectAssistant::_approve_cb(lv_event_t* e)
{
    // Stop propagation FIRST — the card's overlay will be deleted below, and the
    // event must not reach _scr_click_cb which would then walk a freed object's
    // parent chain and cause a crash.
    lv_event_stop_processing(e);
    auto* self = static_cast<AppProjectAssistant*>(lv_event_get_user_data(e));
    self->_perm_approved.store(true);
    self->_perm_action_pending.store(true);
    self->_hidePermissionCard();
    self->_setStatus("Thinking...");
}

void AppProjectAssistant::_deny_cb(lv_event_t* e)
{
    lv_event_stop_processing(e);
    auto* self = static_cast<AppProjectAssistant*>(lv_event_get_user_data(e));
    self->_perm_approved.store(false);
    self->_perm_action_pending.store(true);
    self->_hidePermissionCard();
    self->_setStatus("Thinking...");
}

void AppProjectAssistant::_scr_click_cb(lv_event_t* e)
{
    auto* self = static_cast<AppProjectAssistant*>(lv_event_get_user_data(e));
    if (!self->_keyboard || lv_obj_has_flag(self->_keyboard, LV_OBJ_FLAG_HIDDEN)) return;
    // Don't dismiss while permission card is showing
    if (self->_perm_overlay) return;

    // Use tap Y-position instead of parent-chain walk to avoid touching potentially
    // freed objects (any deleted overlay child could still be the event target).
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    // When keyboard is up, input row sits above it; don't dismiss if tap lands there.
    if (pt.y > SCREEN_H - INPUT_ROW_H - KEYBOARD_H - 10) return;

    self->_hideKeyboard();
}

void AppProjectAssistant::_send_cb(lv_event_t* e)
{
    static_cast<AppProjectAssistant*>(lv_event_get_user_data(e))->_sendMessage();
}

void AppProjectAssistant::_textarea_cb(lv_event_t* e)
{
    auto* self = static_cast<AppProjectAssistant*>(lv_event_get_user_data(e));
    auto  code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        // Focus in the physical keyboard group so key events reach this textarea
        if (GetHAL()->lvKbGroup) {
            lv_group_focus_obj(self->_input);
        }
        // Tapping the textarea shows the on-screen keyboard. Voice input is
        // gated through the 🎤 button that appears with the keyboard — see
        // _voice_btn_cb.
        if (lv_obj_has_flag(self->_keyboard, LV_OBJ_FLAG_HIDDEN))
            self->_showKeyboard();
    } else if (code == LV_EVENT_DEFOCUSED) {
        // Don't tear down the keyboard/chat layout while the voice input
        // mic (big-mic overlay) is on screen — tapping the mic moves focus
        // and we don't want the keyboard to disappear mid-recording.
        if (!AppVoiceInput::isMicActive()) {
            self->_hideKeyboard();
        }
    } else if (code == LV_EVENT_KEY) {
        // Physical keyboard Enter → send message instead of inserting newline
        if (lv_event_get_key(e) == LV_KEY_ENTER) {
            lv_event_stop_processing(e);
            self->_sendMessage();
        }
    }
}

void AppProjectAssistant::_voice_btn_cb(lv_event_t* e)
{
    auto* self = static_cast<AppProjectAssistant*>(lv_event_get_user_data(e));
    // Bail if xiaozhi is still coming up in the background. The user can
    // tap again once the status line clears.
    if (!self->_voice_ready.load()) {
        self->_setStatus("Voice service starting, please wait...");
        return;
    }
    // Make sure the on-screen keyboard is up (it should be — the voice
    // button is only visible while the keyboard is up — but be defensive).
    if (lv_obj_has_flag(self->_keyboard, LV_OBJ_FLAG_HIDDEN))
        self->_showKeyboard();
    // Keep the on-screen keyboard visible while recording. AppVoiceInput
    // shows a full-screen mic overlay on lv_layer_top() that covers the
    // keyboard anyway, so hiding the keyboard here would just be
    // unnecessary churn. The user wanted to see the keyboard stay put.
    AppVoiceInput::requestMicInput(self->_input);
}

void AppProjectAssistant::_keyboard_btn_cb(lv_event_t* e)
{
    auto* self = static_cast<AppProjectAssistant*>(lv_event_get_user_data(e));
    if (!self->_keyboard) return;
    if (lv_obj_has_flag(self->_keyboard, LV_OBJ_FLAG_HIDDEN)) {
        // Dismiss any in-flight voice session so the keyboard and mic don't
        // fight for the same area.
        AppVoiceInput::dismissMicButton();
        self->_showKeyboard();
    } else {
        self->_hideKeyboard();
    }
}

void AppProjectAssistant::_keyboard_cb(lv_event_t* e)
{
    auto* self = static_cast<AppProjectAssistant*>(lv_event_get_user_data(e));
    auto  code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        self->_hideKeyboard();
}
