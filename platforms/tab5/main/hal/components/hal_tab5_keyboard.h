#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the M5Stack Tab5 official keyboard over I2C (address 0x6D,
 * SDA=G0, SCL=G1, INT=G50) and start a background task that forwards
 * key events into the LVGL keyboard group. Idempotent: safe to call
 * from any startup hook.
 *
 * Returns true if the keyboard was found and the polling task is
 * running; false if the chip didn't respond (e.g. keyboard not
 * attached, or the I2C bus couldn't be created on G0/G1).
 */
bool hal_tab5_keyboard_init(void);

// ── 原始 HID 事件钩子 (2026-10-29, app_ssh 终端输入用) ──
// 设置后每个按键事件先交给回调; 返回 true = 已消费, 不再进 LVGL 组。
// 回调运行在 kb_poll 任务(非 LVGL 线程): 不得触碰 LVGL, 只能入队/置原子量。
// shift 参数已并入 hal 侧的 CapsLock 粘滞状态, 回调不用再管。
// 传 cb=nullptr 清除。
typedef bool (*hal_tab5_kb_raw_cb_t)(uint8_t modifier, uint8_t keycode,
                                     bool shift, void* user);
void hal_tab5_keyboard_set_raw_sink(hal_tab5_kb_raw_cb_t cb, void* user);

#ifdef __cplusplus
}
#endif
