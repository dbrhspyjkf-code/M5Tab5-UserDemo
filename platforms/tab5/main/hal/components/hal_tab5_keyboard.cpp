/***************************************************
 * M5Stack Tab5 official keyboard → LVGL integration.
 *
 * Reference: https://github.com/m5stack/M5Tab5-Keyboard-UserDemo
 *
 * The Tab5 keyboard has its own STM32F030C8T6 that scans the 5x14
 * matrix and exposes events over I2C. We use **HID mode (mode=1)**
 * so the keyboard's firmware converts (row, col) → (modifier, keycode)
 * per the USB HID Usage Tables spec — no hand-written 5×14 keymap
 * needed and the modifier/shift/CapsLock logic is handled correctly.
 *
 * Protocol:
 *   - Address 0x6D
 *   - SDA = G0 (GPIO 0), SCL = G1 (GPIO 1), INT = G50 (GPIO 50)
 *   - KB_REG_KEYBOARD_MODE (0x10) → set mode (0=NORMAL, 1=HID, 2=STRING)
 *   - KB_REG_HID_EVENT   (0x30)  → 2 bytes: [modifier, keycode]
 *
 * The polling task forwards keys to the focused LVGL widget. The
 * Claude app registers its textarea with the HAL keyboard group on
 * open (see app_project_assistant.cpp:348), so it just works once the
 * group is created here.
 ***************************************************/
#include <hal/hal.h>

#include <mooncake_log.h>
#include <esp_log.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include "bsp/esp-bsp.h"

static const char* TAG = "hal_tab5_kb";

// === M5Stack official protocol constants ===
#define KB_ADDR          0x6D
#define KB_I2C_PORT      1        // I2C_NUM_1 (separate from BSP bus on I2C0)
#define KB_SDA_GPIO      0        // G0 (pogo SDA on EXT PORT1)
#define KB_SCL_GPIO      1        // G1 (pogo SCL on EXT PORT1)
#define KB_INT_GPIO      50       // G50 (pogo INT, unused — polling mode)
#define KB_I2C_FREQ_HZ   100000

// Registers
#define KB_REG_INT_CFG        0x00
#define KB_REG_INT_STA        0x01
#define KB_REG_EVENT_NUM      0x02
#define KB_REG_KEYBOARD_MODE  0x10
#define KB_REG_HID_EVENT      0x30

// Modes
#define KB_MODE_NORMAL   0
#define KB_MODE_HID      1
#define KB_MODE_STRING   2

// === USB HID modifier bits (byte 0 of every HID event) ===
#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08
#define MOD_RCTRL   0x10
#define MOD_RSHIFT  0x20
#define MOD_RALT    0x40
#define MOD_RGUI    0x80

static inline bool mod_shift(uint8_t m) { return m & (MOD_LSHIFT | MOD_RSHIFT); }
static inline bool mod_ctrl(uint8_t m)   { return m & (MOD_LCTRL | MOD_RCTRL); }
static inline bool mod_alt(uint8_t m)    { return m & (MOD_LALT | MOD_RALT); }

// === USB HID keycode → ASCII (US-English layout) ===
// Letters 0x04-0x1D handled separately (CapsLock aware).
// Numbers 0x1E-0x27 handled via lookup tables.
static char hid_keycode_to_ascii(uint8_t kc, bool shift)
{
    // Letters
    if (kc >= 0x04 && kc <= 0x1D) {
        char c = 'a' + (kc - 0x04);
        return shift ? (char)(c - 'a' + 'A') : c;
    }
    // Number row
    static const char num_unshifted[] = "1234567890";
    static const char num_shifted[]   = "!@#$%^&*()";
    if (kc >= 0x1E && kc <= 0x27) {
        return shift ? num_shifted[kc - 0x1E] : num_unshifted[kc - 0x1E];
    }
    // Punctuation / whitespace (Enter/Tab are handled by the caller)
    switch (kc) {
        case 0x2C: return ' ';     // Space
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
    }
    return 0;  // unmapped / special
}

// === I2C state ===
static i2c_master_bus_handle_t s_kb_bus     = NULL;
static i2c_master_dev_handle_t s_kb_dev     = NULL;
static volatile bool             s_kb_ready  = false;
static volatile bool             s_caps_lock = false;  // sticky CapsLock

// Read N bytes from a register.
static esp_err_t kb_read_regs(uint8_t reg, uint8_t* buf, size_t len)
{
    return i2c_master_transmit_receive(s_kb_dev, &reg, 1, buf, len, 100);
}

static esp_err_t kb_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_kb_dev, buf, sizeof(buf), 100);
}

// Forward a HID key event to the focused LVGL widget. The HAL keyboard
// group must exist (hal_tab5_keyboard_init creates it if the USB path
// hasn't already).
//
// IMPORTANT: this runs in the kb_poll_task FreeRTOS task, NOT the LVGL
// render task. Any lv_* call that can trigger a redraw (e.g.
// lv_textarea_add_char → lv_obj_invalidate) will assert if it lands
// while LVGL is mid-render. Hold the HAL LVGL mutex around every
// LVGL call below.
static void kb_forward_to_lvgl(uint8_t modifier, uint8_t keycode)
{
    hal::HalBase* hal = GetHAL();
    if (hal == nullptr) return;
    lv_group_t* g = hal->lvKbGroup;
    if (g == nullptr) return;

    // CapsLock — sticky, affects letters only. No LVGL access needed.
    if (keycode == 0x39) {
        s_caps_lock = !s_caps_lock;
        ESP_LOGD(TAG, "CapsLock → %s", s_caps_lock ? "ON" : "OFF");
        return;
    }

    // Skip release / modifier-only events. No LVGL access needed.
    if (keycode == 0) return;

    // Letters get XOR'd with our sticky CapsLock; other keys only respect
    // the modifier byte. No LVGL access needed.
    bool is_letter       = (keycode >= 0x04 && keycode <= 0x1D);
    bool effective_shift = mod_shift(modifier) ^ (is_letter && s_caps_lock);
    char c = hid_keycode_to_ascii(keycode, effective_shift);

    hal->lvglLock();
    {
        lv_obj_t* focused = lv_group_get_focused(g);
        bool is_textarea  = (focused != nullptr) && lv_obj_check_type(focused, &lv_textarea_class);

        // Backspace / Enter / Tab / Forward delete — always handled for textareas.
        if (keycode == 0x2A) {  // Backspace
            if (is_textarea) lv_textarea_delete_char(focused);
            hal->lvglUnlock();
            return;
        }
        if (keycode == 0x28) {  // Enter
            // If the focused app registered an "Enter pressed" hook (e.g.
            // chat apps use it to send), call that and skip LVGL entirely.
            // Otherwise fall back to the textarea default: insert '\n' so
            // the user can compose multi-line messages.
            if (hal->onEnterPressed) {
                hal->onEnterPressed();
            } else if (is_textarea) {
                lv_textarea_add_char(focused, '\n');
            }
            hal->lvglUnlock();
            return;
        }
        if (keycode == 0x2B) {  // Tab
            if (is_textarea) lv_textarea_add_char(focused, '\t');
            hal->lvglUnlock();
            return;
        }
        if (keycode == 0x4C) {  // Delete (forward)
            if (is_textarea) lv_textarea_delete_char_forward(focused);
            hal->lvglUnlock();
            return;
        }
        // Arrow keys — move the cursor in textareas, navigate focus elsewhere.
        if (keycode == 0x50) {  // Left
            if (is_textarea) lv_textarea_cursor_left(focused);
            else             lv_group_send_data(g, LV_KEY_LEFT);
            hal->lvglUnlock();
            return;
        }
        if (keycode == 0x4F) {  // Right
            if (is_textarea) lv_textarea_cursor_right(focused);
            else             lv_group_send_data(g, LV_KEY_RIGHT);
            hal->lvglUnlock();
            return;
        }
        if (keycode == 0x52) {  // Up
            if (is_textarea) lv_textarea_cursor_up(focused);
            hal->lvglUnlock();
            return;
        }
        if (keycode == 0x51) {  // Down
            if (is_textarea) lv_textarea_cursor_down(focused);
            hal->lvglUnlock();
            return;
        }

        if (c == 0) {
            static uint32_t unhandled = 0;
            if (unhandled++ < 30) {
                ESP_LOGW(TAG, "unhandled HID key mod=0x%02X key=0x%02X", modifier, keycode);
            }
            hal->lvglUnlock();
            return;
        }

        if (is_textarea) {
            lv_textarea_add_char(focused, c);
        } else if (focused != nullptr) {
            // Fallback: forward as a key event for non-textarea widgets so
            // things like lv_buttonmatrix can still react to alphanumerics.
            lv_group_send_data(g, (uint32_t)(uint8_t)c);
        }
    }
    hal->lvglUnlock();
}

// Background task: poll the keyboard's HID event queue and forward each
// event to LVGL. INT mode is DISABLED so we don't need GPIO 50 wired —
// polling every 30 ms is fast enough for typing.
static void kb_poll_task(void* arg)
{
    uint32_t polls = 0;
    uint32_t total_events = 0;
    while (true) {
        polls++;

        uint8_t count = 0;
        esp_err_t err = kb_read_regs(KB_REG_EVENT_NUM, &count, 1);
        if (err != ESP_OK) {
            if ((polls & 0x3F) == 0) {
                ESP_LOGE(TAG, "poll #%u: EVENT_NUM read failed: %s", polls, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        for (uint8_t i = 0; i < count; ++i) {
            uint8_t buf[2] = {0, 0};
            err = kb_read_regs(KB_REG_HID_EVENT, buf, 2);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "HID_EVENT read failed: %s", esp_err_to_name(err));
                break;
            }
            uint8_t modifier = buf[0];
            uint8_t keycode  = buf[1];

            // Log the first 30 events to verify the mapping, then quiet down.
            if (++total_events <= 30) {
                ESP_LOGI(TAG, "event[%u] mod=0x%02X key=0x%02X", total_events, modifier, keycode);
            }

            // Skip empty reports (key release with no other keys held).
            if (keycode == 0 && modifier == 0) continue;

            kb_forward_to_lvgl(modifier, keycode);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

extern "C" bool hal_tab5_keyboard_init(void)
{
    ESP_LOGI(TAG, "hal_tab5_keyboard_init: creating I2C bus on port %d (SDA=%d SCL=%d)",
             KB_I2C_PORT, KB_SDA_GPIO, KB_SCL_GPIO);

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source               = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                 = KB_I2C_PORT;
    bus_cfg.sda_io_num               = (gpio_num_t)KB_SDA_GPIO;
    bus_cfg.scl_io_num               = (gpio_num_t)KB_SCL_GPIO;
    bus_cfg.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_kb_bus);
    if (err != ESP_OK || s_kb_bus == nullptr) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = KB_ADDR;
    dev_cfg.scl_speed_hz    = KB_I2C_FREQ_HZ;
    err = i2c_master_bus_add_device(s_kb_bus, &dev_cfg, &s_kb_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return false;
    }

    // Probe: any readable register should ACK if the keyboard is attached.
    uint8_t probe = 0;
    if (kb_read_regs(KB_REG_INT_STA, &probe, 1) != ESP_OK) {
        ESP_LOGE(TAG, "keyboard probe failed (no ACK from 0x%02X)", KB_ADDR);
        return false;
    }

    // Switch the keyboard firmware to HID mode (modifier + keycode) so
    // we can use the standard USB HID Usage Table instead of a hand-
    // written 5×14 matrix keymap.
    if (kb_write_reg(KB_REG_KEYBOARD_MODE, KB_MODE_HID) != ESP_OK) {
        ESP_LOGE(TAG, "could not set keyboard mode to HID");
        return false;
    }

    // Create the LVGL keyboard group if the USB path hasn't already.
    // The Claude app (and any other app) calls lv_group_add_obj() on
    // this group so physical key events route to its focused widget.
    hal::HalBase* hal = GetHAL();
    if (hal == nullptr) {
        ESP_LOGE(TAG, "GetHAL() returned null; cannot create lvKbGroup");
        return false;
    }
    if (hal->lvKbGroup == nullptr) {
        hal->lvKbGroup = lv_group_create();
        ESP_LOGI(TAG, "created lvKbGroup=%p", (void*)hal->lvKbGroup);
    }

    ESP_LOGI(TAG, "Tab5 keyboard ready on I2C port %d @ 0x%02X (probe=0x%02X, mode=HID)",
             KB_I2C_PORT, KB_ADDR, probe);

    s_kb_ready = true;
    xTaskCreate(kb_poll_task, "tab5_kb_poll", 4096, nullptr, 5, nullptr);
    return true;
}
