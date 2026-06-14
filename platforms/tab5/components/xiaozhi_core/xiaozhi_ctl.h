#pragma once
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

#ifdef __cplusplus
}
#endif
