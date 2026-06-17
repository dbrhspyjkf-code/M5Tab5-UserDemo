#pragma once
#include <stdint.h>
#include <stddef.h>
#include <functional>
#include <string>
#ifdef __cplusplus
extern "C" {
#endif

/** Start the xiaozhi Application task (once; safe to call multiple times). */
void xiaozhi_start_task(void);

/** Activate xiaozhi's dedicated LVGL screen on the bridge display. */
void xiaozhi_activate_screen(void);

/**
 * Suspend xiaozhi when leaving the app: stop the audio input/output/opus tasks,
 * clear their queues, and close the network/audio channel. This stops the mic
 * pipeline (so it no longer listens/talks in the background) and releases the
 * audio task stacks + DMA so a heavy app like HA doesn't OOM. Idempotent.
 */
void xiaozhi_suspend(void);

/**
 * Resume xiaozhi when re-entering the app: restart the audio service and
 * re-enable wake word detection. Idempotent; no-op before the first start or
 * while already running.
 */
void xiaozhi_resume(void);

/**
 * Update battery percent shown in xiaozhi's info bar.
 * Call from the main application (e.g. onRunning every ~10 s).
 * Pass -1 to indicate unknown/unavailable.
 */
void xiaozhi_set_battery_percent(int percent);

/** Read the last value set by xiaozhi_set_battery_percent(). */
int  xiaozhi_get_battery_percent(void);

/** Set speaker output volume (0-100) via the Application audio codec. */
void xiaozhi_set_speaker_volume(int volume);

/** Get current speaker output volume (0-100). */
int  xiaozhi_get_speaker_volume(void);

/**
 * True while xiaozhi is in an active conversation (connecting / listening /
 * speaking). Used to dismiss the screensaver when woken by voice.
 */
bool xiaozhi_is_active(void);

/**
 * True after the xiaozhi Application task has been started and initialized
 * (i.e. the audio codec + event group are ready). Safe to call before
 * xiaozhi_start_task() — will return false.
 */
bool xiaozhi_is_initialized(void);

/**
 * Enable or disable the mic input (ES7210 ADC).
 * Safe to call even if xiaozhi is not initialized — falls back to direct
 * Board access.
 */
void xiaozhi_codec_enable_input(bool enable);

/**
 * Read PCM samples from the audio codec input.
 * @param buf    Destination buffer (int16_t)
 * @param samples Number of samples to read
 * @return Actual number of samples read, or 0 on failure.
 */
int  xiaozhi_codec_read(int16_t* buf, int samples);

/**
 * Opus encoder wrappers (thin veneer over esp_opus_enc_*).
 * Caller must pair create/destroy. Encoder is 16kHz mono 60ms frames.
 */
void* xiaozhi_opus_encoder_create(int* frame_size_out, int* outbuf_size_out);
void  xiaozhi_opus_encoder_destroy(void* enc);
int   xiaozhi_opus_encode(void* enc, int16_t* pcm, uint8_t* outbuf, int outbuf_size);

/* ------------------------------------------------------------------------- */
/* STT-only listen session (used by the voice input service).                 */
/* These send messages through xiaozhi's existing WebSocket connection, so   */
/* no extra TLS handshake / extra internal RAM is required. Callers must     */
/* have invoked xiaozhi_suspend() first to free the audio pipeline, then    */
/* these calls piggy-back on the still-open protocol channel.                */
/* ------------------------------------------------------------------------- */

/**
 * Send a "listen start" message through xiaozhi's protocol.
 * @param mode 0=manual_stop, 1=auto_stop, 2=realtime. Use 0 for STT-only.
 * @return true if the protocol is open and the message was scheduled.
 */
bool xiaozhi_send_listen_start(int mode);

/**
 * Send a raw Opus frame (16kHz mono, 60ms) as a binary audio packet.
 * The buffer must remain valid for the duration of the call only; the
 * implementation copies it before returning.
 * @return true if scheduled successfully.
 */
bool xiaozhi_send_audio(const uint8_t* data, size_t len);

/**
 * Send a "listen stop" message. The server will then emit a single
 * {"type":"stt","text":"..."} response on the same WebSocket.
 * @return true if scheduled successfully.
 */
bool xiaozhi_send_listen_stop(void);

/**
 * True while the protocol channel is open and we are not in an error state.
 * Callers can poll this before sending a listen session.
 */
bool xiaozhi_protocol_is_open(void);

#ifdef __cplusplus
}
#endif

/* C++-only APIs. */
#ifdef __cplusplus

/**
 * Open the protocol's audio channel on demand if it isn't already open.
 *
 * Background: when the user clicks the mic button on Claude, xiaozhi may
 * have closed the audio channel after the last conversation ended (server
 * sent a "goodbye" message). Trying to STT over a closed channel fails.
 * This API re-opens it via the normal handshake (MQTT hello + UDP bind for
 * mqtt_protocol) so STT can proceed.
 *
 * Blocks the calling task for up to 5 s while the handshake runs on
 * xiaozhi's main task; the audio service is assumed suspended by the caller
 * (so the main task is otherwise idle and serialised).
 *
 * @return true on success, false on timeout / failure.
 */
bool xiaozhi_open_audio_channel(void);

/**
 * Close the audio channel. Schedules CloseAudioChannel() on xiaozhi's main
 * task and returns immediately. Safe to call even if the channel is not
 * open (no-op).
 */
void xiaozhi_close_audio_channel(void);

/**
 * Register a callback to be invoked on xiaozhi's main task whenever an STT
 * result is received. Pass nullptr to unregister. Only one callback is
 * supported. The string is owned by xiaozhi; copy if needed.
 */
void xiaozhi_register_stt_callback(std::function<void(const std::string&)> cb);
#endif
