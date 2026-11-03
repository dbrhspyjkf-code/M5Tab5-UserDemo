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
 *   u32[0] magic | [1] version | [2] frameSeq | [3] inputWrite
 *   u32[4..35] bbox[8][4] (槽 = seq&7) | u32[36..] ring[256][4]
 *   byte 4240 staging u16[1280*720]
 *   (bbox/ring 重叠曾致幽灵输入循环, 2026-11-03 修复)
 */
"use strict";

(function () {
  const W = 1280, H = 720;
  const RING_SLOTS = 256;
  const STAGING_U16 = 4272 >> 1;
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

  // 全局错误 → loader 区直接显示 (设备端排障: iPad/车机无法看 console)
  window.addEventListener("error", e => fail("JS错误: " + e.message + " @" + (e.filename||"") + ":" + e.lineno));
  window.addEventListener("unhandledrejection", e => fail("Promise拒绝: " + (e.reason && e.reason.message || e.reason)));

  /* ---------- 收音机 <audio> (主线程; worker 经 postMessage 命令) ---------- */
  const radio = document.createElement("audio");
  radio.preload = "none";
  radio.crossOrigin = "use-credentials";   // 同源 cookie (网关会话)
  document.body.appendChild(radio);
  let radioUnlocked = false;
  const writeRadioStatus = () => {
    if (!SAB) return;
    U32[1060] = (!radio.paused && radio.readyState >= 2) ? 1 : 0;
    U32[1061] = Math.round(radio.volume * 100);
    U32[1062] = radio.error ? 1 : 0;
  };
  ["play", "pause", "playing", "error", "volumechange", "stalled", "waiting"]
    .forEach(ev => radio.addEventListener(ev, writeRadioStatus));

  const radioCmd = (c) => {
    if (c.cmd === "play" && c.key) {
      radio.src = "/gw/8769/radio/" + encodeURIComponent(c.key) + "/ext";
      radio.play().catch(e => console.warn("[tab5web] radio play:", e.message));
    } else if (c.cmd === "stop") {
      radio.pause();
      radio.removeAttribute("src");
      radio.load();
    } else if (c.cmd === "vol") {
      radio.volume = Math.max(0, Math.min(1, (c.v | 0) / 100));
    }
    setTimeout(writeRadioStatus, 50);
  };

  /* ---------- 接收 worker 上报的 SAB ---------- */
  let SAB = null, U32 = null, U16 = null;

  let theWorker = null;   // tab5 wasm worker (回传 jpeg 解码结果用)

  const _Worker = window.Worker;
  window.Worker = class extends _Worker {
    constructor(u, o) {
      super(u, o);
      theWorker = this;
      this.addEventListener("message", ev => {
        const d = ev.data;
        if (d && d.__tab5uart) {
          (window.__tab5uart = window.__tab5uart || []).push(d.__tab5uart);
          return;
        }
        if (d && d.__tab5radio) { radioCmd(d.__tab5radio); return; }
        if (d && d.__tab5jpg) { decodeJpgForWorker(d.__tab5jpg.bytes); return; }
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
            showBack();
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
    // 车机 WebView 探针实锤 window 高度可为 0 —— 兜底到屏幕尺寸
    const vw = window.innerWidth || screen.availWidth || W;
    const vh = window.innerHeight || screen.availHeight || H;
    const scale = Math.max(0.1, Math.min(vw / W, vh / H));
    canvas.style.width = Math.floor(W * scale) + "px";
    canvas.style.height = Math.floor(H * scale) + "px";
  }
  window.addEventListener("resize", fit);
  fit();

  /* ---------- 输入事件 → ring ---------- */
  window.__tab5evlog = [];
  // 事件直写 ring (无重放队列): setInterval 重放在后台标签会被浏览器
  // 节流至停摆, 且合并截断曾误删排在 MOVE 后的 PRESS/RELEASE。
  // 时序保障改由: ①release 延迟 60ms 入队 (PRESS 跨 indev 采样周期);
  // ②worker 侧 drain 每周期限 2 条; ③事件 seq 自校验防套圈重放。
  function pushEvent(type, a, b) {
    if (!SAB) return;
    window.__tab5evlog.push([type, a, b, Date.now()]);
    if (window.__tab5evlog.length > 40) window.__tab5evlog.shift();
    const w = Atomics.load(U32, 3);
    const base = 36 + (w % RING_SLOTS) * 4;
    U32[base] = type;
    U32[base + 1] = a >>> 0;
    U32[base + 2] = (b | 0) >>> 0;
    U32[base + 3] = (w + 1) >>> 0;   // seq: 事件自校验 (防套圈重读幽灵)
    Atomics.store(U32, 3, w + 1);
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
    // Safari/iOS: play() 必须发生在真实手势内 —— 首次按下用静音空源
    // 解锁元素, 之后 worker 触发的 play() 才被放行 (Chrome 靠粘性激活天然通过)
    if (!radioUnlocked) {
      radioUnlocked = true;
      const keep = radio.src;
      radio.src = "data:audio/wav;base64,UklGRiQAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQAAAAA=";
      radio.muted = true;
      radio.play().then(() => { radio.pause(); radio.muted = false; if (keep) radio.src = keep; })
                  .catch(() => { radio.muted = false; if (keep) radio.src = keep; });
    }
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
          const bo = 4 + (s & 7) * 4;
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

  // 主线程 JPEG→RGB565 解码 (worker 经 postMessage 送字节; DOM canvas 路径)
  async function decodeJpgForWorker(buf) {
    const done = (ok, px) => theWorker &&
      theWorker.postMessage({__tab5jpgdone: {ok, px: px || null}});
    try {
      const url = URL.createObjectURL(new Blob([buf], {type: "image/jpeg"}));
      const img = new Image();
      await new Promise((res, rej) => { img.onload = res; img.onerror = rej; img.src = url; });
      const c = document.createElement("canvas");
      c.width = img.naturalWidth; c.height = img.naturalHeight;
      const ctx = c.getContext("2d");
      ctx.drawImage(img, 0, 0);
      URL.revokeObjectURL(url);
      const d = ctx.getImageData(0, 0, c.width, c.height).data;
      const px = new Uint16Array(c.width * c.height);
      for (let i = 0, j = 0; j < px.length; i += 4, j++)
        px[j] = ((d[i] >> 3) << 11) | ((d[i + 1] >> 2) << 5) | (d[i + 2] >> 3);
      done(1, px);
    } catch (e) { console.warn("[tab5web] jpg decode:", e); done(0); }
  }

  /* ---------- 浮动返回键: 注入标准"底部起手上拨"手势 ---------- */
  // iPad 上真实上拨会被 Chrome 手势吞掉(返回/关页), 浮动键是网页版的返回。
  // 序列满足所有 app 的判定: 起点 y>650, dy<-120, 纵向主导。
  const backBtn = document.createElement("div");
  backBtn.id = "tab5back";
  backBtn.style.cssText =
    "position:fixed;right:14px;top:14px;z-index:50;width:52px;height:52px;" +
    "border-radius:50%;background:rgba(0,0,0,.45);border:1px solid rgba(126,200,126,.55);" +
    "display:none;align-items:center;justify-content:center;cursor:pointer;" +
    "-webkit-user-select:none;user-select:none;touch-action:none;";
  backBtn.innerHTML =
    '<svg width="26" height="26" viewBox="0 0 24 24" fill="none">' +
    '<path d="M12 19V6M6 12l6-6 6 6" stroke="#bfe8c4" stroke-width="2.4" ' +
    'stroke-linecap="round" stroke-linejoin="round"/></svg>';
  document.body.appendChild(backBtn);
  const showBack = () => { backBtn.style.display = "flex"; };
  const fireBack = () => {
    if (!SAB) return;
    pushEvent(EV.MOVE, 640, 700);
    pushEvent(EV.PRESS, 640, 700);
    const path = [[640, 620], [640, 520], [640, 400], [640, 280], [640, 160], [640, 110]];
    path.forEach((p, i) => setTimeout(() => pushEvent(EV.MOVE, p[0], p[1]), 25 + i * 25));
    setTimeout(() => pushEvent(EV.RELEASE, 640, 100), 25 + path.length * 25 + 40);
  };
  backBtn.addEventListener("pointerdown", e => {
    e.preventDefault(); e.stopPropagation();
    backBtn.style.background = "rgba(47,90,59,.75)";
    fireBack();
  });
  backBtn.addEventListener("pointerup", () => { backBtn.style.background = "rgba(0,0,0,.45)"; });

  /* ---------- 全屏切换 (F11 / 双击) ---------- */
  window.addEventListener("dblclick", () => toggleFs());
  window.addEventListener("keydown", e => { if (e.key === "F11") { e.preventDefault(); toggleFs(); } });
  function toggleFs() {
    const el = document.documentElement;
    if (document.fullscreenElement) document.exitFullscreen && document.exitFullscreen();
    else if (el.requestFullscreen) el.requestFullscreen().catch(() => {});
  }

  /* ---------- 调试浮层 (?debug=1): 实时显示 worker uart 日志 ---------- */
  if (location.search.includes("debug")) {
    const dbg = document.createElement("div");
    dbg.style.cssText = "position:fixed;left:4px;bottom:4px;z-index:99;background:rgba(0,0,0,.75);" +
      "color:#7ec87e;font:10px/1.4 ui-monospace,Menlo,monospace;padding:4px 6px;" +
      "border-radius:6px;max-width:60vw;max-height:40vh;overflow:hidden;pointer-events:none;" +
      "white-space:pre-wrap;word-break:break-all;";
    document.body.appendChild(dbg);
    setInterval(() => {
      const u = (window.__tab5uart || []).slice(-8);
      const ev = (window.__tab5evlog || []).slice(-4)
        .map(e => "ev " + e[0] + "@" + e[1] + "," + e[2]);
      dbg.textContent = u.concat(ev).join("\n");
    }, 400);
  }

  /* ---------- emscripten 模块钩子 ---------- */
  window.Module = window.Module || {};
  Module.onRuntimeInitialized = function () {
    setProgress(0.9, "启动驾驶舱…");
  };
  window.__tab5logs = [];
  Module.printErr = function (t) { window.__tab5logs.push("E " + t); console.warn("[wasm]", t); };
  Module.print = function (t) { window.__tab5logs.push(t); console.log("[wasm]", t); };
  Module.locateFile = function (p) {
    // wasm 地址挂构建版本: 新部署即换缓存键 (部署后旧缓存不再干扰),
    // 平时可长缓存; index.html 本身 no-cache, 版本号每次部署更新
    if (p.endsWith(".wasm") && window.TAB5_BUILD) return p + "?v=" + window.TAB5_BUILD;
    return p;
  };
})();
