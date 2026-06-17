#include "xiaozhi_ctl.h"

#include "application.h"
#include "boards/common/board.h"
#include "protocol.h"
#include "tab5_bridge_lcd.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstring>

static const char* TAG = "xiaozhi_ctl";
static TaskHandle_t s_task = nullptr;

// Set true once app.Initialize() has returned, i.e. the event group + audio
// service exist. Scheduling work (suspend/resume) before this would crash.
static volatile bool s_initialized = false;
// True while the audio pipeline is running; false after a suspend.
static volatile bool s_running = false;

// Shared battery state updated by the main app via xiaozhi_set_battery_percent().
static int s_battery_percent = -1;

// Synchronisation between the calling (recording) task and xiaozhi's main
// task, used by xiaozhi_open_audio_channel() to wait for the MQTT hello /
// UDP bind to complete. Lazily created the first time it's needed.
static SemaphoreHandle_t s_open_sem  = nullptr;
static std::atomic<bool> s_open_done{false};
static std::atomic<bool> s_open_ok{false};

static void xiaozhi_task_fn(void*)
{
    auto& app = Application::GetInstance();
    app.Initialize();
    s_initialized = true;
    s_running = true;
    app.Run();
    vTaskDelete(nullptr);
}

extern "C" void xiaozhi_start_task(void)
{
    if (s_task != nullptr) return;
    ESP_LOGI(TAG, "starting xiaozhi task");
    // 32 KB stack: TLS/websocket + LVGL UI setup + esp_srmodel_init (SPIFFS mount).
    xTaskCreate(xiaozhi_task_fn, "xiaozhi", 32768, nullptr, 5, &s_task);
}

extern "C" void xiaozhi_suspend(void)
{
    if (!s_initialized || !s_running) return;
    s_running = false;
    ESP_LOGI(TAG, "suspend: stopping audio service + closing protocol");
    auto& app = Application::GetInstance();
    // Mirror the OTA-failure path (Application::UpgradeFirmware): just stop the
    // audio service (frees the input/output/opus task stacks + stops the mic/AFE),
    // keeping the protocol ALIVE. Do NOT ResetProtocol(): with protocol_ == null,
    // HandleWakeWordDetectedEvent()/ToggleChat() early-return, so a reset would
    // make xiaozhi unable to wake or talk after resume. Runs on the xiaozhi main
    // task so it is serialized with the event loop.
    app.Schedule([&app]() {
        app.AbortSpeaking(kAbortReasonNone);
        app.GetAudioService().Stop();   // stop input/output/opus tasks, clear queues
    });
}

extern "C" void xiaozhi_resume(void)
{
    if (!s_initialized || s_running) return;
    s_running = true;
    ESP_LOGI(TAG, "resume: restarting audio service");
    auto& app = Application::GetInstance();
    app.Schedule([&app]() {
        auto& as = app.GetAudioService();
        as.Start();                      // recreate input/output/opus tasks
        as.EnableWakeWordDetection(true);// start listening for "你好小智" again
        app.SetDeviceState(kDeviceStateIdle);
    });
}

extern "C" void xiaozhi_activate_screen(void)
{
    auto* display = static_cast<Tab5BridgeLcdDisplay*>(
        Board::GetInstance().GetDisplay()
    );
    if (display) {
        display->ActivateScreen();
    }
}

extern "C" void xiaozhi_set_battery_percent(int percent)
{
    s_battery_percent = percent;
}

extern "C" int xiaozhi_get_battery_percent(void)
{
    return s_battery_percent;
}

extern "C" void xiaozhi_set_speaker_volume(int volume)
{
    if (!s_initialized) return;
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec) codec->SetOutputVolume(volume);
}

extern "C" int xiaozhi_get_speaker_volume(void)
{
    if (!s_initialized) return 70;
    auto* codec = Board::GetInstance().GetAudioCodec();
    return codec ? codec->output_volume() : 70;
}

extern "C" bool xiaozhi_is_active(void)
{
    if (!s_initialized) return false;
    auto st = Application::GetInstance().GetDeviceState();
    return st == kDeviceStateConnecting
        || st == kDeviceStateListening
        || st == kDeviceStateSpeaking;
}

extern "C" bool xiaozhi_is_initialized(void)
{
    return s_initialized;
}

extern "C" void xiaozhi_codec_enable_input(bool enable)
{
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec) codec->EnableInput(enable);
}

extern "C" int xiaozhi_codec_read(int16_t* buf, int samples)
{
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec) return 0;
    std::vector<int16_t> tmp(samples);
    if (!codec->InputData(tmp)) return 0;
    int n = std::min((int)tmp.size(), samples);
    memcpy(buf, tmp.data(), n * sizeof(int16_t));
    return n;
}

extern "C" void* xiaozhi_opus_encoder_create(int* frame_size_out, int* outbuf_size_out)
{
    esp_opus_enc_config_t cfg = {
        .sample_rate      = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel          = ESP_AUDIO_MONO,
        .bits_per_sample  = ESP_AUDIO_BIT16,
        .bitrate          = ESP_OPUS_BITRATE_AUTO,
        .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity       = 0,
        .enable_fec       = false,
        .enable_dtx       = true,
        .enable_vbr       = true,
    };
    void* enc = nullptr;
    auto ret = esp_opus_enc_open(&cfg, sizeof(cfg), &enc);
    if (ret != 0 || !enc) {
        ESP_LOGE(TAG, "opus encoder create failed: %d", ret);
        return nullptr;
    }
    esp_opus_enc_get_frame_size(enc, frame_size_out, outbuf_size_out);
    ESP_LOGI(TAG, "opus encoder created: frame_size=%d outbuf=%d", *frame_size_out, *outbuf_size_out);
    return enc;
}

extern "C" void xiaozhi_opus_encoder_destroy(void* enc)
{
    if (enc) esp_opus_enc_close(enc);
}

extern "C" int xiaozhi_opus_encode(void* enc, int16_t* pcm, uint8_t* outbuf, int outbuf_size)
{
    if (!enc || !pcm || !outbuf) return 0;
    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t*)pcm,
        .len = (uint32_t)(960 * sizeof(int16_t)),  // 60ms @ 16kHz mono
    };
    esp_audio_enc_out_frame_t out = {
        .buffer = outbuf,
        .len = (uint32_t)outbuf_size,
        .encoded_bytes = 0,
    };
    auto ret = esp_opus_enc_process(enc, &in, &out);
    if (ret != 0) return 0;
    return (int)out.encoded_bytes;
}

/* ── STT-only listen session helpers ────────────────────────────────────── */

extern "C" bool xiaozhi_protocol_is_open(void)
{
    if (!s_initialized) return false;
    auto* p = Application::GetInstance().GetProtocol();
    return p != nullptr && p->IsAudioChannelOpened();
}

extern "C" bool xiaozhi_send_listen_start(int mode)
{
    if (!s_initialized) return false;
    auto& app = Application::GetInstance();
    auto* p = app.GetProtocol();
    if (!p || !p->IsAudioChannelOpened()) {
        ESP_LOGW(TAG, "send_listen_start: protocol not open");
        return false;
    }
    ListeningMode m;
    switch (mode) {
        case 0:  m = kListeningModeManualStop; break;
        case 1:  m = kListeningModeAutoStop;    break;
        default: m = kListeningModeRealtime;   break;
    }
    // Schedule on xiaozhi's main task to serialize with the event loop.
    app.Schedule([p, m]() {
        p->SendStartListening(m);
    });
    ESP_LOGI(TAG, "listen start scheduled (mode=%d)", mode);
    return true;
}

extern "C" bool xiaozhi_send_audio(const uint8_t* data, size_t len)
{
    if (!s_initialized || !data || len == 0) return false;
    auto& app = Application::GetInstance();
    auto* p = app.GetProtocol();
    if (!p || !p->IsAudioChannelOpened()) return false;

    // Copy the packet payload before scheduling (caller may free after return).
    auto payload = std::vector<uint8_t>(data, data + len);
    app.Schedule([p, payload = std::move(payload)]() mutable {
        auto pkt = std::make_unique<AudioStreamPacket>();
        pkt->sample_rate    = 16000;
        pkt->frame_duration = 60;
        pkt->timestamp      = 0;
        pkt->payload        = std::move(payload);
        p->SendAudio(std::move(pkt));
    });
    return true;
}

extern "C" bool xiaozhi_send_listen_stop(void)
{
    if (!s_initialized) return false;
    auto& app = Application::GetInstance();
    auto* p = app.GetProtocol();
    if (!p || !p->IsAudioChannelOpened()) return false;
    app.Schedule([p]() {
        p->SendStopListening();
    });
    ESP_LOGI(TAG, "listen stop scheduled");
    return true;
}

// xiaozhi_register_stt_callback uses std::function (C++ only) so it cannot
// have C linkage — declared without extern "C" here to match the header.
void xiaozhi_register_stt_callback(std::function<void(const std::string&)> cb)
{
    auto& app = Application::GetInstance();
    app.RegisterSttCallback(std::move(cb));
}

bool xiaozhi_open_audio_channel(void)
{
    if (!s_initialized) return false;
    auto* p = Application::GetInstance().GetProtocol();
    if (p == nullptr) {
        ESP_LOGW(TAG, "open_audio_channel: protocol not initialised");
        return false;
    }
    if (p->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "open_audio_channel: already open");
        return true;
    }

    if (s_open_sem == nullptr) {
        s_open_sem = xSemaphoreCreateBinary();
        if (s_open_sem == nullptr) {
            ESP_LOGE(TAG, "open_audio_channel: failed to create semaphore");
            return false;
        }
    }

    s_open_done.store(false);
    s_open_ok.store(false);
    auto& app = Application::GetInstance();
    app.Schedule([&app]() {
        auto* proto = app.GetProtocol();
        bool ok = false;
        if (proto != nullptr) {
            if (proto->IsAudioChannelOpened()) {
                ok = true;
            } else {
                ok = proto->OpenAudioChannel();
            }
        }
        s_open_ok.store(ok);
        s_open_done.store(true);
        xSemaphoreGive(s_open_sem);
    });

    if (xSemaphoreTake(s_open_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "open_audio_channel: timed out waiting for handshake");
        return false;
    }
    bool ok = s_open_ok.load();
    ESP_LOGI(TAG, "open_audio_channel: %s", ok ? "ok" : "failed");
    return ok;
}

void xiaozhi_close_audio_channel(void)
{
    if (!s_initialized) return;
    auto& app = Application::GetInstance();
    app.Schedule([&app]() {
        auto* p = app.GetProtocol();
        if (p != nullptr && p->IsAudioChannelOpened()) {
            p->CloseAudioChannel();
        }
    });
}
