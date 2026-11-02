/*
 * SPDX-License-Identifier: MIT
 *
 * web_display — LVGL 显示与输入驱动 (Emscripten pthread 构建)。
 *
 * 架构 (与 SDL 桌面驱动的差异):
 *   - 整个 C++ main() 跑在 pthread worker (PROXY_TO_PTHREAD), LVGL 在此渲染;
 *   - 帧数据写入 worker 堆内的 staging 双缓冲, 经"共享 wasm heap"暴露给浏览器
 *     主线程 (pthreads 构建的 wasmMemory 是 SharedArrayBuffer, 主线程的
 *     Module.HEAPU16 与 worker 同一缓冲), rAF 侧只读 staging 转 RGBA 上 canvas;
 *   - 布局偏移 (staging/ring/atomics 地址) 由 worker 在 HAL init 时 postMessage
 *     给主线程 (shell.js 经 patch 的 Worker 构造器接收);
 *   - 输入事件 (触摸/滚轮/键盘) 由主线程写入 ring, worker 在 indev read_cb 消费。
 */
#include <lvgl.h>
#include <emscripten.h>
#include <mooncake_log.h>
#include <algorithm>
#include <atomic>
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

struct SharedHeader {
    uint32_t magic;
    uint32_t version;
    std::atomic<uint32_t> frameSeq;    // worker 发布帧后 +1 (release)
    std::atomic<uint32_t> inputWrite;  // 主线程写入事件后 +1 (release)
    uint32_t publishedIdx;             // 当前已发布 staging (0/1)
    uint32_t bbox[4];                  // x1, y1, x2, y2
    uint32_t ring[RING_SLOTS][4];      // {type, a, b, c}
};

static SharedHeader* sh = nullptr;
static uint16_t staging[2][SCREEN_W * SCREEN_H];  // 双缓冲帧区 (bss)
static uint16_t draw_buf[SCREEN_W * 180];         // LVGL partial 渲染缓冲

static int cur_staging = 0;
static uint32_t bbox_acc[4] = {0, 0, 0, 0};       // 本帧累积脏区
static bool bbox_valid = false;

/* ----------------------------- 布局上报主线程 ----------------------------- */

EM_JS(void, post_layout, (int magic, int frameSeq, int inputWrite, int publishedIdx,
                          int bbox, int ring, int staging0, int staging1),
{
    postMessage({__tab5layout: {magic, frameSeq, inputWrite, publishedIdx, bbox, ring, staging0, staging1}});
})

/* --------------------------------- flush --------------------------------- */

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    const int x1 = area->x1, y1 = area->y1;
    const int x2 = area->x2, y2 = area->y2;
    const int w = x2 - x1 + 1;
    const size_t row_bytes = (size_t)w * 2;

    auto* dst = staging[cur_staging];
    for (int y = y1; y <= y2; y++) {
        memcpy(&dst[(size_t)y * SCREEN_W + x1], px_map + (size_t)(y - y1) * row_bytes, row_bytes);
    }

    if (!bbox_valid) {
        bbox_acc[0] = x1; bbox_acc[1] = y1; bbox_acc[2] = x2; bbox_acc[3] = y2;
        bbox_valid = true;
    } else {
        if ((int)bbox_acc[0] > x1) bbox_acc[0] = x1;
        if ((int)bbox_acc[1] > y1) bbox_acc[1] = y1;
        if ((int)bbox_acc[2] < x2) bbox_acc[2] = x2;
        if ((int)bbox_acc[3] < y2) bbox_acc[3] = y2;
    }

    lv_display_flush_ready(disp);
}

void publishFrame()
{
    if (!sh || !bbox_valid) return;
    sh->publishedIdx = (uint32_t)cur_staging;
    sh->bbox[0] = bbox_acc[0];
    sh->bbox[1] = bbox_acc[1];
    sh->bbox[2] = bbox_acc[2];
    sh->bbox[3] = bbox_acc[3];
    bbox_valid = false;
    cur_staging ^= 1;
    sh->frameSeq.fetch_add(1, std::memory_order_release);
}

/* --------------------------------- 输入 ring ------------------------------- */

static void ring_push(uint32_t type, uint32_t a, uint32_t b)
{
    if (!sh) return;
    uint32_t w = sh->inputWrite.load(std::memory_order_relaxed);
    // 满则丢弃最旧 (主线程单写者, 256 槽足够)
    uint32_t slot = w % RING_SLOTS;
    sh->ring[slot][0] = type;
    sh->ring[slot][1] = a;
    sh->ring[slot][2] = b;
    sh->ring[slot][3] = 0;
    sh->inputWrite.store(w + 1, std::memory_order_release);
}

static uint32_t read_idx = 0;   // worker 已消费到的 ring 序号

/* --------------------------------- indevs --------------------------------- */

static int32_t ptr_x = 0, ptr_y = 0;
static bool ptr_pressed = false;
static int32_t wheel_acc = 0;
static std::mutex key_mutex;
static struct { int key; bool down; } key_pending[32];
static int key_head = 0, key_tail = 0;

static void pointer_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    if (!sh) return;
    uint32_t w = sh->inputWrite.load(std::memory_order_acquire);
    while (read_idx != w) {
        uint32_t slot = read_idx % RING_SLOTS;
        uint32_t type = sh->ring[slot][0];
        int32_t a = (int32_t)sh->ring[slot][1];
        int32_t b = (int32_t)sh->ring[slot][2];
        read_idx++;
        switch (type) {
            case EV_PRESS:
                ptr_x = a; ptr_y = b; ptr_pressed = true;
                break;
            case EV_RELEASE:
                ptr_x = a; ptr_y = b; ptr_pressed = false;
                break;
            case EV_MOVE:
                ptr_x = a; ptr_y = b;
                break;
            case EV_WHEEL:
                wheel_acc += b;
                break;
            case EV_KEYDOWN:
            case EV_KEYUP: {
                std::lock_guard<std::mutex> lk(key_mutex);
                int next = (key_tail + 1) % 32;
                if (next != key_head) {
                    key_pending[key_tail] = {(int)b, type == EV_KEYDOWN};
                    key_tail = next;
                }
                break;
            }
            default:
                break;
        }
    }
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
    lv_display_set_buffers(disp, draw_buf, nullptr, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    // 共享头布局就绪, 上报主线程
    static SharedHeader header;
    header.magic = SHM_MAGIC;
    header.version = SHM_VERSION;
    sh = &header;

    auto* touchpad = lv_indev_create(LV_INDEV_TYPE_POINTER, 20);
    lv_indev_set_read_cb(touchpad, pointer_read_cb);
    lv_indev_set_display(touchpad, disp);
    lv_indev_set_group(touchpad, lv_group_get_default());

    auto* wheel = lv_indev_create(LV_INDEV_TYPE_ENCODER, 20);
    lv_indev_set_read_cb(wheel, encoder_read_cb);
    lv_indev_set_display(wheel, disp);
    lv_indev_set_group(wheel, lv_group_get_default());

    auto* keyboard = lv_indev_create(LV_INDEV_TYPE_KEYBOARD, 20);
    lv_indev_set_read_cb(keyboard, keyboard_read_cb);
    lv_indev_set_display(keyboard, disp);
    lv_indev_set_group(keyboard, lv_group_get_default());

    post_layout((int)(uintptr_t)&header.magic,
                (int)(uintptr_t)&header.frameSeq,
                (int)(uintptr_t)&header.inputWrite,
                (int)(uintptr_t)&header.publishedIdx,
                (int)(uintptr_t)&header.bbox[0],
                (int)(uintptr_t)&header.ring[0][0],
                (int)(uintptr_t)&staging[0][0],
                (int)(uintptr_t)&staging[1][0]);

    mclog::tagInfo(_tag, "display driver ready, layout posted");
}

}  // namespace webui
