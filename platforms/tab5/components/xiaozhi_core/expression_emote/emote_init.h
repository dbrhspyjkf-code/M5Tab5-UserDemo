/* Minimal stub for espressif2022/esp_emote_expression — not used on Tab5 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct emote_s *emote_handle_t;

typedef enum { EMOTE_SOURCE_PATH = 0, EMOTE_SOURCE_PARTITION } emote_source_type_t;

typedef struct {
    emote_source_type_t type;
    union { const char *path; const char *partition_label; } source;
    struct { uint8_t mmap_enable: 1; } flags;
} emote_data_t;

typedef void (*emote_flush_ready_cb_t)(int x1, int y1, int x2, int y2,
                                       const void *data, emote_handle_t h);
typedef void (*emote_update_cb_t)(gfx_player_event_t event,
                                   const void *obj, emote_handle_t h);

typedef struct {
    struct { bool swap; bool double_buffer; bool buff_dma; bool buff_spiram; } flags;
    struct { int h_res; int v_res; int fps; } gfx_emote;
    struct { size_t buf_pixels; } buffers;
    struct { int task_priority; int task_stack; int task_affinity;
             bool task_stack_in_ext; } task;
    emote_flush_ready_cb_t flush_cb;
    emote_update_cb_t      update_cb;
    void                  *user_data;
} emote_config_t;

emote_handle_t emote_init(const emote_config_t *config);
bool           emote_deinit(emote_handle_t handle);
bool           emote_is_initialized(emote_handle_t handle);
void          *emote_get_user_data(emote_handle_t handle);
void           emote_notify_flush_finished(emote_handle_t handle);
void           emote_notify_all_refresh(emote_handle_t handle);

#ifdef __cplusplus
}
#endif
