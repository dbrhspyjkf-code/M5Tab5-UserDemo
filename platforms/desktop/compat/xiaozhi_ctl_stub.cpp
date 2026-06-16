// Desktop simulator no-op implementation of the xiaozhi control API.
// See xiaozhi_ctl.h — the real audio/bridge logic only runs on ESP32-P4.
#include "xiaozhi_ctl.h"

static int s_batt_pct = -1;
static int s_volume   = 60;

void xiaozhi_start_task(void) {}
void xiaozhi_activate_screen(void) {}
void xiaozhi_suspend(void) {}
void xiaozhi_resume(void) {}
void xiaozhi_set_battery_percent(int percent) { s_batt_pct = percent; }
int  xiaozhi_get_battery_percent(void) { return s_batt_pct; }
void xiaozhi_set_speaker_volume(int volume) { s_volume = volume; }
int  xiaozhi_get_speaker_volume(void) { return s_volume; }
bool xiaozhi_is_active(void) { return false; }
