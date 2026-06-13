#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the xiaozhi Application task (once; safe to call multiple times).
 * Creates an 8 KB FreeRTOS task that calls Application::Initialize + Run.
 */
void xiaozhi_start_task(void);

/**
 * Activate xiaozhi's dedicated LVGL screen on the bridge display.
 */
void xiaozhi_activate_screen(void);

#ifdef __cplusplus
}
#endif
