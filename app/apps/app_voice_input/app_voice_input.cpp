#include "app_voice_input.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <esp_log.h>
#include <algorithm>

#ifdef PLATFORM_BUILD_DESKTOP
// Voice input is a no-op on desktop simulator (no audio codec).
// The VoiceInputService and xiaozhi headers are only available in the ESP-IDF
// component tree, not on the desktop build.
#else
#include "voice_input_service.h"
#include "xiaozhi_ctl.h"
#include "boards/common/board.h"
#endif

namespace {

constexpr int MIC_BTN_SIZE   = 64;
constexpr int MIC_BTN_MARGIN = 8;
static const char* TAG = "AppVoiceInput";

// Unicode glyphs for mic button states
static const char* MIC_ICON_IDLE      = "\xF0\x9F\x8E\x99";  // studio mic
// MIC_ICON_RECORDING removed: the big-mic overlay used to show a red circle
// (U+1F534). Now it shows a live audio-waveform (5 pulsing bars).
static const char* MIC_ICON_DONE      = "\xE2\x9C\x85";      // green checkmark
static const char* MIC_ICON_ERROR     = "\xE2\x9D\x8C";      // red X

// Wave bar height/position during the pulse. C function pointer (LVGL 9
// exec_cb can't capture lambdas) — `var` is the bar pointer, set per-anim
// in _createBigMicButton.
static void wave_anim_cb(void* var, int32_t v) {
    lv_obj_t* bar = (lv_obj_t*)var;
    lv_bar_set_value(bar, v, LV_ANIM_OFF);
    int new_h = 16 + (110 - 16) * v / 100;
    lv_obj_set_size(bar, 16, new_h);
    lv_obj_set_y(bar, (128 - new_h) / 2);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Static state
// ─────────────────────────────────────────────────────────────────────────────
lv_obj_t* AppVoiceInput::s_focused_ta = nullptr;
AppVoiceInput* AppVoiceInput::s_instance = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

AppVoiceInput::AppVoiceInput() {}

AppVoiceInput::~AppVoiceInput() {
    if (s_instance == this) s_instance = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Mooncake lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void AppVoiceInput::onCreate() {
#ifndef PLATFORM_BUILD_DESKTOP
    _service = new voice_input::VoiceInputService();
#endif
    s_instance = this;
    mclog::tagInfo(TAG, "voice input service created");
}

void AppVoiceInput::onResume() {
    mclog::tagInfo(TAG, "voice input resumed");
}

void AppVoiceInput::onRunning() {
#ifndef PLATFORM_BUILD_DESKTOP
    // Process pending STT result on the LVGL thread
    if (resultPending) {
        ESP_LOGI("AppVoiceInput", "onRunning: resultPending=true, calling onSttResult (mic=%p ta=%p)",
                 (void*)_mic_btn, (void*)_target_ta);
        resultPending = false;
        onSttResult(lastResult);
    }
    // Update mic button animation
    if (_mic_btn) {
        _updateMicButton();
    }
#endif
}

void AppVoiceInput::onPause() {
#ifndef PLATFORM_BUILD_DESKTOP
    if (_service) _service->cancel();
    _removeMicButton();
#endif
}

void AppVoiceInput::onDestroy() {
    s_instance = nullptr;
#ifndef PLATFORM_BUILD_DESKTOP
    onPause();
    delete _service;
    _service = nullptr;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/* static */ void AppVoiceInput::requestVoiceInput(lv_obj_t* ta, int keyboard_h) {
#ifdef PLATFORM_BUILD_DESKTOP
    (void)ta; (void)keyboard_h;  // no-op on desktop
    return;
#else
    ESP_LOGI("AppVoiceInput", "requestVoiceInput: ta=%p keyboard_h=%d mic_active=%d",
             (void*)ta, keyboard_h, (int)isMicActive());
    if (!s_instance || !s_instance->_service || !ta) return;
    if (s_instance->_mic_btn) {
        s_instance->_removeMicButton();
    }
    s_instance->_target_ta = ta;
    s_instance->_createMicButton(ta, keyboard_h);
#endif
}

/* static */ void AppVoiceInput::requestMicInput(lv_obj_t* ta) {
#ifdef PLATFORM_BUILD_DESKTOP
    (void)ta;  // no-op on desktop
    return;
#else
    ESP_LOGI("AppVoiceInput", "requestMicInput: ta=%p mic_active=%d",
             (void*)ta, (int)isMicActive());
    if (!s_instance || !s_instance->_service || !ta) return;
    if (s_instance->_mic_btn) {
        s_instance->_removeMicButton();
    }
    s_instance->_target_ta = ta;
    s_instance->_createBigMicButton(ta);
#endif
}

/* static */ void AppVoiceInput::dismissMicButton() {
    ESP_LOGI("AppVoiceInput", "dismissMicButton: mic_active=%d", (int)isMicActive());
    if (s_instance) s_instance->_removeMicButton();
}

/* static */ bool AppVoiceInput::isEnabled() {
#ifndef PLATFORM_BUILD_DESKTOP
    return GetHAL() != nullptr;
#else
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// STT callback (runs on recording task — forwarded to UI thread)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef PLATFORM_BUILD_DESKTOP
static void stt_callback_wrapper(const voice_input::Result& result) {
    ESP_LOGI("AppVoiceInput", "stt_callback_wrapper: success=%d text='%s' s_instance=%p",
             (int)result.success, result.text.c_str(), (void*)AppVoiceInput::s_instance);
    if (!AppVoiceInput::s_instance) return;
    AppVoiceInput::s_instance->lastResult = {result.success, result.text, result.error};
    AppVoiceInput::s_instance->resultPending = true;
}
#endif

void AppVoiceInput::onSttResult(const VoiceInputResult& result) {
    ESP_LOGI("AppVoiceInput", "onSttResult: success=%d text='%s' mic=%p ta=%p",
             (int)result.success, result.text.c_str(), (void*)_mic_btn, (void*)_target_ta);
    // _target_ta is intentionally kept even after the mic button is removed,
    // so we can still deliver the STT result into the textarea. The mic may
    // be torn down for reasons unrelated to this session (e.g. the host app
    // blurred the textarea mid-recording); the result must still land.
    if (!_target_ta) {
        ESP_LOGW("AppVoiceInput", "onSttResult: no target textarea, dropping result");
        _removeMicButton();
        return;
    }

    if (result.success && !result.text.empty()) {
        lv_textarea_add_text(_target_ta, result.text.c_str());
        mclog::tagInfo(TAG, "inserted '%s' into textarea", result.text.c_str());
    } else {
        const char* err = result.error.empty() ? "recognition failed" : result.error.c_str();
        mclog::tagWarn(TAG, "voice input failed: %s", err);
        if (_status_label) {
            lv_label_set_text(_status_label, err);
        }
    }
    _removeMicButton();
}

// ─────────────────────────────────────────────────────────────────────────────
// Mic button UI
// ─────────────────────────────────────────────────────────────────────────────

void AppVoiceInput::_createMicButton(lv_obj_t* ta, int keyboard_h) {
    if (_mic_btn || !ta) return;
    s_focused_ta = ta;
    _keyboard_h = keyboard_h;

    // Create on the top layer so it floats above all app screens
    lv_obj_t* btn = lv_btn_create(lv_layer_top());
    lv_obj_set_size(btn, MIC_BTN_SIZE, MIC_BTN_SIZE);
    lv_obj_set_style_radius(btn, MIC_BTN_SIZE / 2, 0);  // circle
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2196F3), 0);  // blue
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
    lv_obj_set_style_shadow_width(btn, 20, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);

    // Position: just above the on-screen keyboard so it isn't covered.
    // If the caller didn't supply a keyboard height, fall back to the old
    // position (60px above screen bottom) which works when no keyboard is up.
    int lift;
    if (keyboard_h > 0) {
        lift = keyboard_h + 16;  // 8px margin + mic radius, sit fully above kb
    } else {
        lift = 60;
    }
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -lift);

    // Icon label inside the button
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, MIC_ICON_IDLE);
    lv_obj_center(label);

    // Status label just above the mic button
    _status_label = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_status_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(_status_label, "");
    lv_obj_align(_status_label, LV_ALIGN_BOTTOM_MID, 0, -lift - MIC_BTN_SIZE - 4);

    // Click handler
    lv_obj_add_event_cb(btn, [](lv_event_t* e) {
        auto* self = (AppVoiceInput*)lv_event_get_user_data(e);
#ifndef PLATFORM_BUILD_DESKTOP
        if (!self || !self->_service) return;
        // Allow click from any non-Recording/Processing state. The service
        // will reset from Done/Error back to Idle, then transition to
        // Recording. Only bail out if a session is genuinely in flight.
        auto st = self->_service->state();
        if (st != voice_input::State::Recording && st != voice_input::State::Processing) {
            // Copy config to ESP types and start
            voice_input::Config cfg;
            cfg.max_record_ms = self->_config.max_record_ms;
            cfg.silence_timeout_ms = self->_config.silence_timeout_ms;
            cfg.stt_response_timeout_ms = self->_config.stt_response_timeout_ms;
            cfg.opus_frame_duration_ms = self->_config.opus_frame_duration_ms;
            self->_service->start(cfg, stt_callback_wrapper);
            if (self->_status_label) lv_label_set_text(self->_status_label, "Recording...");
        }
#else
        (void)self;
#endif
    }, LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(btn, this);

    _mic_btn = btn;
}

void AppVoiceInput::_createBigMicButton(lv_obj_t* ta) {
    if (_mic_btn || !ta) return;
    _big_mic_mode = true;
    s_focused_ta = ta;

    // Hint label above the wave — placed first so the wave sits above it visually.
    _status_label = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_status_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(_status_label, "Recording... tap anywhere to stop");
    lv_obj_align(_status_label, LV_ALIGN_BOTTOM_MID, 0, -260);

    // Audio visualizer: 5 vertical bars that pulse in height via lv_anim.
    // Each bar has a different period and phase so the pattern looks like
    // a moving waveform rather than a single synchronized pulse.
    lv_obj_t* wave = lv_obj_create(lv_layer_top());
    lv_obj_set_size(wave, 5 * 16 + 4 * 8, 128);  // 5 bars × 16w + 4 × 8 gap
    lv_obj_set_style_bg_opa(wave, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wave, 0, 0);
    lv_obj_set_style_pad_all(wave, 0, 0);
    lv_obj_clear_flag(wave, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(wave, LV_ALIGN_BOTTOM_MID, 0, -120);

    // Per-bar period / phase offsets so they don't pulse in lockstep.
    const int periods_ms[WAVE_BAR_COUNT]   = { 280, 360, 240, 420, 320 };
    const int delays_ms[WAVE_BAR_COUNT]   = {   0,  80, 160,  40, 200 };
    const int min_h = 16;
    const int max_h = 110;

    for (int i = 0; i < WAVE_BAR_COUNT; i++) {
        lv_obj_t* bar = lv_bar_create(wave);
        lv_obj_set_size(bar, 16, min_h);
        // Lay out left-to-right inside the wave container.
        lv_obj_set_pos(bar, i * (16 + 8), (128 - min_h) / 2);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x4FC3F7), 0);  // cyan
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 4, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 50, LV_ANIM_OFF);
        _wave_bars[i] = bar;

        // Pulse: 0..100 with playback so it bounces back down. Different
        // period per bar = realistic waveform. var=bar so the C callback
        // can find its target (LVGL 9 callbacks are C function pointers,
        // no lambda capture — use a free function defined in this file).
        lv_anim_init(&_wave_anims[i]);
        lv_anim_set_var(&_wave_anims[i], bar);
        lv_anim_set_exec_cb(&_wave_anims[i], wave_anim_cb);
        lv_anim_set_values(&_wave_anims[i], 0, 100);
        lv_anim_set_time(&_wave_anims[i], periods_ms[i]);
        lv_anim_set_playback_time(&_wave_anims[i], periods_ms[i]);
        lv_anim_set_repeat_count(&_wave_anims[i], LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&_wave_anims[i], delays_ms[i]);
        lv_anim_start(&_wave_anims[i]);
    }
    _mic_btn = wave;  // reused field so _removeMicButton deletes the wave

    // Full-screen invisible capture layer — any tap while recording ends it.
    _capture_layer = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_capture_layer, 1280, 720);
    lv_obj_set_pos(_capture_layer, 0, 0);
    lv_obj_set_style_bg_opa(_capture_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_capture_layer, 0, 0);
    lv_obj_set_style_pad_all(_capture_layer, 0, 0);
    lv_obj_clear_flag(_capture_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_capture_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_capture_layer, [](lv_event_t* e) {
        auto* self = (AppVoiceInput*)lv_event_get_user_data(e);
#ifndef PLATFORM_BUILD_DESKTOP
        if (!self || !self->_service) return;
        if (self->_service->state() == voice_input::State::Recording) {
            ESP_LOGI("AppVoiceInput", "capture tap: stopping recording");
            self->_service->stop();
            if (self->_status_label) {
                lv_label_set_text(self->_status_label, "Recognizing...");
            }
        }
#else
        (void)self;
#endif
    }, LV_EVENT_CLICKED, this);
    // Send the capture layer to the back so the wave and hint stay clickable.
    lv_obj_move_background(_capture_layer);
    lv_obj_move_foreground(_mic_btn);
    lv_obj_move_foreground(_status_label);

#ifndef PLATFORM_BUILD_DESKTOP
    // Start recording immediately.
    voice_input::Config cfg;
    cfg.max_record_ms = _config.max_record_ms;
    cfg.silence_timeout_ms = _config.silence_timeout_ms;
    cfg.stt_response_timeout_ms = _config.stt_response_timeout_ms;
    cfg.opus_frame_duration_ms = _config.opus_frame_duration_ms;
    _service->start(cfg, stt_callback_wrapper);
#endif
}

void AppVoiceInput::_updateMicButton() {
#ifndef PLATFORM_BUILD_DESKTOP
    if (!_mic_btn || !_service) return;

    auto state = _service->state();
    lv_obj_t* label = lv_obj_get_child(_mic_btn, 0);

    switch (state) {
    case voice_input::State::Idle:
        if (label) lv_label_set_text(label, MIC_ICON_IDLE);
        lv_obj_set_style_bg_color(_mic_btn, lv_color_hex(0x2196F3), 0);  // blue
        break;
    case voice_input::State::Recording:
        // Keep the mic glyph (not a red circle — see comment on
        // MIC_ICON_RECORDING). Colour shift signals "active".
        if (label) lv_label_set_text(label, MIC_ICON_IDLE);
        lv_obj_set_style_bg_color(_mic_btn, lv_color_hex(0x4FC3F7), 0);  // cyan
        break;
    case voice_input::State::Processing:
        lv_obj_set_style_bg_color(_mic_btn, lv_color_hex(0xFF9800), 0);
        if (_status_label) lv_label_set_text(_status_label, "Recognizing...");
        break;
    case voice_input::State::Done:
        if (label) lv_label_set_text(label, MIC_ICON_DONE);
        lv_obj_set_style_bg_color(_mic_btn, lv_color_hex(0x4CAF50), 0);
        break;
    case voice_input::State::Error:
        if (label) lv_label_set_text(label, MIC_ICON_ERROR);
        lv_obj_set_style_bg_color(_mic_btn, lv_color_hex(0xF44336), 0);
        break;
    }
#endif
}

void AppVoiceInput::_removeMicButton() {
    ESP_LOGW("AppVoiceInput", "_removeMicButton called from %p (mic=%p capture=%p ta=%p)",
             (void*)__builtin_return_address(0), (void*)_mic_btn, (void*)_capture_layer, (void*)_target_ta);
    // Stop the wave anims BEFORE deleting their bar objects — otherwise the
    // anim keeps writing to freed memory. lv_anim_delete takes (var, exec_cb).
    for (int i = 0; i < WAVE_BAR_COUNT; i++) {
        if (_wave_bars[i]) {
            lv_anim_delete(_wave_bars[i], wave_anim_cb);
        }
        _wave_bars[i] = nullptr;
        memset(&_wave_anims[i], 0, sizeof(_wave_anims[i]));
    }
    if (_capture_layer) {
        lv_obj_delete(_capture_layer);
        _capture_layer = nullptr;
    }
    if (_mic_btn) {
        lv_obj_delete(_mic_btn);
        _mic_btn = nullptr;
    }
    if (_status_label) {
        lv_obj_delete(_status_label);
        _status_label = nullptr;
    }
    _big_mic_mode = false;
    // NOTE: keep _target_ta set so a pending STT result can still be inserted
    // into the textarea even if the host app tore down the mic UI mid-session.
    // The textarea itself is owned by the host app and outlives the mic.
    s_focused_ta = nullptr;
}
