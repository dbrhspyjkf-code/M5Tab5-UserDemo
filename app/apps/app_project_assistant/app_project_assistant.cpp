#include "app_project_assistant.h"

#include <hal/hal.h>
#include <mooncake_log.h>
#include <nlohmann/json.hpp>
#include "../app_voice_input/app_voice_input.h"
#include "xiaozhi_ctl.h"
#include <cbin_font.h>
#include <esp_timer.h>
#include <regex>

using json = nlohmann::json;

static const std::string TAG = "app-claude";
LV_IMAGE_DECLARE(key_icon);

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

const lv_font_t* chat_font() { return zh_font(); }
const lv_font_t* input_font() { return zh_font(); }

}  // namespace

// ── Lifecycle ─────────────────────────────────────────────────────────────

AppProjectAssistant::AppProjectAssistant()
{
    setAppInfo().name = "Claude";
}

void AppProjectAssistant::onCreate() {}

void AppProjectAssistant::onOpen()
{
    mclog::tagInfo(TAG, "open");
    _closing = false;
    _buildUi();
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
        std::string s;
        if (!upd.tools.empty()) {
            s = upd.tools[0];
            if (upd.tools.size() > 1) s += " +" + std::to_string(upd.tools.size() - 1);
        } else if (!upd.partial.empty()) {
            s = upd.partial.substr(0, 60);
        } else {
            s = "Thinking...";
        }
        _setStatus(s);
        break;
    }
    case UpdateKind::Done:
        _setStatus("");
        _inflight = false;
        _addBubble(false, _stripMarkdown(upd.text));
        break;
    case UpdateKind::Error:
        _setStatus("");
        _inflight = false;
        _addBubble(false, "⚠ " + upd.text);
        break;
    default:
        break;
    }
}

void AppProjectAssistant::onClose()
{
    mclog::tagInfo(TAG, "close");
    _closing = true;
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
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _chat_scroll = _chat_panel = _status_lbl = nullptr;
    _input = _input_row = _send_btn = _voice_btn = _keyboard = _keyboard_btn = nullptr;
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

    lv_obj_t* back = lv_btn_create(hdr);
    lv_obj_set_size(back, 72, 48);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x1A3A5C), 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0E2240), LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, _back_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(back_lbl);

    lv_obj_t* title = lv_label_create(hdr);
    lv_label_set_text(title, "Claude");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

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
    lv_obj_set_style_text_font(_status_lbl, &lv_font_montserrat_22, 0);
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
    constexpr int SIDE_GAP = 8;
    constexpr int BTN_GAP  = 20;
    constexpr int SEND_GAP = 32;  // wider gap so Send isn't accidentally hit
    constexpr int KB_W     = 80;
    constexpr int SEND_W   = 164;
    constexpr int MIC_W    = 80;
    const int input_w = SCREEN_W - 2*SIDE_GAP - KB_W - SEND_W - 2*BTN_GAP - SEND_GAP;
    const int input_w_with_mic = input_w - MIC_W - BTN_GAP;

    _input = lv_textarea_create(_input_row);
    lv_obj_set_size(_input, input_w, 80);
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
    // keyboard button. When the keyboard is down the input is wider so we
    // remember the voice-button's "shown" x as a constant for _showKeyboard.
    const int voice_x       = SIDE_GAP + input_w_with_mic + BTN_GAP;
    const int kb_x_no_voice = SIDE_GAP + input_w + BTN_GAP;
    const int kb_x_voice    = voice_x + MIC_W + BTN_GAP;
    lv_obj_align(_voice_btn, LV_ALIGN_LEFT_MID, voice_x, 0);
    lv_obj_set_style_bg_color(_voice_btn, lv_color_hex(0xC0392B), 0);
    lv_obj_set_style_bg_color(_voice_btn, lv_color_hex(0x8E2A20), LV_STATE_PRESSED);
    lv_obj_set_style_radius(_voice_btn, 12, 0);
    lv_obj_set_style_border_width(_voice_btn, 0, 0);
    lv_obj_add_event_cb(_voice_btn, _voice_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* mic_glyph = lv_label_create(_voice_btn);
    lv_label_set_text(mic_glyph, "\xF0\x8F\x84\x99");  // studio mic
    lv_obj_set_style_text_font(mic_glyph, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(mic_glyph, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(mic_glyph);
    lv_obj_add_flag(_voice_btn, LV_OBJ_FLAG_HIDDEN);  // shown by _showKeyboard

    // Keyboard toggle — left of Send. The voice button is to its left when
    // shown; the wider SEND_GAP reduces mis-taps on Send.
    _keyboard_btn = lv_btn_create(_input_row);
    lv_obj_set_size(_keyboard_btn, KB_W, 80);
    // Initial position assumes voice button is hidden (kb is right next to input).
    // _showKeyboard moves it right by (MIC_W + BTN_GAP) to make room for the
    // voice button, and _hideKeyboard moves it back.
    lv_obj_align(_keyboard_btn, LV_ALIGN_LEFT_MID, kb_x_no_voice, 0);
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
    // right by (MIC_W + BTN_GAP) to make room. Magic numbers match _buildUi.
    constexpr int SIDE_GAP = 8, BTN_GAP = 20, MIC_W = 80, KB_W = 80;
    constexpr int input_w         = 968;  // 8+input_w+20+80+32+164+8 = 1280
    constexpr int input_w_with_mic = 868;  // input_w - MIC_W - BTN_GAP
    if (_voice_btn)
        lv_obj_clear_flag(_voice_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(_input, input_w_with_mic, 80);
    if (_keyboard_btn)
        lv_obj_align(_keyboard_btn, LV_ALIGN_LEFT_MID, SIDE_GAP + input_w_with_mic + BTN_GAP + MIC_W + BTN_GAP, 0);
}

void AppProjectAssistant::_hideKeyboard()
{
    if (!_keyboard) return;
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    AppVoiceInput::dismissMicButton();
    if (_chat_scroll)
        lv_obj_set_height(_chat_scroll, SCREEN_H - HEADER_H - STATUS_H - INPUT_ROW_H);
    // Slide the input row back to the bottom.
    if (_input_row)
        lv_obj_align(_input_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    // Hide the voice button and restore the wider input + original kb button
    // position. Same magic numbers as _showKeyboard.
    constexpr int SIDE_GAP = 8, BTN_GAP = 20, KB_W = 80;
    constexpr int input_w = 968;
    if (_voice_btn)
        lv_obj_add_flag(_voice_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(_input, input_w, 80);
    if (_keyboard_btn)
        lv_obj_align(_keyboard_btn, LV_ALIGN_LEFT_MID, SIDE_GAP + input_w + BTN_GAP, 0);
}

// ── Chat bubble ────────────────────────────────────────────────────────────

void AppProjectAssistant::_addBubble(bool is_user, const std::string& text)
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
}

void AppProjectAssistant::_setStatus(const std::string& text)
{
    if (_status_lbl) lv_label_set_text(_status_lbl, text.c_str());
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
    while (!_closing.load() && attempts < 60) {
        ++attempts;
        try {
            auto resp = GetHAL()->httpGet(poll_url);
            if (!resp.ok) {
                _setPending(UpdateKind::Error, "Poll failed " + std::to_string(resp.status));
                break;
            }
            auto parsed = json::parse(resp.body);
            bool done = parsed.value("done", false);

            if (done) {
                std::string reply = parsed.value("text", "");
                _setPending(UpdateKind::Done, reply);
                return;
            }

            // Still running — report progress
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

std::string AppProjectAssistant::_bridgeUrl(const std::string& path)
{
    std::string host = GetHAL()->getConfig("ha_host", "192.168.1.142");
    return "http://" + host + ":8770" + path;
}

std::string AppProjectAssistant::_stripMarkdown(const std::string& s)
{
    // Remove **bold**, *em*, `code`, ## headings, > blockquotes
    static const std::regex re_bold(R"(\*{1,3}([^*]+)\*{1,3})");
    static const std::regex re_head(R"(^#{1,6}\s+)", std::regex::multiline);
    static const std::regex re_bq(R"(^>\s*)", std::regex::multiline);
    static const std::regex re_code(R"(`([^`]+)`)");
    std::string r = std::regex_replace(s, re_bold, "$1");
    r = std::regex_replace(r, re_head, "");
    r = std::regex_replace(r, re_bq, "");
    r = std::regex_replace(r, re_code, "$1");
    return r;
}

// ── Event callbacks ────────────────────────────────────────────────────────

void AppProjectAssistant::_back_cb(lv_event_t* e)
{
    static_cast<AppProjectAssistant*>(lv_event_get_user_data(e))->close();
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
    // Hide the keyboard before starting recording so the full-screen mic
    // overlay has a clean stage. The input row slides down over the
    // (now-hidden) keyboard position.
    self->_hideKeyboard();
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
