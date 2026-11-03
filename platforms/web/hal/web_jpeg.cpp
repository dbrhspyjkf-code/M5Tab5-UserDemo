// web JPEG → RGB565 (webkit 兼容版):
//   worker 把 jpg 字节 postMessage 给主线程 → 主线程 <img>+canvas 解码并
//   转 RGB565 (Uint16Array) 回传 → worker 监听器写入 wasm 堆 → 忙等解除。
//   (worker OffscreenCanvas 在 iPad WebKit 全败, DOM canvas 主线程 100%。)
#include <cstdint>
#include <cstring>
#include <emscripten.h>

static uint16_t* s_out    = nullptr;
static int       s_out_px = 0;
static volatile int s_done = -999;   // -999=进行中, -1=失败, >=0=写入像素数

extern "C" void tab5_jpeg_begin(uint16_t* out, int px)
{
    s_out = out; s_out_px = px; s_done = -999;
}

extern "C" void tab5_jpeg_finish(const uint16_t* data, int n)
{
    if (n > 0 && data) {
        const int c = n < s_out_px ? n : s_out_px;
        memcpy(s_out, data, (size_t)c * 2);
        s_done = c;
    } else {
        s_done = -1;
    }
}

EM_JS(void, jpeg_hook_install, (), {
    if (globalThis.__t5jpgHook) return;
    globalThis.__t5jpgHook = 1;
    self.addEventListener('message', e => {
        const d = e.data && e.data.__tab5jpgdone;
        if (!d) return;
        if (d.ok) {
            const n = d.px.length;
            const p = Module._malloc(n * 2);
            HEAPU16.set(d.px, p >> 1);
            Module._tab5_jpeg_finish(p, n);
            Module._free(p);
        } else {
            Module._tab5_jpeg_finish(0, 0);
        }
    });
})

EM_JS(void, jpeg_post_js, (const uint8_t* jpg, int len), {
    const bytes = new Uint8Array(HEAPU8.buffer, jpg, len).slice();
    postMessage({__tab5jpg: {bytes: bytes.buffer}});
})

extern "C" int web_jpeg_to_rgb565(const uint8_t* jpg, int jpg_len, uint16_t* out, int out_bytes)
{
    jpeg_hook_install();
    tab5_jpeg_begin(out, out_bytes / 2);
    jpeg_post_js(jpg, jpg_len);
    const double t0 = emscripten_get_now();
    while (s_done == -999) {
        emscripten_sleep(2);   // 让出本 worker 事件循环, 消息得以送达
        if (emscripten_get_now() - t0 > 15000) return -1;
    }
    return s_done;
}
