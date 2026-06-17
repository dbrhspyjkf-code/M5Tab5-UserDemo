#pragma once
#include "voice_input_types.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <deque>

namespace voice_input {

/**
 * Voice recording + Opus encoding + cloud STT pipeline.
 *
 * On start():
 *   1. Suspends xiaozhi (if running) to free I2S_NUM_0 (the WebSocket channel
 *      stays open, see xiaozhi_suspend() rationale).
 *   2. Enables the shared codec input (ES7210 mic).
 *   3. Spawns a FreeRTOS task that reads TDM PCM, extracts mono channel,
 *      Opus-encodes 60ms frames, and collects them in a buffer.
 *   4. On stop (silence timeout or max duration):
 *        a. Transitions to Processing.
 *        b. Reuses xiaozhi's still-open protocol channel to send
 *           listen-start + each Opus frame + listen-stop.
 *        c. Waits on a FreeRTOS semaphore (timeout = stt_response_timeout_ms)
 *           for the server's {"type":"stt","text":"..."} reply, dispatched
 *           from xiaozhi's main task via xiaozhi_register_stt_callback().
 *        d. Invokes the user callback with the recognized text (or error).
 */
class VoiceInputService {
public:
    VoiceInputService();
    ~VoiceInputService();

    void start(const Config& config, Callback cb);
    void cancel();
    /**
     * User-initiated stop. Signals the recording loop to break; the captured
     * audio so far is sent to STT (no error). Idempotent / no-op if not
     * currently recording. Distinct from cancel(), which aborts the whole
     * session with an Error result.
     */
    void stop();
    State state() const { return state_.load(); }

private:
    static void task_fn(void* self);
    void run();

    bool init_codec();
    void deinit_codec();
    bool init_opus();
    void deinit_opus();
    int  record_pcm(int16_t* buf, int samples);

    void reset_silence();
    bool is_silence(const int16_t* buf, int samples);

    // STT request/reply helpers (run on the recording task).
    bool send_listen_session();  // returns true on success, fills result.text
    void on_stt_reply(const std::string& text);

    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stop_requested_{false};
    Config config_;
    Callback callback_ = nullptr;
    TaskHandle_t task_ = nullptr;

    // Opus
    void* opus_encoder_ = nullptr;
    int encoder_frame_size_ = 0;
    int encoder_outbuf_size_ = 0;

    // Silence detection
    int64_t silence_start_ms_ = 0;
    int64_t recording_start_ms_ = 0;

    // Output buffer
    std::deque<std::vector<uint8_t>> opus_packets_;
    size_t peak_opus_mem_ = 0;

    // STT session synchronization. The recording task waits on stt_sem_;
    // the xiaozhi stt callback (running on xiaozhi's main task) copies the
    // recognized text into stt_text_ and gives the semaphore. Mutex guards
    // stt_text_ because the callback's text lifetime is bounded.
    SemaphoreHandle_t stt_sem_ = nullptr;
    std::mutex        stt_mu_;
    std::string       stt_text_;
    bool              stt_received_ = false;
};

} // namespace voice_input
