// web JPEG → RGB565 (保留未用; 二进制拉取已迁 web_fetch.cpp S4 通道)
//   worker: jpg 字节 → SAB (状态字 + RGB565 缓冲) 随 postMessage 发主线程
//   → 主线程 <img>+canvas 解码, 像素直写 SAB → Atomics.notify 唤醒。
//   全程零事件循环依赖 (emscripten_sleep 在 detached 线程会 Abort,
//   promise/消息回投在 Atomics.wait 阻塞下死锁 —— 都踩过, 弃)。
#include <cstdint>
#include <cstring>
#include <emscripten.h>

EM_JS(int, jpeg_decode_js, (const uint8_t* jpg, int jpg_len, uint16_t* out, int out_px), {
    const MAX_PX = 1280 * 720;
    const sab = new SharedArrayBuffer(8 + MAX_PX * 2);
    const st = new Int32Array(sab, 0, 2);        // [0]=状态 0pending/1ok/2fail, [1]=px
    const bytes = new Uint8Array(HEAPU8.buffer, jpg, jpg_len).slice();
    postMessage({__tab5jpg: {sab: sab, bytes: bytes.buffer}});
    const r = Atomics.wait(st, 0, 0, 15000);
    if (r !== "ok" || Atomics.load(st, 0) !== 1) return -1;
    const n = Math.min(Atomics.load(st, 1), out_px, MAX_PX);
    const px16 = new Uint16Array(sab, 8, MAX_PX);
    HEAPU16.set(px16.subarray(0, n), out >> 1);
    return n;
})

extern "C" int web_jpeg_to_rgb565(const uint8_t* jpg, int jpg_len, uint16_t* out, int out_bytes)
{
    return jpeg_decode_js(jpg, jpg_len, out, out_bytes / 2);
}
