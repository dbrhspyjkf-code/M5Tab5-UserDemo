// Desktop stub for <driver/gpio.h> from ESP-IDF. app_unit_puzzle uses a few
// gpio_* symbols and GPIO_NUM_* constants; the desktop build doesn't drive
// real pins, so all of them are no-ops / inert integers. Lives under
// desktop/compat/driver/ so `#include <driver/gpio.h>` resolves on desktop.

#pragma once
#include <cstdint>

typedef int gpio_num_t;

#define GPIO_NUM_NC  -1
#define GPIO_NUM_0   0
#define GPIO_NUM_1   1
#define GPIO_NUM_2   2
// ... numbers 3..52 omitted; only the ones referenced by app_unit_puzzle and
// any other app are listed here.
#define GPIO_NUM_53  53
#define GPIO_NUM_6   6
#define GPIO_NUM_22  22
#define GPIO_NUM_50  50

enum gpio_mode_t   { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT };
enum gpio_pullup_t { GPIO_FLOATING = 0, GPIO_PULLUP_ONLY, GPIO_PULLDOWN_ONLY, GPIO_BOTH_PULLUP, GPIO_BOTH_PULLDOWN, GPIO_PULLUP_ENABLE, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_ENABLE, GPIO_PULLDOWN_DISABLE };
enum gpio_int_type_t { GPIO_INTR_DISABLE = 0, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE, GPIO_INTR_ANYEDGE, GPIO_INTR_LOW_LEVEL, GPIO_INTR_HIGH_LEVEL };

inline int gpio_reset_pin(int) { return 0; }
inline int gpio_set_direction(int, int) { return 0; }
inline int gpio_set_level(int, int) { return 0; }
inline int gpio_set_pull_mode(int, int) { return 0; }
inline int gpio_set_intr_type(int, int) { return 0; }
inline int gpio_install_isr_service(int) { return 0; }
inline int gpio_isr_handler_add(int, void (*)(void*), void*) { return 0; }
