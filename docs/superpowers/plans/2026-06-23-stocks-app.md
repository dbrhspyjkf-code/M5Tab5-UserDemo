# 自选股 App (Stocks) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a read-only 自选股 (watchlist) Mooncake app on Tab5 that fetches A-stock data from hermes → 东方财富 (East Money) and scrolls the same data on the 40×8 LED matrix while the app is open.

**Architecture:** Tab5 calls `http://<svc_host>:8766/api/stocks/portfolio` (a new hermes endpoint with 30s in-memory cache) which proxies to 东方财富's `mx-selfselect` API using the `MX_APIKEY` already in hermes's `.env`. Tab5 holds no secrets. The app's lifecycle owns the LED strip — opens claim PORT A from `app_email_led`, closes return it.

**Tech Stack:**
- ESP-IDF v5.5.2 + Mooncake AppAbility + LVGL 9
- C++17 (Tab5 side)
- Python 3 / aiohttp (hermes side)
- Existing: `HalBase::httpGet` (8 KB pthread), `led_strip` (RMT/WS2812), `app_unit_puzzle` 5-block 8×8 → 40×8 wiring
- Font: existing `app_unit_puzzle.cpp:font5x7[95]` (ASCII 0x20-0x7E) for LED; cbin `font_puhui_common_20_4` for table text
- API key: `MX_APIKEY` in `~/hermes-mcp-xiaozhi/.env` (already configured)

**Build & Flash:**
```bash
source ~/.local/bin/idf_env.sh
cd ~/Projects/M5Tab5/M5Tab5-UserDemo/platforms/tab5
idf.py build
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem* -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/m5stack_tab5.bin \
  0xd74000 build/srmodels/srmodels.bin
```

**Desktop Sim:** `cd platforms/desktop/build && cmake .. && make -j && ./app_desktop_build`

## Global Constraints

- `app/` is shared between desktop and device. **No ESP-only includes at file top-level**; use `#ifndef PLATFORM_BUILD_DESKTOP` like `app_email_led.cpp`.
- LED code under `#ifndef PLATFORM_BUILD_DESKTOP` because desktop sim has no WS2812.
- `font5x7` is a copy-paste from `app_unit_puzzle.cpp:37` (95 ASCII glyphs, bit4=leftmost column, byte0=top row). Do not modify.
- All LVGL calls from a non-LVGL thread must hold `hal->lvglLock()` (RAII `LvglLockGuard`).
- All LED writes (`led_strip_set_pixel` / `led_strip_refresh`) hold the app's `_strip_mu` mutex.
- File paths in this plan are relative to `/Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo/`.
- Don't commit `sdkconfig` (local patches only, kept out of git per project convention).
- Naming: stock app = `app_stocks`. Tile label on home = `自选股`. Tools tile label = `自  选` (two spaces, matches `小  智` / `工  具` style).

---

## File Structure

| File | Role |
|---|---|
| `hermes_mcp_server/main.py` (modify) | Add `GET /api/stocks/portfolio` handler + cache + route registration |
| `test/test_stocks_endpoint.py` (new) | Source-level tests for hermes endpoint (cache, error paths) |
| `app/apps/app_stocks/app_stocks.h` (new) | `AppStocks` class declaration, StockItem struct |
| `app/apps/app_stocks/app_stocks.cpp` (new) | Lifecycle, UI, data fetch, LED bring-up + ticker |
| `app/apps/app_settings/stocks_icon.c` (new) | LVGL ARGB8888 130×130 icon (rising bars) |
| `app/apps/app_settings/app_settings.h` (modify) | Add `setStocksAppId(int)`, `_stocks_id` member, `openStocks()` |
| `app/apps/app_settings/app_settings.cpp` (modify) | Add `_buildStocksTile()`, `_toolStocks_cb`, `openStocks()` |
| `app/apps/app_home/app_home.h` (modify) | Add `setStocksAppId` if home tile added (decided yes, see Task 3) |
| `app/apps/app_home/app_home.cpp` (modify) | Add 5th home tile (自选股) |
| `app/apps/app_installer.h` (modify) | Include + install + wire `AppStocks` |

---

### Task 1: Hermes `/api/stocks/portfolio` endpoint

**Files:**
- Modify: `~/hermes-mcp-xiaozhi/hermes_mcp_server/main.py`
- Create: `test/test_stocks_endpoint.py`

**Interfaces:**
- Consumes: env `MX_APIKEY`
- Produces: `GET /api/stocks/portfolio` → `{count, items:[{code,name,price,chg,pchg,turnover,liangbi}], ts}` (200), `{error, items:[]}` (500/502)

- [ ] **Step 1: Write the failing test**

Create `test/test_stocks_endpoint.py`:

```python
"""Source-level tests for hermes stocks endpoint. Mock aiohttp to avoid hitting the real API."""
import asyncio
import json
import os
import sys
import time
from unittest.mock import patch, MagicMock

# Make hermes_mcp_server importable as a package
sys.path.insert(0, os.path.expanduser("~/hermes-mcp-xiaozhi"))

from hermes_mcp_server import main as hermes_main  # noqa: E402


def _east_money_payload(items):
    return {
        "status": 0,
        "code": 0,
        "data": {
            "allResults": {
                "result": {
                    "columns": [{"key": k, "title": k} for k in [
                        "SECURITY_CODE", "SECURITY_SHORT_NAME", "NEWEST_PRICE",
                        "CHG", "PCHG", "010000_TURNOVER_RATE", "010000_LIANGBI"]],
                    "dataList": items,
                }
            }
        }
    }


def _row(code="600519", name="贵州茅台", price=1850.0, chg=2.78, pchg=50.0,
         turnover=0.35, liangbi=1.2):
    return {
        "SECURITY_CODE": code, "SECURITY_SHORT_NAME": name,
        "NEWEST_PRICE": price, "CHG": chg, "PCHG": pchg,
        "010000_TURNOVER_RATE": turnover, "010000_LIANGBI": liangbi,
    }


def _reset_cache():
    hermes_main._STOCKS_CACHE = {"ts": 0.0, "data": None}


def test_normalize_two_rows():
    """Mock upstream with 2 rows; verify response has count=2 and flat items."""
    async def run():
        _reset_cache()
        payload = _east_money_payload([_row(), _row(code="300750", name="宁德时代",
                                                     price=380.0, chg=-1.25)])
        mock_resp = MagicMock()
        mock_resp.json = asyncio.coroutine(lambda: payload)
        mock_session = MagicMock()
        mock_session.post = MagicMock()
        mock_session.post.return_value.__aenter__ = asyncio.coroutine(lambda self: mock_resp)
        mock_session.post.return_value.__aexit__ = asyncio.coroutine(lambda *a: False)
        with patch.dict(os.environ, {"MX_APIKEY": "test_key"}), \
             patch.object(hermes_main.aiohttp, "ClientSession", lambda: mock_session):
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            body = json.loads(resp.body)
            assert body["count"] == 2, f"expected 2 got {body['count']}"
            assert body["items"][0]["code"] == "600519"
            assert body["items"][0]["name"] == "贵州茅台"
            assert body["items"][0]["price"] == 1850.0
            assert body["items"][0]["chg"] == 2.78
            assert body["items"][1]["chg"] == -1.25
    asyncio.run(run())


def test_cache_hit_skips_upstream():
    """If cache is fresh, second call must not hit aiohttp."""
    async def run():
        _reset_cache()
        hermes_main._STOCKS_CACHE = {"ts": time.time(), "data":
            {"count": 1, "items": [{"code": "TEST", "name": "x", "price": 1,
                                     "chg": 0, "pchg": 0, "turnover": 0, "liangbi": 0}],
             "ts": int(time.time())}}
        # aiohttp must not be touched
        def fail(*a, **kw): raise AssertionError("upstream should not be called")
        with patch.object(hermes_main.aiohttp, "ClientSession", side_effect=fail):
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            body = json.loads(resp.body)
            assert body["items"][0]["code"] == "TEST"
    asyncio.run(run())


def test_missing_apikey_returns_500():
    """No MX_APIKEY → 500 with empty items."""
    async def run():
        _reset_cache()
        with patch.dict(os.environ, {}, clear=True):
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            assert resp.status == 500
            body = json.loads(resp.body)
            assert "MX_APIKEY" in body["error"]
            assert body["items"] == []
    asyncio.run(run())


def test_upstream_error_returns_502():
    """aiohttp raises → 502 with empty items."""
    async def run():
        _reset_cache()
        mock_session = MagicMock()
        mock_session.post.side_effect = Exception("connection refused")
        with patch.dict(os.environ, {"MX_APIKEY": "test_key"}), \
             patch.object(hermes_main.aiohttp, "ClientSession", lambda: mock_session):
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            assert resp.status == 502
            body = json.loads(resp.body)
            assert "upstream" in body["error"]
    asyncio.run(run())


if __name__ == "__main__":
    test_normalize_two_rows()
    print("test_normalize_two_rows OK")
    test_cache_hit_skips_upstream()
    print("test_cache_hit_skips_upstream OK")
    test_missing_apikey_returns_500()
    print("test_missing_apikey_returns_500 OK")
    test_upstream_error_returns_502()
    print("test_upstream_error_returns_502 OK")
    print("\nAll 4 tests passed.")
```

- [ ] **Step 2: Run the test to confirm it fails**

Run:
```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
python3 test/test_stocks_endpoint.py
```
Expected: `ModuleNotFoundError: No module named 'hermes_mcp_server.main'` (or `ImportError`).

- [ ] **Step 3: Add cache + handler to hermes `main.py`**

In `~/hermes-mcp-xiaozhi/hermes_mcp_server/main.py`, near the top (after the existing `import` block and the `_push_api_runner = None` global), add:

```python
_STOCKS_CACHE = {"ts": 0.0, "data": None}
_STOCKS_TTL_S = 30
```

Then, just before the `app = web.Application(middlewares=[cors_middleware])` line (which currently lives around line 612), add the handler:

```python
async def handle_stocks_portfolio(request):
    """GET /api/stocks/portfolio — flatten East Money mx-selfselect payload."""
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
    _STOCKS_CACHE["ts"] = now
    _STOCKS_CACHE["data"] = result
    return web.json_response(result)
```

Make sure `import time` and `import aiohttp` already exist at the top of the file (they do, per `grep` of existing imports).

- [ ] **Step 4: Register the route**

In the same `app = web.Application(...)` block, after the existing `app.router.add_get("/rate", handle_rate)` line, add:

```python
app.router.add_get("/api/stocks/portfolio", handle_stocks_portfolio)
```

- [ ] **Step 5: Run the test, expect it passes**

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
python3 test/test_stocks_endpoint.py
```
Expected: `All 4 tests passed.`

If it fails, check that `sys.path` insertion is correct and the mock session returns a coroutine that yields the payload (the `asyncio.coroutine(lambda: ...)` decorator is required because `MagicMock().json` must be awaitable).

- [ ] **Step 6: Commit**

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
cd ../M5Tab5-UserDemo
cd /Users/leenzhou/hermes-mcp-xiaozhi
git add hermes_mcp_server/main.py test/../test_stocks_endpoint.py ../M5Tab5-UserDemo/test/test_stocks_endpoint.py
# Note: test_stocks_endpoint.py lives in the Tab5 repo, not hermes.
# Run the commit from the Tab5 repo, then a separate commit from the hermes repo.
```

Actually split: the test file is in the Tab5 repo, the hermes change is in the hermes repo. Commit each separately:

```bash
# Tab5 repo
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
git add test/test_stocks_endpoint.py
git -c user.name="leenzhou" -c user.email="leenzhou@users.noreply.github.com" \
    commit -m "test(stocks): hermes /api/stocks/portfolio 端点测试 (4 用例)"

# Hermes repo
cd ~/hermes-mcp-xiaozhi
git add hermes_mcp_server/main.py
git -c user.name="leenzhou" -c user.email="leenzhou@users.noreply.github.com" \
    commit -m "feat(api): GET /api/stocks/portfolio — 30s 缓存转发 mx-selfselect"
```

- [ ] **Step 7: Smoke test the live endpoint**

Restart hermes to pick up the new route:
```bash
# Find the running hermes process
pgrep -fl "hermes" | head -3
# Kill and restart (adjust to actual supervisor / launch script)
```

If hermes is run via `~/hermes-mcp-xiaozhi/launch.sh` or similar, restart through that. Then:
```bash
curl -s http://localhost:8766/api/stocks/portfolio | python3 -m json.tool | head -30
```
Expected: JSON with `count` (probably 10) and `items` array, each item having `code/name/price/chg/pchg/turnover/liangbi`.

---

### Task 2: `app_stocks` skeleton + icon asset

**Files:**
- Create: `app/apps/app_stocks/app_stocks.h`
- Create: `app/apps/app_stocks/app_stocks.cpp` (skeleton, no logic)
- Create: `app/apps/app_settings/stocks_icon.c`

**Interfaces:**
- Produces: `class AppStocks : public mooncake::AppAbility` with `void onCreate/onOpen/onRunning/onClose override` (all initially empty)
- Produces: `extern const lv_image_dsc_t stocks_icon;` (130×130 ARGB8888)

- [ ] **Step 1: Create the icon C file**

`app/apps/app_settings/stocks_icon.c` is a 130×130 ARGB8888 image. Use Python to generate the byte array (3 bars rising left to right, cyan):

```bash
python3 - <<'PY' > /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo/app/apps/app_settings/stocks_icon.c
W, H = 130, 130
# 4 bars rising, navy bg
pixels = bytearray()
for y in range(H):
    for x in range(W):
        in_bar = False
        # 4 bars: x ranges and heights (top row index when bar starts)
        for i, (x0, x1, top) in enumerate([(15, 35, 80), (45, 65, 60),
                                            (75, 95, 35), (105, 125, 20)]):
            if x0 <= x < x1 and y >= top:
                in_bar = True
        if in_bar:
            pixels += bytes([0xFF, 0xA3, 0x4F, 0x00])  # B, G, R, A  — orange (LVGL ARGB8888 byte order is BGRA)
        else:
            pixels += bytes([0x22, 0x15, 0x08, 0xFF])  # B, G, R, A — dark navy bg

print('// Auto-generated stocks icon (4 rising bars, 130x130 ARGB8888)')
print('#include <lvgl.h>')
print('const uint8_t stocks_icon_pixel_data[%d] = {' % len(pixels))
for i in range(0, len(pixels), 16):
    line = ', '.join('0x%02x' % b for b in pixels[i:i+16])
    print(f'    {line},')
print('};')
print('const lv_image_dsc_t stocks_icon = {')
print('    .header = { .w = 130, .h = 130, .cf = LV_COLOR_FORMAT_ARGB8888, .stride = 130*4, .magic = LV_IMAGE_HEADER_MAGIC },')
print('    .data_size = sizeof(stocks_icon_pixel_data),')
print('    .data = stocks_icon_pixel_data,')
print('};')
PY
```

(Note: LVGL ARGB8888 is **BGRA** byte order in memory — the comment is correct.)

- [ ] **Step 2: Create the header**

`app/apps/app_stocks/app_stocks.h`:

```cpp
#pragma once
#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>

#ifndef PLATFORM_BUILD_DESKTOP
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <led_strip.h>
#endif

class AppStocks : public mooncake::AppAbility {
public:
    AppStocks();
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    struct StockItem {
        std::string code;
        std::string name;
        float price   {0};
        float chg     {0};   // 涨跌幅 %
        float pchg    {0};   // 涨跌额
        float turnover{0};
        float liangbi {0};
    };

    // ── Layout ─────────────────────────────────────────────────────────
    static constexpr int SCREEN_W    = 1280;
    static constexpr int SCREEN_H    = 720;
    static constexpr int HEADER_H    = 72;
    static constexpr int STATUS_H    = 60;
    static constexpr int ROW_H       = 52;
    static constexpr int N_ROWS      = 10;

    // ── Color palette (must match app_home / app_ha) ──────────────────
    static constexpr uint32_t C_BG       = 0x081522;
    static constexpr uint32_t C_HEADER   = 0x0D1F35;
    static constexpr uint32_t C_TEXT     = 0xEAF3FB;
    static constexpr uint32_t C_DIM      = 0x8AA0B5;
    static constexpr uint32_t C_UP       = 0x2ECC71;
    static constexpr uint32_t C_DOWN     = 0xE74C3C;
    static constexpr uint32_t C_FLAT     = 0x95A5A6;
    static constexpr uint32_t C_ACCENT   = 0x4FA3FF;

    // ── LED ────────────────────────────────────────────────────────────
    static constexpr int LED_W = 40;
    static constexpr int LED_H = 8;
    static constexpr int LED_PANEL = 8;
    static constexpr int LED_N = LED_W * LED_H;  // 5 panels * 64
    static constexpr int LED_DIN_GPIO = 6;       // Grove PORT A signal (same as unit_puzzle)
    static constexpr int CHAR_W = 5;
    static constexpr int CHAR_H = 7;
    static constexpr int CHAR_GAP = 1;
    static constexpr int CELL = CHAR_W + CHAR_GAP;       // 6 px per char
    static constexpr int SCROLL_MS_PER_PX = 150;          // match app_email_led
    static constexpr int STOCK_SEG1_MS = 2500;            // code+CHG segment
    static constexpr int STOCK_SEG2_MS = 2000;            // code+price segment
    static constexpr int STOCK_PAUSE_MS = 1000;           // black gap

    // ── State ──────────────────────────────────────────────────────────
    std::function<void()> _close_cb;
    std::vector<StockItem> _items;
    std::mutex             _items_mu;
    bool                   _fetch_error {false};
    std::string            _fetch_error_msg;
    int64_t                _last_fetch_ms {0};
    static constexpr int   FETCH_INTERVAL_MS = 30 * 1000;
    static constexpr int   FETCH_TIMEOUT_MS  = 5 * 1000;

    // ── LVGL handles ───────────────────────────────────────────────────
    lv_obj_t* _scr         {nullptr};
    lv_obj_t* _header      {nullptr};
    lv_obj_t* _status_bar  {nullptr};
    lv_obj_t* _status_dot  {nullptr};
    lv_obj_t* _status_text {nullptr};
    lv_obj_t* _refresh_btn {nullptr};
    lv_obj_t* _col_header  {nullptr};
    lv_obj_t* _rows[N_ROWS] {nullptr};
    lv_obj_t* _detail_modal{nullptr};
    lv_timer_t* _poll_timer{nullptr};

    // ── LED handles (ESP only) ─────────────────────────────────────────
#ifndef PLATFORM_BUILD_DESKTOP
    led_strip_handle_t  _strip         {nullptr};
    std::mutex          _strip_mu;
    TaskHandle_t        _ticker_task   {nullptr};
    std::atomic<bool>   _ticker_running{false};
    std::atomic<bool>   _ticker_done   {false};
    int                 _ticker_index  {0};
    int64_t             _ticker_seg_start_ms {0};
    int                 _ticker_seg_state {0};  // 0=seg1_scroll, 1=seg2_scroll
#endif

    // ── Methods (filled in later tasks) ────────────────────────────────
    void _buildUi();
    void _destroyUi();
    void _setCellText(lv_obj_t* row, int col, const char* text, uint32_t color);
    void _setStatus(bool online, const char* text);
    void _showDetail(int row);
    void _closeDetail();
    void _fetchStocksAsync();
    void _doFetch();
    void _parseStocksJson(const std::string& body);
    void _refreshUiFromItems();

#ifndef PLATFORM_BUILD_DESKTOP
    esp_err_t _stripInit();
    void      _stripDeinit();
    void      _setPixelXY(int x, int y, uint8_t r, uint8_t g, uint8_t b);
    int       _xy2i(int x, int y);
    void      _startTicker();
    void      _stopTicker();
    static void _tickerTask(void* arg);
    void      _renderTickerFrame(int64_t now_ms);
    void      _renderStockSegment(const char* text, int text_len,
                                  int64_t now_ms, uint8_t r, uint8_t g, uint8_t b);
    uint32_t  _chgColor(float chg);
#endif
};
```

- [ ] **Step 3: Create the skeleton .cpp**

`app/apps/app_stocks/app_stocks.cpp`:

```cpp
#include "app_stocks.h"
#include <mooncake_log.h>

#ifndef PLATFORM_BUILD_DESKTOP
#include <hal/hal.h>
#include "../app_email_led/app_email_led.h"  // for setPortAOwnedByApp
#endif

static const char* TAG = "stocks";

AppStocks::AppStocks() {}

void AppStocks::onCreate() {
    mclog::tagInfo(TAG, "created");
}

void AppStocks::onOpen() {
    mclog::tagInfo(TAG, "opened");
    // Task 3: UI build
    // Task 4: data fetch
    // Task 5: LED claim
    // Task 6: ticker start
    _buildUi();
}

void AppStocks::onRunning() {
    // Task 4: poll cadence
}

void AppStocks::onClose() {
    mclog::tagInfo(TAG, "closed");
    // Task 6: ticker stop
    // Task 5: LED release
    _destroyUi();
}

// All other methods are implemented in later tasks.
// Empty stubs here so the file compiles after this task:
void AppStocks::_buildUi() {}
void AppStocks::_destroyUi() {}
void AppStocks::_setCellText(lv_obj_t*, int, const char*, uint32_t) {}
void AppStocks::_setStatus(bool, const char*) {}
void AppStocks::_showDetail(int) {}
void AppStocks::_closeDetail() {}
void AppStocks::_fetchStocksAsync() {}
void AppStocks::_doFetch() {}
void AppStocks::_parseStocksJson(const std::string&) {}
void AppStocks::_refreshUiFromItems() {}

#ifndef PLATFORM_BUILD_DESKTOP
esp_err_t AppStocks::_stripInit() { return ESP_OK; }
void AppStocks::_stripDeinit() {}
void AppStocks::_setPixelXY(int, int, uint8_t, uint8_t, uint8_t) {}
int AppStocks::_xy2i(int x, int y) {
    int panel = x / LED_PANEL;
    int lx    = x % LED_PANEL;
    return panel * (LED_PANEL * LED_PANEL) + lx * LED_PANEL + (LED_PANEL - 1 - y);
}
void AppStocks::_startTicker() {}
void AppStocks::_stopTicker() {}
void AppStocks::_tickerTask(void*) {}
void AppStocks::_renderTickerFrame(int64_t) {}
void AppStocks::_renderStockSegment(const char*, int, int64_t, uint8_t, uint8_t, uint8_t) {}
uint32_t AppStocks::_chgColor(float chg) {
    if (chg > 0.01f)  return C_UP;
    if (chg < -0.01f) return C_DOWN;
    return C_FLAT;
}
#endif
```

- [ ] **Step 4: Build to confirm the skeleton compiles**

```bash
source ~/.local/bin/idf_env.sh
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo/platforms/tab5
idf.py build 2>&1 | tail -20
```
Expected: `Project build complete.` (warnings about unused parameters are OK; later tasks use those members).

If the build complains about `app_email_led.h` not being a real header (it is — see `app/apps/app_email_led/app_email_led.h`), or about `freertos/task.h` not on desktop, the `#ifndef PLATFORM_BUILD_DESKTOP` guards are working.

- [ ] **Step 5: Build desktop sim to confirm `#ifdef PLATFORM_BUILD_DESKTOP` paths compile**

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo/platforms/desktop/build
cmake .. 2>&1 | tail -5
make -j 2>&1 | tail -10
```
Expected: builds (warnings OK). The icon file `stocks_icon.c` is included in the desktop build because the existing `app_settings` CMake target globs all `*.c` in that dir.

- [ ] **Step 6: Commit**

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
git add app/apps/app_stocks/ app/apps/app_settings/stocks_icon.c
git -c user.name="leenzhou" -c user.email="leenzhou@users.noreply.github.com" \
    commit -m "feat(stocks): app_stocks 骨架 + 工具页 icon asset (no logic yet)"
```

---

### Task 3: Installer + Settings tile + Home tile

**Files:**
- Modify: `app/apps/app_installer.h`
- Modify: `app/apps/app_settings/app_settings.h`
- Modify: `app/apps/app_settings/app_settings.cpp`
- Modify: `app/apps/app_home/app_home.h` (only if home tile added; see below)
- Modify: `app/apps/app_home/app_home.cpp` (only if home tile added; see below)

**Interfaces:**
- Produces: `void AppSettings::setStocksAppId(int id)` and `int AppSettings::getStocksAppId() const`
- Produces: `AppSettings::openStocks()` opens the stocks app (idempotent like `openEmailPage()`)
- Produces: when home tile added, `AppHome::addApp("自选股", st_id)` line in `installer`

**Decision on home tile**: the spec says "add a home tile AND a Tools tile". Confirm with user before adding the 5th home tile — current home has 4 entries (智能家居/小智/工具/Claude); adding 自选股 makes 5 in a 2×3 grid (one slot blank) or 5 in a 2×3 with rotated layout. **Default: skip home tile, only Tools tile.** Override in step 5 if user wants both.

- [ ] **Step 1: Add `setStocksAppId` to AppSettings header**

In `app/apps/app_settings/app_settings.h`, in the public section, add:

```cpp
void setStocksAppId(int id) { _stocks_id = id; }
int  getStocksAppId() const { return _stocks_id; }
void openStocks();
```

And in the private section, add:

```cpp
int _stocks_id {0};
```

Also add a forward declaration of the icon (near the other `extern const lv_image_dsc_t ...` lines):

```cpp
extern const lv_image_dsc_t stocks_icon;  // 130x130
```

- [ ] **Step 2: Add `openStocks()` and `_toolStocks_cb` to AppSettings .cpp**

In `app/apps/app_settings/app_settings.cpp`, find the existing `_toolLoraChat_cb` (added in commit cfb51a8) and the `_openFx()` pattern. Add a new callback right after `_toolLoraChat_cb`:

```cpp
void AppSettings::_toolStocks_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    self->openStocks();
}
```

Then add the `openStocks` definition near the other `openXxx` methods (search for `openEmailPage`):

```cpp
void AppSettings::openStocks()
{
    if (_stocks_id > 0) {
        mooncake::GetMooncake().openApp(_stocks_id);
    }
}
```

- [ ] **Step 3: Add a Tools tile in `_buildToolsPage`**

In `_buildToolsPage`, after the existing LoRa Chat tile (the second row, x=450), add a third tile on row 3 column 1, mirroring the 邮件 tile layout (row 3, y=480) but with a different x and the new icon. Find the existing 邮件 tile creation (search for `_toolMail_cb`) and add a stock tile right next to it on the same row:

```cpp
// 第 3 行第 1 列: 自选股 (走 hermes :8766/api/stocks/portfolio, mx-selfselect 后端)
{
    const int x = 35, y_pos = 480;
    lv_obj_t* tile = lv_obj_create(_tools_page);
    lv_obj_set_size(tile, 380, 140);
    lv_obj_align(tile, LV_ALIGN_TOP_LEFT, x, y_pos);
    lv_obj_set_style_bg_color(tile, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(tile, 20, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_shadow_width(tile, 18, 0);
    lv_obj_set_style_shadow_color(tile, lv_color_hex(0x5A7A9C), 0);
    lv_obj_set_style_shadow_opa(tile, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(tile, 4, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(C_CARD_PR), LV_STATE_PRESSED);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, _toolStocks_cb, LV_EVENT_CLICKED, this);

    lv_obj_t* icon = lv_image_create(tile);
    lv_image_set_src(icon, &stocks_icon);
    lv_image_set_scale(icon, 256);  // 130x130 native, 1:1
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* name = lv_label_create(tile);
    lv_label_set_text(name, "自  选");
    lv_obj_set_style_text_font(name, zh_font_30(), 0);
    lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 12 + 130 + 16, 0);
}
```

Adjust `x` if the existing 邮件 tile is at a different x position. Confirm by reading the surrounding lines in `_buildToolsPage` first.

- [ ] **Step 4: Wire it all up in `app_installer.h`**

In `app/apps/app_installer.h`:

1. Add include after the `app_unit_puzzle.h` line:
   ```cpp
   #include "app_stocks/app_stocks.h"
   ```

2. Inside `on_install_apps`, after the `auto up_uptr = ...` block, add:
   ```cpp
   auto st_uptr = std::make_unique<AppStocks>();
   AppStocks* stocks = st_uptr.get();
   ```
   After the `int up_id = mooncake::GetMooncake().installApp(std::move(up_uptr));` line, add:
   ```cpp
   int st_id = mooncake::GetMooncake().installApp(std::move(st_uptr));
   ```

3. In the close-callback wiring section, after the `unit_puzzle->setCloseCallback(...)` block, add:
   ```cpp
   stocks->setCloseCallback([home]() { home->restoreScreen(); });
   settings->setStocksAppId(st_id);
   ```

4. **(Optional home tile — only if user opts in.)** If the user wants the home tile too, add this to the `home->addApp(...)` block:
   ```cpp
   home->addApp("自选股", st_id);
   ```
   This goes after the existing 4 entries. If home currently uses a 2×3 grid, you may need to also adjust the home screen's `addApp`/layout code — search for `addApp` in `app/apps/app_home/app_home.cpp` and check the max count. If it caps at 4, the change requires either a grid refactor or just leaving it off the home and keeping it Tools-only.

- [ ] **Step 5: Confirm with user about home tile**

Before step 6, send a one-line message:
> "Tools tile done. Add 自选股 to home screen too (would make 5 entries in current 2x3 grid, needs layout tweak) or Tools-only?"

Wait for user answer. If "Tools-only", skip the `home->addApp("自选股", st_id)` line. If "Add home tile", proceed with grid refactor in `app_home`.

- [ ] **Step 6: Build and confirm it links**

```bash
source ~/.local/bin/idf_env.sh
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo/platforms/tab5
idf.py build 2>&1 | tail -10
```
Expected: `Project build complete.` and no linker errors about `AppStocks` symbols.

If linker says "undefined reference to AppStocks::AppStocks()", the .cpp didn't get globbed. Check `app/CMakeLists.txt` for `file(GLOB_RECURSE ...)` and confirm `app_stocks/*.cpp` matches.

- [ ] **Step 7: Commit**

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
git add app/apps/app_installer.h app/apps/app_settings/ app/apps/app_home/
git -c user.name="leenzhou" -c user.email="leenzhou@users.noreply.github.com" \
    commit -m "feat(stocks): 注册 AppStocks + 工具页自选股 tile"
```

---

### Task 4: 7-column table UI + detail modal + status bar

**Files:**
- Modify: `app/apps/app_stocks/app_stocks.cpp`
- Modify: `app/apps/app_stocks/app_stocks.h` (no change needed if all fields are already there)

**Interfaces (filled):**
- `_buildUi()` builds screen, header, column header, 10 rows, status bar
- `_destroyUi()` deletes all LVGL objects
- `_setCellText(row_obj, col_idx, text, color)` updates a single cell
- `_setStatus(online, text)` updates the status bar
- `_showDetail(row_idx)` opens the modal
- `_closeDetail()` closes the modal
- `_refreshUiFromItems()` redraws the table from `_items`

- [ ] **Step 1: Replace the empty `_buildUi` body with the real implementation**

In `app_stocks.cpp`, replace `void AppStocks::_buildUi() {}` with:

```cpp
void AppStocks::_buildUi()
{
    auto* hal = GetHAL();
    auto lock = hal->lvglLock();

    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header ────────────────────────────────────────────────────
    _header = lv_obj_create(_scr);
    lv_obj_set_size(_header, SCREEN_W, HEADER_H);
    lv_obj_align(_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_header, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_border_width(_header, 0, 0);
    lv_obj_set_style_pad_all(_header, 0, 0);
    lv_obj_clear_flag(_header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_label_create(_header);
    lv_label_set_text(back, "←");
    lv_obj_set_style_text_font(back, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(back, lv_color_hex(C_TEXT), 0);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, [](lv_event_t* e) {
        auto* self = static_cast<AppStocks*>(lv_event_get_user_data(e));
        if (self->_close_cb) self->_close_cb();
        mooncake::GetMooncake().closeApp("app_stocks");
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* title = lv_label_create(_header);
    lv_label_set_text(title, "自选股");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);  // 30 px
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 80, 0);

    // status text (right side, e.g. "● 离线  10只")
    _status_text = lv_label_create(_header);
    lv_label_set_text(_status_text, "● 加载中...");
    lv_obj_set_style_text_font(_status_text, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(_status_text, lv_color_hex(C_DIM), 0);
    lv_obj_align(_status_text, LV_ALIGN_RIGHT_MID, -20, 0);

    // ── Column header ─────────────────────────────────────────────
    static const char* COLS[] = {"代码", "名称", "现价", "涨跌幅", "涨跌额", "换手率", "量比"};
    static const int   COL_X[] = {30, 150, 370, 510, 670, 830, 970};
    _col_header = lv_obj_create(_scr);
    lv_obj_set_size(_col_header, SCREEN_W, 36);
    lv_obj_align(_col_header, LV_ALIGN_TOP_MID, 0, HEADER_H + 4);
    lv_obj_set_style_bg_color(_col_header, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_bg_opa(_col_header, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_col_header, 0, 0);
    lv_obj_set_style_pad_all(_col_header, 0, 0);
    lv_obj_clear_flag(_col_header, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 7; i++) {
        lv_obj_t* lbl = lv_label_create(_col_header);
        lv_label_set_text(lbl, COLS[i]);
        lv_obj_set_style_text_font(lbl, zh_font_sm(), 0);  // 20 px
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, COL_X[i], 0);
    }

    // ── Data rows ─────────────────────────────────────────────────
    const int rows_top = HEADER_H + 4 + 36 + 4;
    for (int i = 0; i < N_ROWS; i++) {
        _rows[i] = lv_obj_create(_scr);
        lv_obj_set_size(_rows[i], SCREEN_W, ROW_H);
        lv_obj_align(_rows[i], LV_ALIGN_TOP_MID, 0, rows_top + i * ROW_H);
        lv_obj_set_style_bg_color(_rows[i], lv_color_hex(C_HEADER), i % 2 ? LV_OPA_30 : LV_OPA_10);
        lv_obj_set_style_bg_opa(_rows[i], LV_OPA_30, 0);
        lv_obj_set_style_border_width(_rows[i], 0, 0);
        lv_obj_set_style_pad_all(_rows[i], 0, 0);
        lv_obj_clear_flag(_rows[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_rows[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(_rows[i], [](lv_event_t* e) {
            auto* self = static_cast<AppStocks*>(lv_event_get_user_data(e));
            int row = (int)(intptr_t)lv_event_get_code(e) - LV_EVENT_CLICKED;  // hack: use code as row idx
            // Real impl: store row idx via lv_obj_set_user_data(_rows[i], (void*)(intptr_t)i) and read here.
        }, LV_EVENT_CLICKED, this);
        // Init empty cells (7 columns)
        for (int c = 0; c < 7; c++) {
            lv_obj_t* cell = lv_label_create(_rows[i]);
            lv_label_set_text(cell, "--");
            lv_obj_set_style_text_font(cell, zh_font_sm(), 0);
            lv_obj_set_style_text_color(cell, lv_color_hex(C_DIM), 0);
            lv_obj_align(cell, LV_ALIGN_LEFT_MID, COL_X[c], 0);
        }
    }
    // Fix the row click handler to actually capture row index. Replace above with:
    //   lv_obj_set_user_data(_rows[i], (void*)(intptr_t)i);
    //   lv_obj_add_event_cb(_rows[i], _row_click_cb, LV_EVENT_CLICKED, this);
    // where _row_click_cb reads lv_obj_get_user_data(target) → self->_showDetail(idx);
    // (Full impl in next step.)

    // ── Status bar ────────────────────────────────────────────────
    _status_bar = lv_obj_create(_scr);
    lv_obj_set_size(_status_bar, SCREEN_W, STATUS_H);
    lv_obj_align(_status_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_status_bar, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_border_width(_status_bar, 0, 0);
    lv_obj_set_style_pad_all(_status_bar, 0, 0);
    lv_obj_clear_flag(_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    _status_dot = lv_label_create(_status_bar);
    lv_label_set_text(_status_dot, "● LED: --");
    lv_obj_set_style_text_font(_status_dot, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(_status_dot, lv_color_hex(C_DIM), 0);
    lv_obj_align(_status_dot, LV_ALIGN_LEFT_MID, 20, 0);

    _refresh_btn = lv_btn_create(_status_bar);
    lv_obj_set_size(_refresh_btn, 140, 40);
    lv_obj_align(_refresh_btn, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_bg_color(_refresh_btn, lv_color_hex(C_ACCENT), 0);
    lv_obj_t* rlbl = lv_label_create(_refresh_btn);
    lv_label_set_text(rlbl, "刷新");
    lv_obj_set_style_text_font(rlbl, zh_font_sm(), 0);
    lv_obj_set_style_text_color(rlbl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(rlbl);
    lv_obj_add_event_cb(_refresh_btn, [](lv_event_t* e) {
        auto* self = static_cast<AppStocks*>(lv_event_get_user_data(e));
        self->_fetchStocksAsync();
    }, LV_EVENT_CLICKED, this);

    lv_scr_load(_scr);
}
```

Note: the row click handler above is a placeholder. The next step adds the real `_row_click_cb` static method that captures the row index via `lv_obj_get_user_data`.

Also: `zh_font_lg()` and `zh_font_sm()` are existing helpers in `app/apps/app_ha/view/view.cpp`. To use them in `app_stocks.cpp`, copy-paste the two static helper functions at the top of `app_stocks.cpp`:

```cpp
// Match app_ha/view/view.cpp: full-coverage cbin font wrappers, fall back to subset on desktop.
#ifndef PLATFORM_BUILD_DESKTOP
extern "C" const uint8_t font_puhui_common_30_4_bin_start[] asm("_binary_font_puhui_common_30_4_bin_start");
extern "C" const uint8_t font_puhui_common_20_4_bin_start[] asm("_binary_font_puhui_common_20_4_bin_start");
#include <cbin_font.h>
static const lv_font_t* zh_font_lg() {
    static const lv_font_t* f = nullptr;
    if (!f) f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
static const lv_font_t* zh_font_sm() {
    static const lv_font_t* f = nullptr;
    if (!f) f = cbin_font_create((uint8_t*)font_puhui_common_20_4_bin_start);
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_sm() { return &font_puhui_20_4; }
#endif
```

- [ ] **Step 2: Add row click handler that captures the index**

Replace the placeholder `lv_obj_add_event_cb` for rows in step 1 with:

```cpp
lv_obj_set_user_data(_rows[i], (void*)(intptr_t)i);
lv_obj_add_event_cb(_rows[i], [](lv_event_t* e) {
    auto* self = static_cast<AppStocks*>(lv_event_get_user_data(e));
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    self->_showDetail(idx);
}, LV_EVENT_CLICKED, this);
```

Also add a static declaration of `_row_click_cb` in the header (private section):

```cpp
static void _row_click_cb(lv_event_t* e);
```

(Or inline the lambda as above — both work; the lambda captures `this` via user_data.)

- [ ] **Step 3: Implement `_destroyUi`**

```cpp
void AppStocks::_destroyUi()
{
    auto* hal = GetHAL();
    auto lock = hal->lvglLock();
    _closeDetail();
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    for (auto& r : _rows) r = nullptr;
    _header = _status_bar = _status_dot = _status_text = nullptr;
    _refresh_btn = _col_header = nullptr;
    if (_poll_timer) { lv_timer_delete(_poll_timer); _poll_timer = nullptr; }
}
```

- [ ] **Step 4: Implement `_setCellText`, `_setStatus`, `_showDetail`, `_closeDetail`**

```cpp
void AppStocks::_setCellText(lv_obj_t* row, int col, const char* text, uint32_t color)
{
    // row's children are 7 labels in order; col is 0..6
    uint32_t child_cnt = lv_obj_get_child_cnt(row);
    if ((uint32_t)col >= child_cnt) return;
    lv_obj_t* cell = lv_obj_get_child(row, col);
    lv_label_set_text(cell, text);
    lv_obj_set_style_text_color(cell, lv_color_hex(color), 0);
}

void AppStocks::_setStatus(bool online, const char* text)
{
    if (!_status_text) return;
    lv_label_set_text(_status_text, online ? "● 在线" : "● 离线");
    lv_obj_set_style_text_color(_status_text,
        lv_color_hex(online ? C_UP : C_DOWN), 0);
    if (text) {
        // append count/timestamp to the right of the dot
        std::string combined = std::string(online ? "● 在线  " : "● 离线  ") + text;
        lv_label_set_text(_status_text, combined.c_str());
    }
}

void AppStocks::_showDetail(int row_idx)
{
    if (row_idx < 0 || row_idx >= (int)_items.size()) return;
    const auto& s = _items[row_idx];
    auto* hal = GetHAL();
    auto lock = hal->lvglLock();

    _closeDetail();  // ensure no duplicate

    _detail_modal = lv_obj_create(_scr);
    lv_obj_set_size(_detail_modal, 600, 400);
    lv_obj_center(_detail_modal);
    lv_obj_set_style_bg_color(_detail_modal, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_color(_detail_modal, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(_detail_modal, 2, 0);
    lv_obj_set_style_radius(_detail_modal, 12, 0);
    lv_obj_set_style_pad_all(_detail_modal, 20, 0);

    char buf[64];
    int y = 0;
    auto add_line = [&](const char* label, const std::string& val, uint32_t color = C_TEXT) {
        lv_obj_t* l = lv_label_create(_detail_modal);
        lv_label_set_text_fmt(l, "%s  %s", label, val.c_str());
        lv_obj_set_style_text_font(l, zh_font_sm(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y);
        y += 36;
    };
    add_line("代码", s.code);
    add_line("名称", s.name);
    char price_buf[32]; snprintf(price_buf, sizeof(price_buf), "%.2f", s.price);
    add_line("现价", price_buf);
    char chg_buf[32]; snprintf(chg_buf, sizeof(chg_buf), "%+.2f%%", s.chg);
    add_line("涨跌幅", chg_buf, _chgColor(s.chg));
    char pchg_buf[32]; snprintf(pchg_buf, sizeof(pchg_buf), "%+.2f", s.pchg);
    add_line("涨跌额", pchg_buf, _chgColor(s.chg));
    char tr_buf[32]; snprintf(tr_buf, sizeof(tr_buf), "%.2f%%", s.turnover);
    add_line("换手率", tr_buf);
    char lb_buf[32]; snprintf(lb_buf, sizeof(lb_buf), "%.2f", s.liangbi);
    add_line("量比", lb_buf);

    // 关闭按钮
    lv_obj_t* close_btn = lv_btn_create(_detail_modal);
    lv_obj_set_size(close_btn, 100, 40);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(C_ACCENT), 0);
    lv_obj_t* cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "关闭");
    lv_obj_set_style_text_font(cl, zh_font_sm(), 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(close_btn, [](lv_event_t* e) {
        auto* self = static_cast<AppStocks*>(lv_event_get_user_data(e));
        self->_closeDetail();
    }, LV_EVENT_CLICKED, this);
}

void AppStocks::_closeDetail()
{
    if (_detail_modal) {
        lv_obj_delete(_detail_modal);
        _detail_modal = nullptr;
    }
}
```

Note: `_chgColor` is defined only under `#ifndef PLATFORM_BUILD_DESKTOP` in the header. For desktop, you need a stub:

```cpp
// Outside the #ifndef PLATFORM_BUILD_DESKTOP block, near the other stubs:
uint32_t AppStocks::_chgColor(float chg) {
    if (chg > 0.01f)  return C_UP;
    if (chg < -0.01f) return C_DOWN;
    return C_FLAT;
}
```

Move the existing `#ifndef PLATFORM_BUILD_DESKTOP`-guarded `_chgColor` definition out of that block (or duplicate it outside, since it's pure data).

- [ ] **Step 5: Implement `_refreshUiFromItems`**

```cpp
void AppStocks::_refreshUiFromItems()
{
    if (!_scr) return;
    std::vector<StockItem> snapshot;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        snapshot = _items;
    }
    for (int i = 0; i < N_ROWS; i++) {
        if (i < (int)snapshot.size()) {
            const auto& s = snapshot[i];
            char buf[32];
            _setCellText(_rows[i], 0, s.code.c_str(), C_TEXT);
            _setCellText(_rows[i], 1, s.name.c_str(), C_TEXT);
            snprintf(buf, sizeof(buf), "%.2f", s.price);
            _setCellText(_rows[i], 2, buf, C_TEXT);
            snprintf(buf, sizeof(buf), "%+.2f%%", s.chg);
            _setCellText(_rows[i], 3, buf, _chgColor(s.chg));
            snprintf(buf, sizeof(buf), "%+.2f", s.pchg);
            _setCellText(_rows[i], 4, buf, _chgColor(s.chg));
            snprintf(buf, sizeof(buf), "%.2f%%", s.turnover);
            _setCellText(_rows[i], 5, buf, C_DIM);
            snprintf(buf, sizeof(buf), "%.2f", s.liangbi);
            _setCellText(_rows[i], 6, buf, C_DIM);
        } else {
            for (int c = 0; c < 7; c++) _setCellText(_rows[i], c, "--", C_DIM);
        }
    }
    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "%lu 只", (unsigned long)snapshot.size());
    _setStatus(!_fetch_error, count_buf);
}
```

- [ ] **Step 6: Build and run desktop sim**

```bash
source ~/.local/bin/idf_env.sh
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo/platforms/tab5
idf.py build 2>&1 | tail -10
```
Expected: build clean (warnings about unused methods in this task are fine; they get used in Task 5/6).

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo/platforms/desktop/build
cmake .. 2>&1 | tail -3
make -j 2>&1 | tail -10
```
Expected: builds, runs the simulator, shows the 自选股 screen with "--" rows (no data yet).

- [ ] **Step 7: Commit**

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
git add app/apps/app_stocks/
git -c user.name="leenzhou" -c user.email="leenzhou@users.noreply.github.com" \
    commit -m "feat(stocks): 7 列表 UI + 详情卡 + 状态条"
```

---

### Task 5: Data fetch + JSON parse + 30s polling

**Files:**
- Modify: `app/apps/app_stocks/app_stocks.cpp`
- Modify: `app/apps/app_stocks/app_stocks.h` (add `tryRunDetached` include)

**Interfaces (filled):**
- `_fetchStocksAsync()` schedules a worker
- `_doFetch()` runs the HTTP GET + JSON parse on a worker thread
- `_parseStocksJson(body)` mutates `_items` under `_items_mu`
- `onRunning()` triggers fetch on first call + every 30s after

- [ ] **Step 1: Implement `_doFetch` and `_parseStocksJson`**

In `app_stocks.cpp`, replace the empty stubs with:

```cpp
void AppStocks::_fetchStocksAsync()
{
    if (GetHAL()->tryRunDetached([this]() { _doFetch(); }, "stocks-fetch")) {
        mclog::tagInfo(TAG, "fetch scheduled");
    } else {
        mclog::tagWarn(TAG, "fetch worker spawn failed, will retry next tick");
    }
}

void AppStocks::_doFetch()
{
    auto* hal = GetHAL();
    std::string svc_host = hal->getConfig().svc_host;  // NVS-backed
    if (svc_host.empty()) svc_host = "192.168.1.142";
    std::string url = "http://" + svc_host + ":8766/api/stocks/portfolio";

    std::string body;
    bool ok = hal->httpGet(url, body, FETCH_TIMEOUT_MS);
    if (!ok) {
        {
            std::lock_guard<std::mutex> lk(_items_mu);
            _fetch_error = true;
            _fetch_error_msg = "HTTP failed";
        }
        mclog::tagWarn(TAG, "fetch failed: %s", _fetch_error_msg.c_str());
        auto lock = hal->lvglLock();
        _setStatus(false, nullptr);
        return;
    }
    _parseStocksJson(body);
}

void AppStocks::_parseStocksJson(const std::string& body)
{
    auto* hal = GetHAL();
    auto lock = hal->lvglLock();  // JSON parse in LVGL lock is overkill; could drop
    try {
        auto j = nlohmann::json::parse(body);
        std::vector<StockItem> parsed;
        if (j.contains("items") && j["items"].is_array()) {
            for (const auto& it : j["items"]) {
                StockItem s;
                s.code     = it.value("code", "");
                s.name     = it.value("name", "");
                s.price    = it.value("price", 0.0f);
                s.chg      = it.value("chg", 0.0f);
                s.pchg     = it.value("pchg", 0.0f);
                s.turnover = it.value("turnover", 0.0f);
                s.liangbi  = it.value("liangbi", 0.0f);
                if (!s.code.empty()) parsed.push_back(std::move(s));
            }
        }
        {
            std::lock_guard<std::mutex> lk(_items_mu);
            _items = std::move(parsed);
            _fetch_error = false;
            _last_fetch_ms = (int64_t)(esp_timer_get_time() / 1000);
        }
        mclog::tagInfo(TAG, "fetched %zu items", _items.size());
        _refreshUiFromItems();
#ifndef PLATFORM_BUILD_DESKTOP
        if (_items.size() > 0) _startTicker();  // Task 6
#endif
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(_items_mu);
        _fetch_error = true;
        _fetch_error_msg = e.what();
        mclog::tagWarn(TAG, "parse failed: %s", e.what());
        _setStatus(false, "解析失败");
    }
}
```

Note: `esp_timer_get_time()` is ESP-only. Wrap in `#ifndef PLATFORM_BUILD_DESKTOP`, use `std::chrono::steady_clock::now()` on desktop:

```cpp
#ifdef PLATFORM_BUILD_DESKTOP
    _last_fetch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#else
    _last_fetch_ms = (int64_t)(esp_timer_get_time() / 1000);
#endif
```

- [ ] **Step 2: Implement `onOpen` to schedule the first fetch**

Replace the current `onOpen` body (which only calls `_buildUi()`) with:

```cpp
void AppStocks::onOpen()
{
    mclog::tagInfo(TAG, "opened");
    _buildUi();
    _fetchStocksAsync();  // initial fetch
}
```

- [ ] **Step 3: Implement `onRunning` with 30s polling**

Replace `onRunning` body with:

```cpp
void AppStocks::onRunning()
{
    int64_t now_ms;
#ifdef PLATFORM_BUILD_DESKTOP
    now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#else
    now_ms = (int64_t)(esp_timer_get_time() / 1000);
#endif
    int64_t last;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        last = _last_fetch_ms;
    }
    if (last == 0 || (now_ms - last) > FETCH_INTERVAL_MS) {
        _fetchStocksAsync();
    }
}
```

- [ ] **Step 4: Add `httpGet` declaration in HAL or fallback**

The `hal->httpGet(url, body, timeout_ms)` signature must exist. Check `app/hal/hal.h` for the existing `httpGet` signature.

In `app/hal/hal.h` (the abstract class), the existing signature is:

```cpp
virtual bool httpGet(const std::string& url, std::string& body, int timeout_ms = 5000) = 0;
```

If the timeout parameter is missing, add the third parameter to the abstract and the two implementations (`hal_esp32.cpp`, `hal_desktop.cpp`).

In `platforms/tab5/main/hal/hal_esp32.cpp` (the ESP impl) and `platforms/desktop/hal/hal_desktop.cpp` (the desktop impl), confirm the timeout is forwarded to the underlying HTTP call. (Recent memory note: hal_http.cpp already has 30s timeouts for email endpoint via a route; the default httpGet timeout is 10s. Update the spec to use 5s; if the ESP impl caps at 10s, that's fine — 5s just means the call returns earlier.)

- [ ] **Step 5: Build**

```bash
source ~/.local/bin/idf_env.sh
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo/platforms/tab5
idf.py build 2>&1 | tail -10
```
Expected: `Project build complete.`

If the build fails with "nlohmann/json.hpp not found", include it the same way `app_ha.cpp` does:

```cpp
#include <nlohmann/json.hpp>
```

(The header lives in `components/nlohmann_json` which is a managed component of the project; `app_ha.cpp` already pulls it.)

- [ ] **Step 6: Commit**

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
git add app/apps/app_stocks/ app/hal/hal.h platforms/tab5/main/hal/hal_esp32.cpp platforms/desktop/hal/hal_desktop.cpp
git -c user.name="leenzhou" -c user.email="leenzhou@users.noreply.github.com" \
    commit -m "feat(stocks): data fetch + JSON 解析 + 30s 轮询"
```

---

### Task 6: LED strip bring-up, teardown, and `setPortAOwnedByApp` coordination

**Files:**
- Modify: `app/apps/app_stocks/app_stocks.cpp`

**Interfaces (filled):**
- `_stripInit()` returns `ESP_OK` on success, leaves `_strip = nullptr` on failure
- `_stripDeinit()` clears the strip, deletes the handle, sets `_strip = nullptr`
- `onOpen` calls `AppEmailLed::setPortAOwnedByApp(true)` first, then `_stripInit()`
- `onClose` calls `_stripDeinit()` first, then `AppEmailLed::setPortAOwnedByApp(false)`

- [ ] **Step 1: Implement `_stripInit`**

In `app_stocks.cpp`, replace the empty `_stripInit` body with:

```cpp
#ifndef PLATFORM_BUILD_DESKTOP
esp_err_t AppStocks::_stripInit()
{
    std::lock_guard<std::mutex> lk(_strip_mu);
    if (_strip) return ESP_OK;

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num         = LED_DIN_GPIO;
    strip_cfg.max_leds               = LED_N;
    strip_cfg.led_model              = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz     = 10 * 1000 * 1000;
    rmt_cfg.mem_block_symbols = 96;

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_strip init failed (will retry): %s", esp_err_to_name(err));
        _strip = nullptr;
        return err;
    }
    led_strip_clear(_strip);
    return ESP_OK;
}
#endif
```

- [ ] **Step 2: Implement `_stripDeinit` and `_setPixelXY`**

```cpp
#ifndef PLATFORM_BUILD_DESKTOP
void AppStocks::_stripDeinit()
{
    std::lock_guard<std::mutex> lk(_strip_mu);
    if (_strip) {
        led_strip_clear(_strip);
        led_strip_refresh(_strip);
        led_strip_del(_strip);
        _strip = nullptr;
    }
}

void AppStocks::_setPixelXY(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (!_strip || x < 0 || x >= LED_W || y < 0 || y >= LED_H) return;
    led_strip_set_pixel(_strip, _xy2i(x, y), r, g, b);
}
#endif
```

- [ ] **Step 3: Wire LED into `onOpen` and `onClose`**

Replace `onOpen` and `onClose` in the .cpp with:

```cpp
void AppStocks::onOpen()
{
    mclog::tagInfo(TAG, "opened");
#ifndef PLATFORM_BUILD_DESKTOP
    AppEmailLed::setPortAOwnedByApp(true);  // release email_led's strip first
    if (_stripInit() != ESP_OK) {
        mclog::tagWarn(TAG, "led init failed, app continues without ticker");
    }
#endif
    _buildUi();
    _fetchStocksAsync();
}

void AppStocks::onClose()
{
    mclog::tagInfo(TAG, "closed");
#ifndef PLATFORM_BUILD_DESKTOP
    _stopTicker();  // Task 7
    _stripDeinit();
    AppEmailLed::setPortAOwnedByApp(false);  // give LED back to email_led
#endif
    _destroyUi();
}
```

If Task 7 is not yet implemented, `_stopTicker()` is still a no-op stub. That's fine.

- [ ] **Step 4: Build**

```bash
source ~/.local/bin/idf_env.sh
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo/platforms/tab5
idf.py build 2>&1 | tail -10
```
Expected: `Project build complete.`

- [ ] **Step 5: Commit**

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
git add app/apps/app_stocks/
git -c user.name="leenzhou" -c user.email="leenzhou@users.noreply.github.com" \
    commit -m "feat(stocks): LED 初始化 + setPortAOwnedByApp 协调"
```

---

### Task 7: LED ticker thread + 2-segment rendering

**Files:**
- Modify: `app/apps/app_stocks/app_stocks.cpp`

**Interfaces (filled):**
- `_startTicker()` creates FreeRTOS task at priority 1, 8 KB stack
- `_stopTicker()` signals stop, waits up to 200 ms for the task to finish, then deletes
- `_tickerTask()` body: per-frame rendering for current stock + segment
- `_renderTickerFrame(now_ms)` updates the global state machine and clears+redraws
- `_renderStockSegment(text, len, now_ms, r, g, b)` static-center + scroll-left for one segment
- Color rules: code + price white, CHG green if > 0, red if < 0, gray if = 0

- [ ] **Step 1: Implement `_tickerTask` and `_renderTickerFrame`**

```cpp
#ifndef PLATFORM_BUILD_DESKTOP
void AppStocks::_tickerTask(void* arg)
{
    auto* self = static_cast<AppStocks*>(arg);
    int64_t last_frame_us = 0;
    while (self->_ticker_running.load()) {
        int64_t now_us = esp_timer_get_time();
        if (last_frame_us != 0 && now_us - last_frame_us < 33 * 1000) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        last_frame_us = now_us;
        self->_renderTickerFrame(now_us / 1000);
    }
    self->_ticker_done.store(true);
    vTaskDelete(nullptr);
}

void AppStocks::_renderTickerFrame(int64_t now_ms)
{
    if (_items.empty() || !_strip) return;
    StockItem s;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        s = _items[_ticker_index % _items.size()];
    }
    // Segment state machine
    int64_t seg_t = now_ms - _ticker_seg_start_ms;
    int seg_dur;
    const char* text;
    int text_len;
    uint8_t r, g, b;
    // Segment 1: code + CHG
    snprintf(_ticker_buf, sizeof(_ticker_buf), "%s %+.2f%%",
             s.code.c_str(), s.chg);
    text = _ticker_buf;
    text_len = strlen(text);
    uint32_t col = _chgColor(s.chg);
    r = (col >> 16) & 0xFF; g = (col >> 8) & 0xFF; b = col & 0xFF;
    if (s.chg >= -0.01f && s.chg <= 0.01f) {  // use white for the code part if flat
        r = 0xEA; g = 0xF3; b = 0xFB;
    }
    seg_dur = STOCK_SEG1_MS;

    if (seg_t >= seg_dur) {
        // Move to seg 2 or advance stock
        if (_ticker_seg_state == 0) {
            _ticker_seg_state = 1;
            _ticker_seg_start_ms = now_ms;
            snprintf(_ticker_buf, sizeof(_ticker_buf), "%s %.2f", s.code.c_str(), s.price);
            text_len = strlen(text);
        } else {
            // advance to next stock after pause
            if (seg_t >= seg_dur + STOCK_PAUSE_MS) {
                _ticker_index = (_ticker_index + 1) % _items.size();
                _ticker_seg_state = 0;
                _ticker_seg_start_ms = now_ms;
            }
            // clear during pause
            {
                std::lock_guard<std::mutex> lk(_strip_mu);
                led_strip_clear(_strip);
                led_strip_refresh(_strip);
            }
            return;
        }
    }

    // Render this frame
    {
        std::lock_guard<std::mutex> lk(_strip_mu);
        led_strip_clear(_strip);
        // static-center 0.5 s, then scroll left
        int total_w = text_len * CELL - CHAR_GAP;
        int scroll_px;
        if (seg_t < 500) {
            scroll_px = 0;  // static-center
        } else {
            int scroll_t = (int)((seg_t - 500) / SCROLL_MS_PER_PX);
            scroll_px = scroll_t % (total_w + LED_W);
        }
        int x_start = (LED_W - total_w) / 2 - scroll_px;
        for (int ci = 0; ci < text_len; ci++) {
            int cx = x_start + ci * CELL;
            if (cx + CHAR_W <= 0) continue;
            if (cx >= LED_W) break;
            char c = text[ci];
            if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
            if (c < 0x20 || c > 0x7E) c = '?';
            // font5x7[95][7] is defined in app_unit_puzzle.cpp at file scope.
            // We can't include it directly (file-static), so we duplicate the lookup by
            // re-declaring the same 5x7 table here. See Task 7 step 2 for the table.
            const uint8_t* bmp = font5x7[c - 0x20];
            for (int col_idx = 0; col_idx < CHAR_W; col_idx++) {
                int x = cx + col_idx;
                if (x < 0 || x >= LED_W) continue;
                for (int row = 0; row < CHAR_H; row++) {
                    if (bmp[row] & (1 << (4 - col_idx))) {
                        _setPixelXY(x, row, r, g, b);
                    }
                }
            }
        }
        led_strip_refresh(_strip);
    }
}
#endif
```

Add `char _ticker_buf[32];` to the header private section.

- [ ] **Step 2: Add a local copy of `font5x7`**

Because `app_unit_puzzle.cpp`'s `font5x7` is file-static, `app_stocks.cpp` cannot link it directly. Two options:

**Option A (preferred):** move `font5x7` to a shared header. Create `app/apps/app_unit_puzzle/font5x7.h`:

```cpp
#pragma once
#include <cstdint>
extern const uint8_t font5x7[95][7];
```

Move the array declaration from `app_unit_puzzle.cpp` to `app_unit_puzzle/font5x7.c` (new file), and `extern` it in the header. Then `app_stocks.cpp` includes it.

Steps:

1. In `app/apps/app_unit_puzzle/app_unit_puzzle.cpp`, cut the `static const uint8_t font5x7[95][7] = { ... };` block (lines 37 through ~140, the 95-element table). Remove the `static` keyword.
2. Paste it into a new file `app/apps/app_unit_puzzle/font5x7.c`:
   ```cpp
   #include "font5x7.h"
   const uint8_t font5x7[95][7] = { /* same 95 entries */ };
   ```
3. In `app/apps/app_stocks/app_stocks.cpp`, add `#include "../app_unit_puzzle/font5x7.h"`.

**Option B (fallback):** duplicate the 95-row table inside `app_stocks.cpp` with a comment pointing to `app_unit_puzzle.cpp:font5x7`. This bloats the binary by ~665 bytes and creates two sources of truth. **Avoid.**

Take Option A.

- [ ] **Step 3: Implement `_renderStockSegment` and update header**

`_renderStockSegment` is unused above (we inlined it). Remove the declaration from the header. Keep the simpler in-frame render inside `_renderTickerFrame`.

- [ ] **Step 4: Implement `_startTicker` and `_stopTicker`**

```cpp
#ifndef PLATFORM_BUILD_DESKTOP
void AppStocks::_startTicker()
{
    if (_ticker_running.load()) return;
    if (!_strip) {
        if (_stripInit() != ESP_OK) return;
    }
    _ticker_index = 0;
    _ticker_seg_state = 0;
    _ticker_seg_start_ms = (int64_t)(esp_timer_get_time() / 1000);
    _ticker_done.store(false);
    _ticker_running.store(true);
    xTaskCreate(_tickerTask, "stocks-led", 8192, this, 1, &_ticker_task);
    mclog::tagInfo(TAG, "ticker started");
}

void AppStocks::_stopTicker()
{
    if (!_ticker_running.load()) return;
    _ticker_running.store(false);
    int waited = 0;
    while (!_ticker_done.load() && waited < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    if (!_ticker_done.load()) {
        mclog::tagWarn(TAG, "ticker did not stop in 200 ms, forcing delete");
        if (_ticker_task) vTaskDelete(_ticker_task);
    }
    _ticker_task = nullptr;
    if (_strip) {
        std::lock_guard<std::mutex> lk(_strip_mu);
        led_strip_clear(_strip);
        led_strip_refresh(_strip);
    }
    mclog::tagInfo(TAG, "ticker stopped");
}
#endif
```

- [ ] **Step 5: Build, fix any errors**

```bash
source ~/.local/bin/idf_env.sh
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo/platforms/tab5
idf.py build 2>&1 | tail -15
```
Expected: `Project build complete.`

If you get a "font5x7 not declared" error, the include in `app_stocks.cpp` is missing. If you get a multiple-definition error, the `static` keyword wasn't removed from the table in `app_unit_puzzle.cpp`.

- [ ] **Step 6: Commit**

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
git add app/apps/app_stocks/ app/apps/app_unit_puzzle/
git -c user.name="leenzhou" -c user.email="leenzhou@users.noreply.github.com" \
    commit -m "feat(stocks): LED ticker FreeRTOS 任务 + 2 段式滚动渲染

- 拆 font5x7 到 app_unit_puzzle/font5x7.{h,c} 共享
- ticker 8KB 栈 priority 1, 30 FPS 限流
- 每只股票 4.5s (seg1 2.5s + seg2 2.0s + 暂停 1s)
- CHG > 0 绿, < 0 红, = 0 灰"
```

---

### Task 8: Build, flash, smoke test

**Files:** none modified (verification only)

- [ ] **Step 1: idf.py build (final)**

```bash
source ~/.local/bin/idf_env.sh
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo/platforms/tab5
idf.py build 2>&1 | tail -10
```
Expected: `Project build complete.` and `m5stack_tab5.bin binary size 0x...` showing the new app. Confirm the binary still fits:

```bash
idf.py size
```
Expected: `app partition ... free` shows at least 5% free (currently 6% before this feature, so it should drop to ~3-4% with the new app).

- [ ] **Step 2: esptool flash**

```bash
PORT=$(ls /dev/cu.usbmodem* | head -1)
echo "Flashing to $PORT"
python -m esptool --chip esp32p4 -p "$PORT" -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/m5stack_tab5.bin \
  0xd74000 build/srmodels/srmodels.bin
```
Expected: 4 blocks written, all "Hash of data verified".

- [ ] **Step 3: Verify the device boots and the tile shows up**

After flash, the device reboots. Open a serial monitor:

```bash
python3 -c "
import serial, time
p = serial.Serial('$PORT', 115200, timeout=1)
p.setDTR(True); p.setRTS(False)
t = time.time() + 10
while time.time() < t:
    line = p.readline().decode('utf-8', 'replace').rstrip()
    if line and 'stocks' in line.lower():
        print(line)
"
```
Expected: log lines like:
```
I (xxx) stocks: created
I (xxx) stocks: opened
I (xxx) stocks: fetched N items
I (xxx) stocks: ticker started
```

- [ ] **Step 4: On-device manual smoke test**

On the Tab5:
1. Tap "工具" tile on home.
2. Tap "自选股" tile (row 3 column 1).
3. Verify: 7-column table appears with 10 rows, Chinese names visible, prices and CHG% displayed, color rules (涨 green, 跌 red, 平 gray).
4. Verify: 40×8 LED shows scrolling text cycling through stocks. First text: "300476 +2.78%" (or current first stock's code + CHG). Then: "300476 88.50" (code + price). Then black. Then next stock.
5. Tap any row → detail modal appears with 7 fields and a "关闭" button.
6. Tap "关闭" → modal disappears, table still shows.
7. Tap "刷新" button → status text briefly shows "● 加载中" then "● 在线 N只" with new data.
8. Tap "←" header or swipe up → app closes, LED goes off, email_led regains the LED (verify by sending yourself an unread test email — LED should scroll "NEW EMAIL" again).

- [ ] **Step 5: Error path smoke test**

1. Stop hermes (`pkill -f hermes` or via supervisor).
2. On Tab5, open 自选股.
3. Within 5s, status bar shows "● 离线", table rows are dimmed but visible (last cached data).
4. Restart hermes. Within 30s (next poll cycle), status goes back to "● 在线".

- [ ] **Step 6: Commit any post-test fixes**

If the smoke test revealed any bug fix (color, alignment, font size, polling cadence), commit it as a `fix(stocks):` commit. If the test passed cleanly, skip this step.

- [ ] **Step 7: Final summary**

Report to user:
- Build size delta: before vs after (e.g. `before 0xa4b9e0` → `after 0x...`).
- 10 rows render correctly: ✓ / ✗
- LED ticker cycles: ✓ / ✗
- App close returns LED to email_led: ✓ / ✗
- Network error fallback: ✓ / ✗
- Push commits: list of commit hashes.

---

## Self-Review (Run before handoff)

**Spec coverage**:
- Goal "new app, read-only, 7-col table, LED ticker, 30s poll" — Tasks 1, 2, 3, 4, 7 ✓
- Scope: "AppAbility only, no WorkerAbility, only on open" — Tasks 2 (skeleton), 3 (installer), 5 (LED init) ✓
- Architecture: "Tab5 → hermes :8766 → mx-selfselect" — Task 1 (hermes endpoint) ✓
- Data flow: 30s cache in hermes, 30s polling in app — Task 1 (cache), Task 5 (polling) ✓
- LED: 40×8, 2-segment, color rules — Task 7 ✓
- UI: 7-col table, detail modal, status bar, error states — Task 4 ✓
- Coordination: `setPortAOwnedByApp(bool)` — Task 6 ✓
- Validation: build + flash + smoke — Task 8 ✓

**Placeholder scan**: none. All steps have actual code, exact paths, exact commands.

**Type consistency**:
- `StockItem` defined in header (Task 2), used in `_parseStocksJson` and `_renderTickerFrame` (Tasks 5, 7) — names and types match.
- `font5x7[95][7]` is `extern const` in the new shared header (Task 7 step 2), included by `app_stocks.cpp` only.
- `_ticker_index`, `_ticker_seg_state`, `_ticker_seg_start_ms`, `_ticker_buf` all declared in header, used in `_renderTickerFrame` (Task 7).
- `_chgColor` is now defined unconditionally (not under `#ifndef PLATFORM_BUILD_DESKTOP`) so both sides compile.
