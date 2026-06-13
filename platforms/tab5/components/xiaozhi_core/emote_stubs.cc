/* Stub implementations of espressif2022/esp_emote_expression APIs.
 * Tab5 uses LcdDisplay, not EmoteDisplay. These stubs exist so assets.cc
 * and emote_display.cc compile; dynamic_cast<EmoteDisplay*> returns nullptr
 * at runtime since our display is never an EmoteDisplay. */

#include "expression_emote.h"
#include "display/emote_display.h"
#include "display/display.h"
#include <esp_log.h>

// ── Emote C API stubs (all no-ops returning safe values) ──────────────────

extern "C" {

emote_handle_t emote_init(const emote_config_t *) { return nullptr; }
bool           emote_deinit(emote_handle_t)        { return true; }
bool           emote_is_initialized(emote_handle_t){ return false; }
void          *emote_get_user_data(emote_handle_t) { return nullptr; }
void           emote_notify_flush_finished(emote_handle_t) {}
void           emote_notify_all_refresh(emote_handle_t) {}

esp_err_t emote_mount_assets(emote_handle_t, const emote_data_t *) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_unmount_assets(emote_handle_t)                      { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_load_assets(emote_handle_t)                         { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_mount_and_load_assets(emote_handle_t, const emote_data_t *) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_get_icon_data_by_name(emote_handle_t, const char *, icon_data_t **)  { return ESP_ERR_NOT_FOUND; }
esp_err_t emote_get_emoji_data_by_name(emote_handle_t, const char *, emoji_data_t **){ return ESP_ERR_NOT_FOUND; }
esp_err_t emote_get_asset_data_by_name(emote_handle_t, const char *,
                                        const uint8_t **, size_t *)  { return ESP_ERR_NOT_FOUND; }

esp_err_t emote_set_anim_emoji(emote_handle_t, const char *)        { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_set_qrcode_data(emote_handle_t, const char *)       { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_set_dialog_anim(emote_handle_t, const char *)       { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_set_event_msg(emote_handle_t, const char *, const char *) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_stop_anim_dialog(emote_handle_t)                    { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_insert_anim_dialog(emote_handle_t, const char *, uint32_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_set_theme(emote_handle_t, const char *)             { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_set_battery(emote_handle_t, int, bool)              { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_set_notification(emote_handle_t, const char *, int) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_set_power_save(emote_handle_t, bool)                { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t emote_update_status_bar(emote_handle_t, bool)             { return ESP_ERR_NOT_SUPPORTED; }

} // extern "C"

// ── EmoteDisplay stub (vtable + RTTI needed for dynamic_cast in assets.cc) ─

namespace emote {

EmoteDisplay::EmoteDisplay(esp_lcd_panel_handle_t, esp_lcd_panel_io_handle_t,
                           int /*w*/, int /*h*/)
    : Display() {}

EmoteDisplay::~EmoteDisplay() {}

void EmoteDisplay::SetEmotion(const char *) {}
void EmoteDisplay::SetStatus(const char *) {}
void EmoteDisplay::SetChatMessage(const char *, const char *) {}
void EmoteDisplay::SetTheme(Theme *) {}
void EmoteDisplay::ShowNotification(const char *, int) {}
void EmoteDisplay::UpdateStatusBar(bool) {}
void EmoteDisplay::SetPowerSaveMode(bool) {}
void EmoteDisplay::SetPreviewImage(const void *) {}
bool EmoteDisplay::StopAnimDialog() { return false; }
bool EmoteDisplay::InsertAnimDialog(const char *, uint32_t) { return false; }
void EmoteDisplay::RefreshAll() {}
bool EmoteDisplay::Lock(int) { return false; }
void EmoteDisplay::Unlock() {}

} // namespace emote
