# 自选股交易时段刷新 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让自选股 App 每次打开时拉取一次，并且仅在北京时间 A 股交易时段、App 位于前台时每 30 秒自动拉取。

**Architecture:** 新增一个无状态、可独立测试的北京时间交易时段判断头文件；`AppStocks::onRunning()` 在现有前台判断后调用它，再决定是否进入 30 秒刷新逻辑。异步拉取入口使用原子标志保证同一时间最多存在一个请求，现有 Hermes 接口、缓存、UI 和 LED ticker 不变。

**Tech Stack:** C++17、Mooncake AppAbility、LVGL 9、ESP-IDF 5.5.2、Python `unittest` 源码契约测试。

## Global Constraints

- 时区固定为 UTC+8，不依赖设备当前时区配置。
- 交易日为周一至周五，交易区间为 `[09:30:00, 11:30:00)` 和 `[13:00:00, 15:00:00)`。
- Unix 时间早于 `2024-01-01 00:00:00 UTC` 时视为尚未校时，不自动刷新。
- `onOpen()` 和手动刷新始终允许发起请求；交易时段限制只作用于 `onRunning()` 的周期刷新。
- App 不在前台时不得拉取。
- 同一时间最多存在一个股票数据请求。
- 成功、HTTP 失败和 JSON 解析失败都继续更新 `_last_fetch_ms`，保留现有 30 秒失败退避行为。
- 不修改 Hermes 接口、服务端缓存、股票 UI、LED ticker 和用户现有 LoRa 未提交改动。
- 不提交 `platforms/tab5/sdkconfig`。

---

## File Structure

| File | Responsibility |
|---|---|
| `app/apps/app_stocks/stock_market_hours.h` | 将 Unix 时间转换为北京时间并判断 A 股交易时段。 |
| `test/test_stock_market_hours.cpp` | 覆盖工作日、午休、收盘、周末和未校时边界。 |
| `test/test_stocks_refresh_policy.py` | 固定 App 生命周期接线和原子防重入契约。 |
| `app/apps/app_stocks/app_stocks.h` | 保存 `_fetch_in_flight` 原子状态。 |
| `app/apps/app_stocks/app_stocks.cpp` | 把交易时段判断接入前台轮询，并为异步拉取加防重入和异常收尾。 |

### Task 1: 可测试的 A 股交易时段判断

**Files:**
- Create: `app/apps/app_stocks/stock_market_hours.h`
- Create: `test/test_stock_market_hours.cpp`

**Interfaces:**
- Consumes: Unix UTC 时间戳 `std::time_t`。
- Produces: `stocks_refresh::isAshareTradingTime(std::time_t utc_epoch) -> bool`。

- [ ] **Step 1: 写失败的边界测试**

创建 `test/test_stock_market_hours.cpp`：

```cpp
#include "app/apps/app_stocks/stock_market_hours.h"
#include <cassert>
#include <iostream>

int main()
{
    using stocks_refresh::isAshareTradingTime;

    assert(!isAshareTradingTime(0));           // 未校时
    assert(!isAshareTradingTime(1782091799));  // 周一 09:29:59 CST
    assert( isAshareTradingTime(1782091800));  // 周一 09:30:00 CST
    assert( isAshareTradingTime(1782098999));  // 周一 11:29:59 CST
    assert(!isAshareTradingTime(1782099000));  // 周一 11:30:00 CST
    assert( isAshareTradingTime(1782104400));  // 周一 13:00:00 CST
    assert( isAshareTradingTime(1782111599));  // 周一 14:59:59 CST
    assert(!isAshareTradingTime(1782111600));  // 周一 15:00:00 CST
    assert(!isAshareTradingTime(1782007200));  // 周日 10:00:00 CST

    std::cout << "stock market hours tests passed\n";
    return 0;
}
```

- [ ] **Step 2: 运行测试并确认失败**

Run:

```bash
c++ -std=c++17 -I. test/test_stock_market_hours.cpp -o /tmp/test_stock_market_hours
```

Expected: 编译失败，提示 `stock_market_hours.h` 不存在。

- [ ] **Step 3: 实现最小交易时段判断**

创建 `app/apps/app_stocks/stock_market_hours.h`：

```cpp
#pragma once

#include <ctime>

namespace stocks_refresh {

inline bool isAshareTradingTime(std::time_t utc_epoch)
{
    constexpr std::time_t MIN_VALID_EPOCH = 1704067200;  // 2024-01-01 UTC
    constexpr std::time_t UTC8_OFFSET_S = 8 * 60 * 60;
    if (utc_epoch < MIN_VALID_EPOCH) return false;

    std::time_t beijing_epoch = utc_epoch + UTC8_OFFSET_S;
    std::tm beijing = {};
    if (gmtime_r(&beijing_epoch, &beijing) == nullptr) return false;

    if (beijing.tm_wday == 0 || beijing.tm_wday == 6) return false;

    const int seconds = beijing.tm_hour * 3600
                      + beijing.tm_min * 60
                      + beijing.tm_sec;
    constexpr int MORNING_OPEN  = 9 * 3600 + 30 * 60;
    constexpr int MORNING_CLOSE = 11 * 3600 + 30 * 60;
    constexpr int AFTERNOON_OPEN  = 13 * 3600;
    constexpr int AFTERNOON_CLOSE = 15 * 3600;

    return (seconds >= MORNING_OPEN && seconds < MORNING_CLOSE)
        || (seconds >= AFTERNOON_OPEN && seconds < AFTERNOON_CLOSE);
}

}  // namespace stocks_refresh
```

- [ ] **Step 4: 运行测试并确认通过**

Run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror -I. \
  test/test_stock_market_hours.cpp -o /tmp/test_stock_market_hours \
  && /tmp/test_stock_market_hours
```

Expected: 输出 `stock market hours tests passed`，退出码为 0。

- [ ] **Step 5: 提交独立的时间策略**

```bash
git add app/apps/app_stocks/stock_market_hours.h test/test_stock_market_hours.cpp
git commit -m "feat(stocks): 增加A股交易时段判断"
```

### Task 2: 接入前台刷新并阻止并发请求

**Files:**
- Create: `test/test_stocks_refresh_policy.py`
- Modify: `app/apps/app_stocks/app_stocks.h:71-82`
- Modify: `app/apps/app_stocks/app_stocks.cpp:1-8,71-112,416-446`

**Interfaces:**
- Consumes: `stocks_refresh::isAshareTradingTime(std::time_t) -> bool`。
- Produces: `_fetchStocksAsync()` 的单请求保证；`onRunning()` 的交易时段门控。

- [ ] **Step 1: 写失败的生命周期契约测试**

创建 `test/test_stocks_refresh_policy.py`：

```python
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "app/apps/app_stocks/app_stocks.h"
SOURCE = ROOT / "app/apps/app_stocks/app_stocks.cpp"


class StocksRefreshPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = HEADER.read_text()
        cls.source = SOURCE.read_text()

    def test_open_always_requests_one_fetch(self):
        on_open = self.source.split("void AppStocks::onOpen()", 1)[1]
        on_open = on_open.split("void AppStocks::onRunning()", 1)[0]
        self.assertIn("_fetchStocksAsync();", on_open)

    def test_periodic_fetch_is_gated_by_market_hours(self):
        on_running = self.source.split("void AppStocks::onRunning()", 1)[1]
        on_running = on_running.split("void AppStocks::onClose()", 1)[0]
        self.assertIn("stocks_refresh::isAshareTradingTime(std::time(nullptr))", on_running)
        market_gate = on_running.index("stocks_refresh::isAshareTradingTime")
        interval_gate = on_running.index("FETCH_INTERVAL_MS")
        self.assertLess(market_gate, interval_gate)

    def test_async_fetch_has_atomic_in_flight_guard(self):
        self.assertRegex(
            self.header,
            r"std::atomic<bool>\s+_fetch_in_flight\s*\{false\};",
        )
        fetch = self.source.split("void AppStocks::_fetchStocksAsync()", 1)[1]
        fetch = fetch.split("void AppStocks::_doFetch()", 1)[0]
        self.assertIn("compare_exchange_strong", fetch)
        self.assertGreaterEqual(fetch.count("_fetch_in_flight.store(false)"), 2)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: 运行契约测试并确认失败**

Run:

```bash
python3 -m unittest test/test_stocks_refresh_policy.py -v
```

Expected: `test_open_always_requests_one_fetch` 通过，其余两个测试失败。

- [ ] **Step 3: 在 App 状态中增加请求防重入标志**

在 `app/apps/app_stocks/app_stocks.h` 的 `_app_open` 后加入：

```cpp
    // 同一时间只允许一个 HTTP 请求，避免 30 秒边界上重复创建 worker。
    std::atomic<bool>      _fetch_in_flight {false};
```

- [ ] **Step 4: 接入北京时间交易时段门控**

在 `app/apps/app_stocks/app_stocks.cpp` 顶部加入：

```cpp
#include "stock_market_hours.h"
#include <ctime>
```

将 `onRunning()` 中现有的周期刷新段替换为：

```cpp
    // App 打开时已经无条件拉取一次；周期刷新只在 A 股交易时段执行。
    if (!stocks_refresh::isAshareTradingTime(std::time(nullptr))) return;

    int64_t last;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        last = _last_fetch_ms;
    }
    if (last != 0 && (mono_ms() - last) >= FETCH_INTERVAL_MS) {
        _fetchStocksAsync();
    }
```

- [ ] **Step 5: 为异步入口增加原子占用和所有出口释放**

将 `_fetchStocksAsync()` 替换为：

```cpp
void AppStocks::_fetchStocksAsync()
{
    bool expected = false;
    if (!_fetch_in_flight.compare_exchange_strong(expected, true)) {
        mclog::tagInfo(TAG, "fetch already in flight");
        return;
    }

    // app 实例随 mooncake 常驻 (从不 uninstall), 捕获 this 安全。
    if (GetHAL()->tryRunDetached([this]() {
            try {
                _doFetch();
            } catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> lk(_items_mu);
                    _fetch_error = true;
                    _fetch_error_msg = e.what();
                    _last_fetch_ms = mono_ms();
                }
                mclog::tagWarn(TAG, "fetch exception: {}", e.what());
                _refreshUiFromItems();
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lk(_items_mu);
                    _fetch_error = true;
                    _fetch_error_msg = "unknown fetch exception";
                    _last_fetch_ms = mono_ms();
                }
                mclog::tagWarn(TAG, "unknown fetch exception");
                _refreshUiFromItems();
            }
            _fetch_in_flight.store(false);
        })) {
        mclog::tagInfo(TAG, "fetch scheduled");
    } else {
        _fetch_in_flight.store(false);
        mclog::tagWarn(TAG, "fetch worker spawn failed");
    }
}
```

- [ ] **Step 6: 运行新增测试和现有股票接口测试**

Run:

```bash
python3 -m unittest test/test_stocks_refresh_policy.py -v
c++ -std=c++17 -Wall -Wextra -Werror -I. \
  test/test_stock_market_hours.cpp -o /tmp/test_stock_market_hours \
  && /tmp/test_stock_market_hours
python3 test/test_stocks_endpoint.py
```

Expected: 契约测试 3 项全部通过；C++ 测试输出 `stock market hours tests passed`；接口测试输出 `All 4 tests passed.`。

- [ ] **Step 7: 验证桌面构建**

Run:

```bash
cmake --build platforms/desktop/build -j8
```

Expected: 构建退出码为 0，并生成 `platforms/desktop/build/app_desktop_build`。

- [ ] **Step 8: 提交 App 集成**

```bash
git add test/test_stocks_refresh_policy.py \
  app/apps/app_stocks/app_stocks.h \
  app/apps/app_stocks/app_stocks.cpp
git commit -m "fix(stocks): 仅在交易时段自动刷新"
```

### Task 3: ESP-IDF 构建、刷机和串口验收

**Files:**
- Verify only: `platforms/tab5/build_idf552/`

**Interfaces:**
- Consumes: Task 1 和 Task 2 的完整固件变更。
- Produces: ESP32-P4 构建、刷机和运行日志证据。

- [ ] **Step 1: 构建 ESP-IDF 固件**

Run:

```bash
cd platforms/tab5
source "$HOME/esp-idf-v5.5.2/export.sh"
idf.py -B build_idf552 build
```

Expected: 输出 `Project build complete.`，无编译或链接错误。

- [ ] **Step 2: 确认串口未被旧 monitor 占用**

Run:

```bash
pgrep -fl "idf_monitor|serial.tools.miniterm" || true
ls /dev/cu.usbmodem*
```

Expected: 没有旧 monitor 进程；设备端口存在，例如 `/dev/cu.usbmodem12401`。

- [ ] **Step 3: 刷写固件**

Run:

```bash
idf.py -B build_idf552 -p /dev/cu.usbmodem12401 flash
```

Expected: 所有分区写入成功并完成硬件复位。

- [ ] **Step 4: 串口验证当前非交易时段行为**

Run:

```bash
idf.py -B build_idf552 -p /dev/cu.usbmodem12401 monitor
```

操作并观察：

1. 打开自选股 App，日志出现一次 `fetch scheduled` 和一次完成结果。
2. 保持 App 前台至少 70 秒。
3. 若当前北京时间不在交易时段，期间不得出现第二条 `fetch scheduled`。
4. 点击“刷新”，应立即出现一条新的 `fetch scheduled`。
5. 返回工具页后继续观察，不能出现股票请求。

Expected: 打开即拉取、非交易时段不轮询、手动刷新有效、后台不拉取。

- [ ] **Step 5: 用自动化测试替代设备改时验证交易时段边界**

Run:

```bash
cd ../../
/tmp/test_stock_market_hours
```

Expected: 输出 `stock market hours tests passed`。不修改 Tab5 系统时间，不引入测试时钟到生产固件。

- [ ] **Step 6: 检查最终工作区范围**

Run:

```bash
git status --short --branch
git log --oneline -4
```

Expected: 仅保留用户原有的 LoRa 未提交改动；股票刷新实现已经由两个独立提交记录，`sdkconfig` 未进入提交。
