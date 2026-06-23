/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <vector>
#include <driver/gpio.h>
#include <memory>
#include <mutex>
#include <lvgl.h>
#include <usb/usb_host.h>
#include <usb/hid_host.h>
#include <usb/hid_usage_keyboard.h>
#include <usb/hid_usage_mouse.h>
#include <esp_log.h>
#include <assets/assets.h>

#define TAG "usba"

static std::mutex _usba_detect_mutex;
static bool _is_usba_connected   = false;
static bool _is_keyboard_connected = false;
static lv_obj_t* _cursor_img;

// ── Physical keyboard support ──────────────────────────────────────────────
struct KeyEvent_t {
    uint32_t         key;
    lv_indev_state_t state;
};
static QueueHandle_t _key_event_queue = nullptr;
static uint8_t       _prev_keys[6]    = {0};
static uint8_t       _prev_modifier   = 0;

// Map a USB HID boot-protocol keycode + modifier byte to an LVGL key code.
// Returns 0 for keys we don't handle (Fn keys, Print Screen, etc.).
static uint32_t _hid_keycode_to_lvgl(uint8_t keycode, uint8_t modifier)
{
    bool shift = (modifier & (HID_LEFT_SHIFT | HID_RIGHT_SHIFT)) != 0;

    // a–z / A–Z
    if (keycode >= HID_KEY_A && keycode <= HID_KEY_Z) {
        char c = 'a' + (keycode - HID_KEY_A);
        return shift ? (uint32_t)(c - 32) : (uint32_t)c;
    }

    // Number row 1–9 and 0 (0x1E–0x27)
    if (keycode >= HID_KEY_1 && keycode <= HID_KEY_0) {
        static const char nums[] = "1234567890";
        static const char syms[] = "!@#$%^&*()";
        int idx = (keycode == HID_KEY_0) ? 9 : (keycode - HID_KEY_1);
        return shift ? (uint32_t)syms[idx] : (uint32_t)nums[idx];
    }

    switch (keycode) {
        case HID_KEY_ENTER:        return LV_KEY_ENTER;
        case HID_KEY_ESC:          return LV_KEY_ESC;
        case HID_KEY_DEL:          return LV_KEY_BACKSPACE;  // PC "Backspace" key
        case HID_KEY_DELETE:       return LV_KEY_DEL;        // PC "Delete" (forward)
        case HID_KEY_TAB:          return shift ? LV_KEY_PREV : LV_KEY_NEXT;
        case HID_KEY_SPACE:        return ' ';
        case HID_KEY_MINUS:        return shift ? '_' : '-';
        case HID_KEY_EQUAL:        return shift ? '+' : '=';
        case HID_KEY_OPEN_BRACKET: return shift ? '{' : '[';
        case HID_KEY_CLOSE_BRACKET:return shift ? '}' : ']';
        case HID_KEY_BACK_SLASH:   return shift ? '|' : '\\';
        case HID_KEY_COLON:        return shift ? ':' : ';';
        case HID_KEY_QUOTE:        return shift ? '"' : '\'';
        case HID_KEY_TILDE:        return shift ? '~' : '`';
        case HID_KEY_LESS:         return shift ? '<' : ',';
        case HID_KEY_GREATER:      return shift ? '>' : '.';
        case HID_KEY_SLASH:        return shift ? '?' : '/';
        case HID_KEY_HOME:         return LV_KEY_HOME;
        case HID_KEY_END:          return LV_KEY_END;
        case HID_KEY_RIGHT:        return LV_KEY_RIGHT;
        case HID_KEY_LEFT:         return LV_KEY_LEFT;
        case HID_KEY_DOWN:         return LV_KEY_DOWN;
        case HID_KEY_UP:           return LV_KEY_UP;
        default:                   return 0;
    }
}

static void hid_host_keyboard_report_callback(const uint8_t* const data, const int length)
{
    if (length < 8 || !_key_event_queue) return;

    uint8_t modifier      = data[0];
    const uint8_t* keys   = data + 2;  // keycodes[6]

    // Detect key releases (present in prev report, absent from current)
    for (int i = 0; i < 6; i++) {
        if (_prev_keys[i] == 0) continue;
        bool still = false;
        for (int j = 0; j < 6; j++) {
            if (keys[j] == _prev_keys[i]) { still = true; break; }
        }
        if (!still) {
            uint32_t lv_key = _hid_keycode_to_lvgl(_prev_keys[i], _prev_modifier);
            if (lv_key) {
                KeyEvent_t evt = { lv_key, LV_INDEV_STATE_RELEASED };
                xQueueSend(_key_event_queue, &evt, 0);
            }
        }
    }

    // Detect new key presses (absent from prev, present in current)
    for (int i = 0; i < 6; i++) {
        if (keys[i] == 0) continue;
        bool was = false;
        for (int j = 0; j < 6; j++) {
            if (_prev_keys[j] == keys[i]) { was = true; break; }
        }
        if (!was) {
            uint32_t lv_key = _hid_keycode_to_lvgl(keys[i], modifier);
            if (lv_key) {
                KeyEvent_t evt = { lv_key, LV_INDEV_STATE_PRESSED };
                xQueueSend(_key_event_queue, &evt, 0);
            }
        }
    }

    memcpy(_prev_keys, keys, 6);
    _prev_modifier = modifier;
}

QueueHandle_t app_event_queue = NULL;
typedef enum { APP_EVENT = 0, APP_EVENT_HID_HOST } app_event_group_t;

typedef struct {
    app_event_group_t event_group;
    struct {
        hid_host_device_handle_t handle;
        hid_host_driver_event_t event;
        void* arg;
    } hid_host_device;
} app_event_queue_t;

static const char* hid_proto_name_str[] = {"NONE", "KEYBOARD", "MOUSE"};

typedef struct {
    enum key_state { KEY_STATE_PRESSED = 0x00, KEY_STATE_RELEASED = 0x01 } state;
    uint8_t modifier;
    uint8_t key_code;
} key_event_t;

#define KEYBOARD_ENTER_MAIN_CHAR '\r'
#define KEYBOARD_ENTER_LF_EXTEND 1

static void hid_print_new_device_report_header(hid_protocol_t proto)
{
    static hid_protocol_t prev_proto_output;

    if (prev_proto_output != proto) {
        prev_proto_output = proto;
        printf("\r\n");
        if (proto == HID_PROTOCOL_MOUSE) {
            printf("Mouse\r\n");
        } else if (proto == HID_PROTOCOL_KEYBOARD) {
            printf("Keyboard\r\n");
        } else {
            printf("Generic\r\n");
        }
        fflush(stdout);
    }
}

static void hid_host_mouse_report_callback(const uint8_t* const data, const int length)
{
    hid_mouse_input_report_boot_t* mouse_report = (hid_mouse_input_report_boot_t*)data;

    if (length < sizeof(hid_mouse_input_report_boot_t)) {
        return;
    }

    static int x_pos = 720 / 2;
    static int y_pos = 1280 / 2;

    // Calculate absolute position from displacement
    x_pos += mouse_report->y_displacement;
    y_pos -= mouse_report->x_displacement;

    x_pos = std::clamp(x_pos, 0, 720);
    y_pos = std::clamp(y_pos, 0, 1280);

    hid_print_new_device_report_header(HID_PROTOCOL_MOUSE);

    // printf("X: %06d\tY: %06d\t|%c|%c|\r", x_pos, y_pos, (mouse_report->buttons.button1 ? 'o' : ' '),
    //        (mouse_report->buttons.button2 ? 'o' : ' '));

    GetHAL()->hidMouseData.mutex.lock();
    GetHAL()->hidMouseData.x        = x_pos;
    GetHAL()->hidMouseData.y        = y_pos;
    GetHAL()->hidMouseData.btnLeft  = mouse_report->buttons.button1;
    GetHAL()->hidMouseData.btnRight = mouse_report->buttons.button2;
    GetHAL()->hidMouseData.mutex.unlock();

    fflush(stdout);
}

void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event,
                                 void* arg)
{
    uint8_t data[64]   = {0};
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
            ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle, data, 64, &data_length));

            if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
                if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                    hid_host_keyboard_report_callback(data, data_length);
                } else if (HID_PROTOCOL_MOUSE == dev_params.proto) {
                    hid_host_mouse_report_callback(data, data_length);
                }
            }

            break;
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HID Device, protocol '%s' DISCONNECTED", hid_proto_name_str[dev_params.proto]);
            ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));

            _usba_detect_mutex.lock();
            _is_usba_connected = false;
            if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                _is_keyboard_connected = false;
                // Discard stale key states
                memset(_prev_keys, 0, sizeof(_prev_keys));
                _prev_modifier = 0;
            }
            _usba_detect_mutex.unlock();

            break;
        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGI(TAG, "HID Device, protocol '%s' TRANSFER_ERROR", hid_proto_name_str[dev_params.proto]);
            break;
        default:
            ESP_LOGE(TAG, "HID Device, protocol '%s' Unhandled event", hid_proto_name_str[dev_params.proto]);
            break;
    }
}

void hid_host_device_event(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void* arg)
{
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
        case HID_HOST_DRIVER_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "HID Device, protocol '%s' CONNECTED", hid_proto_name_str[dev_params.proto]);

            const hid_host_device_config_t dev_config = {.callback = hid_host_interface_callback, .callback_arg = NULL};

            ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
            if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
                ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
                if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                    ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
                }
            }
            ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));

            _usba_detect_mutex.lock();
            _is_usba_connected = true;
            if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                _is_keyboard_connected = true;
                ESP_LOGI(TAG, "Physical keyboard connected");
            }
            _usba_detect_mutex.unlock();

            break;
        }
        default:
            break;
    }
}

void hid_host_device_callback(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event,
                              void* arg)
{
    const app_event_queue_t evt_queue = {.event_group = APP_EVENT_HID_HOST,
                                         // HID Host Device related info
                                         .hid_host_device = {.handle = hid_device_handle, .event = event, .arg = arg}};

    if (app_event_queue) {
        xQueueSend(app_event_queue, &evt_queue, 0);
    }
}

static void tab5_usb_host_task(void* pvParameters)
{
    // BaseType_t task_created;
    app_event_queue_t evt_queue;
    // ESP_LOGI(TAG, "HID Host example");
    // task_created = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, xTaskGetCurrentTaskHandle(), 2, NULL,
    // 0); assert(task_created == pdTRUE);

    ulTaskNotifyTake(false, 1000);
    const hid_host_driver_config_t hid_host_driver_config = {.create_background_task = true,
                                                             .task_priority          = 5,
                                                             .stack_size             = 4096,
                                                             .core_id                = 0,
                                                             .callback               = hid_host_device_callback,
                                                             .callback_arg           = NULL};

    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

    // Create queue
    app_event_queue = xQueueCreate(10, sizeof(app_event_queue_t));

    ESP_LOGI(TAG, "Waiting for HID Device to be connected");

    while (1) {
        // Wait queue
        if (xQueueReceive(app_event_queue, &evt_queue, portMAX_DELAY)) {
            if (APP_EVENT == evt_queue.event_group) {
                // User pressed button
                usb_host_lib_info_t lib_info;
                ESP_ERROR_CHECK(usb_host_lib_info(&lib_info));
                if (lib_info.num_devices == 0) {
                    // End while cycle
                    break;
                } else {
                    ESP_LOGW(TAG, "To shutdown example, remove all USB devices and press button again.");
                    // Keep polling
                }
            }

            if (APP_EVENT_HID_HOST == evt_queue.event_group) {
                hid_host_device_event(evt_queue.hid_host_device.handle, evt_queue.hid_host_device.event,
                                      evt_queue.hid_host_device.arg);
            }
        }
    }
}

static void lvgl_keyboard_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    KeyEvent_t evt;
    if (_key_event_queue && xQueueReceive(_key_event_queue, &evt, 0) == pdTRUE) {
        data->key   = evt.key;
        data->state = evt.state;
        data->continue_reading = (uxQueueMessagesWaiting(_key_event_queue) > 0);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

[[maybe_unused]] static void lvgl_mouse_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    _usba_detect_mutex.lock();
    if (!_is_usba_connected) {
        _usba_detect_mutex.unlock();
        data->state = LV_INDEV_STATE_REL;
        if (lv_obj_get_style_opa(_cursor_img, LV_PART_MAIN) == LV_OPA_COVER) {
            lv_obj_set_style_opa(_cursor_img, LV_OPA_TRANSP, LV_PART_MAIN);
        }
        return;
    }
    _usba_detect_mutex.unlock();
    if (lv_obj_get_style_opa(_cursor_img, LV_PART_MAIN) == LV_OPA_TRANSP) {
        lv_obj_set_style_opa(_cursor_img, LV_OPA_COVER, LV_PART_MAIN);
    }

    std::lock_guard<std::mutex> lock(GetHAL()->hidMouseData.mutex);
    data->point.x = GetHAL()->hidMouseData.x;
    data->point.y = GetHAL()->hidMouseData.y;
    data->state   = GetHAL()->hidMouseData.btnLeft ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void HalEsp32::hid_init()
{
    mclog::tagInfo(TAG, "hid init");
    xTaskCreatePinnedToCore(tab5_usb_host_task, "usba", 4096 * 2, NULL, 5, NULL, 0);

    // USB 鼠标 pointer indev 暂时禁用.
    // 原因: LVGL 新建 indev 插链表头部. 触摸 indev 在 bsp_display_start 里先建 (排尾),
    // 这个鼠标 indev 后建 (排头), 于是各 app 的"找第一个 pointer indev 注册上拨手势"
    // 会错误地注册到鼠标 indev 上, 导致全部 app 触摸上拨退出失效.
    // 用户当前只用 USB 键盘, 不用鼠标. 如需恢复鼠标, 要改各 app 改成精确拿触摸 indev.
    // auto lvMouse = lv_indev_create();
    // lv_indev_set_type(lvMouse, LV_INDEV_TYPE_POINTER);
    // lv_indev_set_read_cb(lvMouse, lvgl_mouse_read_cb);
    // lv_indev_set_display(lvMouse, lvDisp);
    //
    // _cursor_img = lv_image_create(lv_screen_active());
    // lv_image_set_src(_cursor_img, &mouse_cursor);
    // lv_indev_set_cursor(lvMouse, _cursor_img);

    // Physical keyboard indev
    _key_event_queue = xQueueCreate(32, sizeof(KeyEvent_t));

    lvKeyboard = lv_indev_create();
    lv_indev_set_type(lvKeyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(lvKeyboard, lvgl_keyboard_read_cb);
    lv_indev_set_display(lvKeyboard, lvDisp);

    // Global keyboard group: apps add their widgets to this group so physical
    // key events are routed to the focused widget (typically a textarea).
    lvKbGroup = lv_group_create();
    lv_indev_set_group(lvKeyboard, lvKbGroup);
}

bool HalEsp32::usbADetect()
{
    std::lock_guard<std::mutex> lock(_usba_detect_mutex);
    return _is_usba_connected;
}

bool HalEsp32::usbKeyboardDetect()
{
    std::lock_guard<std::mutex> lock(_usba_detect_mutex);
    return _is_keyboard_connected;
}
