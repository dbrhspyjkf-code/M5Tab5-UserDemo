/* Minimal stub — not used on Tab5 */
#pragma once
#include "emote_init.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { const void *data; size_t size; }               icon_data_t;
typedef struct { const void *data; size_t size;
                 uint8_t fps; bool loop; }                       emoji_data_t;

esp_err_t emote_mount_assets(emote_handle_t h, const emote_data_t *d);
esp_err_t emote_unmount_assets(emote_handle_t h);
esp_err_t emote_load_assets(emote_handle_t h);
esp_err_t emote_mount_and_load_assets(emote_handle_t h, const emote_data_t *d);
esp_err_t emote_get_icon_data_by_name(emote_handle_t h, const char *name, icon_data_t **out);
esp_err_t emote_get_emoji_data_by_name(emote_handle_t h, const char *name, emoji_data_t **out);
esp_err_t emote_get_asset_data_by_name(emote_handle_t h, const char *name,
                                        const uint8_t **data, size_t *size);

#ifdef __cplusplus
}
#endif
