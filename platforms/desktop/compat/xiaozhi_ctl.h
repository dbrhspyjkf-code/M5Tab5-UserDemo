#pragma once
// Desktop simulator stub of the tab5 xiaozhi_core control API.
// The real implementation lives in
// platforms/tab5/components/xiaozhi_core/xiaozhi_ctl.{h,cc} and drives the
// audio pipeline + bridge display, which only exist on the ESP32-P4 hardware.
// On desktop these are no-ops so the launcher / settings / xiaozhi app still
// build and run in the SDL simulator.
#ifdef __cplusplus
extern "C" {
#endif

void xiaozhi_start_task(void);
bool xiaozhi_is_initialized(void);
void xiaozhi_activate_screen(void);
void xiaozhi_suspend(void);
void xiaozhi_resume(void);
void xiaozhi_set_battery_percent(int percent);
int  xiaozhi_get_battery_percent(void);
void xiaozhi_set_speaker_volume(int volume);
int  xiaozhi_get_speaker_volume(void);
bool xiaozhi_is_active(void);

#ifdef __cplusplus
}
#endif
