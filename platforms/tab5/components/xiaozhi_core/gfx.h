/* Minimal stub for espressif2022/esp_emote_gfx — not used on Tab5 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gfx_s *gfx_handle_t;

typedef enum {
    GFX_PLAYER_EVENT_NONE = 0,
    GFX_PLAYER_EVENT_STARTED,
    GFX_PLAYER_EVENT_STOPPED,
    GFX_PLAYER_EVENT_PAUSED,
    GFX_PLAYER_EVENT_RESUMED,
    GFX_PLAYER_EVENT_COMPLETED,
} gfx_player_event_t;

#ifdef __cplusplus
}
#endif
