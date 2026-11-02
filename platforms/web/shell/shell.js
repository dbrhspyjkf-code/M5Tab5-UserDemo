/*
 * shell.js — Tab5 WASM 外壳: 等比缩放 / 输入采集 / 帧绘制。
 * 在 tab5web.js (emscripten glue) 之前加载, 提供 Module 全局。
 *
 * 通信模型 (与 web_display.cpp 约定):
 *   - worker (PROXY_TO_PTHREAD main) 创建显式 SharedArrayBuffer, 经
 *     postMessage {__tab5layout, sab} 交给本文件 (主线程无 wasm 实例);
 *   - 帧路径: worker flush 脏区进 SAB staging + frameSeq+1 (双槽 bbox 与
 *     seq 配对); 本文件 rAF 读 seq → bbox → RGB565→RGBA → putImageData;
 *   - 输入路径: 本文件写 SAB ring → Atomics.store inputWrite; worker
 *     indev read_cb 消费。
 *
 * SAB 布局 (字节):
 *   0 magic | 4 version | 8 frameSeq | 12 inputWrite
 *   16 bbox[2][4] (槽 = seq&1, u32 索引 3+(seq&1)*4)
 *   32 ring[256*4] | 4128 staging u16[1280*720]
 */
"use strict";

(function () {
  const W = 1280, H = 720;
  const RING_SLOTS = 256;
  const STAGING_U16 = 4128 >> 1;
  const EV = { PRESS: 1, RELEASE: 2, MOVE: 3, WHEEL: 4, KEYDOWN: 5, KEYUP: 6 };
  // KeyboardEvent → LVGL key codes
  const KMAP = {
    ArrowLeft: 17, ArrowUp: 18, ArrowRight: 19, ArrowDown: 20,
    Enter: 13, Escape: 27, Backspace: 8, Tab: 9, Delete: 4,
  };

  const $ = id => document.getElementById(id);
  const canvas = $("screen");
  const ctx = canvas.getContext("2d", { alpha: false });
  const off = document.createElement("canvas");
  off.width = W; off.height = H;
  const offCtx = off.getContext("2d", { alpha: false });
  const imgData = offCtx.createImageData(W, H);

  function setProgress(p, msg) {
    $("bar").style.width = Math.round(p * 100) + "%";
    if (msg) $("msg").textContent = msg;
  }
  function fail(text) {
    $("msg").textContent = "加载失败";
    $("err").textContent = text;
    console.error(text);
  }

  setProgress(0.05, "正在加载运行时…");

  /* ---------- 接收 worker 上报的 SAB ---------- */
  let SAB = null, U32 = null, U16 = null;

  const _Worker = window.Worker;
  window.Worker = class extends _Worker {
    constructor(u, o) {
      super(u, o);
      this.addEventListener("message", ev => {
        const d = ev.data;
        if (d && d.__tab5uart) {
          (window.__tab5uart = window.__tab5uart || []).push(d.__tab5uart);
          return;
        }
        if (d && d.__tab5layout && d.__tab5layout.sab) {
          try {
            SAB = d.__tab5layout.sab;
            U32 = new Uint32Array(SAB);
            U16 = new Uint16Array(SAB);
            if (U32[0] !== 0x54414235) throw new Error("magic mismatch");
            if (U32[1] !== 1) throw new Error("version mismatch");
            window.__tab5dbg = { get seq() { return Atomics.load(U32, 2); },
                                 get inputW() { return Atomics.load(U32, 3); },
                                 sab: SAB };
            setProgress(1, "启动中…");
            canvas.style.display = "block";
            $("loader").style.display = "none";
            fit();
          } catch (e) {
            fail("共享内存校验失败: " + e);
          }
        }
      });
    }
  };

  /* ---------- 等比缩放 ---------- */
  function fit() {
    const vw = window.innerWidth, vh = window.innerHeight;
    const scale = Math.min(vw / W, vh / H);
    canvas.style.width = Math.floor(W * scale) + "px";
    canvas.style.height = Math.floor(H * scale) + "px";
  }
  window.addEventListener("resize", fit);
  fit();

  /* ---------- 输入事件 → ring ---------- */
  window.__tab5evlog = [];
  // 事件重放节流: 合成 click/drag 会把 press+轨迹+release 同瞬间灌入,
  // LVGL 的 CLICKED/GESTURE 判定需要 PRESSED 状态跨多个 indev tick。
  // 真实手指/鼠标事件自带时间间隔 (队列近乎恒空), 此节流对它们无感。
  const evQueue = [];
  setInterval(() => {
    if (!SAB || evQueue.length === 0) return;
    const [type, a, b] = evQueue.shift();
    const w = Atomics.load(U32, 3);
    const base = 8 + (w % RING_SLOTS) * 4;
    U32[base] = type;
    U32[base + 1] = a >>> 0;
    U32[base + 2] = (b | 0) >>> 0;
    U32[base + 3] = 0;
    Atomics.store(U32, 3, w + 1);
  }, 8);
  function pushEvent(type, a, b) {
    if (!SAB) return;
    window.__tab5evlog.push([type, a, b, Date.now()]);
    if (window.__tab5evlog.length > 12) window.__tab5evlog.shift();
    evQueue.push([type, a, b]);
    if (evQueue.length > 64) evQueue.shift();  // 防洪
  }

  function toScreen(e) {
    const r = canvas.getBoundingClientRect();
    return [
      Math.max(0, Math.min(W - 1, Math.round((e.clientX - r.left) * W / r.width))),
      Math.max(0, Math.min(H - 1, Math.round((e.clientY - r.top) * H / r.height))),
    ];
  }

  canvas.addEventListener("pointerdown", e => {
    e.preventDefault();
    const [x, y] = toScreen(e);
    pushEvent(EV.MOVE, x, y);
    pushEvent(EV.PRESS, x, y);
  });
  canvas.addEventListener("pointermove", e => {
    const [x, y] = toScreen(e);
    pushEvent(EV.MOVE, x, y);
  });
  canvas.addEventListener("pointerup", e => {
    e.preventDefault();
    const [x, y] = toScreen(e);
    // 合成点击的 press→release 间隔短于 LVGL indev read 周期 (30ms) 时,
    // PRESSED 瞬态被采样跳过 → CLICKED 丢失。延迟 release 保证 LVGL 至少
    // 采样到一次完整按下; 真实手指 press→release 通常 >100ms, 无感。
    setTimeout(() => pushEvent(EV.RELEASE, x, y), 60);
  });
  canvas.addEventListener("pointercancel", e => {
    const [x, y] = toScreen(e);
    setTimeout(() => pushEvent(EV.RELEASE, x, y), 60);
  });
  canvas.addEventListener("contextmenu", e => e.preventDefault());
  canvas.addEventListener("wheel", e => {
    e.preventDefault();
    const diff = Math.max(-5, Math.min(5, Math.round(e.deltaY / 53)));
    if (diff !== 0) pushEvent(EV.WHEEL, 0, diff);
  }, { passive: false });

  window.addEventListener("keydown", e => {
    if (!SAB) return;
    let code = KMAP[e.key];
    if (code === undefined && e.key.length === 1) code = e.key.charCodeAt(0);
    if (code === undefined) return;
    if (e.key === "Escape" || e.key === "Tab") e.preventDefault();
    pushEvent(EV.KEYDOWN, 0, code);
  });
  window.addEventListener("keyup", e => {
    if (!SAB) return;
    let code = KMAP[e.key];
    if (code === undefined && e.key.length === 1) code = e.key.charCodeAt(0);
    if (code === undefined) return;
    pushEvent(EV.KEYUP, 0, code);
  });

  /* ---------- rAF 绘制: 读 seq 配对的 bbox → RGBA → canvas ---------- */
  let lastSeq = 0;
  function blit() {
    if (SAB) {
      const seq = Atomics.load(U32, 2);
      if (seq !== lastSeq) {
        // 逐 seq 消费 bbox ring (8 槽), 合并本周期所有脏区
        if (seq - lastSeq > 8) lastSeq = seq - 8;  // 落后过多直接对齐
        let mx1 = 1 << 30, my1 = 1 << 30, mx2 = -1, my2 = -1;
        for (let s = lastSeq + 1; s <= seq; s++) {
          const bo = 3 + (s & 7) * 4;
          mx1 = Math.min(mx1, U32[bo]); my1 = Math.min(my1, U32[bo + 1]);
          mx2 = Math.max(mx2, U32[bo + 2]); my2 = Math.max(my2, U32[bo + 3]);
        }
        lastSeq = seq;
        const x1 = mx1, y1 = my1, x2 = mx2, y2 = my2;
        const bw = x2 - x1 + 1, bh = y2 - y1 + 1;
        if (bw > 0 && bh > 0 && bw <= W && bh <= H) {
          const px = imgData.data;
          for (let y = 0; y < bh; y++) {
            let si = STAGING_U16 + (y1 + y) * W + x1;
            let di = ((y1 + y) * W + x1) << 2;   // imgData 为全屏布局
            
            for (let x = 0; x < bw; x++, si++, di += 4) {
              const v = U16[si];
              const r5 = (v >> 11) & 31, g6 = (v >> 5) & 63, b5 = v & 31;
              px[di]     = (r5 * 255 + 15) / 31 | 0;
              px[di + 1] = (g6 * 255 + 31) / 63 | 0;
              px[di + 2] = (b5 * 255 + 15) / 31 | 0;
              px[di + 3] = 255;
            }
          }
          offCtx.putImageData(imgData, 0, 0, x1, y1, bw, bh);
          ctx.imageSmoothingEnabled = true;
          ctx.drawImage(off, 0, 0, W, H, 0, 0, canvas.width, canvas.height);
        }
      }
    }
    requestAnimationFrame(blit);
  }
  requestAnimationFrame(blit);

  /* ---------- 全屏切换 (F11 / 双击) ---------- */
  window.addEventListener("dblclick", () => toggleFs());
  window.addEventListener("keydown", e => { if (e.key === "F11") { e.preventDefault(); toggleFs(); } });
  function toggleFs() {
    const el = document.documentElement;
    if (document.fullscreenElement) document.exitFullscreen && document.exitFullscreen();
    else if (el.requestFullscreen) el.requestFullscreen().catch(() => {});
  }

  /* ---------- emscripten 模块钩子 ---------- */
  window.Module = window.Module || {};
  Module.onRuntimeInitialized = function () {
    setProgress(0.9, "启动驾驶舱…");
  };
  window.__tab5logs = [];
  Module.printErr = function (t) { window.__tab5logs.push("E " + t); console.warn("[wasm]", t); };
  Module.print = function (t) { window.__tab5logs.push(t); console.log("[wasm]", t); };
  Module.locateFile = function (p) { return p; };
})();
