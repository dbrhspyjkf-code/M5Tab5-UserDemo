#include "voice_input_service.h"
#include "xiaozhi_ctl.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace voice_input {

static const char* TAG = "voice_input";

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static int64_t now_ms() {
    return esp_timer_get_time() / 1000;
}

static int16_t extract_mono_ch0(const int16_t* tdm_buf, int total_samples) {
    int64_t sum = 0;
    int count = total_samples / 4;
    for (int i = 0; i < count; ++i)
        sum += tdm_buf[i * 4];
    return static_cast<int16_t>(sum / count);
}

// ─────────────────────────────────────────────────────────────────────────────
// VoiceInputService
// ─────────────────────────────────────────────────────────────────────────────

VoiceInputService::VoiceInputService() {}

// Pointer to the service instance currently waiting on an STT reply. The
// xiaozhi stt callback (running on xiaozhi's main task) routes the reply
// here. Only one voice input session is in flight at a time, so a single
// static pointer is sufficient.
static voice_input::VoiceInputService* s_active_stt_session = nullptr;

VoiceInputService::~VoiceInputService() {
    cancel();
    if (task_) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    deinit_opus();
    if (stt_sem_) {
        vSemaphoreDelete(stt_sem_);
        stt_sem_ = nullptr;
    }
}

void VoiceInputService::cancel() {
    State expected = State::Recording;
    if (state_.compare_exchange_strong(expected, State::Error)) {
        ESP_LOGI(TAG, "cancel requested");
    }
    // If we're already in Processing, the recording task is blocked on the
    // STT semaphore; releasing it here lets run() exit promptly with Error.
    if (stt_sem_) xSemaphoreGive(stt_sem_);
}

void VoiceInputService::stop() {
    // Set a flag the recording loop checks. The loop then breaks out, and
    // run() proceeds to STT with whatever opus packets have been captured.
    // Distinct from cancel(): we don't set Error; we just stop accumulating.
    State cur = state_.load();
    if (cur == State::Recording) {
        stop_requested_.store(true);
        ESP_LOGI(TAG, "stop requested (user tap)");
    }
}

// ────────────────────────────────────────────────────────────────────
// Public API
// ────────────────────────────────────────────────────────────────────

void VoiceInputService::start(const Config& config, Callback cb) {
    // Allow restart from terminal states (Done / Error) — a previous
    // session that left the mic button visible should be retryable.
    // Reject only when a session is genuinely in flight (Recording/Processing).
    State current = state_.load();
    if (current == State::Done || current == State::Error) {
        ESP_LOGI(TAG, "start() while in terminal state %d — resetting", (int)current);
        state_.store(State::Idle);
        // Drop any leftover opus packets from the previous session.
        opus_packets_.clear();
    }
    State expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, State::Recording)) {
        ESP_LOGW(TAG, "start() while not Idle (state=%d)", (int)expected);
        if (cb) {
            Result r; r.success = false; r.error = "busy";
            cb(r);
        }
        return;
    }
    config_ = config;
    callback_ = cb;

    // Lazily allocate the STT semaphore (idempotent across sessions).
    if (!stt_sem_) {
        stt_sem_ = xSemaphoreCreateBinary();
        if (!stt_sem_) {
            ESP_LOGE(TAG, "failed to create stt semaphore");
            state_ = State::Error;
            if (cb) { Result r; r.error = "oom"; cb(r); }
            return;
        }
    } else {
        // Drain any leftover give from a prior session before starting.
        xSemaphoreTake(stt_sem_, 0);
    }
    stt_received_ = false;
    { std::lock_guard<std::mutex> lk(stt_mu_); stt_text_.clear(); }
    // Also clear any packets that survived the terminal-state reset (defensive).
    opus_packets_.clear();

    ESP_LOGI(TAG, "starting voice input session");
    // 32 KB stack: send_listen_session() schedules many lambdas into
    // xiaozhi's main task via std::function, and the local pcm_buf/
    // mono_buf/opus_out vectors add up. 8 KB blew the stack (Guru
    // Meditation Stack protection fault on Core 1).
    BaseType_t ok = xTaskCreate(task_fn, "vi_task", 32768, this, 3, &task_);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed (likely OOM in internal SRAM)");
        state_.store(State::Error);
        if (callback_) {
            Result r; r.success = false; r.error = "task create failed";
            callback_(r);
        }
        callback_ = nullptr;
        task_ = nullptr;
    }
}

// ────────────────────────────────────────────────────────────────────
// FreeRTOS task
// ────────────────────────────────────────────────────────────────────

void VoiceInputService::task_fn(void* self) {
    static_cast<VoiceInputService*>(self)->run();
    vTaskDelete(nullptr);
}

void VoiceInputService::run() {
    Result result;
    result.success = false;

    // 1. Suspend xiaozhi + init codec input
    if (!init_codec()) {
        result.error = "codec unavailable";
        goto done;
    }

    // 2. Init Opus encoder
    if (!init_opus()) {
        result.error = "opus init failed";
        goto done;
    }

    // 3. Record audio
    state_ = State::Recording;
    stop_requested_.store(false);
    recording_start_ms_ = now_ms();
    reset_silence();
    ESP_LOGI(TAG, "recording started (max %d ms)", config_.max_record_ms);

    {
        const int samples_per_frame = config_.opus_frame_duration_ms * 16;  // 960 for 60ms @ 16kHz
        const int read_buf_samples = samples_per_frame * 4;  // 4-channel TDM
        std::vector<int16_t> pcm_buf(read_buf_samples);
        std::vector<int16_t> mono_buf(samples_per_frame);
        std::vector<uint8_t> opus_out(encoder_outbuf_size_);

        while (state_ == State::Recording && !stop_requested_.load()) {
            int64_t elapsed = now_ms() - recording_start_ms_;
            if (elapsed >= config_.max_record_ms) {
                ESP_LOGI(TAG, "max record duration reached");
                break;
            }

            int read = record_pcm(pcm_buf.data(), read_buf_samples);
            if (read < samples_per_frame * 4) continue;

            // TDM → mono
            for (int i = 0; i < samples_per_frame; ++i)
                mono_buf[i] = pcm_buf[i * 4];

            // Silence detection
            if (is_silence(mono_buf.data(), samples_per_frame)) {
                if (silence_start_ms_ == 0) silence_start_ms_ = now_ms();
                int64_t silence_dur = now_ms() - silence_start_ms_;
                if (silence_dur >= config_.silence_timeout_ms) {
                    ESP_LOGI(TAG, "silence timeout (%lld ms)", silence_dur);
                    break;
                }
            } else {
                reset_silence();
            }

            // Opus encode
            int encoded_bytes = xiaozhi_opus_encode(opus_encoder_, mono_buf.data(), opus_out.data(), (int)opus_out.size());
            if (encoded_bytes <= 0) continue;

            opus_packets_.push_back(std::vector<uint8_t>(opus_out.data(), opus_out.data() + encoded_bytes));
            if (opus_packets_.size() * (size_t)encoded_bytes > peak_opus_mem_)
                peak_opus_mem_ = opus_packets_.size() * (size_t)encoded_bytes;
        }
    }

    if (stop_requested_.load()) {
        ESP_LOGI(TAG, "user-stopped recording with %zu packets", opus_packets_.size());
    }

    // 4. Send recorded opus to tenclass STT via xiaozhi's open WebSocket.
    {
        int total_bytes = 0;
        for (auto& pkt : opus_packets_) total_bytes += pkt.size();
        ESP_LOGI(TAG, "recorded %d opus packets (%d bytes total)",
                 (int)opus_packets_.size(), total_bytes);
        if (opus_packets_.empty()) {
            result.error = "no speech detected";
        } else {
            state_ = State::Processing;
            ESP_LOGI(TAG, "entering Processing state, calling send_listen_session");
            if (send_listen_session()) {
                result.success = true;
                std::lock_guard<std::mutex> lk(stt_mu_);
                result.text = stt_text_;
            } else {
                result.error = "stt failed";
            }
        }
    }

done:
    ESP_LOGI(TAG, "session done: success=%d text='%s' err='%s' peak_opus=%zu",
             result.success, result.text.c_str(), result.error.c_str(), peak_opus_mem_);
    deinit_opus();
    deinit_codec();
    state_ = result.success ? State::Done : State::Error;
    if (callback_) callback_(result);
    callback_ = nullptr;
    task_ = nullptr;
    // Don't reset state_ here — caller reads it in onRunning
}

// ────────────────────────────────────────────────────────────────────
// Codec
// ────────────────────────────────────────────────────────────────────

bool VoiceInputService::init_codec() {
    if (xiaozhi_is_initialized()) {
        ESP_LOGI(TAG, "suspending xiaozhi for voice input");
        xiaozhi_suspend();
    }
    xiaozhi_codec_enable_input(true);
    ESP_LOGI(TAG, "codec input enabled");
    return true;
}

void VoiceInputService::deinit_codec() {
    xiaozhi_codec_enable_input(false);
    if (xiaozhi_is_initialized()) {
        ESP_LOGI(TAG, "resuming xiaozhi");
        xiaozhi_resume();
    }
}

int VoiceInputService::record_pcm(int16_t* buf, int samples) {
    return xiaozhi_codec_read(buf, samples);
}

// ────────────────────────────────────────────────────────────────────
// Opus encoder
// ────────────────────────────────────────────────────────────────────

bool VoiceInputService::init_opus() {
    opus_encoder_ = xiaozhi_opus_encoder_create(&encoder_frame_size_, &encoder_outbuf_size_);
    return opus_encoder_ != nullptr;
}

void VoiceInputService::deinit_opus() {
    if (opus_encoder_) {
        xiaozhi_opus_encoder_destroy(opus_encoder_);
        opus_encoder_ = nullptr;
    }
}

// ────────────────────────────────────────────────────────────────────
// Silence detection
// ────────────────────────────────────────────────────────────────────

void VoiceInputService::reset_silence() { silence_start_ms_ = 0; }

bool VoiceInputService::is_silence(const int16_t* buf, int samples) {
    if (samples <= 0) return true;
    int64_t sum_sq = 0;
    for (int i = 0; i < samples; ++i) sum_sq += (int32_t)buf[i] * buf[i];
    double rms = std::sqrt((double)sum_sq / samples);
    return rms < 150.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// STT session (recording task → xiaozhi's WebSocket → reply on xiaozhi task)
// ─────────────────────────────────────────────────────────────────────────────

void VoiceInputService::on_stt_reply(const std::string& text)
{
    // Runs on xiaozhi's main task. Copy the text under the lock, then signal.
    {
        std::lock_guard<std::mutex> lk(stt_mu_);
        stt_text_ = text;
    }
    stt_received_ = true;
    if (stt_sem_) xSemaphoreGive(stt_sem_);
}

bool VoiceInputService::send_listen_session()
{
    ESP_LOGI(TAG, "STT: send_listen_session start, opus_packets=%zu",
             opus_packets_.size());
    if (!stt_sem_) {
        ESP_LOGE(TAG, "stt semaphore not initialised");
        return false;
    }

    // 0. Make sure the audio channel is open. The user may have clicked the
    //    mic after xiaozhi finished a conversation — in that case the server
    //    sent a "goodbye" message and the protocol's UDP transport is gone,
    //    so we have to re-open it before we can stream audio. We track
    //    whether we opened it (vs. it was already open) so we only close it
    //    if we were the one to open it.
    bool we_opened_channel = false;
    if (!xiaozhi_protocol_is_open()) {
        ESP_LOGI(TAG, "STT: audio channel closed, re-opening for STT");
        if (!xiaozhi_open_audio_channel()) {
            ESP_LOGE(TAG, "STT: failed to open audio channel (network/credentials?)");
            return false;
        }
        we_opened_channel = true;
        ESP_LOGI(TAG, "STT: audio channel opened");
    }

    // Register our reply handler for the duration of this session.
    s_active_stt_session = this;
    auto cb = [this](const std::string& text) {
        if (s_active_stt_session == this) s_active_stt_session->on_stt_reply(text);
    };
    xiaozhi_register_stt_callback(cb);
    ESP_LOGI(TAG, "STT: callback registered");

    // 1. listen start (manual mode = wait for explicit stop)
    if (!xiaozhi_send_listen_start(0)) {
        ESP_LOGE(TAG, "send_listen_start failed");
        xiaozhi_register_stt_callback(nullptr);
        s_active_stt_session = nullptr;
        if (we_opened_channel) xiaozhi_close_audio_channel();
        return false;
    }
    ESP_LOGI(TAG, "STT: listen_start sent");

    // 2. stream every recorded opus frame as a binary audio packet
    int sent = 0;
    int total_bytes = 0;
    for (const auto& pkt : opus_packets_) {
        if (!pkt.empty()) {
            if (!xiaozhi_send_audio(pkt.data(), pkt.size())) {
                ESP_LOGW(TAG, "STT: send_audio[%d] failed", sent);
            }
            sent++;
            total_bytes += pkt.size();
        }
    }
    ESP_LOGI(TAG, "STT: sent %d audio packets, %d bytes", sent, total_bytes);

    // 3. listen stop — server will emit one {"type":"stt","text":...} shortly
    if (!xiaozhi_send_listen_stop()) {
        ESP_LOGE(TAG, "send_listen_stop failed");
        xiaozhi_register_stt_callback(nullptr);
        s_active_stt_session = nullptr;
        if (we_opened_channel) xiaozhi_close_audio_channel();
        return false;
    }
    ESP_LOGI(TAG, "STT: listen_stop sent, waiting for reply (timeout %d ms)",
             config_.stt_response_timeout_ms);

    // 4. wait for reply (with timeout)
    TickType_t ticks = pdMS_TO_TICKS(config_.stt_response_timeout_ms);
    bool got = xSemaphoreTake(stt_sem_, ticks) == pdTRUE;

    // Detach the callback. The lambda captured `this`, so make sure it won't
    // fire after we return by nulling the global session pointer first.
    s_active_stt_session = nullptr;
    xiaozhi_register_stt_callback(nullptr);

    if (!got) {
        ESP_LOGE(TAG, "STT reply timeout (%d ms)", config_.stt_response_timeout_ms);
        if (we_opened_channel) xiaozhi_close_audio_channel();
        return false;
    }
    if (!stt_received_) {
        ESP_LOGE(TAG, "semaphore given but no text received");
        if (we_opened_channel) xiaozhi_close_audio_channel();
        return false;
    }
    ESP_LOGI(TAG, "STT result: '%s'", stt_text_.c_str());
    if (we_opened_channel) xiaozhi_close_audio_channel();
    return true;
}

} // namespace voice_input
