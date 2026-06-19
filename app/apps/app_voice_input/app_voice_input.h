#pragma once
#include <mooncake.h>
#include <lvgl.h>
#include <string>
#include <memory>

/// Lightweight value types used by the App side — mirrors voice_input::Result/Config
/// without pulling in the ESP-IDF component tree (needed for desktop builds).
struct VoiceInputResult {
    bool success = false;
    std::string text;
    std::string error;
};

struct VoiceInputConfig {
    int max_record_ms = 10000;
    int silence_timeout_ms = 2000;
    int stt_response_timeout_ms = 10000;
    int opus_frame_duration_ms = 60;
};

// Forward-declare the ESP-only service; only created on real hardware.
namespace voice_input { class VoiceInputService; }

/**
 * Global voice-input WorkerAbility.
 *
 * Installed once at boot (not a user-launchable "app").  Other apps call
 * requestVoiceInput(textarea) to pop up a microphone button anchored to the
 * current keyboard.  When tapped, a short recording is sent to the cloud STT
 * server and the result is inserted into the target textarea.
 */
class AppVoiceInput : public mooncake::WorkerAbility {
public:
    AppVoiceInput();
    ~AppVoiceInput() override;

    // ── Mooncake lifecycle ──
    void onCreate() override;
    void onResume() override;
    void onRunning() override;
    void onPause() override;
    void onDestroy() override;

    // ── Public API for other apps ──

    /// Request voice input for a specific textarea.
    /// Creates a floating mic button anchored above the on-screen keyboard.
    /// On tap, records speech and inserts STT result into `ta`.
    /// Safe to call from any LVGL thread.
    /// @param ta          Target textarea for the recognized text.
    /// @param keyboard_h  Height of the on-screen keyboard the mic should sit
    ///                    above. 0 = no keyboard / caller doesn't know — the
    ///                    mic will sit near the screen bottom (legacy mode).
    static void requestVoiceInput(lv_obj_t* ta, int keyboard_h = 0);

    /// Big-mic mode: shows a large mic + full-screen tap-to-stop overlay,
    /// starts recording immediately, and inserts the STT result into `ta`
    /// when the user taps anywhere (or the recording auto-stops on silence
    /// / max duration). The on-screen keyboard is not involved.
    /// Safe to call from any LVGL thread.
    static void requestMicInput(lv_obj_t* ta);

    /// Hide the mic button if currently shown (e.g., keyboard dismissed).
    static void dismissMicButton();

    /// True while the mic button is currently visible. Other apps can poll
    /// this from LVGL event handlers to avoid dismissing the keyboard (or
    /// tearing down other UI) in response to a transient focus change —
    /// e.g. when the user taps the mic, focus briefly moves to the button
    /// and the previously focused widget receives LV_EVENT_DEFOCUSED.
    static bool isMicActive() {
        return s_instance && s_instance->_mic_btn != nullptr;
    }

    /// True if voice input is enabled (configurable via NVS).
    static bool isEnabled();

    // Called from STT callback (forwarded from recording task)
    void onSttResult(const VoiceInputResult& result);
    bool resultPending = false;
    VoiceInputResult lastResult;

    // Static singleton pointer (accessed by the C-style stt_callback_wrapper
    // defined in app_voice_input.cpp to forward results to the running
    // service instance — needs to be public for the file-scope function).
    static AppVoiceInput* s_instance;
    // Last-focused textarea (read by the keyboard focus machinery).
    static lv_obj_t* s_focused_ta;

private:
    // Mic button UI
    void _createMicButton(lv_obj_t* ta, int keyboard_h);
    void _createBigMicButton(lv_obj_t* ta);
    void _updateMicButton();
    void _removeMicButton();

    // Core service (only created on ESP32; always nullptr on desktop)
    // Raw ptr: forward-declared type, complete only in the device .cpp
    voice_input::VoiceInputService* _service = nullptr;
    VoiceInputConfig _config;

    // UI state
    lv_obj_t* _mic_btn = nullptr;
    lv_obj_t* _target_ta = nullptr;
    lv_obj_t* _status_label = nullptr;
    lv_obj_t* _capture_layer = nullptr;  // full-screen tap-to-stop (big-mic mode)
    bool       _big_mic_mode = false;
    int        _keyboard_h = 0;  // height of the on-screen keyboard the mic
                                 // is anchored above (passed by the caller).

    // Big-mic audio visualizer — 5 vertical bars that pulse in height via
    // independent lv_anim timers. Stored so _removeMicButton can delete the
    // anims before deleting the bars (otherwise the anim keeps touching
    // freed memory).
    static constexpr int WAVE_BAR_COUNT = 5;
    lv_obj_t* _wave_bars[WAVE_BAR_COUNT] = {nullptr};
    lv_anim_t  _wave_anims[WAVE_BAR_COUNT] = {};
};
