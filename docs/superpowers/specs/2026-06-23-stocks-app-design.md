# Stocks App (自选股) Design

## Goal

Add a new "自选股" tool to M5Tab5 that:

1. Shows a 7-column stock list (代码 / 名称 / 现价 / 涨跌幅 / 涨跌额 / 换手率 / 量比) read-only, fetched from the Mac `hermes-mcp-xiaozhi` server.
2. While the app is open, scrolls the same data on the 40×8 WS2812 LED matrix (5 daisy-chained 8×8 panels via Grove PORT A): each stock shows two static+scroll segments — `CODE ±PCT%` and `CODE PRICE` — cycled until the app closes.
3. Source of truth for the watchlist is the East Money 妙想 account (`mx-selfselect` skill). Tab5 never edits the list; users manage it from the East Money app or `mx_self_select.py` CLI.

## Scope

In scope:

- New Mooncake `AppAbility` `app/apps/app_stocks/` with full-screen table UI + 40×8 LED ticker.
- New hermes HTTP endpoint `GET /api/stocks/portfolio` with 30-second in-memory cache.
- New tile in `app_settings::_buildToolsPage` (Tools page row 3, next to 邮件).
- PORT A coordination with `app_email_led` via the existing `setPortAOwnedByApp(bool)` API.
- Build the firmware, flash to the device, and confirm the tile opens, the table renders Chinese names, the LED ticker cycles stocks, and closing the app returns control to `email_led` (or to a black LED if no unread mail).

Out of scope:

- Stock add/remove UI on Tab5. Users manage the list in the East Money app.
- Per-stock detail screens (K-line, fund flow, MA60, fundamentals). A simple modal with the row's data only.
- A separate WorkerAbility / background ticker. The ticker lives inside the app and stops when the app closes.
- Voice control, push notifications, alert thresholds, sorting, filtering, search.
- US / HK stock handling. East Money's `mx-selfselect` A股 list is the only source.

## Architecture

```
┌────────────────────┐    HTTP GET        ┌──────────────────┐   POST    ┌──────────────────┐
│  Tab5 app_stocks   │ ──────────────────►│  hermes (新增)   │ ────────► │  mx-selfselect   │
│  (AppAbility)      │  :8766/api/        │  GET /api/stocks │  apikey:  │  东方财富 API     │
│                    │   stocks/portfolio │  /portfolio      │  MX_APIKEY│                  │
│  onOpen:           │ ◄───────────────── │  (30s TTL 缓存)  │           └──────────────────┘
│   - 拉自选股       │   JSON             │                  │
│   - 占 PORT A      │                    │  用户在东方财富  │
│   - 启 LED ticker  │                    │  App / CLI 加删  │
│   - 显示 7 列表    │                    │  (Tab5 不管)     │
│  onClose:          │                    │                  │
│   - 停 ticker      │                    │                  │
│   - 还 PORT A      │                    │                  │
│   - email_led 接管 │                    │                  │
└────────────────────┘
```

Key constraints:

- **40×8 LED** is `app_unit_puzzle`'s hardware (5 chained 8×8 WS2812, GPIO53, `app_unit_puzzle.cpp:_xy2i` mapping). Tab5 灯阵, app_email_led, and the new app_stocks all share this matrix through the `setPortAOwnedByApp(bool)` coordination API.
- **font5x7** in `app_unit_puzzle.cpp` covers only ASCII `0x20-0x7E` (95 glyphs). Chinese names cannot render on the LED; only the 6-digit code, the percent, and the price are shown.
- **API key stays in hermes**. Tab5 holds no secrets; it calls hermes via the existing `svc_host` NVS entry (default `192.168.1.142`).
- **Data is fetched only while the app is open.** There is no background polling, no `WorkerAbility`, and no NVS cache. Open the app → fetch; close the app → no fetch, no ticker.

## Components

### 1. `app/apps/app_stocks/app_stocks.{h,cpp}` — Mooncake app

Public surface:

```cpp
class AppStocks : public mooncake::AppAbility {
public:
    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }
private:
    struct StockItem { std::string code, name; float price, chg, pchg, turnover, liangbi; };
    std::vector<StockItem> _items;
    led_strip_handle_t      _strip{nullptr};
    TaskHandle_t            _ticker_task{nullptr};
    std::atomic<bool>       _ticker_running{false};
    std::atomic<bool>       _ticker_done{false};
    // … LVGL handles, mutex, timers, status strings
};
```

Methods (private):

- `_stripInit()` / `_stripDeinit()` — wraps `led_strip_new_rmt_device` with the 5-block config from `app_unit_puzzle`.
- `_buildUi()` / `_destroyUi()` — creates the 7-column table, header, footer status bar.
- `_showDetail(int row)` — modal card with that row's 7 fields.
- `_fetchStocksAsync()` — `hal->httpGet("http://" + svc_host + ":8766/api/stocks/portfolio")` via `tryRunDetached(fn)`. 5-second timeout, plain HTTP, 8 KB pthread stack is enough (no TLS).
- `_parseStocksJson(const std::string& body)` — nlohmann::json parse to `_items`.
- `_startTicker()` / `_stopTicker()` — FreeRTOS task that drives LED scrolling.
- `_renderCurrentStock(int64_t now_ms)` — one frame of LED work for `_ticker_index`.
- `_setPixelXY(int x, int y, uint8_t r, g, b)` — same mapping as `app_email_led::_xy2i`.

### 2. `hermes_mcp_server/main.py` — new endpoint

```python
_STOCKS_CACHE = {"ts": 0.0, "data": None}
_STOCKS_TTL_S = 30

async def handle_stocks_portfolio(request):
    now = time.time()
    if _STOCKS_CACHE["data"] and now - _STOCKS_CACHE["ts"] < _STOCKS_TTL_S:
        return web.json_response(_STOCKS_CACHE["data"])

    apikey = os.environ.get("MX_APIKEY", "")
    if not apikey:
        return web.json_response(
            {"error": "MX_APIKEY not set", "items": []}, status=500)

    try:
        async with aiohttp.ClientSession() as session:
            async with session.post(
                "https://mkapi2.dfcfs.com/finskillshub/api/claw/self-select/get",
                headers={"apikey": apikey, "Content-Type": "application/json"},
                json={},
                timeout=aiohttp.ClientTimeout(total=10),
            ) as resp:
                raw = await resp.json()
    except Exception as e:
        return web.json_response(
            {"error": f"upstream: {e}", "items": []}, status=502)

    items = []
    if raw.get("status") == 0:
        rows = (raw.get("data", {})
                   .get("allResults", {})
                   .get("result", {})
                   .get("dataList", []))
        for r in rows:
            items.append({
                "code":     r.get("SECURITY_CODE", ""),
                "name":     r.get("SECURITY_SHORT_NAME", ""),
                "price":    r.get("NEWEST_PRICE", 0),
                "chg":      r.get("CHG", 0),
                "pchg":     r.get("PCHG", 0),
                "turnover": r.get("010000_TURNOVER_RATE", 0),
                "liangbi":  r.get("010000_LIANGBI", 0),
            })
    result = {"count": len(items), "items": items, "ts": int(now)}
    _STOCKS_CACHE = {"ts": now, "data": result}
    return web.json_response(result)
```

Route registration in the existing `app = web.Application(...)` block:

```python
app.router.add_get("/api/stocks/portfolio", handle_stocks_portfolio)
```

### 3. `app/apps/app_installer.h` — register the new app

Mirror the lora_chat / unit_puzzle pattern:

```cpp
#include "app_stocks/app_stocks.h"
// …
auto st_uptr = std::make_unique<AppStocks>();
AppStocks* stocks = st_uptr.get();
// …
int st_id = mooncake::GetMooncake().installApp(std::move(st_uptr));
// …
stocks->setCloseCallback([home]() { home->restoreScreen(); });
settings->setStocksAppId(st_id);
// …
home->addApp("自选股", st_id);  // optional: or just expose via Tools page
```

(Decision: add a home tile AND a Tools tile. Home entry mirrors the lora_chat precedent; Tools tile keeps the alt path consistent. Both open the same `AppStocks`.)

### 4. `app/apps/app_settings/app_settings.{h,cpp}` — Tools tile

- Add `setStocksAppId(int id)` / `getStocksAppId()` plus `_toolStocks_cb` static handler.
- Add a new tile in `_buildToolsPage` row 3 column 1, next to 邮件: 130×130 icon + "自 选" label, opens via `_openStocks()`.
- The icon is a 4-bar rising signal graphic, hand-built as a new `app/apps/app_settings/stocks_icon.c` (LVGL ARGB8888, similar to `lora_logo.c`).

### 5. Coordinate GPIO sharing

- `onOpen`: call `AppEmailLed::setPortAOwnedByApp(true)` first; this releases `email_led`'s `led_strip`. Then call our own `_stripInit()`.
- `onClose`: call `_stopTicker()` and wait for `_ticker_done`; call `_stripDeinit()`; call `AppEmailLed::setPortAOwnedByApp(false)`.
- All `_setPixelXY` calls hold the same `_strip_mu` mutex pattern that `app_email_led` uses, so a future scenario where two LED apps overlap during handoff cannot corrupt the frame buffer.

## Data flow

### Fetch path (Tab5 side)

1. `onOpen` posts a worker task via `tryRunDetached([this](){ _doFetch(); })`. The closure:
   1. Builds URL `http://<svc_host>:8766/api/stocks/portfolio` from `hal->getConfig().svc_host` (default `192.168.1.142`).
   2. Calls `hal->httpGet(url, 5000)`.
   3. On success: parses JSON, swaps `_items` (under mutex), sets `_lastFetchMs = now`, kicks `_startTicker()` if it was idle.
   4. On failure: sets `_fetch_error = true`, keeps the previous `_items` (so the user still sees the last good data), logs the error.
2. `onRunning` polls `_lastFetchMs` and re-fires `_fetchStocksAsync()` every 30 000 ms.

### Hermes cache

- TTL 30 s. Multiple Tab5 apps (or repeated opens) hit the cache.
- Cache is process-local; restart of hermes loses the cache, first request re-fetches from East Money.

### LED ticker

- One FreeRTOS task at priority 1, 8 KB stack. ~30 FPS frame rate (33 ms tick).
- For each `StockItem` in `_items` (round-robin):
  - **Segment 1** (`CODE ±PCT%`, ~14 chars): static-center 0.5 s → scroll left at 150 ms/px → 1.5 s total.
  - **Segment 2** (`CODE PRICE`, ~12 chars): static-center 0.5 s → scroll left 1.0 s.
  - 1 s black pause.
- 10 stocks × ~5 s = 50 s per full cycle.
- Color: code + price white (`0xEAF3FB`), CHG green if `> 0` (`0x2ECC71`), red if `< 0` (`0xE74C3C`), gray if `0` (`0x95A5A6`).
- Ticker is owned by the app; no WorkerAbility, no app backgrounded LED.

## UI design

### Main screen — 7-column table (1280×720)

| Region | Size | Content |
|---|---|---|
| Header | 72 px | `← 自选股`  (CST clock)  (●在线/离线)  (count) |
| Column header | 40 px | 代码 名称 现价 涨跌幅 涨跌额 换手率 量比 |
| Data rows | 52 px × 10 | One row per stock |
| Status bar | 60 px | `● LED: 滚动中 (i/n)` + 上次更新时间 + 刷新 button |

Column widths: 代码 100, 名称 200, 现价 130, 涨跌幅 140, 涨跌额 140, 换手率 130, 量比 100, 状态点 30, gap 190. Total ≈ 1240 px (fits 1280 with 20 px side padding).

Row text: cbin `font_puhui_common_20_4` (zh_font_sm). Header / status bar: `font_puhui_common_30_4` (zh_font_lg).

### Detail modal

600×400 centered card, 0x081522 background, 4FA3FF border, cbin 20px. Shows the 7 fields + last update timestamp. `[关闭]` button bottom-right.

### Error states

| Condition | UI |
|---|---|
| First fetch fails | Centered text "●离线  上次无数据" |
| Fetch fails, cached data exists | Table renders with greyed rows, status bar shows "●离线 (使用上次数据)" |
| Empty list (weekends, no items) | "自选股列表为空，请到东方财富 App 添加" centered |
| MX_APIKEY not set in hermes | "服务端 MX_APIKEY 未配置" centered |

### Touch

- Tap a row → open detail modal.
- Tap [刷新] → force re-fetch and reset ticker to index 0.
- Tap ← header or swipe up → `close()`.

## Error handling

- **hermes down** (connection refused / timeout 5 s): `_fetch_error=true`, last `_items` shown, status bar shows offline.
- **East Money 5xx / network** (hermes 502 from upstream): same as above; hermes logs upstream error, returns empty `items` with `error` field.
- **JSON parse failure**: log error, treat as empty fetch (show offline state).
- **led_strip init failure** (e.g., port A still held): log, set `_strip=nullptr`, ticker thread becomes a no-op; app UI still works.
- **Empty `_items` on first onOpen**: render "自选股列表为空" message; do not start the ticker.

## Validation

1. `cd platforms/tab5 && idf.py build` — must succeed.
2. `cd platforms/desktop && make -j` — desktop sim must compile (LED code under `#ifndef PLATFORM_BUILD_DESKTOP`).
3. `python3 test/test_stocks_app.py` — round-trip test against a mock hermes endpoint that returns the East Money response shape; verify parsing and 30 s cache.
4. `cd platforms/tab5 && python -m esptool --chip esp32p4 -p /dev/cu.usbmodem* -b 460800 --before default-reset --after hard-reset write-flash …` — flash and verify:
   - Tools page shows 自选股 tile.
   - Tap → table renders 10 rows with Chinese names.
   - 40×8 LED cycles: code+CHG first (colored), then code+price (white), per stock.
   - Status bar updates every 30 s.
   - Close app → LED goes off, `email_led` reclaim (verify by sending a test unread email and watching the matrix).
5. Restart hermes mid-flight, confirm Tab5 recovers within 30 s (next refresh hits the rebuilt cache).

## Open questions

None remaining. Decisions captured:

- Data path: Tab5 → hermes → mx-selfselect (decided).
- Watchlist: read-only on Tab5 (decided).
- Ticker: AppAbility only, stops on close (decided).
- LED: 40×8 (5×8 chained) (decided).
- Display format: per-stock 2-segment static+scroll (decided).
- Polling cadence: 30 s while open, 0 s when closed (decided).
- Cache: 30 s in hermes, in-memory only (decided).

## Out of scope (deferred)

- Detail screen with K-line / 资金流 / 财务数据.
- Per-stock alert thresholds and push notifications.
- US / HK stock support.
- Add/remove UI on Tab5.
- Sorting (by code / by chg%) — fixed order = mx-selfselect return order.
- Background ticker (WorkerAbility).
- A股 market hours detection (we just poll every 30 s; during closed hours hermes returns last known values or empty, and the UI degrades gracefully).
