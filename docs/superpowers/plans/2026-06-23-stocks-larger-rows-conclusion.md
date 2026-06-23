# 自选股大字体与一句话结论 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 放大 Tab5 自选股数据行，并让点击股票后只显示来自 `latest_analysis_api.py` 的最新一句话结论。

**Architecture:** Hermes 使用自身 Python 解释器执行本地分析脚本，将按六位代码匹配的结论合并进现有 `/api/stocks/portfolio` 响应并随行情一起缓存。Tab5 延续单请求模型，只扩展 `StockItem` JSON 字段和详情卡片；行情、刷新策略及 LED ticker 保持不变。

**Tech Stack:** Python 3.11、aiohttp、`subprocess`、Python `unittest`/mock、C++17、LVGL 9、Mooncake、ESP-IDF 5.5.2。

## Global Constraints

- 结论固定来自 `/Users/leenzhou/daily_stock_analysis/latest_analysis_api.py`。
- Tab5 仍只请求 `GET /api/stocks/portfolio`，不直接连接 `daily_stock_analysis:8000` 获取结论。
- 脚本失败不得让行情接口失败；无结论时返回空字段并在设备显示“暂无分析结论”。
- 股票代码按六位数字字符串匹配，保留前导零。
- 顶部标题约 36px、列表列标题 30px、数据行约 34px，行高固定为 54px；十行必须继续完整显示。
- 工具页自选股图标使用 `/Users/leenzhou/Downloads/ICONS/stock.png` 转换的 130×130 ARGB8888 资源。
- 点击详情不再显示价格、涨跌幅、涨跌额、换手率或量比。
- 不修改一分钟交易时段刷新规则和 LED ticker 内容。
- 不覆盖或提交用户现有 LoRa 改动，不提交 `platforms/tab5/sdkconfig`。

---

## File Structure

| File | Responsibility |
|---|---|
| `/Users/leenzhou/hermes-mcp-xiaozhi/hermes_mcp_server/main.py` | 执行分析脚本、解析结论、按代码合并并提供容错。 |
| `test/test_stocks_endpoint.py` | 模拟结论脚本结果，验证接口合并和失败降级契约。 |
| `test/test_stocks_conclusion_ui.py` | 固定固件字段解析、大字体行和纯结论详情的源码契约。 |
| `app/apps/app_stocks/app_stocks.h` | 为每只股票保存结论和分析日期，调整行高。 |
| `app/apps/app_stocks/app_stocks.cpp` | 使用 30px 行字体、按下态、解析结论并重做详情卡。 |

### Task 1: Hermes 读取并合并一句话结论

**Files:**
- Modify: `/Users/leenzhou/hermes-mcp-xiaozhi/hermes_mcp_server/main.py:7-15,569-703`
- Modify: `test/test_stocks_endpoint.py`

**Interfaces:**
- Consumes: `latest_analysis_api.py` stdout JSON：`{"date": str, "items": [{"code": str, "one_sentence": str}]}`。
- Produces: `_load_latest_stock_conclusions() -> tuple[str, dict[str, str]]`；行情 item 的 `one_sentence`、`analysis_date` 字段和顶层 `analysis_date`。

- [ ] **Step 1: 写失败的接口测试**

在 `test/test_stocks_endpoint.py` 增加一个可复用的 `patch.object(hermes_main, "_load_latest_stock_conclusions", ...)`，并增加以下断言场景：

```python
with patch.object(
    hermes_main,
    "_load_latest_stock_conclusions",
    return_value=("2026-06-23", {
        "600519": "多头结构保持，等待放量确认。",
    }),
):
    resp = await hermes_main.handle_stocks_portfolio(MagicMock())
    body = json.loads(resp.body)
    assert body["analysis_date"] == "2026-06-23"
    assert body["items"][0]["one_sentence"] == "多头结构保持，等待放量确认。"
    assert body["items"][0]["analysis_date"] == "2026-06-23"
    assert body["items"][1]["one_sentence"] == ""
    assert body["items"][1]["analysis_date"] == ""
```

再增加失败降级测试：让 `_load_latest_stock_conclusions` 抛出 `RuntimeError("analysis unavailable")`，断言响应仍为 200、行情字段保留、`one_sentence == ""`。

- [ ] **Step 2: 运行测试并确认失败**

Run:

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
python3 test/test_stocks_endpoint.py
```

Expected: 新测试因 `_load_latest_stock_conclusions` 不存在或响应缺少结论字段而失败。

- [ ] **Step 3: 实现脚本读取器**

在 Hermes `main.py` 增加 `subprocess`、`sys`、`Path` 导入和以下实现：

```python
_LATEST_ANALYSIS_SCRIPT = Path(
    "/Users/leenzhou/daily_stock_analysis/latest_analysis_api.py"
)
_LATEST_ANALYSIS_TIMEOUT_S = 5


def _normalize_stock_code(value):
    code = str(value or "").strip()
    return code.zfill(6) if code.isdigit() else code


def _load_latest_stock_conclusions():
    completed = subprocess.run(
        [sys.executable, str(_LATEST_ANALYSIS_SCRIPT)],
        capture_output=True,
        text=True,
        timeout=_LATEST_ANALYSIS_TIMEOUT_S,
        check=True,
    )
    payload = json.loads(completed.stdout)
    if payload.get("error"):
        raise RuntimeError(payload["error"])
    analysis_date = str(payload.get("date") or "")
    conclusions = {}
    for item in payload.get("items", []):
        code = _normalize_stock_code(item.get("code"))
        sentence = str(item.get("one_sentence") or "").strip()
        if code and sentence:
            conclusions[code] = sentence
    return analysis_date, conclusions
```

- [ ] **Step 4: 在行情成功后合并结论**

在 `handle_stocks_portfolio()` 生成响应前用 `asyncio.to_thread` 调用同步读取器：

```python
    analysis_date = ""
    conclusions = {}
    try:
        analysis_date, conclusions = await asyncio.to_thread(
            _load_latest_stock_conclusions
        )
    except Exception as exc:
        logger.warning("latest stock conclusions unavailable: %s", exc)

    for item in items:
        sentence = conclusions.get(_normalize_stock_code(item.get("code")), "")
        item["one_sentence"] = sentence
        item["analysis_date"] = analysis_date if sentence else ""

    result = {
        "count": len(items),
        "items": items,
        "ts": int(now),
        "source": source,
        "analysis_date": analysis_date,
    }
```

- [ ] **Step 5: 运行 Hermes 接口测试并提交**

Run:

```bash
cd /Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo
python3 test/test_stocks_endpoint.py
```

Expected: 所有股票接口测试通过，包括脚本成功、部分无匹配和脚本失败降级。

Commit:

```bash
cd /Users/leenzhou/hermes-mcp-xiaozhi
git add hermes_mcp_server/main.py
git commit -m "feat(api): 自选股响应合并一句话结论"
```

### Task 2: Tab5 数据模型和大字体列表

**Files:**
- Create: `test/test_stocks_conclusion_ui.py`
- Modify: `app/apps/app_stocks/app_stocks.h:27-45`
- Modify: `app/apps/app_stocks/app_stocks.cpp:17-44,218-250,486-506`

**Interfaces:**
- Consumes: item 字段 `one_sentence: string`、`analysis_date: string`。
- Produces: `StockItem::one_sentence`、`StockItem::analysis_date`；30px、54px 高的可点击行。

- [ ] **Step 1: 写失败的源码契约测试**

创建 `test/test_stocks_conclusion_ui.py`，读取头文件和源文件并断言：

```python
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "app/apps/app_stocks/app_stocks.h").read_text()
SOURCE = (ROOT / "app/apps/app_stocks/app_stocks.cpp").read_text()


class StocksConclusionUiTests(unittest.TestCase):
    def test_stock_item_has_conclusion_fields(self):
        self.assertIn("std::string one_sentence;", HEADER)
        self.assertIn("std::string analysis_date;", HEADER)
        self.assertIn('it.value("one_sentence", std::string())', SOURCE)
        self.assertIn('it.value("analysis_date", std::string())', SOURCE)

    def test_rows_use_large_font_and_54px_height(self):
        self.assertRegex(HEADER, r"ROW_H\s*=\s*54")
        rows = SOURCE.split("// ── Data rows", 1)[1].split(
            "// ── Status bar", 1
        )[0]
        self.assertIn("zh_font_lg()", rows)
        self.assertIn("LV_STATE_PRESSED", rows)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: 运行测试并确认失败**

Run:

```bash
python3 -m unittest test/test_stocks_conclusion_ui.py -v
```

Expected: 结论字段、54px 行高和大字体断言失败。

- [ ] **Step 3: 扩展数据模型与 JSON 解析**

在 `StockItem` 增加：

```cpp
        std::string one_sentence;
        std::string analysis_date;
```

将 `ROW_H` 改为 `54`，并在 `_parseStocksJson()` 中增加：

```cpp
                s.one_sentence = it.value("one_sentence", std::string());
                s.analysis_date = it.value("analysis_date", std::string());
```

- [ ] **Step 4: 放大数据行并增加按下反馈**

数据行中的七个 cell 使用 `zh_font_lg()`，并为 row 增加：

```cpp
        lv_obj_set_style_bg_color(_rows[i], lv_color_hex(C_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(_rows[i], LV_OPA_40, LV_STATE_PRESSED);
```

- [ ] **Step 5: 运行契约测试并确认通过**

Run:

```bash
python3 -m unittest test/test_stocks_conclusion_ui.py -v
```

Expected: 两项测试通过。

### Task 3: 用纯结论卡片替换行情详情

**Files:**
- Modify: `test/test_stocks_conclusion_ui.py`
- Modify: `app/apps/app_stocks/app_stocks.cpp:318-372`

**Interfaces:**
- Consumes: `StockItem::name`、`code`、`one_sentence`、`analysis_date`。
- Produces: 960x360 结论弹窗；无结论时固定文案“暂无分析结论”。

- [ ] **Step 1: 增加失败的详情卡契约测试**

在测试中抽取 `_showDetail()` 函数并断言：

```python
    def test_detail_shows_only_conclusion_content(self):
        detail = SOURCE.split("void AppStocks::_showDetail", 1)[1]
        detail = detail.split("void AppStocks::_closeDetail", 1)[0]
        self.assertIn("s.one_sentence", detail)
        self.assertIn("s.analysis_date", detail)
        self.assertIn("暂无分析结论", detail)
        for forbidden in (
            "s.price", "s.chg", "s.pchg", "s.turnover", "s.liangbi"
        ):
            self.assertNotIn(forbidden, detail)
```

- [ ] **Step 2: 运行测试并确认失败**

Run:

```bash
python3 -m unittest test/test_stocks_conclusion_ui.py -v
```

Expected: 旧详情仍含行情字段，测试失败。

- [ ] **Step 3: 实现大号自动换行结论卡**

将 `_showDetail()` 中 600x400 七行行情内容替换为：

```cpp
    lv_obj_set_size(_detail_modal, 960, 360);

    lv_obj_t* title = lv_label_create(_detail_modal);
    lv_label_set_text_fmt(title, "%s  %s", s.name.c_str(), s.code.c_str());
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);

    lv_obj_t* date = lv_label_create(_detail_modal);
    std::string date_text = s.analysis_date.empty()
        ? "分析日期：--" : "分析日期：" + s.analysis_date;
    lv_label_set_text(date, date_text.c_str());
    lv_obj_set_style_text_font(date, zh_font_sm(), 0);

    lv_obj_t* conclusion = lv_label_create(_detail_modal);
    const char* text = s.one_sentence.empty()
        ? "暂无分析结论" : s.one_sentence.c_str();
    lv_label_set_text(conclusion, text);
    lv_label_set_long_mode(conclusion, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(conclusion, 860);
    lv_obj_set_style_text_font(conclusion, zh_font_lg(), 0);
```

保留并放大关闭按钮为 150x54，分别将标题、日期、结论排列在卡片顶部 0、48、92 像素处。

- [ ] **Step 4: 运行全部固件股票测试**

Run:

```bash
python3 -m unittest \
  test/test_stocks_conclusion_ui.py \
  test/test_stocks_refresh_policy.py -v
c++ -std=c++17 -Wall -Wextra -Werror -I. \
  test/test_stock_market_hours.cpp -o /tmp/test_stock_market_hours \
  && /tmp/test_stock_market_hours
```

Expected: 所有测试通过，输出 `stock market hours tests passed`。

- [ ] **Step 5: 构建桌面模拟器并提交固件改动**

Run:

```bash
cmake --build platforms/desktop/build -j8
```

Expected: 构建退出码为 0。

Commit:

```bash
git add app/apps/app_stocks/app_stocks.h \
        app/apps/app_stocks/app_stocks.cpp \
        test/test_stocks_endpoint.py \
        test/test_stocks_conclusion_ui.py
git commit -m "feat(stocks): 放大列表并显示一句话结论"
```

### Task 4: 真实服务与 Tab5 实机验证

**Files:**
- No source changes expected.

**Interfaces:**
- Consumes: 已升级的 Hermes `8766` 服务和连接的 M5Tab5。
- Produces: 真实 HTTP、构建、刷机和屏幕/串口证据。

- [ ] **Step 1: 重启 Hermes 并验证真实组合响应**

重启当前 `hermes-mcp-xiaozhi` LaunchAgent，然后运行：

```bash
curl -fsS http://127.0.0.1:8766/api/stocks/portfolio | python3 -m json.tool
```

Expected: `count` 为 10，顶层 `analysis_date` 为最新分析日，每只匹配股票含非空 `one_sentence` 和 `analysis_date`。

- [ ] **Step 2: 构建 Tab5 固件**

使用仓库现有 ESP-IDF 5.5.2 构建流程重新构建 `platforms/tab5`。

Expected: 固件构建退出码为 0，不改写或提交 `sdkconfig`。

- [ ] **Step 3: 刷入已连接设备并查看串口日志**

使用当前枚举出的 Tab5 串口刷机，随后监控启动与打开自选股 App 的日志。

Expected: 日志包含股票接口成功和 `fetched 10 items`，没有 JSON 解析异常或 LVGL 崩溃。

- [ ] **Step 4: 实机 UI 验收**

在设备上打开自选股，确认十行文字明显大于原版且整行易点击；点击至少一只长结论股票，确认只显示名称、代码、分析日期和自动换行结论；再检查无行情明细字段及关闭按钮可用。

## Self-Review

- Spec coverage: Hermes 合并、失败降级、六位代码、单请求、30px/54px 列表、按下反馈、纯结论详情、真实服务和实机验证均有对应任务。
- Placeholder scan: 无占位语句或缺少具体内容的实施步骤。
- Type consistency: Python helper固定返回 `tuple[str, dict[str, str]]`；JSON 字段和 C++ 成员统一使用 `one_sentence`、`analysis_date`。
