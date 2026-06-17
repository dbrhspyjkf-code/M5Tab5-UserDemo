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

#ifdef __cplusplus
}
#endif
