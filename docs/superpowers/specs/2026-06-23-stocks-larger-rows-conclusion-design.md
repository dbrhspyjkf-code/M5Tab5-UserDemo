# 自选股大字体与一句话结论设计

## 目标

提升 Tab5 自选股 App 的可读性和点击准确度，并将点击股票后的详情弹窗从行情明细改为最新分析的“一句话结论”。

## 列表界面

- 现有列结构、十只股票展示能力、涨跌配色和 LED ticker 保持不变。
- 标题、列标题和数据行使用轻量 36px 股票字体；未收录的新股票名称自动回退到 30px 全字库。
- 数据行高度由 52px 调整为 54px；十行合计 540px，仍可放入表头与底部状态栏之间的 544px 区域。
- 顶部“自选股”标题、列表列标题和数据行均为 36px；顶部状态和底部按钮继续使用 20px。
- 行保持整行可点击，并增加按下态背景反馈，让用户明确知道点中了哪一行。
- 工具页自选股图标替换为 `/Users/leenzhou/Downloads/ICONS/stock.png`，等比缩放为 130×130 ARGB8888 设备资源。

## 一句话结论数据

结论来源固定为：

`/Users/leenzhou/daily_stock_analysis/latest_analysis_api.py`

该脚本从 `daily_stock_analysis` 的 SQLite 数据库读取最近分析日、每只股票最新的 `simple` 分析记录，并输出包含 `date`、`code`、`one_sentence` 等字段的 JSON。

Hermes 的现有 `GET /api/stocks/portfolio` 仍是 Tab5 的唯一请求入口。Hermes 在刷新行情时，用服务自身的 Python 解释器执行上述脚本一次，并按六位股票代码合并结果：

```json
{
  "code": "002050",
  "name": "三花智控",
  "price": 42.61,
  "chg": -4.51,
  "one_sentence": "空头排列明确……等待趋势修复信号。",
  "analysis_date": "2026-06-23"
}
```

Tab5 不直接连接 `daily_stock_analysis:8000` 获取结论，也不发送第二个 HTTP 请求。行情和结论共享 Hermes 现有的 30 秒组合结果缓存；Tab5 的一分钟前台刷新会自然取得最新组合结果。

## 服务端错误处理

- 脚本使用 `sys.executable` 执行，只依赖脚本自身所需的 Python 标准库，不依赖 `daily_stock_analysis/.venv` 的解释器链接。
- 子进程设置短超时，并要求退出码为 0、标准输出为合法 JSON。
- 脚本不存在、超时、执行失败、JSON 不合法或数据库暂无分析时，只记录警告并让所有股票的结论为空；不能使行情接口失败。
- 股票代码统一转换为六位字符串后匹配，避免 SQLite 或 JSON 数字格式导致前导零丢失。
- Hermes 返回顶层 `analysis_date`，并在每只匹配股票上附加相同的 `analysis_date` 和对应 `one_sentence`。

## 点击详情

点击有效股票行后显示居中的结论卡片，不再显示现价、涨跌幅、涨跌额、换手率和量比。

卡片内容为：

1. 股票名称和六位代码。
2. 分析日期。
3. 30px、自动换行的一句话结论。
4. 大尺寸“关闭”按钮。

若该股票没有匹配结论，结论区域显示“暂无分析结论”；股票列表和点击操作仍然可用。

## 数据模型

`AppStocks::StockItem` 增加：

- `std::string one_sentence`
- `std::string analysis_date`

JSON 解析对两个字段使用空字符串默认值，兼容尚未升级或暂时返回旧格式的 Hermes 服务。

## 验证

1. Hermes 单元测试覆盖脚本成功、部分代码无匹配、脚本失败但行情仍成功三种情况。
2. 固件源码契约测试覆盖 30px 数据行字体、54px 行高、结论字段解析及详情弹窗不再引用行情字段。
3. 运行 Hermes 股票接口测试和 Tab5 股票相关测试。
4. 构建桌面目标，检查字体与弹窗布局没有编译或 LVGL API 问题。
5. 重启本机 Hermes 服务，调用真实 `/api/stocks/portfolio`，确认十只股票携带结论和分析日期。
6. 构建并刷入 Tab5，通过实机屏幕确认列表更易点击、长结论正确换行、无行情明细残留。

## 不在本次范围内

- 修改 `latest_analysis_api.py` 的分析生成逻辑。
- 在 Tab5 上显示信号类型或情绪分数。
- 修改一分钟交易时段刷新策略。
- 修改 LED ticker 的播报内容。
