// web JPEG → RGB565 (v3, 全平台铁律版):
//   worker: sync XHR 拿 jpg 字节 → 与主线程共用一块 SAB (状态字 + RGB565 缓冲)
//   → 主线程 <img>+canvas 解码, 像素直写 SAB → Atomics.notify 唤醒。
//   全程零事件循环依赖 (emscripten_sleep 在 detached 线程会 Abort,
//   promise/消息回投在 Atomics.wait 阻塞下死锁 —— 都踩过, 弃)。
#include <cstdint>
#include <cstring>
#include <emscripten.h>

// 二进制安全拉取: WebKit 的 sync XHR 不支持 responseType=arraybuffer
// (静默退回文本, JPEG 字节被文本解码绞碎 —— iPad px=-1 真凶)。
// x-user-defined: 每字节映射为 U+0000..U+00FF, charCodeAt 即原字节。
EM_JS(int, web_fetch_bin_js, (const char* url, uint8_t* out, int cap), {
    try {
        const xhr = new XMLHttpRequest();
        xhr.open("GET", UTF8ToString(url), false);
        xhr.overrideMimeType("text/plain; charset=x-user-defined");
        xhr.timeout = 30000;
        xhr.send(null);
        if (xhr.status !== 200) return -xhr.status;
        const t = xhr.responseText;
        const n = Math.min(t.length, cap);
        for (let i = 0; i < n; i++) HEAPU8[out + i] = t.charCodeAt(i) & 0xFF;
        return n;
    } catch (e) { return -1; }
})

extern "C" int web_fetch_bin(const char* url, uint8_t* out, int cap)
{
    return web_fetch_bin_js(url, out, cap);
}

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
