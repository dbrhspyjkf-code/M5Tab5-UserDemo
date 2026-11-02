/*
 * SPDX-License-Identifier: MIT
 *
 * web_display — LVGL 显示与输入驱动 (Emscripten pthread 构建)。
 *
 * 架构 (与 SDL 桌面驱动的差异):
 *   - 整个 C++ main() 跑在 pthread worker (PROXY_TO_PTHREAD), LVGL 在此渲染;
 *   - worker 创建一块显式 SharedArrayBuffer (帧 staging + 输入 ring + 原子
 *     序号), 随 postMessage 交给浏览器主线程 (PROXY_TO_PTHREAD 下主线程
 *     没有 wasm 实例/HEAPU8, 显式 SAB 是唯一可靠通道);
 *   - flush_cb 把 LVGL 脏区从 wasm 堆拷进 SAB → 扩展脏区 bbox → frameSeq+1;
 *     主线程 rAF 检查 seq, 把该区域 RGB565→RGBA 画上 canvas;
 *   - 输入事件 (触摸/滚轮/键盘) 由主线程写入 SAB ring, worker 在 indev
 *     read_cb 消费。
 *
 * SAB 布局 (与 shell.js 逐字段对应, 全部 4 字节):
 *   0     u32  magic 'TAB5'
 *   4     u32  version
 *   8     u32  frameSeq        (worker Atomics.add, 发布一帧脏区后 +1)
 *   12    u32  inputWrite      (主线程 Atomics.store, 写入事件后前进)
 *   16    u32  bbox[8][4]      (环形队列, 槽号 = seq&7 —— 每个 seq 一格,
 *                               主线程逐 seq 消费合并, 启动画廊多块 flush
 *                               挤进一个 rAF 时不会丢上半屏)
 *   32    u32  ring[256*4]     ({type,a,b,0})
 *   4128  u16  staging[1280*720]
 */
#include <lvgl.h>
#include <emscripten.h>
#include <mooncake_log.h>
#include <algorithm>
#include <cstring>
#include <mutex>

#include "hal_config.h"

static const std::string _tag = "webdisp";

namespace webui {

static constexpr int SCREEN_W = HAL_SCREEN_WIDTH;
static constexpr int SCREEN_H = HAL_SCREEN_HEIGHT;
static constexpr uint32_t SHM_MAGIC   = 0x54414235;  // 'TAB5'
static constexpr uint32_t SHM_VERSION = 1;
static constexpr int RING_SLOTS = 256;

// 输入事件类型 (与 shell.js 约定)
enum : uint32_t {
    EV_PRESS   = 1,
    EV_RELEASE = 2,
    EV_MOVE    = 3,
    EV_WHEEL   = 4,   // b = diff (向下为 +1)
    EV_KEYDOWN = 5,   // b = LVGL key code
    EV_KEYUP   = 6,
};

static constexpr int SAB_SIZE =
    4128 + SCREEN_W * SCREEN_H * 2;

/* -------------------------- SAB 建立与布局上报 ----------------------------- */

EM_JS(void, sab_init_and_post, (), {
    const SIZE = 4128 + 1280 * 720 * 2;
    const sab = new SharedArrayBuffer(SIZE);
    const u32 = new Uint32Array(sab);
    u32[0] = 0x54414235;  // 'TAB5'
    u32[1] = 1;
    globalThis.__tab5sab = sab;  // 供后续 EM_JS 使用 (worker 侧)
    postMessage({__tab5layout: {sab: sab}});
})

// 帧脏区: 从 wasm 堆拷进 SAB staging, 扩展 bbox 双槽, 发布 seq
EM_JS(void, sab_flush, (int x1, int y1, int x2, int y2, int src_ptr, int row_bytes),
{
    const sab = globalThis.__tab5sab;
    const u32 = new Uint32Array(sab);
    const u16 = new Uint16Array(sab);
    const STAGING = 4128 >> 1;   // u16 索引
    const rows = y2 - y1 + 1;
    const src = HEAPU8.subarray(src_ptr, src_ptr + row_bytes * rows);
    for (let r = 0; r < rows; r++) {
        u16.set(new Uint16Array(src.buffer, src.byteOffset + r * row_bytes, row_bytes >> 1),
                STAGING + (y1 + r) * 1280 + x1);
    }
    const nseq = Atomics.add(u32, 2, 1) + 1;
    const slot = 3 + (nseq & 7) * 4;              // bbox[8][4] @ u32[3..34]
    u32[slot + 0] = x1; u32[slot + 1] = y1; u32[slot + 2] = x2; u32[slot + 3] = y2;
})

// 事件消费: 有事件则写 3×i32 到 wasm 堆并返回 1
EM_JS(int, sab_pop_event, (int out_ptr),
{
    const sab = globalThis.__tab5sab;
    if (!sab) return 0;
    const u32 = new Uint32Array(sab);
    const w = Atomics.load(u32, 3);
    if (globalThis.__tab5evIdx === undefined) globalThis.__tab5evIdx = 0;
    if (globalThis.__tab5evIdx === w) return 0;
    const slot = (globalThis.__tab5evIdx % 256) * 4;
    const base = 8 + slot;
    HEAP32[out_ptr >> 2] = u32[base];
    HEAP32[(out_ptr >> 2) + 1] = u32[base + 1];
    HEAP32[(out_ptr >> 2) + 2] = u32[base + 2];
    globalThis.__tab5evIdx++;
    return 1;
})

/* --------------------------------- flush --------------------------------- */

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    const int w = area->x2 - area->x1 + 1;
    sab_flush(area->x1, area->y1, area->x2, area->y2,
              (int)(uintptr_t)px_map, (int)(w * 2));
    lv_display_flush_ready(disp);
}

/* --------------------------------- indevs --------------------------------- */

static int32_t ptr_x = 0, ptr_y = 0;
static bool ptr_pressed = false;
static int32_t wheel_acc = 0;
static std::mutex key_mutex;
static struct { int key; bool down; } key_pending[32];
static int key_head = 0, key_tail = 0;

// 每次最多消费 2 条: 浏览器合成事件 (click/drag) 会把 press+轨迹+release
// 在同一瞬间灌进 ring, 一次吃完 LVGL 只看到终态 (press 被覆盖 → 无
// CLICKED/GESTURE)。限流后 press/轨迹/release 分批进入 indev 周期。
static void drain_events()
{
    struct { int32_t type, a, b; } ev;
    int budget = 2;
    while (budget-- > 0 && sab_pop_event((int)(uintptr_t)&ev)) {
        switch (ev.type) {
            case EV_PRESS:
                ptr_x = ev.a; ptr_y = ev.b; ptr_pressed = true;
                break;
            case EV_RELEASE:
                ptr_x = ev.a; ptr_y = ev.b; ptr_pressed = false;
                break;
            case EV_MOVE:
                ptr_x = ev.a; ptr_y = ev.b;
                break;
            case EV_WHEEL:
                wheel_acc += ev.b;
                break;
            case EV_KEYDOWN:
            case EV_KEYUP: {
                std::lock_guard<std::mutex> lk(key_mutex);
                int next = (key_tail + 1) % 32;
                if (next != key_head) {
                    key_pending[key_tail] = {(int)ev.b, ev.type == EV_KEYDOWN};
                    key_tail = next;
                }
                break;
            }
            default:
                break;
        }
    }
}

static void pointer_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    data->point.x = ptr_x;
    data->point.y = ptr_y;
    data->state = ptr_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void encoder_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    data->enc_diff = std::clamp(wheel_acc, -20, 20);
    wheel_acc = 0;
    data->state = LV_INDEV_STATE_RELEASED;
}

static void keyboard_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    std::lock_guard<std::mutex> lk(key_mutex);
    if (key_head == key_tail) return;
    data->key  = (uint32_t)key_pending[key_head].key;
    data->state = key_pending[key_head].down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    key_head = (key_head + 1) % 32;
    if (key_head != key_tail) data->continue_reading = true;
}

/* ---------------------------------- init ---------------------------------- */

void lvgl_display_init()
{
    lv_display_t* disp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    static uint16_t draw_buf[SCREEN_W * 180];
    lv_display_set_buffers(disp, draw_buf, nullptr, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    sab_init_and_post();

    // 事件环只在此处消费一次 (三个 indev 共享状态; 若各自 drain 会互相抢事件)
    lv_timer_create([](lv_timer_t*) { drain_events(); }, 10, nullptr);

    auto* touchpad = lv_indev_create();
    lv_indev_set_type(touchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touchpad, pointer_read_cb);
    lv_indev_set_display(touchpad, disp);
    lv_indev_set_group(touchpad, lv_group_get_default());

    auto* wheel = lv_indev_create();
    lv_indev_set_type(wheel, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(wheel, encoder_read_cb);
    lv_indev_set_display(wheel, disp);
    lv_indev_set_group(wheel, lv_group_get_default());

    auto* keyboard = lv_indev_create();
    lv_indev_set_type(keyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keyboard, keyboard_read_cb);
    lv_indev_set_display(keyboard, disp);
    lv_indev_set_group(keyboard, lv_group_get_default());

    mclog::tagInfo(_tag, "display driver ready, SAB posted");
}

}  // namespace webui
