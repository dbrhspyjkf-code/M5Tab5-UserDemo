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
// SAB 命令区 (byte): STAGING_END = 4272+1843200 = 1847472
//   [0..3]  seq   (worker 每次请求 +1)
//   [4..7]  op    (1=fetch 请求; 0=空闲)
//   [8..11] status(0=处理中 1=ok 2=err)
//   [12..15] len
//   [16..207] url (ASCII)
//   [208..]  数据缓冲 2MB
// 页面 shell 每 30ms 轮询 op; 全程 SAB+Atomics, 零 postMessage
// (CriOS 上 pthread→page 消息不到达, 访问日志实锤; 而 SAB/Atomics 是
//  屏幕/触摸一直在用的、iPad 探针 wait-ok 验证过的原语)
EM_JS(int, web_fetch_bin_js, (const char* url, uint8_t* out, int cap), {
    const sab = globalThis.__tab5sab;
    if (!sab) return -1;
    const BASE = 4272 + 1280 * 720 * 2;
    const u32 = new Uint32Array(sab, BASE, 4);
    const urlBuf = new Uint8Array(sab, BASE + 16, 192);
    const dataBuf = new Uint8Array(sab, BASE + 208);
    const DATA_CAP = dataBuf.length;
    const c = new Uint8Array(sab, BASE, 1);
    // 写 url
    const u = UTF8ToString(url);
    if (u.length >= 192) return -1;
    for (let i = 0; i < u.length; i++) urlBuf[i] = u.charCodeAt(i);
    urlBuf[u.length] = 0;
    Atomics.store(u32, 2, 0);                    // status=处理中
    Atomics.store(u32, 0, Atomics.load(u32, 0) + 1);  // seq++
    Atomics.store(u32, 1, 1);                    // op=fetch
    const t0 = Date.now();
    for (;;) {
        const st = Atomics.wait(u32, 2, 0, 50);  // worker 内合法
        if (Atomics.load(u32, 2) !== 0) break;
        if (Date.now() - t0 > 30000) return -1;
    }
    if (Atomics.load(u32, 2) !== 1) return -(Atomics.load(u32, 2));
    const n = Math.min(Atomics.load(u32, 3), cap, DATA_CAP);
    HEAPU8.set(dataBuf.subarray(0, n), out);
    Atomics.store(u32, 1, 0);                    // op=空闲
    return n;
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
