// web JPEG → RGB565 解码 (worker 内 OffscreenCanvas; Safari 16.4+/Chrome 均支持)
#include <cstdint>
#include <emscripten.h>

EM_JS(int, jpeg_decode_js, (const uint8_t* jpg, int jpg_len, uint16_t* out, int out_bytes), {
    // 同步桥: JS 解码是异步的 —— 用 Atomics.wait 阻塞等待 (worker 内合法)
    const sab = new SharedArrayBuffer(8);
    const done = new Int32Array(sab);
    const bytes = HEAPU8.subarray(jpg, jpg + jpg_len);
    createImageBitmap(new Blob([bytes], {type: "image/jpeg"})).then(bm => {
        const c = new OffscreenCanvas(bm.width, bm.height);
        const ctx = c.getContext("2d");
        ctx.drawImage(bm, 0, 0);
        const d = ctx.getImageData(0, 0, bm.width, bm.height).data;
        const n = bm.width * bm.height;
        const maxOut = out_bytes >> 1;
        const cnt = Math.min(n, maxOut);
        for (let i = 0; i < cnt; i++) {
            const p = i << 2;
            out[i] = ((d[p] >> 3) << 11) | ((d[p + 1] >> 2) << 5) | (d[p + 2] >> 3);
        }
        Atomics.store(done, 0, cnt);
        Atomics.notify(done, 0);
    }).catch(e => {
        console.warn("[tab5web] jpeg decode failed:", e);
        Atomics.store(done, 0, -1);
        Atomics.notify(done, 0);
    });
    const r = Atomics.wait(done, 0, 0, 15000);   // 等非 0
    return r === "ok" ? Atomics.load(done, 0) : -1;
})

extern "C" int web_jpeg_to_rgb565(const uint8_t* jpg, int jpg_len, uint16_t* out, int out_bytes)
{
    return jpeg_decode_js(jpg, jpg_len, out, out_bytes);
}
