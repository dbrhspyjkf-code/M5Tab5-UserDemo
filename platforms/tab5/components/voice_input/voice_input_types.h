#pragma once
#include <cstdint>
#include <string>

namespace voice_input {

/// Recording / STT pipeline state (single-shot, no loops).
enum class State {
    Idle,
    Recording,    // mic active, capturing Opus frames
    Processing,   // sent listen/stop, waiting for STT result
    Done,         // result received successfully
    Error,        // network / silence / timeout / codec-unavailable
};

/// Result delivered to the callback on completion (success or failure).
struct Result {
    bool success = false;
    std::string text;    // STT text (empty on failure)
    std::string error;   // human-readable error (empty on success)
};

/// Tuning knobs.
struct Config {
    int max_record_ms = 10000;       // hard cap on recording duration
    int silence_timeout_ms = 2000;   // auto-stop after continuous silence
    int stt_response_timeout_ms = 10000;  // wait for cloud STT reply
    int opus_frame_duration_ms = 60;      // must match server expectation
};

/// Callback type: invoked once from the recording task (NOT the LVGL thread).
/// The caller is responsible for forwarding the result to the UI thread.
using Callback = void (*)(const Result&);

} // namespace voice_input
