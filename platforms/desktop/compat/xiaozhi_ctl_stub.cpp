// Desktop simulator no-op implementation of the xiaozhi control API.
// See xiaozhi_ctl.h — the real audio/bridge logic only runs on ESP32-P4.
#include "xiaozhi_ctl.h"

static int s_batt_pct = -1;
static int s_volume   = 60;

void xiaozhi_start_task(void) {}
bool xiaozhi_is_initialized(void) { return true; }
void xiaozhi_activate_screen(void) {}
void xiaozhi_deactivate_screen(void) {}
int  xiaozhi_get_device_state(void) { return 3; /* kDeviceStateIdle */ }
void xiaozhi_ensure_output(void) {}
void xiaozhi_stop_output(void) {}
void xiaozhi_suspend(void) {}
void xiaozhi_resume(void) {}
void xiaozhi_set_battery_percent(int percent) { s_batt_pct = percent; }
int  xiaozhi_get_battery_percent(void) { return s_batt_pct; }
void xiaozhi_set_speaker_volume(int volume) { s_volume = volume; }
int  xiaozhi_get_speaker_volume(void) { return s_volume; }
bool xiaozhi_is_active(void) { return false; }

// C++ only stubs
void xiaozhi_register_stt_callback(std::function<void(const std::string&)> cb) {}
void xiaozhi_register_wake_callback(std::function<void(const std::string&)> cb) {}
void xiaozhi_ctl_fire_wake_callback(const std::string& word) {}
