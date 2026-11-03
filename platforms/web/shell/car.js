"use strict";
/* car.html 逻辑 — CSP script-src 禁内联 <script>, 必须外置 */
const $ = id => document.getElementById(id);

/* ════ 农历 (逐字移植 app/apps/utils/datetime/lunar_calendar.h, 与设备一致) ════ */
const LUNAR_INFO = [
0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
0x06566,0x0d4a0,0x0ea50,0x16a95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
0x06ca0,0x0b550,0x15355,0x04da0,0x0a5b0,0x14573,0x052b0,0x0a9a8,0x0e950,0x06aa0,
0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b6a0,0x195a6,
0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x05ac0,0x0ab60,0x096d5,0x092e0,
0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0,
0x14b63,0x09370,0x049f8,0x04970,0x064b0,0x168a6,0x0ea50,0x06b20,0x1a6c4,0x0aae0,
0x092e0,0x0d2e3,0x0c960,0x0d557,0x0d4a0,0x0da50,0x05d55,0x056a0,0x0a6d0,0x055d4,
0x052d0,0x0a9b8,0x0a950,0x0b4a0,0x0b6a6,0x0ad50,0x055a0,0x0aba4,0x0a5b0,0x052b0,
0x0b273,0x06930,0x07337,0x06aa0,0x0ad50,0x14b55,0x04b60,0x0a570,0x054e4,0x0d260,
0x0e968,0x0d520,0x0daa0,0x16aa6,0x056d0,0x04ae0,0x0a9d4,0x0a4d0,0x0d150,0x0f252,
0x0d520];
// Howard Hinnant days_from_civil: 自 0000-03-01 起的连续纪日
function daysFromCivil(y, m, d) {
  y -= (m <= 2) ? 1 : 0;
  const era = Math.floor((y >= 0 ? y : y - 399) / 400);
  const yoe = y - era * 400;
  const doy = Math.floor((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1;
  const doe = yoe * 365 + Math.floor(yoe / 4) - Math.floor(yoe / 100) + doy;
  return era * 146097 + doe - 719468;
}
function leapMonthOf(ly)   { return LUNAR_INFO[ly - 1900] & 0xf; }
function leapDaysOf(ly)    { const lm = leapMonthOf(ly);
                             return lm ? ((LUNAR_INFO[ly - 1900] & 0x10000) ? 30 : 29) : 0; }
function monthDaysOf(ly, m){ return (LUNAR_INFO[ly - 1900] & (0x10000 >> m)) ? 30 : 29; }
function yearDaysOf(ly) {
  let sum = 348;
  for (let bit = 0x8000; bit > 0x8; bit >>= 1)
    if (LUNAR_INFO[ly - 1900] & bit) ++sum;
  return sum + leapDaysOf(ly);
}
const LM_CHARS = ["正","二","三","四","五","六","七","八","九","十","冬","腊"];
const LD_CHARS = ["初一","初二","初三","初四","初五","初六","初七","初八","初九","初十",
                  "十一","十二","十三","十四","十五","十六","十七","十八","十九","二十",
                  "廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十"];
function solarToLunar(y, m, d) {
  if (y < 1900 || y > 2100) return null;
  let rest = daysFromCivil(y, m, d) - daysFromCivil(1900, 1, 31);
  if (rest < 0) return null;
  let ly = 1900;
  for (;;) {
    const yd = yearDaysOf(ly);
    if (rest < yd) break;
    rest -= yd;
    if (++ly > 2100) return null;
  }
  const leap = leapMonthOf(ly);
  let lm = 1, isLeap = false;
  for (let ord = 1; ord <= 12; ++ord) {
    const md = monthDaysOf(ly, ord);
    if (rest < md) { lm = ord; isLeap = false; break; }
    rest -= md;
    if (leap === ord) {
      const ld = leapDaysOf(ly);
      if (rest < ld) { lm = ord; isLeap = true; break; }
      rest -= ld;
    }
    if (ord === 12) return null;
  }
  return { y: ly, m: lm, d: rest + 1, leap: isLeap };
}
function lunarText(y, m, d) {
  const ld = solarToLunar(y, m, d);
  if (!ld) return "";
  return (ld.leap ? "闰" : "") + LM_CHARS[ld.m - 1] + "月" + LD_CHARS[ld.d - 1];
}

/* ════ 主题 (色值与 theme.h kThemes 一致; 晨白为网页端新增) ════ */
const DARK_TXT = { hi:"#F2F7F3", soft:"#C7D6CB", dim:"#7E9486" };
const DARK_SEM = { ok:"#34D399", warn:"#FBBF24", danger:"#EF4444" };
const THEMES = [
  { name:"松绿", hero:"#204234", accent:"#34D399", press:"#2F8066",
    cards:["#1B362C","#173024","#13291F","#0F221A"], border:"#2C4433",
    track:"#0A1811", page:"#000000", ...DARK_TXT, ...DARK_SEM },
  { name:"暖橙", hero:"#6E3A16", accent:"#F5A263", press:"#B3661F",
    cards:["#3A2412","#30200F","#271A0C","#1D1309"], border:"#5C3A1C",
    track:"#160D04", page:"#000000", ...DARK_TXT, ...DARK_SEM },
  { name:"海蓝", hero:"#1C4258", accent:"#4FA3FF", press:"#2F6FA8",
    cards:["#16334A","#122B40","#0E2233","#0A1A28"], border:"#2A4A6A",
    track:"#081522", page:"#000000", ...DARK_TXT, ...DARK_SEM },
  { name:"岩紫", hero:"#3A2C55", accent:"#B08CFF", press:"#6A4FA0",
    cards:["#2B213F","#251C37","#1E162C","#171022"], border:"#453663",
    track:"#120D1E", page:"#000000", ...DARK_TXT, ...DARK_SEM },
  { name:"晨白", hero:"#F4FAF6", accent:"#1E9E6A", press:"#DCEFE4",
    cards:["#FFFFFF","#F7FAF8","#F2F6F3","#EDF2EE"], border:"#D8E2DA",
    track:"#E4ECE6", page:"#EEF3EF",
    hi:"#1A2B22", soft:"#4A5D52", dim:"#8AA094",
    ok:"#18A05E", warn:"#D99A12", danger:"#DC4444" },
];
let themeIdx = 0;
try { themeIdx = (parseInt(localStorage.getItem("car_theme_idx"), 10) || 0) % THEMES.length; } catch (e) {}
function applyTheme(i) {
  themeIdx = ((i % THEMES.length) + THEMES.length) % THEMES.length;
  const t = THEMES[themeIdx], s = document.documentElement.style;
  s.setProperty("--hero", t.hero);   s.setProperty("--accent", t.accent);
  s.setProperty("--press", t.press);
  for (let k = 0; k < 4; k++) s.setProperty("--card" + k, t.cards[k]);
  s.setProperty("--border", t.border); s.setProperty("--track", t.track);
  s.setProperty("--page", t.page);
  s.setProperty("--hi", t.hi);       s.setProperty("--soft", t.soft);
  s.setProperty("--dim", t.dim);
  s.setProperty("--ok", t.ok);       s.setProperty("--warn", t.warn);
  s.setProperty("--danger", t.danger);
  $("theme-name").textContent = t.name;
  try { localStorage.setItem("car_theme_idx", String(themeIdx)); } catch (e) {}
  renderOpi();   // 温度格等 JS 着色元素跟随新语义色
}

/* ════ Toast ════ */
let _toastTimer = 0;
function toast(msg, ms) {
  const el = $("toast");
  el.textContent = msg; el.style.opacity = "1";
  clearTimeout(_toastTimer);
  _toastTimer = setTimeout(() => { el.style.opacity = "0"; }, ms || 1600);
}

/* ════ 数据拉取 (同源 /gw 代理, cookie 自动带; 失败静默降级) ════ */
let _warned401 = false;
async function getJSON(url, timeoutMs) {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), timeoutMs || 10000);
  try {
    const r = await fetch(url, { signal: ctl.signal, credentials: "same-origin" });
    if (!r.ok) { note401(r.status); return null; }
    return await r.json();
  } catch (e) { return null; }
  finally { clearTimeout(t); }
}
function setText(id, txt) {   // 变了才写, 免无谓重排
  const el = $(id);
  if (el.textContent !== txt) el.textContent = txt;
}

/* ── 时钟 / 日期 / 农历 (本地, 每秒) ── */
const WEEK = ["日","一","二","三","四","五","六"];
function tickClock() {
  const n = new Date();
  const p2 = v => String(v).padStart(2, "0");
  $("clock").innerHTML = p2(n.getHours()) + ":" + p2(n.getMinutes()) +
                         '<span class="sec">:' + p2(n.getSeconds()) + "</span>";
  setText("date", n.getFullYear() + "年" + (n.getMonth() + 1) + "月" + n.getDate() + "日 星期" + WEEK[n.getDay()]);
  setText("lunar", "农历 " + (lunarText(n.getFullYear(), n.getMonth() + 1, n.getDate()) || "--"));
}

/* ── 01 GLM / DeepSeek ── */
async function refreshGlm() {
  const j = await getJSON("/gw/8766/glm/quota");
  if (j && j.ok) {
    let lvl = String(j.level || "");
    if (lvl && lvl[0] >= "a" && lvl[0] <= "z") lvl = lvl[0].toUpperCase() + lvl.slice(1);
    const p2 = v => String(v).padStart(2, "0");
    const fmtReset = (ms, withDate) => {
      if (!ms || ms <= 0) return "--";
      const d = new Date(ms);
      return withDate
        ? p2(d.getMonth() + 1) + "-" + p2(d.getDate()) + " " + p2(d.getHours()) + ":" + p2(d.getMinutes())
        : p2(d.getHours()) + ":" + p2(d.getMinutes());
    };
    if (j.pct_5h != null || j.pct_7d != null) {
      setText("glm", lvl + "  5H " + (j.pct_5h ?? 0) + "%  7D " + (j.pct_7d ?? 0) + "%");
      setText("glm-sub", "重置 " + fmtReset(j.reset_5h_ms, false) + "  /  " + fmtReset(j.reset_7d_ms, true));
    } else {
      setText("glm", lvl + "级 剩" + (j.remaining_pct ?? 0) + "%");
    }
  }
  const d = await getJSON("/gw/8766/api/deepseek/balance");
  if (d && d.ok) setText("ds", (d.total_balance || "?") + " 元");
}

/* ── 02 提醒/日程 ── */
async function refreshAgenda() {
  const j = await getJSON("/gw/8766/agenda");
  if (!j || !j.ok) return;
  setText("ag1", (j.next && j.next.text) ? j.next.text : "今日无提醒/行程");
  setText("ag2", (j.second && j.second.text) ? j.second.text : "");
}

/* ── 03 AI 新闻 (10s 自动推进 + 点击推进, 与设备一致) ── */
let newsItems = [], newsIdx = 0;
async function refreshNews() {
  const j = await getJSON("/gw/8766/news/ai");
  if (!j || !j.ok) return;
  const items = [];
  const secs = Array.isArray(j.secs) ? j.secs : [];
  for (const sec of secs) {
    const label = sec.label || "新闻";
    const its = Array.isArray(sec.items) ? sec.items : [];
    for (const it of its) {
      if (typeof it !== "string" || !it) continue;
      items.push({ t: "【" + label + "】" + it, s: "AI 新闻 · " + (j.date || "") });
      if (items.length >= 12) break;
    }
    if (items.length >= 12) break;
  }
  if (items.length) {
    newsItems = items;
    if (newsIdx >= newsItems.length) newsIdx = 0;
    renderNews();
  }
}
function renderNews() {
  if (!newsItems.length) return;
  const it = newsItems[newsIdx % newsItems.length];
  setText("news-t", it.t); setText("news-s", it.s);
}
$("news").addEventListener("click", () => { newsIdx++; renderNews(); });

/* ── 04 健康 4×2 (字段/顺序与 app_home 一致; 数据时间右上角) ── */
const HEALTH_CELLS = [
  { t:"step_count",                  u:" 步",   dec:0,  label:"步数" },
  { t:"walking_running_distance",    u:" km",   dec:2,  label:"步行距离" },
  { t:"heart_rate",                  u:" bpm",  dec:0,  label:"心率" },
  { t:"blood_oxygen_saturation",     u:" %",    dec:0,  label:"血氧" },
  { t:"respiratory_rate",            u:" 次/分", dec:1,  label:"呼吸速率" },
  { t:"active_energy_burned",        u:" kcal", dec:0,  label:"活动能量" },
  { t:"flights_climbed",             u:" 层",   dec:0,  label:"爬楼层" },
  { t:"sleep_analysis",              u:"",      dec:-2, label:"睡眠" },
];
function buildHealthGrid() {
  $("hgrid").innerHTML = HEALTH_CELLS.map((c, i) =>
    '<div class="hcell"><div class="hl">' + c.label +
    '</div><div class="hv" id="hv' + i + '">-</div></div>').join("");
}
async function refreshHealth() {
  const j = await getJSON("/gw/8766/api/health");
  if (!j || !j.ok) return;
  const types = (j.types && typeof j.types === "object") ? j.types : {};
  const ua = String(j.updated_at || "");
  if (ua.length >= 16) setText("h-time", ua.substr(11, 5));
  HEALTH_CELLS.forEach((c, i) => {
    let txt = "-";
    const e = types[c.t];
    if (e && typeof e === "object" && e.value != null) {
      const v = typeof e.value === "number" ? e.value : parseFloat(e.value);
      if (!isNaN(v)) {
        if (c.dec === -2) txt = Math.floor(v / 60) + ":" + String(Math.floor(v) % 60).padStart(2, "0") + " 小时";
        else if (c.dec === 0) txt = Math.round(v) + c.u;
        else txt = v.toFixed(c.dec) + c.u;
      }
    }
    setText("hv" + i, txt);
  });
}

/* ── 05 OPi 状态 3×2 + 阈值色条 (阈值与 _updateOpiWidgets 一致) ── */
const OPI_MODS = [
  { key:"load",  label:"负载" }, { key:"cpu",   label:"CPU" },
  { key:"mem",   label:"内存" }, { key:"swap",  label:"交换" },
  { key:"disk",  label:"磁盘" }, { key:"temp",  label:"温度" },
];
function buildOpiGrid() {
  $("ogrid").innerHTML = OPI_MODS.map((m, i) =>
    '<div class="omod"><div class="ol">' + m.label +
    '</div><div class="ov" id="ov' + i + '">--</div>' +
    (m.key !== "temp" ? '<div class="bar"><div class="fill" id="ob' + i + '"></div></div>' : "") +
    "</div>").join("");
}
let _opiOk = false, _opiTemp = 0;
function tierColor(pct, warn, danger) {
  const t = THEMES[themeIdx];
  if (pct >= danger) return t.danger;
  if (pct >= warn) return t.warn;
  return t.ok;
}
function setBar(i, pct, warn, danger) {
  const el = $("ob" + i);
  if (!el) return;
  pct = Math.max(0, Math.min(100, pct));
  el.style.width = pct.toFixed(0) + "%";
  el.style.background = tierColor(pct, warn, danger);
}
function renderOpi() {   // 主题切换时温度格重着色
  if (!_opiOk) return;
  $("opi-dot").style.background = THEMES[themeIdx].ok;
  const el = $("ov5");
  el.style.color = tierColor(_opiTemp, 60, 75);
}
async function refreshOpi() {
  const j = await getJSON("/gw/8766/system/status");
  if (!j || !j.ok) {   // 失败: 全 "--" + 红点 (与设备 _updateOpiWidgets 一致)
    _opiOk = false;
    $("opi-dot").style.background = THEMES[themeIdx].danger;
    for (let i = 0; i < OPI_MODS.length; i++) {
      setText("ov" + i, "--");
      const b = $("ob" + i); if (b) b.style.width = "0%";
    }
    setText("opi-up", "--");
    return;
  }
  _opiOk = true;
  $("opi-dot").style.background = THEMES[themeIdx].ok;
  const host = j.host || {};
  setText("opi-up", "up " + (host.uptime_human || "--") + " · " + (host.ip || "--"));
  const ld = j.load || {}, mm = j.memory || {},
        dk = (Array.isArray(j.disk) && j.disk[0]) ? j.disk[0] : {},
        th = j.thermal || {};
  const load1 = ld.load1 || 0;
  setText("ov0", (ld.load1||0).toFixed(2) + " " + (ld.load5||0).toFixed(2) + " " + (ld.load15||0).toFixed(2));
  setBar(0, load1 / 4 * 100, 60, 80);
  const cpu = ld.cpu_percent || 0;
  setText("ov1", cpu.toFixed(1) + "%");
  setBar(1, cpu, 60, 80);
  const memPct = mm.percent || 0;
  const memU10 = Math.trunc((mm.used_mb || 0) / 102.4 + 0.5),   // GB×10 (设备 mem_used_a)
        memT10 = Math.trunc((mm.total_mb || 0) / 102.4 + 0.5);  // GB×10 (设备 mem_tot_a)
  setText("ov2", Math.trunc(memU10 / 10) + "." + (memU10 % 10) + "/" +
                 Math.trunc((memT10 + 5) / 10) + "G " + memPct.toFixed(0) + "%");
  setBar(2, memPct, 70, 85);
  const sw = mm.swap_percent || 0;
  setText("ov3", sw.toFixed(0) + "%");
  setBar(3, sw, 70, 85);
  const dPct = dk.percent || 0;
  setText("ov4", Math.round(dk.used_gb || 0) + "/" + Math.round(dk.total_gb || 0) + "G " + dPct.toFixed(0) + "%");
  setBar(4, dPct, 70, 85);
  _opiTemp = th.cpu_temp_c || 0;
  setText("ov5", _opiTemp.toFixed(0) + "℃");
  $("ov5").style.color = tierColor(_opiTemp, 60, 75);
}

/* ── 每日一句 (点击 offset+1, 与设备一致) ── */
let sentOffset = 0;
async function refreshSentence() {
  const j = await getJSON("/gw/8766/daily/sentence?offset=" + sentOffset);
  if (!j || !j.ok || !j.en) return;   // 失败保持上次内容
  setText("sent-en", j.en);
  setText("sent-cn", j.cn || "");
}
$("sent").addEventListener("click", () => { sentOffset++; refreshSentence(); });

/* ── 新股申购 (hero, 最多 3 条; 无新股隐藏) ── */
async function refreshIpo() {
  const j = await getJSON("/gw/8766/stocks/ipo");
  if (!j || !j.ok) return;
  const lines = [];
  const items = Array.isArray(j.items) ? j.items : [];
  for (const it of items) {
    if (lines.length >= 3) break;
    lines.push((it.name || "") + " " + (it.date || "") + (it.today ? "(今)" : ""));
  }
  $("ipo").hidden = lines.length === 0;
  setText("ipo-list", lines.join("\n"));
}

/* ── 天气 (HA 和风天气实体, 与设备同 entity; 失败 "--") ── */
const WX_ENTITY = "weather.he_feng_tian_qi_heweather";
function condZh(s) {
  const m = { "sunny":"晴", "clear-night":"晴", "partlycloudy":"多云", "cloudy":"阴",
    "fog":"雾", "rainy":"小雨", "pouring":"大雨", "lightning":"雷阵雨",
    "lightning-rainy":"雷雨", "snowy":"雪", "snowy-rainy":"雨夹雪", "hail":"冰雹",
    "windy":"大风", "windy-variant":"大风", "exceptional":"异常" };
  return m[s] || s;
}
async function refreshWeather() {
  const j = await getJSON("/gw/8123/api/states/" + WX_ENTITY);
  if (!j || !j.state || j.state === "unavailable" || j.state === "unknown") return;
  const at = j.attributes || {};
  if (at.temperature != null) {
    const t = typeof at.temperature === "number" ? at.temperature : parseFloat(at.temperature);
    if (!isNaN(t)) setText("wx-t", Math.round(t) + "°C");
  }
  setText("wx-c", condZh(j.state));
}

/* ════ 收音机 (电台表进页一次; mp3 直流; 手势解锁 play) ════ */
const audio = $("audio");
let stations = [], curKey = null, wantPlay = false;
let vol = 80;
try { vol = Math.max(0, Math.min(100, parseInt(localStorage.getItem("car_vol"), 10) || 80)); } catch (e) {}
$("r-vol").value = vol;
audio.volume = vol / 100;

async function loadStations() {
  const j = await getJSON("/gw/8769/radio/stations");
  if (!j || !j.ok || !Array.isArray(j.stations) || !j.stations.length) {
    setText("r-stat", "电台表加载失败");
    return;
  }
  stations = j.stations;
  $("chips").innerHTML = stations.map(s => {
    const fq = s.freq > 0 ? (s.freq / 10).toFixed(1) : "";
    return '<button class="chip" data-key="' + s.key + '" title="' + s.name + '">' + s.name +
           (fq ? '<span class="fq">FM ' + fq + "</span>" : "") + "</button>";
  }).join("");
  Array.of(...$("chips").querySelectorAll(".chip")).forEach(b =>
    b.addEventListener("click", () => playStation(b.dataset.key)));
  setText("r-stat", stations.length + " 个电台");
}
function markChip() {
  Array.of(...$("chips").querySelectorAll(".chip")).forEach(b =>
    b.classList.toggle("on", b.dataset.key === curKey));
}
function playStation(key) {
  const st = stations.find(s => s.key === key);
  if (!st) return;
  curKey = key; wantPlay = true; markChip();
  const url = "/gw/8769/radio/" + st.key + "/ext";
  const abs = location.origin + url;
  if (!audio.src || audio.src !== abs) audio.src = url;
  audio.volume = vol / 100;
  setText("r-cur", st.name + " · 连接中…");
  setText("r-stat", "缓冲中…");
  startLevelStream(key);   // 立即预热: audio 缓冲与 ffmpeg 冷启动并行
  audio.play().catch(() => {   // 自动播放策略/网络失败 → 静默给状态行
    wantPlay = false;
    stopLevelStream();
    setText("r-cur", st.name + " · 播放失败");
    setText("r-stat", "播放失败");
    $("r-play").textContent = "⏵";
  });
}
/* ── VU 电平: relay /radio/<key>/lv 8Hz "b m t\n" (0-100, 三频段 RMS) ──
   20 条渲染与设备 app_radio _updateVu 同款: 钟形频段权重(左 bass 右
   treble)、上升快(0.45)降慢(0.10+i%5×0.04)错落平滑、峰值帽 ×0.92 缓降、
   单色 accent(C_AMBER=主题 accent)。7s 无数据降级 CSS 伪动画 */
const VU_N = 20;
let lvAbort = null, lvFallbackTimer = 0, lvGotData = false;
let lvl = { b: 0, m: 0, t: 0 };
let vuCur = new Array(VU_N).fill(4), vuPk = new Array(VU_N).fill(4), vuTimer = 0;
function buildVU() {
  $("vu20").innerHTML = Array.from({ length: VU_N }, () =>
    '<div class="vbar"><div class="vfill"></div><div class="vcap"></div></div>').join("");
}
function vuTick() {   // 8fps, 设备 _updateVu 同款
  const bars = $("vu20").children;
  for (let i = 0; i < VU_N; i++) {
    const w = i / (VU_N - 1);
    const wb = w < 0.5 ? 1 - 2 * w : 0;          // 左半→bass
    const wt = w > 0.5 ? 2 * w - 1 : 0;          // 右半→treble
    const wm = 1 - wb - wt;                      // 中间→mid
    let target = 4 + (lvl.b * wb + lvl.m * wm + lvl.t * wt) * 1.48;
    if (target > 152) target = 152;
    const k = target > vuCur[i] ? 0.45 : 0.10 + (i % 5) * 0.04;
    vuCur[i] += (target - vuCur[i]) * k;
    let h = Math.round(vuCur[i]);
    if (h < 3) h = 3;
    if (h > 152) h = 152;
    let pk = vuPk[i];
    pk = (h >= pk) ? h : pk * 0.92;
    if (pk < h) pk = h;
    let ph = Math.round(pk);
    if (ph > 152) ph = 152;
    if (ph < h) ph = h;
    vuPk[i] = pk;
    const fill = bars[i].children[0], cap = bars[i].children[1];
    const hp = (h / 152 * 100).toFixed(1) + "%", pp = (ph / 152 * 100).toFixed(1) + "%";
    if (fill.style.height !== hp) fill.style.height = hp;
    if (cap.style.bottom !== pp) cap.style.bottom = pp;
  }
}
function stopLevelStream() {
  if (lvAbort) { lvAbort.abort(); lvAbort = null; }
  clearTimeout(lvFallbackTimer);
  $("vu20").classList.remove("fake");
  clearInterval(vuTimer); vuTimer = 0;
  lvl = { b: 0, m: 0, t: 0 };
  vuCur.fill(4); vuPk.fill(4);
  if ($("vu20").children.length)
    Array.of(...$("vu20").children).forEach(bar => {
      bar.children[0].style.height = "2%";
      bar.children[1].style.bottom = "0%";
    });
}
function startLevelStream(key) {
  stopLevelStream();
  if (!key) return;
  lvGotData = false;
  lvFallbackTimer = setTimeout(() => {
    if (!lvGotData && wantPlay) $("vu20").classList.add("fake");
  }, 7000);
  lvAbort = new AbortController();
  const sig = lvAbort.signal;
  (async () => {
    const r = await fetch("/gw/8769/radio/" + key + "/lv",
                          { signal: sig, credentials: "same-origin" });
    if (!r.ok || !r.body) throw new Error("lv " + r.status);
    const rd = r.body.getReader(), dec = new TextDecoder();
    let buf = "";
    for (;;) {
      const { done, value } = await rd.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      let nl;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const p = buf.slice(0, nl).trim().split(" ");
        buf = buf.slice(nl + 1);
        if (p.length < 3) continue;
        lvGotData = true;
        $("vu20").classList.remove("fake");
        lvl = { b: parseInt(p[0], 10) || 0, m: parseInt(p[1], 10) || 0, t: parseInt(p[2], 10) || 0 };
        if (!vuTimer) vuTimer = setInterval(vuTick, 120);   // 8fps 与设备一致
      }
    }
  })().catch(err => {
    if (sig.aborted) return;
    if (!lvGotData && wantPlay) $("vu20").classList.add("fake");
  });
}
$("r-play").addEventListener("click", () => {
  if (!stations.length) return;
  if (!audio.paused) {          // 暂停 (流停, 与设备"彻底停止=页内暂停"一致)
    audio.pause();
  } else {
    playStation(curKey || stations[0].key);
    return;
  }
});
audio.addEventListener("playing", () => {
  setText("r-stat", "播放中");
  const st = stations.find(s => s.key === curKey);
  setText("r-cur", st ? st.name : "");
  $("r-play").textContent = "⏸";
});
audio.addEventListener("pause", () => {
  setText("r-stat", "已暂停");
  $("r-play").textContent = "⏵";
  stopLevelStream();
});
audio.addEventListener("error", () => {
  stopLevelStream();
  if (!wantPlay) return;
  wantPlay = false;
  setText("r-stat", "连接失败");
});
$("r-vol").addEventListener("input", e => {
  vol = parseInt(e.target.value, 10);
  audio.volume = vol / 100;
  try { localStorage.setItem("car_vol", String(vol)); } catch (err) {}
});
/* 注意: PANEL_OPENERS.radio 的注册在文件后部 V2 通用段之后 (const TDZ) */

/* ════ 底部按钮 (v2: 面板 + 滚动) ════ */
$("nav").addEventListener("click", e => {
  const b = e.target.closest("button");
  if (b && b.dataset.panel) openPanel(b.dataset.panel);
});
$("btn-theme").addEventListener("click", () => {
  applyTheme(themeIdx + 1);
  toast("主题: " + THEMES[themeIdx].name);
});

/* ════ V2 通用: 面板框架 ════ */
function esc(s) {
  return String(s).replace(/[&<>"']/g, c =>
    ({ "&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;", "'":"&#39;" }[c]));
}
function note401(status) {
  if (status === 401 && !_warned401) {
    _warned401 = true;
    toast("登录态已过期, 请刷新页面重新登录", 5000);
  }
}
async function postJSON(url, body, timeoutMs) {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), timeoutMs || 10000);
  try {
    const r = await fetch(url, { method:"POST", signal:ctl.signal, credentials:"same-origin",
      headers: { "Content-Type":"application/json", "X-Tab5-Web":"1" },   // CSRF: 网关要求非 GET 带此头
      body: JSON.stringify(body) });
    if (!r.ok) { note401(r.status); return null; }
    try { return await r.json(); } catch (e) { return { ok:true }; }
  } catch (e) { return null; }
  finally { clearTimeout(t); }
}
function cssv(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}
function fmtTime(d) {
  const p = v => String(v).padStart(2, "0");
  return p(d.getHours()) + ":" + p(d.getMinutes());
}

const PANEL_OPENERS = {};   // name → {open, close}
let panelNow = null;
function openPanel(name) {
  if (panelNow === name) return;
  closePanel();
  panelNow = name;
  const p = $("p-" + name);
  if (!p) { panelNow = null; return; }
  p.classList.add("open");
  document.body.style.overflow = "hidden";
  if (PANEL_OPENERS[name] && PANEL_OPENERS[name].open) PANEL_OPENERS[name].open();
}
function closePanel() {
  if (!panelNow) return;
  const m = PANEL_OPENERS[panelNow];
  if (m && m.close) m.close();
  const p = $("p-" + panelNow);
  if (p) p.classList.remove("open");
  document.body.style.overflow = "";
  panelNow = null;
  hideViewer();
}
document.querySelectorAll('.panel .p-close[data-close]').forEach(b =>
  b.addEventListener("click", closePanel));
document.addEventListener("keydown", e => {
  if (e.key !== "Escape") return;
  if ($("viewer").classList.contains("open")) { hideViewer(); return; }
  closePanel();
});

/* ════ V2 家居: HA 设备控制 (网关注入 token, POST 需 X-Tab5-Web) ════ */
const HA_DOMAINS = ["light", "switch", "fan", "cover", "climate"];
const HA_SEC_NAMES = { light:"灯光", switch:"开关/插座", fan:"风扇", cover:"窗帘", climate:"空调温控" };
let haTimer = 0, haEntities = null;
function haName(e) { return (e.attributes && e.attributes.friendly_name) || e.entity_id; }
async function haFetch() {
  const j = await getJSON("/gw/8123/api/states", 12000);
  if (!Array.isArray(j)) return false;
  haEntities = j.filter(e => {
    const d = e.entity_id.split(".")[0];
    return HA_DOMAINS.includes(d) && !(e.attributes && e.attributes.hidden);
  }).slice(0, 80);
  return true;
}
function haStateTxt(e) {
  const s = e.state;
  if (s === "on") return "开"; if (s === "off") return "关";
  if (s === "unavailable" || s === "unknown") return "不可用";
  return s;
}
function haCardHTML(e) {
  const dom = e.entity_id.split(".")[0], on = e.state === "on",
        bad = e.state === "unavailable" || e.state === "unknown";
  const name = '<div class="fnm"><span class="fn">' + esc(haName(e)) + "</span>";
  if (bad)
    return '<div class="fcard hacard bad" data-eid="' + esc(e.entity_id) + '">' + name +
           '<span class="fc">不可用</span></div></div>';
  if (dom === "cover") {
    const st = e.state === "open" ? "开" : e.state === "closed" ? "关" : esc(e.state);
    return '<div class="fcard hacard" data-eid="' + esc(e.entity_id) + '">' + name +
           '<span class="fc">' + st + "</span></div>" +
           '<div class="ha-acts">' +
           '<button class="ha-btn" data-svc="open_cover">开</button>' +
           '<button class="ha-btn" data-svc="close_cover">关</button>' +
           '<button class="ha-btn" data-svc="stop_cover">停</button></div></div>';
  }
  if (dom === "climate") {
    const at = e.attributes || {};
    return '<div class="fcard hacard" data-eid="' + esc(e.entity_id) + '">' + name +
           '<span class="fc">' + esc(haStateTxt(e)) + "</span></div>" +
           '<div class="ha-temp-big">' + Math.round(at.temperature || 0) + "℃</div>" +
           (at.current_temperature != null
             ? '<div class="ha-cur">室温 ' + Math.round(at.current_temperature) + "℃</div>" : "") +
           '<div class="ha-acts">' +
           '<button class="ha-btn narrow" data-act="t-">−</button>' +
           '<button class="ha-btn narrow" data-act="t+">＋</button>' +
           '<button class="ha-btn' + (on ? " on" : "") + '" data-svc="' + (on ? "turn_off" : "turn_on") + '">' +
           (on ? "开" : "关") + "</button></div></div>";
  }
  return '<div class="fcard hacard" data-eid="' + esc(e.entity_id) + '">' + name + "</div>" +
         '<div class="ha-state' + (on ? " on" : "") + '">' + esc(haStateTxt(e)) + "</div>" +
         '<div class="ha-acts"><button class="ha-btn' + (on ? " on" : "") + '" data-svc="' +
         (on ? "turn_off" : "turn_on") + '">' + (on ? "开" : "关") + "</button></div></div>";
}
function renderHa() {
  if (!haEntities) return;
  const body = $("ha-body");
  if (!haEntities.length) {
    body.innerHTML = '<div class="dim ph">' +
                     "HA 里没有可控制设备 (light/switch/fan/cover/climate)</div>";
    setText("ha-sub", "0 台设备");
    return;
  }
  let html = "";
  for (const dom of HA_DOMAINS) {
    const list = haEntities.filter(e => e.entity_id.split(".")[0] === dom)
                           .sort((a, b) => haName(a).localeCompare(haName(b), "zh"));
    if (!list.length) continue;
    html += '<div class="sec">' + HA_SEC_NAMES[dom] + " · " + list.length + "</div>" +
            '<div class="fgrid">' + list.map(haCardHTML).join("") + "</div>";
  }
  body.innerHTML = html;
  setText("ha-sub", haEntities.length + " 台设备 · " + fmtTime(new Date()));
}
async function haCall(svcPath, body) {   // svcPath 形如 light/turn_on
  const r = await postJSON("/gw/8123/api/services/" + svcPath, body, 8000);
  setTimeout(() => { if (panelNow === "ha") haFetch().then(ok => { if (ok) renderHa(); }); }, 1200);
  return r;
}
$("ha-body").addEventListener("click", e => {
  const btn = e.target.closest("button");
  if (!btn) return;
  const row = btn.closest("[data-eid]");
  if (!row) return;
  const eid = row.dataset.eid, dom = eid.split(".")[0];
  if (btn.dataset.svc) haCall(dom + "/" + btn.dataset.svc, { entity_id: eid });
  else if (btn.dataset.act === "t-" || btn.dataset.act === "t+") {
    const ent = haEntities.find(x => x.entity_id === eid);
    if (!ent) return;
    const at = ent.attributes || {};
    const lo = at.min_temp != null ? at.min_temp : 16,
          hi = at.max_temp != null ? at.max_temp : 30;
    let t = (at.temperature || 24) + (btn.dataset.act === "t+" ? 0.5 : -0.5);
    t = Math.max(lo, Math.min(hi, Math.round(t * 2) / 2));
    haCall("climate/set_temperature", { entity_id: eid, temperature: t });
  }
});
PANEL_OPENERS.ha = {
  open() {
    setText("ha-sub", "加载中…");
    $("ha-body").innerHTML = '<div class="dim ph">加载中…</div>';
    haFetch().then(ok => { ok ? renderHa() :
      ($("ha-body").innerHTML = '<div class="dim ph">HA 不可达</div>',
       setText("ha-sub", "连接失败")); });
    haTimer = setInterval(() => { if (panelNow === "ha") haFetch().then(ok => ok && renderHa()); }, 60000);
  },
  close() { clearInterval(haTimer); haTimer = 0; }
};

/* ════ V2 金融: 自选股 + 指数 + 分时/日K (数据链路与 app_stocks 一致) ════ */
const IDX_LIST = [
  ["上证指数", "sh000001"], ["深证成指", "sz399001"], ["创业板指", "sz399006"],
  ["科创综指", "sh000688"], ["沪深300",  "sh000300"], ["恒生指数", "r_hkHSI"],
  ["道琼斯",   "usDJI"],    ["纳斯达克", "usIXIC"],   ["标普500",  "usINX"],
];
const qtCode = code => (String(code)[0] === "6" ? "sh" : "sz") + code;
const chgCls = v => v > 0.005 ? "up" : (v < -0.005 ? "down" : "flat");
let finStocks = [], finTimer = 0, idxTimer = 0;
const chart = { code:null, name:"", view:"min", minute:null, day:null };

function parseQtText(txt) {   // → { code: [fields...] }  (r_hkHSI 等带下划线)
  const out = {};
  const re = /v_([a-zA-Z0-9_]+)="([^"]*)"/g;
  let m;
  while ((m = re.exec(txt)) !== null) out[m[1]] = m[2].split("~");
  return out;
}
function qtFieldsOk(f) {
  return f && (parseFloat(f[3]) > 0) && (parseFloat(f[4]) > 0);
}
async function finFetchStocks() {
  const j = await getJSON("/gw/8766/api/stocks/portfolio");
  if (!j || !Array.isArray(j.items)) return;   // 该端点无 ok 字段 (设备端同款)
  finStocks = j.items.filter(s => s.code).slice(0, 24);
  renderFinStocks();
  finFetchQuotes();
}
function renderFinStocks() {
  const g = $("fin-grid");
  if (!finStocks.length) {
    g.innerHTML = '<div class="dim fin-note">自选股列表为空 (hermes portfolio)</div>';
    return;
  }
  g.innerHTML = finStocks.map((s, i) => {
    const cls = chgCls(s.pchg);
    return '<div class="fcard" data-i="' + i + '" data-code="' + esc(qtCode(s.code)) + '">' +
      '<div class="fnm"><span class="fn">' + esc(s.name || s.code) + '</span><span class="fc">' +
      esc(s.code) + "</span></div>" +
      '<div class="fpx ' + cls + '">' + (s.price > 0 ? s.price.toFixed(2) : "--") + "</div>" +
      '<div class="fchg ' + cls + '">' + (s.chg ? (s.chg > 0 ? "+" : "") + s.chg.toFixed(2) : "--") +
      "  " + (s.pchg ? (s.pchg > 0 ? "+" : "") + s.pchg.toFixed(2) + "%" : "--") + "</div>" +
      (s.one_sentence ? '<div class="fsen" title="' + esc(s.one_sentence) + '">' + esc(s.one_sentence) + "</div>" : "") +
      "</div>";
  }).join("");
  setText("fin-sub", finStocks.length + " 只自选 · " + fmtTime(new Date()));
}
async function finFetchQuotes() {
  if (!finStocks.length) return;
  const codes = finStocks.map(s => qtCode(s.code)).join(",");
  // qt 返回非 JSON 文本, 走裸 fetch:
  try {
    const r = await fetch("/gw/ext/qt/q=" + codes, { credentials: "same-origin" });
    if (!r.ok) { note401(r.status); return; }
    const qt = parseQtText(await r.text());
    finStocks.forEach((s, i) => {
      const f = qt[qtCode(s.code)];
      if (qtFieldsOk(f)) {
        s.price = parseFloat(f[3]); s.chg = parseFloat(f[31]); s.pchg = parseFloat(f[32]);
      }
    });
    renderFinStocks();
  } catch (e) {}
}
function renderFinIdx(qt) {
  const g = $("idx-grid");
  g.innerHTML = IDX_LIST.map(([name, code]) => {
    const f = qt[code];
    if (!qtFieldsOk(f)) return '<div class="fcard"><div class="fnm"><span class="fn">' + name +
      '</span></div><div class="fpx flat">--</div></div>';
    const px = parseFloat(f[3]), amt = parseFloat(f[31]), pct = parseFloat(f[32]), cls = chgCls(pct);
    return '<div class="fcard" data-code="' + esc(code) + '" data-name="' + esc(name) + '">' +
      '<div class="fnm"><span class="fn">' + name + '</span><span class="fc">' + esc(code) + "</span></div>" +
      '<div class="fpx ' + cls + '">' + px.toFixed(2) + "</div>" +
      '<div class="fchg ' + cls + '">' + (amt > 0 ? "+" : "") + amt.toFixed(2) + "  " +
      (pct > 0 ? "+" : "") + pct.toFixed(2) + "%</div></div>";
  }).join("");
}
async function finFetchIdx() {
  try {
    const r = await fetch("/gw/ext/qt/q=" + IDX_LIST.map(x => x[1]).join(","), { credentials:"same-origin" });
    if (!r.ok) { note401(r.status); return; }
    renderFinIdx(parseQtText(await r.text()));
  } catch (e) {}
}
$("fin-grid").addEventListener("click", e => {
  const c = e.target.closest(".fcard");
  if (c && c.dataset.code && finStocks[+c.dataset.i])
    openChart(c.dataset.code, finStocks[+c.dataset.i].name || c.dataset.code);
});
$("idx-grid").addEventListener("click", e => {
  const c = e.target.closest(".fcard");
  if (c && c.dataset.code) openChart(c.dataset.code.replace(/^r_/, ""), c.dataset.name || c.dataset.code);
});
document.querySelectorAll("#p-fin .ptab").forEach(b => b.addEventListener("click", () => {
  chart.view = b.dataset.cv;
  document.querySelectorAll("#p-fin .ptab").forEach(x => x.classList.toggle("on", x === b));
  drawChart();
}));
async function fetchText(url, ms) {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), ms || 15000);
  try {
    const r = await fetch(url, { signal: ctl.signal, credentials: "same-origin" });
    if (!r.ok) { note401(r.status); return null; }
    return await r.text();
  } catch (e) { return null; }
  finally { clearTimeout(t); }
}
async function openChart(code, name) {
  chart.code = code; chart.name = name || code;
  chart.minute = null; chart.day = null;
  $("chart-wrap").hidden = false;
  setText("chart-title", name + " · " + code);
  $("chart-metrics").textContent = "加载中…";
  $("chart-wrap").scrollIntoView({ behavior:"smooth", block:"nearest" });
  const [minTxt, dayTxt, qtTxt] = await Promise.all([
    fetchText("/gw/ext/ifzq/appstock/app/minute/query?code=" + code),
    fetchText("/gw/ext/ifzq/appstock/app/fqkline/get?param=" + code + ",day,,,250,qfq"),
    fetchText("/gw/ext/qt/q=" + code),
  ]);
  if (chart.code !== code) return;   // 期间换了股票
  try {
    const j = JSON.parse(minTxt);
    if (j && j.code === 0 && j.data && j.data[code]) {
      const rows = (j.data[code].data && j.data[code].data.data) || [];
      chart.minute = {
        prices: rows.map(s => parseFloat(String(s).split(" ")[1])).filter(v => !isNaN(v)),
        qt: (j.data[code].qt && j.data[code].qt[code]) || null,
      };
    }
  } catch (e) {}
  try {
    const j = JSON.parse(dayTxt);
    if (j && j.code === 0 && j.data && j.data[code]) {
      const node = j.data[code], arr = node.qfqday || node.day || [];
      chart.day = arr.filter(r => Array.isArray(r) && r.length >= 5).map(r => ({
        d: r[0], o: parseFloat(r[1]), c: parseFloat(r[2]),
        h: parseFloat(r[3]), l: parseFloat(r[4]),
        v: r.length > 5 ? parseFloat(r[5]) || 0 : 0,
      }));
    }
  } catch (e) {}
  // 指标跑马灯: 优先 minute 内嵌 qt 节点, 兜底 /gw/ext/qt
  let qf = chart.minute && chart.minute.qt;
  if (!qf && qtTxt) qf = parseQtText(qtTxt)[code];
  if (qf && qf.length > 40) {
    const num = s => { const v = parseFloat(s); return isNaN(v) ? 0 : v; };
    let m = "";
    if (qf[3]) m += num(qf[3]).toFixed(2);
    if (qf[31]) m += "  " + (num(qf[31]) >= 0 ? "+" : "") + num(qf[31]).toFixed(2);
    if (qf[32]) m += "  " + (num(qf[32]) >= 0 ? "+" : "") + num(qf[32]).toFixed(2) + "%";
    if (qf[37]) { const v = num(qf[37]);
      m += "   成交额 " + (v >= 10000 ? (v / 10000).toFixed(2) + "亿" : v.toFixed(0) + "万"); }
    if (qf[38]) m += "   换手 " + num(qf[38]).toFixed(2) + "%";
    if (qf[49]) m += "   量比 " + num(qf[49]).toFixed(2);
    if (qf[39]) m += "   PE " + num(qf[39]).toFixed(2);
    $("chart-metrics").textContent = m;
  } else $("chart-metrics").textContent = "";
  if (!chart.minute || !chart.minute.prices.length) {   // 无分时 (港美指) 自动切日K
    chart.view = "day";
    document.querySelectorAll("#p-fin .ptab").forEach(x => x.classList.toggle("on", x.dataset.cv === "day"));
  }
  drawChart();
}
function chartCtx() {
  const c = $("chart"), dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth || 800, h = 420;
  c.width = Math.round(w * dpr); c.height = Math.round(h * dpr);
  const ctx = c.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w, h };
}
function drawChart() {
  if (!chart.code) return;
  if (chart.view === "min") {
    if (chart.minute && chart.minute.prices.length) drawMinute();
    else if (chart.day && chart.day.length) drawDayK();
    else chartEmpty("暂无数据");
  } else {
    if (chart.day && chart.day.length) drawDayK();
    else chartEmpty("暂无日K数据");
  }
}
function chartEmpty(msg) {
  const { ctx, w, h } = chartCtx();
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = cssv("--dim"); ctx.font = "14px sans-serif";
  ctx.textAlign = "center"; ctx.fillText(msg, w / 2, h / 2);
}
function drawMinute() {
  const { ctx, w, h } = chartCtx();
  const prices = chart.minute.prices, prev = chart.minute.qt ? parseFloat(chart.minute.qt[4]) : NaN;
  const padL = 52, padR = 14, padT = 14, padB = 22;
  const pw = w - padL - padR, ph = h - padT - padB, n = prices.length;
  let lo = Math.min(...prices), hi = Math.max(...prices);
  if (!isNaN(prev)) { lo = Math.min(lo, prev); hi = Math.max(hi, prev); }
  if (!isFinite(lo) || hi <= 0) return;
  if (hi - lo < hi * 0.001) { hi += hi * 0.001; lo -= hi * 0.001; }
  const m = (hi - lo) * 0.08; hi += m; lo -= m;
  const X = i => padL + (n <= 1 ? pw / 2 : i / (n - 1) * pw);
  const Y = p => padT + (hi - p) / (hi - lo) * ph;
  ctx.clearRect(0, 0, w, h);
  ctx.font = "11px sans-serif"; ctx.textBaseline = "middle";
  // 网格 + 左轴价签
  ctx.strokeStyle = cssv("--border"); ctx.fillStyle = cssv("--dim");
  for (let k = 0; k <= 4; k++) {
    const y = padT + ph * k / 4;
    ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(w - padR, y); ctx.stroke();
    ctx.textAlign = "right";
    ctx.fillText((hi - (hi - lo) * k / 4).toFixed(2), padL - 6, y);
  }
  // 昨收虚线
  if (!isNaN(prev)) {
    ctx.setLineDash([5, 4]); ctx.strokeStyle = cssv("--dim");
    ctx.beginPath(); ctx.moveTo(padL, Y(prev)); ctx.lineTo(w - padR, Y(prev)); ctx.stroke();
    ctx.setLineDash([]);
  }
  const col = !isNaN(prev) && prices[n - 1] < prev ? cssv("--ok") : cssv("--danger");
  // 面积
  ctx.beginPath(); ctx.moveTo(X(0), Y(prices[0]));
  for (let i = 1; i < n; i++) ctx.lineTo(X(i), Y(prices[i]));
  ctx.lineTo(X(n - 1), padT + ph); ctx.lineTo(X(0), padT + ph); ctx.closePath();
  ctx.globalAlpha = 0.13; ctx.fillStyle = col; ctx.fill(); ctx.globalAlpha = 1;
  // 折线
  ctx.beginPath(); ctx.moveTo(X(0), Y(prices[0]));
  for (let i = 1; i < n; i++) ctx.lineTo(X(i), Y(prices[i]));
  ctx.strokeStyle = col; ctx.lineWidth = 1.6; ctx.stroke(); ctx.lineWidth = 1;
  // 时间轴
  ctx.fillStyle = cssv("--dim"); ctx.textAlign = "center";
  ctx.fillText("09:30", padL, h - 9);
  ctx.fillText("11:30/13:00", padL + pw / 2, h - 9);
  ctx.fillText("15:00", w - padR, h - 9);
}
function drawDayK() {
  const { ctx, w, h } = chartCtx();
  const bars = chart.day, n = bars.length;
  const padL = 52, padR = 14, padT = 26, padB = 22;
  const pw = w - padL - padR;
  const volH = Math.min(60, (h - padT - padB) * 0.18);
  const priceH = h - padT - padB - volH - 10;
  const closes = bars.map(b => b.c);
  const ma = (k) => closes.map((_, i) =>
    i < k - 1 ? NaN : closes.slice(i - k + 1, i + 1).reduce((a, b) => a + b, 0) / k);
  const ma5 = ma(5), ma10 = ma(10), ma20 = ma(20);
  let lo = Infinity, hi = -Infinity;
  for (const b of bars) { lo = Math.min(lo, b.l); hi = Math.max(hi, b.h); }
  for (const arr of [ma5, ma10, ma20])
    for (const v of arr) if (!isNaN(v)) { lo = Math.min(lo, v); hi = Math.max(hi, v); }
  const mg = (hi - lo) * 0.05; hi += mg; lo -= mg;
  const maxV = Math.max(...bars.map(b => b.v)) || 1;
  const X = i => padL + (i + 0.5) / n * pw;
  const Y = p => padT + (hi - p) / (hi - lo) * priceH;
  const bw = Math.max(1.2, pw / n * 0.62);
  ctx.clearRect(0, 0, w, h);
  ctx.font = "11px sans-serif"; ctx.textBaseline = "middle";
  ctx.strokeStyle = cssv("--border"); ctx.fillStyle = cssv("--dim");
  for (let k = 0; k <= 4; k++) {
    const y = padT + priceH * k / 4;
    ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(w - padR, y); ctx.stroke();
    ctx.textAlign = "right";
    ctx.fillText((hi - (hi - lo) * k / 4).toFixed(2), padL - 6, y);
  }
  for (let i = 0; i < n; i++) {
    const b = bars[i], up = b.c >= b.o, col = up ? cssv("--danger") : cssv("--ok");
    ctx.strokeStyle = col; ctx.fillStyle = col;
    ctx.beginPath(); ctx.moveTo(X(i), Y(b.h)); ctx.lineTo(X(i), Y(b.l)); ctx.stroke();
    const y1 = Y(Math.max(b.o, b.c)), y2 = Y(Math.min(b.o, b.c));
    ctx.fillRect(X(i) - bw / 2, y1, bw, Math.max(1, y2 - y1));
    ctx.globalAlpha = 0.55;
    const vh = b.v / maxV * volH;
    ctx.fillRect(X(i) - bw / 2, padT + priceH + 10 + (volH - vh), bw, vh);
    ctx.globalAlpha = 1;
  }
  // MA 线 + 图例 (左上角, 设备同款)
  const mas = [[ma5, cssv("--accent"), "MA5"], [ma10, cssv("--warn"), "MA10"], [ma20, cssv("--soft"), "MA20"]];
  ctx.textAlign = "left";
  mas.forEach(([arr, col, label], k) => {
    ctx.strokeStyle = col; ctx.beginPath();
    let started = false;
    for (let i = 0; i < n; i++) {
      if (isNaN(arr[i])) continue;
      if (!started) { ctx.moveTo(X(i), Y(arr[i])); started = true; }
      else ctx.lineTo(X(i), Y(arr[i]));
    }
    ctx.stroke();
    ctx.fillStyle = col;
    ctx.fillText(label, padL + 4 + k * 52, padT - 10);
  });
  // 日期轴 4 刻度
  ctx.fillStyle = cssv("--dim"); ctx.textAlign = "center";
  for (let k = 0; k < 4; k++) {
    const i = Math.round((n - 1) * k / 3);
    ctx.fillText(bars[i].d.slice(5), Math.min(w - 24, Math.max(padL + 18, X(i))), h - 9);
  }
}
PANEL_OPENERS.fin = {
  open() {
    finFetchStocks(); finFetchIdx();
    finTimer = setInterval(finFetchStocks, 30000);
    idxTimer = setInterval(finFetchIdx, 60000);
  },
  close() { clearInterval(finTimer); clearInterval(idxTimer);
            finTimer = idxTimer = 0; $("chart-wrap").hidden = true; chart.code = null; }
};

/* ════ V2 计时器 (纯前端; 关面板继续跑, 结束 toast+蜂鸣) ════ */
const TM_C = 2 * Math.PI * 126;
let tmDur = 300, tmLeft = 300, tmRun = false, tmTimer = 0, tmEndAt = 0, tmCtxAudio = null, tmDone = false;
const TM_PRESETS = [1, 3, 5, 10, 30, 60];
function tmFmt(s) {
  s = Math.max(0, Math.ceil(s));
  const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), sec = s % 60;
  const p = v => String(v).padStart(2, "0");
  return h > 0 ? h + ":" + p(m) + ":" + p(sec) : p(m) + ":" + p(sec);
}
function tmRender() {
  setText("tm-display", tmFmt(tmLeft));
  const frac = tmDur > 0 ? tmLeft / tmDur : 0;
  const prog = $("tm-prog");
  prog.style.strokeDasharray = TM_C;
  prog.style.strokeDashoffset = TM_C * (1 - frac);
  prog.style.stroke = tmRun ? cssv("--accent") : cssv("--dim");
  $("tm-track").style.stroke = cssv("--card1");
  if (tmDone) setText("tm-state", "计时结束!");
  else if (tmRun) setText("tm-state", "运行中 · " + fmtTime(new Date(tmEndAt)) + " 结束");
  else setText("tm-state", tmLeft < tmDur ? "已暂停" : "点预设设定时长");
  document.querySelectorAll("#tm-chips .tm-chip").forEach(c =>
    c.classList.toggle("on", !tmRun && !tmDone && +c.dataset.min * 60 === tmDur));
  $("tm-start").textContent = tmRun ? "暂停" : (tmLeft > 0 && tmLeft < tmDur ? "继续" : "开始");
}
function tmBeep() {
  try {
    tmCtxAudio = tmCtxAudio || new (window.AudioContext || window.webkitAudioContext)();
    for (let k = 0; k < 3; k++) {
      const o = tmCtxAudio.createOscillator(), g = tmCtxAudio.createGain();
      o.frequency.value = 880; o.connect(g); g.connect(tmCtxAudio.destination);
      const t0 = tmCtxAudio.currentTime + k * 0.45;
      g.gain.setValueAtTime(0.001, t0);
      g.gain.exponentialRampToValueAtTime(0.3, t0 + 0.02);
      g.gain.exponentialRampToValueAtTime(0.001, t0 + 0.35);
      o.start(t0); o.stop(t0 + 0.4);
    }
  } catch (e) {}
}
function tmTick() {
  tmLeft = Math.max(0, (tmEndAt - Date.now()) / 1000);
  if (tmLeft <= 0) {
    tmRun = false; tmDone = true; clearInterval(tmTimer); tmTimer = 0;
    tmBeep(); toast("⏰ 计时结束");
  }
  tmRender();
}
$("tm-chips").innerHTML = TM_PRESETS.map(m =>
  '<button class="tm-chip" data-min="' + m + '">' + m + " 分钟</button>").join("");
$("tm-chips").addEventListener("click", e => {
  const c = e.target.closest(".tm-chip");
  if (!c || tmRun) return;
  tmDur = tmLeft = +c.dataset.min * 60; tmDone = false;
  clearInterval(tmTimer); tmTimer = 0; tmRender();
});
$("tm-start").addEventListener("click", () => {
  if (tmRun) {   // 暂停
    tmRun = false; clearInterval(tmTimer); tmTimer = 0;
  } else {
    if (tmLeft <= 0) tmLeft = tmDur;
    tmDone = false; tmRun = true;
    tmEndAt = Date.now() + tmLeft * 1000;
    try { tmCtxAudio = tmCtxAudio || new (window.AudioContext || window.webkitAudioContext)(); } catch (e) {}
    tmTimer = setInterval(tmTick, 250);
  }
  tmRender();
});
$("tm-reset").addEventListener("click", () => {
  tmRun = false; tmDone = false; clearInterval(tmTimer); tmTimer = 0;
  tmLeft = tmDur; tmRender();
});
PANEL_OPENERS.timer = { open() { tmRender(); }, close() {} };
PANEL_OPENERS.radio = {
  open() { if (!stations.length) loadStations(); },
  close() { stopLevelStream(); }   // 音频继续播 (与设备后台播放一致), 只停电平流
};

/* ════ V2 工具抽屉: 图库 / 万年历 / 换算 / 双色球 ════ */
const TOOL_INIT = {};
let toolCurrent = null;
function toolHome() {
  toolCurrent = null;
  $("tool-menu").hidden = false; $("tool-view").hidden = true;
  $("tool-back").hidden = true;
  setText("tool-title", "工具"); setText("tool-sub", "");
}
function showTool(name) {
  toolCurrent = name;
  $("tool-menu").hidden = true; $("tool-view").hidden = false;
  $("tool-back").hidden = false;
  TOOL_INIT[name]();
}
$("tool-back").addEventListener("click", toolHome);
document.querySelectorAll(".tool-card").forEach(c =>
  c.addEventListener("click", () => showTool(c.dataset.tool)));

/* ── 图库: hermes jpg 端点直出, <img> 零解码 ── */
let galItems = [], galIdx = -1;
TOOL_INIT.gallery = function () {
  setText("tool-title", "图库"); setText("tool-sub", "加载中…");
  $("tool-view").innerHTML = '<div class="ggrid" id="ggrid"></div>' +
    '<div class="dim" id="gal-msg"></div>';
  getJSON("/gw/8766/gallery/list").then(j => {
    if (!j || !j.ok || !Array.isArray(j.items) || !j.items.length) {
      setText("tool-sub", "加载失败");
      const m = $("gal-msg"); if (m) m.textContent = "图库不可达或没有照片";
      return;
    }
    galItems = j.items.slice(0, 96);
    setText("tool-sub", galItems.length + " 张照片");
    const g = $("ggrid");
    g.innerHTML = galItems.map((it, i) =>
      '<img loading="lazy" data-i="' + i + '" alt="" ' +
      'src="/gw/8766/gallery/thumb.jpg?id=' + encodeURIComponent(it.id) + '&w=300&h=200">').join("");
    g.addEventListener("click", e => {
      const img = e.target.closest("img[data-i]");
      if (img) showViewer(+img.dataset.i);
    });
  });
};
function showViewer(i) {
  if (!galItems.length) return;
  galIdx = (i + galItems.length) % galItems.length;
  $("viewer-img").src = "/gw/8766/gallery/photo.jpg?id=" + encodeURIComponent(galItems[galIdx].id);
  setText("v-n", (galIdx + 1) + " / " + galItems.length);
  $("viewer").classList.add("open");
}
function hideViewer() {
  $("viewer").classList.remove("open");
  $("viewer-img").removeAttribute("src");   // 置空字符串会触发对页面 URL 的请求
}
$("v-prev").addEventListener("click", () => showViewer(galIdx - 1));
$("v-next").addEventListener("click", () => showViewer(galIdx + 1));
$("v-close").addEventListener("click", hideViewer);
$("viewer-img").addEventListener("click", hideViewer);

/* ── 万年历: 复用已移植农历表 + 生肖 (与 app_calendar 同约定) ── */
const LM_FULL = ["正月","二月","三月","四月","五月","六月","七月","八月","九月","十月","冬月","腊月"];
const ZODIAC = ["鼠","牛","虎","兔","龙","蛇","马","羊","猴","鸡","狗","猪"];
let calY = 0, calM = 0;
function zodiacOf(lunarYear) { return ZODIAC[((lunarYear - 4) % 12 + 12) % 12]; }
function renderCal() {
  const first = new Date(calY, calM - 1, 1).getDay();
  const days = new Date(calY, calM, 0).getDate();
  const today = new Date();
  let html = '<div class="cal-head"><span class="cal-title">' + calY + " 年 " + calM + " 月</span>" +
    '<button class="cal-btn" data-nav="-1">‹ 上月</button>' +
    '<button class="cal-btn" data-nav="1">下月 ›</button>' +
    '<button class="cal-btn" data-nav="0">今天</button></div><div class="cal-grid">';
  for (let k = 0; k < 7; k++)
    html += '<div class="cal-wd' + (k === 0 || k === 6 ? " we" : "") + '">' + "日一二三四五六"[k] + "</div>";
  for (let slot = 0; slot < 42; slot++) {
    const d = slot - first + 1;
    if (d < 1 || d > days) { html += '<div class="cal-cell pad"></div>'; continue; }
    const we = slot % 7 === 0 || slot % 7 === 6;
    const ld = solarToLunar(calY, calM, d);
    const lunar = ld ? (ld.leap ? "闰" : "") + (ld.d === 1 ? LM_FULL[ld.m - 1] : LD_CHARS[ld.d - 1]) : "";
    const isToday = today.getFullYear() === calY && today.getMonth() + 1 === calM && today.getDate() === d;
    html += '<div class="cal-cell' + (we ? " we" : "") + (isToday ? " today" : "") + '">' +
            '<div class="cd">' + d + '</div><div class="cl">' + lunar + "</div></div>";
  }
  html += "</div>";
  const tld = solarToLunar(today.getFullYear(), today.getMonth() + 1, today.getDate());
  if (tld) {
    const wd = "日一二三四五六"[today.getDay()];
    html += '<div class="cal-detail">' + today.getFullYear() + "年" + (today.getMonth() + 1) + "月" +
      today.getDate() + "日 星期" + wd + "　农历" + (tld.leap ? "闰" : "") + LM_FULL[tld.m - 1] +
      LD_CHARS[tld.d - 1] + " " + zodiacOf(tld.y) + "年</div>";
  }
  $("tool-view").innerHTML = html;
}
$("tool-view").addEventListener("click", e => {
  const b = e.target.closest(".cal-btn");
  if (!b) return;
  const nav = +b.dataset.nav;
  if (nav === 0) { const t = new Date(); calY = t.getFullYear(); calM = t.getMonth() + 1; }
  else { calM += nav; if (calM < 1) { calM = 12; calY--; } if (calM > 12) { calM = 1; calY++; } }
  renderCal();
});
TOOL_INIT.cal = function () {
  setText("tool-title", "万年历"); setText("tool-sub", "");
  const t = new Date();
  calY = t.getFullYear(); calM = t.getMonth() + 1;
  renderCal();
};

/* ── 换算: 汇率 (/gw/8766/rate, 静态表兜底) + 7 类单位 (与 app_settings 同表) ── */
const FX_CODES = ["USD","CNY","EUR","JPY","GBP","HKD","KRW","AUD"];
const FX_NAMES = { USD:"美元", CNY:"人民币", EUR:"欧元", JPY:"日元", GBP:"英镑", HKD:"港元", KRW:"韩元", AUD:"澳元" };
const FX_FALLBACK = { USD:1.0, CNY:7.25, EUR:0.92, JPY:157.0, GBP:0.79, HKD:7.81, KRW:1380.0, AUD:1.51 };
let fxRates = null, fxUpdated = "";
const UCATS = {
  "长度": [["mm",0.001],["cm",0.01],["m",1],["km",1000],["in",0.0254],["ft",0.3048],["yd",0.9144],["mi",1609.344]],
  "重量": [["mg",0.001],["g",1],["kg",1000],["t",1e6],["oz",28.3495],["lb",453.592]],
  "温度": [["°C",0],["°F",1],["K",2]],
  "面积": [["mm²",1e-6],["cm²",1e-4],["m²",1],["km²",1e6],["ha",1e4],["亩",666.6667],["acre",4046.86],["ft²",0.092903]],
  "体积": [["mL",0.001],["L",1],["m³",1000],["gal",3.78541],["cup",0.236588]],
  "速度": [["m/s",1],["km/h",0.277778],["mph",0.44704],["kn",0.514444]],
  "数据": [["B",1],["KB",1024],["MB",1048576],["GB",1073741824],["TB",1099511627776]],
};
let cvMode = "fx", cvCat = "长度";
function cvSel(id, opts, sel) {
  return '<select id="' + id + '">' + opts.map(o =>
    '<option value="' + esc(o) + '"' + (o === sel ? " selected" : "") + ">" + esc(o) + "</option>").join("") + "</select>";
}
function tempConvert(v, from, to) {   // → °C 中转
  const c = from === "°C" ? v : (from === "°F" ? (v - 32) * 5 / 9 : v - 273.15);
  return to === "°C" ? c : (to === "°F" ? c * 9 / 5 + 32 : c + 273.15);
}
function cvConvert() {
  if (cvMode === "fx") {
    const v = parseFloat($("fx-amt").value);
    const from = $("fx-from").value, to = $("fx-to").value;
    if (isNaN(v)) { $("fx-res").textContent = "--"; return; }
    const r = fxRates || FX_FALLBACK;
    $("fx-res").textContent = (v / r[from] * r[to]).toLocaleString("zh-CN",
      { maximumFractionDigits: 4 }) + " " + FX_NAMES[to];
  } else {
    const v = parseFloat($("u-amt").value);
    const from = $("u-from").value, to = $("u-to").value;
    if (isNaN(v)) { $("u-res").textContent = "--"; return; }
    if (cvCat === "温度") {
      $("u-res").textContent = tempConvert(v, from, to)
        .toLocaleString("zh-CN", { maximumFractionDigits: 2 }) + " " + to;
    } else {
      const f = Object.fromEntries(UCATS[cvCat]);
      $("u-res").textContent = (v * f[from] / f[to])
        .toLocaleString("zh-CN", { maximumFractionDigits: 6 }) + " " + to;
    }
  }
}
TOOL_INIT.cv = function () {
  setText("tool-title", "换算"); setText("tool-sub", "");
  renderCv();
  if (cvMode === "fx" && !fxRates) {
    getJSON("/gw/8766/rate").then(j => {
      if (j && j.ok && j.rates) {
        fxRates = j.rates;
        fxUpdated = (j.source === "fallback" ? "离线静态表" :
          "更新于 " + fmtTime(new Date((j.updated || 0) * 1000)));
        renderCv();
      }
    });
  }
};
function renderCv() {
  if (cvMode === "fx") {
    $("tool-view").innerHTML =
      '<div class="p-tabs"><button class="ptab on" data-cvm="fx">汇率</button>' +
      '<button class="ptab" data-cvm="unit">单位</button></div>' +
      '<div class="cv-row"><input id="fx-amt" type="number" value="1" inputmode="decimal">' +
      cvSel("fx-from", FX_CODES, "USD") + '<button class="cv-swap" id="fx-swap">⇄</button>' +
      cvSel("fx-to", FX_CODES, "CNY") + "</div>" +
      '<div class="cv-res" id="fx-res">--</div>' +
      '<div class="cv-upd" id="fx-upd">' + (fxRates ? fxUpdated : "拉取实时汇率…失败自动用静态表") + "</div>";
    $("fx-swap").addEventListener("click", () => {
      const a = $("fx-from").value; $("fx-from").value = $("fx-to").value; $("fx-to").value = a; cvConvert();
    });
  } else {
    const units = UCATS[cvCat].map(u => u[0]);
    $("tool-view").innerHTML =
      '<div class="p-tabs"><button class="ptab" data-cvm="fx">汇率</button>' +
      '<button class="ptab on" data-cvm="unit">单位</button>' +
      cvSel("u-cat", Object.keys(UCATS), cvCat) + "</div>" +
      '<div class="cv-row"><input id="u-amt" type="number" value="1" inputmode="decimal">' +
      cvSel("u-from", units, units[0]) + '<button class="cv-swap" id="u-swap">⇄</button>' +
      cvSel("u-to", units, units[1] || units[0]) + "</div>" +
      '<div class="cv-res" id="u-res">--</div>';
    $("u-cat").addEventListener("change", e => { cvCat = e.target.value; renderCv(); });
    $("u-swap").addEventListener("click", () => {
      const a = $("u-from").value; $("u-from").value = $("u-to").value; $("u-to").value = a; cvConvert();
    });
  }
  $("tool-view").querySelectorAll("[data-cvm]").forEach(b => b.addEventListener("click", () => {
    cvMode = b.dataset.cvm; renderCv();
  }));
}
$("tool-view").addEventListener("input", cvConvert);
$("tool-view").addEventListener("change", e => {
  if (e.target.id === "fx-from" || e.target.id === "fx-to") cvConvert();
  if (e.target.id === "u-from" || e.target.id === "u-to") cvConvert();
});

/* ── 双色球: /gw/8766/api/ssq/history (issues[0]=最新, 与 app_lottery 同源) ── */
let ssqData = null, ssqAt = 0, ssqList = null;
function ballsHTML(reds, blue, small) {
  return reds.map(r => '<span class="ball r' + (small ? "" : "") + '">' + r + "</span>").join("") +
         '<span class="ball b">' + blue + "</span>";
}
function fmtYi(v) { return (v / 1e8).toFixed(2) + " 亿"; }
function fmtPrize(g) {
  if (!g || !g.money) return "浮动";   // 缺失/0 → 浮动 (与设备一致)
  return g.money >= 10000 ? (g.money / 10000).toFixed(2) + " 万" : g.money + " 元";
}
TOOL_INIT.ssq = function () {
  setText("tool-title", "双色球");
  const render = () => {
    const it = ssqData;
    const g1 = it.grades ? it.grades.find(g => g.level === 1) : null;
    const g2 = it.grades ? it.grades.find(g => g.level === 2) : null;
    let html = '<div class="ssq-latest"><div class="ssq-h"><span class="code">第 ' +
      esc(it.code) + " 期</span><span class='dt'>" + esc(it.date) + " 周" + esc(it.week || "") + "</span></div>" +
      ballsHTML(it.reds || [], it.blue) +
      '<div class="ssq-meta">销售额 ' + fmtYi(it.sales || 0) + " · 奖池 " + fmtYi(it.pool || 0) +
      "<br>一等奖 " + ((g1 && g1.num) || 0) + " 注 单注 " + fmtPrize(g1) +
      " · 二等奖 " + ((g2 && g2.num) || 0) + " 注 单注 " + fmtPrize(g2) + "</div></div>" +
      '<div class="sec">近 10 期</div>';
    (ssqList || []).slice(1, 11).forEach(x => {
      html += '<div class="ssq-row"><span class="icode">' + esc(x.code) + " 期 · " +
        esc((x.date || "").slice(5)) + "</span>" + ballsHTML(x.reds || [], x.blue, true) + "</div>";
    });
    $("tool-view").innerHTML = html;
  };
  if (ssqData && Date.now() - ssqAt < 10 * 60000) { render(); return; }
  setText("tool-sub", "加载中…");
  $("tool-view").innerHTML = '<div class="dim ph">加载中…</div>';
  getJSON("/gw/8766/api/ssq/history").then(j => {
    if (!j || !j.ok || !Array.isArray(j.issues) || !j.issues.length) {
      setText("tool-sub", "加载失败");
      $("tool-view").innerHTML = '<div class="dim ph">开奖数据不可达</div>';
      return;
    }
    ssqData = j.issues[0]; ssqList = j.issues; ssqAt = Date.now();
    setText("tool-sub", "共 " + j.count + " 期");
    render();
  });
};

/* 工具面板生命周期: 进出回到菜单 */
PANEL_OPENERS.tools = { open() { toolHome(); }, close() { toolHome(); } };

/* ════ V2.1 想法: 灵感记录 (完整复刻 ~/Projects/car-inspiration 源方案;
   localStorage 沿用其 key, 两个应用数据互通; 语音 Web Speech API) ════ */
const IDEA_KEY = "car-inspiration:inspirations:v1";
const IDEA_CATS = [
  { id:"work", name:"工作", emoji:"💼" },
  { id:"life", name:"生活", emoji:"🌿" },
  { id:"idea", name:"创意", emoji:"💡" },
  { id:"todo", name:"待办", emoji:"✅" },
];
let ideaItems = [], ideaCat = "work", ideaFilter = "all", ideaEditId = null;
let recog = null, recogOn = false, ideaInited = false, ideaClockTimer = 0;

function ideaLoad() {
  try {
    const raw = localStorage.getItem(IDEA_KEY);
    const parsed = raw ? JSON.parse(raw) : [];
    ideaItems = Array.isArray(parsed) ? parsed : [];
  } catch (e) { ideaItems = []; }
  ideaItems.sort((a, b) => (b.createdAt || 0) - (a.createdAt || 0));   // 时间倒序兜底
}
function ideaPersist() {
  try { localStorage.setItem(IDEA_KEY, JSON.stringify(ideaItems)); } catch (e) {}
}
function ideaCatOf(id) { return IDEA_CATS.find(c => c.id === id) || null; }
function ideaDayLabel(ts) {
  const d = new Date(ts), now = new Date();
  if (d.toDateString() === now.toDateString()) return "今天";
  const y = new Date(now); y.setDate(now.getDate() - 1);
  if (d.toDateString() === y.toDateString()) return "昨天";
  const p = v => String(v).padStart(2, "0");
  return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate());
}
function ideaFullTime(ts) {
  const d = new Date(ts), p = v => String(v).padStart(2, "0");
  return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) +
         " " + p(d.getHours()) + ":" + p(d.getMinutes());
}
function ideaMetaTick() {
  const d = new Date(), p = v => String(v).padStart(2, "0");
  const week = "日一二三四五六"[d.getDay()];
  setText("idea-date", d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()));
  setText("idea-clock", p(d.getHours()) + ":" + p(d.getMinutes()) + " · 周" + week);
}
function renderIdeaCats() {
  $("idea-cats").innerHTML = IDEA_CATS.map(c =>
    '<button class="category-chip cchip c-' + c.id + (ideaCat === c.id ? " is-active" : "") +
    '" data-cat="' + c.id + '"><span class="category-emoji">' + c.emoji +
    '</span><span class="category-name">' + c.name + "</span></button>").join("");
}
function renderIdeaFilter() {
  $("idea-filter").innerHTML =
    '<button class="mini-chip all' + (ideaFilter === "all" ? " is-active" : "") +
    '" data-f="all">全部</button>' +
    IDEA_CATS.map(c => '<button class="mini-chip mchip c-' + c.id +
      (ideaFilter === c.id ? " is-active" : "") + '" data-f="' + c.id + '">' +
      c.emoji + " " + c.name + "</button>").join("");
}
function renderIdeaList() {
  const q = ($("idea-search").value || "").trim().toLowerCase();
  const list = ideaItems.filter(it =>
    (ideaFilter === "all" || it.categoryId === ideaFilter) &&
    (!q || String(it.text || "").toLowerCase().includes(q)));
  setText("idea-count", String(ideaItems.length));
  setText("idea-count2", list.length + " / " + ideaItems.length);
  const el = $("idea-list");
  if (!list.length) {
    el.innerHTML = '<div class="empty-state"><div class="empty-emoji">🛣️</div>' +
      '<p class="empty-title">还没有灵感</p><p class="empty-hint">' +
      (ideaItems.length ? "当前筛选下没有匹配的灵感" : "点击左侧输入或按下 🎙 开始语音记录") + "</p></div>";
    return;
  }
  let html = "", lastDay = "";
  for (const it of list) {
    const day = ideaDayLabel(it.createdAt);
    if (day !== lastDay) {
      const n = list.filter(x => ideaDayLabel(x.createdAt) === day).length;
      if (lastDay !== "") html += "</div></div>";   // 收上一个 day-cards+group
      html += '<div class="list-day-group"><div class="day-divider">' +
              '<span class="day-label">' + day + '</span><span class="day-line"></span>' +
              '<span class="day-count">' + n + " 条</span></div>" +
              '<div class="day-cards">';
      lastDay = day;
    }
    const cat = ideaCatOf(it.categoryId);
    const catCls = cat ? " c-" + cat.id : " c-none";
    if (ideaEditId === it.id) {
      html += '<article class="inspiration-card' + (cat ? catCls : "") + '">' +
        '<header class="card-meta"><span class="card-category' + catCls + '">' +
        (cat ? '<span>' + cat.emoji + '</span> ' + cat.name : "未分类") + "</span>" +
        '<span class="card-time">' + ideaFullTime(it.createdAt) + "</span></header>" +
        '<textarea class="card-edit" id="ie-' + esc(it.id) + '" rows="3">' + esc(it.text) + "</textarea>" +
        '<footer class="card-footer"><div class="card-category-edit">' +
        IDEA_CATS.map(c => '<button class="mini-chip mchip c-' + c.id +
          (it.categoryId === c.id ? " is-active" : "") + '" data-mc="' + c.id +
          '" data-id="' + esc(it.id) + '">' + c.emoji + " " + c.name + "</button>").join("") +
        '</div><div class="card-actions">' +
        '<button class="link-button" data-act="esave" data-id="' + esc(it.id) + '">保存</button>' +
        '<button class="link-button" data-act="ecancel">取消</button></div></footer></article>';
    } else {
      html += '<article class="inspiration-card' + catCls + '">' +
        '<header class="card-meta"><span class="card-category' + catCls + '">' +
        (cat ? '<span>' + cat.emoji + '</span> ' + cat.name : "未分类") + "</span>" +
        '<span class="card-time">' + ideaFullTime(it.createdAt) + "</span></header>" +
        '<p class="card-text">' + esc(it.text) + "</p>" +
        '<footer class="card-footer"><span></span><div class="card-actions">' +
        '<button class="link-button" data-act="edit" data-id="' + esc(it.id) + '">编辑</button>' +
        '<button class="link-button danger" data-act="del" data-id="' + esc(it.id) + '">删除</button>' +
        "</div></footer></article>";
    }
  }
  html += "</div></div>";   // 收最后一个 group
  el.innerHTML = html;
  if (ideaEditId) {
    const ta = $("ie-" + ideaEditId);
    if (ta) { ta.focus(); ta.selectionStart = ta.value.length; }
  }
}
function ideaRefreshAll() {
  renderIdeaCats(); renderIdeaFilter(); renderIdeaList(); ideaMetaTick();
}

/* ── 语音输入 (Web Speech API; 生产网关 CSP 已放开 Google 语音 connect-src) ── */
function ideaMicInit() {
  const mic = $("idea-mic"), micT = $("idea-mic-t");
  const Ctor = window.SpeechRecognition || window.webkitSpeechRecognition;
  if (!Ctor) {
    mic.disabled = true;
    mic.classList.add("is-disabled");
    micT.textContent = "语音不可用";
    return;
  }
  recog = new Ctor();
  recog.lang = "zh-CN";
  recog.continuous = true;
  recog.interimResults = true;
  recog.onresult = e => {
    let fin = "", interim = "";
    for (let i = e.resultIndex; i < e.results.length; i++) {
      const r = e.results[i];
      if (r.isFinal) fin += r[0].transcript;
      else interim += r[0].transcript;
    }
    if (fin) $("idea-text").value += fin;
    setText("idea-interim", interim ? "识别中: " + interim : "");
  };
  recog.onend = () => {
    recogOn = false;
    mic.classList.remove("is-listening");
    micT.textContent = "按下说话";
    if ($("idea-interim").textContent.indexOf("识别中") === 0) setText("idea-interim", "");
  };
  recog.onerror = ev => {
    const m = { "not-allowed":"麦克风权限被拒", "no-speech":"没有识别到语音",
                "network":"语音服务网络错误", "audio-capture":"没有麦克风",
                "service-not-allowed":"语音服务被禁用" };
    setText("idea-interim", m[ev.error] || ("语音错误: " + (ev.error || "?")));
  };
}
function ideaMicToggle() {
  if (!recog) return;
  if (recogOn) { try { recog.stop(); } catch (e) {} return; }
  try {
    recog.start();
    recogOn = true;
    $("idea-mic").classList.add("is-listening");
    $("idea-mic-t").textContent = "聆听中… 点击结束";
  } catch (e) {
    setText("idea-interim", "启动失败, 重试");
  }
}
function ideaSubmit() {
  const ta = $("idea-text"), txt = ta.value.trim();
  if (!txt) return;
  ideaItems.unshift({
    id: Date.now() + "-" + Math.random().toString(36).slice(2, 8),
    text: txt, categoryId: ideaCat, createdAt: Date.now(),
  });
  ideaPersist();
  ta.value = "";
  setText("idea-interim", "");
  ideaFilter = "all";
  $("idea-search").value = "";
  ideaRefreshAll();
}

/* ── 想法面板事件绑定 (一次) ── */
$("idea-cats").addEventListener("click", e => {
  const b = e.target.closest(".category-chip");
  if (b) { ideaCat = b.dataset.cat; renderIdeaCats(); }
});
$("idea-save").addEventListener("click", ideaSubmit);
$("idea-text").addEventListener("keydown", e => {
  if ((e.metaKey || e.ctrlKey) && e.key === "Enter") { e.preventDefault(); ideaSubmit(); }
});
$("idea-mic").addEventListener("click", ideaMicToggle);
$("idea-filter").addEventListener("click", e => {
  const b = e.target.closest(".mini-chip");
  if (b) { ideaFilter = b.dataset.f; ideaEditId = null; renderIdeaFilter(); renderIdeaList(); }
});
$("idea-search").addEventListener("input", renderIdeaList);
$("idea-clear").addEventListener("click", () => {
  if (!ideaItems.length) return;
  if (confirm("确认清空全部 " + ideaItems.length + " 条灵感？此操作不可恢复。")) {
    ideaItems = []; ideaEditId = null; ideaPersist(); ideaRefreshAll();
  }
});
$("idea-list").addEventListener("click", e => {
  const b = e.target.closest("button");
  if (!b) return;
  const id = b.dataset.id, act = b.dataset.act;
  if (act === "edit") {
    ideaEditId = id; renderIdeaList();
  } else if (act === "esave") {
    const it = ideaItems.find(x => x.id === id), ta = $("ie-" + id);
    if (it && ta && ta.value.trim()) { it.text = ta.value.trim(); ideaPersist(); }
    ideaEditId = null; renderIdeaList();
  } else if (act === "ecancel") {
    ideaEditId = null; renderIdeaList();
  } else if (act === "del") {
    ideaItems = ideaItems.filter(x => x.id !== id);
    if (ideaEditId === id) ideaEditId = null;
    ideaPersist(); ideaRefreshAll();
  } else if (act === "mc") {
    const it = ideaItems.find(x => x.id === id);
    if (it) { it.categoryId = b.dataset.mc; ideaPersist(); renderIdeaList(); }
  }
});
PANEL_OPENERS.idea = {
  open() {
    if (!ideaInited) { ideaMicInit(); ideaInited = true; }
    ideaLoad();
    ideaEditId = null;
    ideaRefreshAll();
    clearInterval(ideaClockTimer);
    ideaClockTimer = setInterval(ideaMetaTick, 30000);
    $("idea-text").focus();
  },
  close() {
    clearInterval(ideaClockTimer); ideaClockTimer = 0;
    if (recogOn) { try { recog.stop(); } catch (e) {} }
  }
};

/* ════ 启动 ════ */
applyTheme(themeIdx);
buildHealthGrid();
buildOpiGrid();
tickClock();
setInterval(tickClock, 1000);
refreshGlm();      refreshAgenda();
refreshNews();     refreshHealth();
refreshOpi();      refreshSentence();
refreshIpo();      refreshWeather();
buildVU();
loadStations();
setInterval(() => { refreshGlm(); refreshAgenda(); refreshNews(); refreshHealth(); refreshOpi(); }, 60000);
setInterval(() => { newsIdx++; renderNews(); }, 10000);
setInterval(refreshIpo, 30 * 60000);
setInterval(refreshWeather, 10 * 60000);