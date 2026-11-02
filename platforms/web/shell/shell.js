/*
 * shell.js — Tab5 WASM 外壳: 等比缩放 / 输入采集 / 帧绘制。
 * 在 tab5web.js (emscripten glue) 之前加载, 提供 Module 全局。
 *
 * 通信模型 (与 web_display.cpp 约定):
 *   - worker (PROXY_TO_PTHREAD main) postMessage {__tab5layout} 上报共享布局偏移;
 *     帧区/事件 ring 直接位于共享 wasm heap, 主线程经 Module.HEAPU16/32 同址访问;
 *   - 主线程写输入 ring → Atomics.store inputWrite; worker 在 indev read_cb 消费;
 *   - worker flush 完成后 frameSeq+1; 主线程 rAF 检查 seq, 把 published staging
 *     的脏区转 RGBA 画上 canvas。
 */
"use strict";

(function () {
  const W = 1280, H = 720;
  const RING_SLOTS = 256;
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

  /* ---------- 捕获 pthread worker 上报的共享布局 ---------- */
  let L = null;          // {frameSeq, inputWrite, publishedIdx, bbox, ring, staging0, staging1, magic}
  let U32 = null, U16 = null;  // 主线程侧共享堆视图

  const _Worker = window.Worker;
  window.Worker = class extends _Worker {
    constructor(u, o) {
      super(u, o);
      this.addEventListener("message", ev => {
        const d = ev.data;
        if (d && d.__tab5layout) {
          try {
            L = d.__tab5layout;
            U32 = new Uint32Array(Module.HEAPU8.buffer);
            U16 = new Uint16Array(Module.HEAPU8.buffer);
            const magic = U32[L.magic >> 2];
            if (magic !== 0x54414235) throw new Error("magic mismatch: 0x" + magic.toString(16));
            setProgress(1, "启动中…");
            canvas.style.display = "block";
            $("loader").style.display = "none";
            fit();
          } catch (e) {
            fail("共享内存布局校验失败: " + e);
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
  function pushEvent(type, a, b) {
    if (!L || !U32) return;
    const w = Atomics.load(U32, L.inputWrite >> 2);
    const slot = (w % RING_SLOTS) * 4;
    U32[(L.ring >> 2) + slot] = type;
    U32[(L.ring >> 2) + slot + 1] = a >>> 0;
    U32[(L.ring >> 2) + slot + 2] = (b | 0) >>> 0;
    U32[(L.ring >> 2) + slot + 3] = 0;
    Atomics.store(U32, L.inputWrite >> 2, w + 1);
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
    if (e.pointerType === "mouse" && e.buttons === 0 && e.pressure === 0) {
      // 悬停也上报 (LVGL hovered 状态需要)
      const [x, y] = toScreen(e);
      pushEvent(EV.MOVE, x, y);
      return;
    }
    const [x, y] = toScreen(e);
    pushEvent(EV.MOVE, x, y);
  });
  canvas.addEventListener("pointerup", e => {
    e.preventDefault();
    const [x, y] = toScreen(e);
    pushEvent(EV.RELEASE, x, y);
  });
  canvas.addEventListener("pointercancel", e => {
    const [x, y] = toScreen(e);
    pushEvent(EV.RELEASE, x, y);
  });
  canvas.addEventListener("contextmenu", e => e.preventDefault());
  canvas.addEventListener("wheel", e => {
    e.preventDefault();
    const diff = Math.max(-5, Math.min(5, Math.round(e.deltaY / 53)));
    if (diff !== 0) pushEvent(EV.WHEEL, 0, diff);
  }, { passive: false });

  window.addEventListener("keydown", e => {
    if (!L) return;
    let code = KMAP[e.key];
    if (code === undefined && e.key.length === 1) code = e.key.charCodeAt(0);
    if (code === undefined) return;
    if (e.key === "Escape" || e.key === "Tab" || (e.key === " " && !e.target.closest("input"))) e.preventDefault();
    pushEvent(EV.KEYDOWN, 0, code);
  });
  window.addEventListener("keyup", e => {
    if (!L) return;
    let code = KMAP[e.key];
    if (code === undefined && e.key.length === 1) code = e.key.charCodeAt(0);
    if (code === undefined) return;
    pushEvent(EV.KEYUP, 0, code);
  });

  /* ---------- rAF 绘制: 读 published staging 脏区 → RGBA → canvas ---------- */
  let lastSeq = 0;
  function blit() {
    if (L && U32 && U16) {
      const seq = Atomics.load(U32, L.frameSeq >> 2);
      if (seq !== lastSeq) {
        lastSeq = seq;
        const idx = U32[L.publishedIdx >> 2];
        const x1 = U32[L.bbox >> 2], y1 = U32[(L.bbox >> 2) + 1];
        const x2 = U32[(L.bbox >> 2) + 2], y2 = U32[(L.bbox >> 2) + 3];
        const bw = x2 - x1 + 1, bh = y2 - y1 + 1;
        if (bw > 0 && bh > 0 && bw <= W && bh <= H) {
          const src = (idx ? L.staging1 : L.staging0) >> 1;  // U16 index
          const px = imgData.data;
          const d = px;
          for (let y = 0; y < bh; y++) {
            let si = src + (y1 + y) * W + x1;
            let di = ((y * bw)) << 2;
            for (let x = 0; x < bw; x++, si++, di += 4) {
              const v = U16[si];
              const r5 = (v >> 11) & 31, g6 = (v >> 5) & 63, b5 = v & 31;
              d[di]     = (r5 * 255 + 15) / 31 | 0;
              d[di + 1] = (g6 * 255 + 31) / 63 | 0;
              d[di + 2] = (b5 * 255 + 15) / 31 | 0;
              d[di + 3] = 255;
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

  /* ---------- 全屏切换 (F 键 / 双击) ---------- */
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
  Module.printErr = function (t) { console.warn("[wasm]", t); };
  Module.print = function (t) { console.log("[wasm]", t); };
  Module.locateFile = function (p) { return p; };
})();
