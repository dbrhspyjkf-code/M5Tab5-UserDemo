// Desktop stub for the ESP-IDF led_strip component (espressif__led_strip).
// Unit-Puzzle's app_unit_puzzle.cpp always includes <led_strip.h> because the
// driver init path is shared between the device and the sim, so the sim needs
// a header that satisfies the typedefs and (statically) the few APIs that
// aren't already wrapped in #ifndef PLATFORM_BUILD_DESKTOP guards in the .cpp.
// Nothing in the desktop build actually drives a strip — _stripInit() takes
// the no-op path on this platform.

#pragma once
#include <cstdint>

typedef void* led_strip_handle_t;

// Mirror the real struct shapes (only the fields touched by app_unit_puzzle).
struct led_strip_config_t {
    int         strip_gpio_num         = 0;
    int         max_leds               = 0;
    int         led_model              = 0;   // LED_MODEL_*
    int         color_component_format = 0;   // LED_STRIP_COLOR_COMPONENT_FMT_*
};
struct led_strip_rmt_config_t {
    uint32_t resolution_hz     = 0;
    int      mem_block_symbols = 0;
};

#define LED_MODEL_WS2812                       0
#define LED_STRIP_COLOR_COMPONENT_FMT_GRB      0

// All return 0 (ESP_OK) so the rest of the app behaves as if init succeeded.
inline int led_strip_clear(led_strip_handle_t)                       { return 0; }
inline int led_strip_refresh(led_strip_handle_t)                     { return 0; }
inline int led_strip_set_pixel(led_strip_handle_t, int, int, int, int) { return 0; }
inline int led_strip_del(led_strip_handle_t)                         { return 0; }
inline int led_strip_new_rmt_device(const led_strip_config_t*,
                                    const led_strip_rmt_config_t*,
                                    led_strip_handle_t*)            { return 0; }
