/* Minimal stub — not used on Tab5 */
#pragma once
#include "emote_init.h"

#define EMOTE_MGR_EVT_IDLE     "evt_idle"
#define EMOTE_MGR_EVT_SPEAK    "evt_speak"
#define EMOTE_MGR_EVT_LISTEN   "evt_listen"
#define EMOTE_MGR_EVT_SYS      "evt_sys"
#define EMOTE_MGR_EVT_SET      "evt_set"
#define EMOTE_MGR_EVT_BAT      "evt_bat"
#define EMOTE_MGR_EVT_QRCODE   "evt_qrcode"

#define EMT_DEF_ELEM_DEFAULT_LABEL  "default_label"
#define EMT_DEF_ELEM_EYE_ANIM       "eye_anim"
#define EMT_DEF_ELEM_EMERG_DLG      "emerg_dlg"
#define EMT_DEF_ELEM_TOAST_LABEL    "toast_label"
#define EMT_DEF_ELEM_CLOCK_LABEL    "clock_label"
#define EMT_DEF_ELEM_LISTEN_ANIM    "listen_anim"
#define EMT_DEF_ELEM_STATUS_ICON    "status_icon"
#define EMT_DEF_ELEM_CHARGE_ICON    "charge_icon"
#define EMT_DEF_ELEM_BAT_LEFT_LABEL "battery_label"
#define EMT_DEF_ELEM_TIMER_STATUS   "clock_timer"
#define EMT_DEF_ELEM_QRCODE         "qrcode"

#define EMOTE_OBJ_TYPE_ANIM    "anim"
#define EMOTE_OBJ_TYPE_IMAGE   "image"
#define EMOTE_OBJ_TYPE_LABEL   "label"
#define EMOTE_OBJ_TYPE_QRCODE  "qrcode"
#define EMOTE_OBJ_TYPE_TIMER   "timer"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t emote_set_anim_emoji(emote_handle_t h, const char *name);
esp_err_t emote_set_qrcode_data(emote_handle_t h, const char *text);
esp_err_t emote_set_dialog_anim(emote_handle_t h, const char *name);
esp_err_t emote_set_event_msg(emote_handle_t h, const char *event, const char *msg);
esp_err_t emote_stop_anim_dialog(emote_handle_t h);
esp_err_t emote_insert_anim_dialog(emote_handle_t h, const char *name, uint32_t ms);
esp_err_t emote_set_theme(emote_handle_t h, const char *theme);
esp_err_t emote_set_battery(emote_handle_t h, int level, bool charging);
esp_err_t emote_set_notification(emote_handle_t h, const char *text, int ms);
esp_err_t emote_set_power_save(emote_handle_t h, bool on);
esp_err_t emote_update_status_bar(emote_handle_t h, bool all);

#ifdef __cplusplus
}
#endif
