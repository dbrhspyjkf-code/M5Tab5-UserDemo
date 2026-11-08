# HANDOFF — LoRa Chat 全链路日（2026-11-08）

> 本日从「LoRa Chat 卡等待」一路打到三向互通 + 语音输入。**全部结论与坑已同步进长期记忆**
> （`m5tab5-lora-chat` / `m5tab5-cardputerzero-lora` / `m5tab5-flash-workflow` / `m5tab5-porta-units` / 项目内
> `memory/m5tab5-cockpit-project.md`），本文是**断点续作指南**：现在到哪儿了、下一步按什么顺序做什么、
> 现场有什么坑。

---

## ✅ 已达成（全部已刷机已推送）

| # | 事项 | commit | 设备状态 |
|---|---|---|---|
| 1 | **PORT A 共存终态**：C6L 侧 TX 开漏（必需）+ Tab5 保持推挽 | `f796a3b` | NFC+LoRa 同 Hub 不拔插，RFID ✓ |
| 2 | **C6L 固件对齐 CardputerZero**：868/SF12/BW125/sync0x34 + TX 超时 3s→20s | （UnitC6L-LoRa 仓库，非 git） | C6L 经 USB@12401 刷入 |
| 3 | **C6L BW 编码表修正**：125k 被错映射 0x02(=31.25k)→0x04，全表按 datasheet 重写 | 同上 | **本日最大坑**：SF7/BW500 时代 0x06 碰巧正确掩盖一切 |
| 4 | **CardputerZero 官方应用 PI4IO bug 补丁**：极性写偶数地址 0x02 被 NACK→放弃 radio 初始化→聋哑；0x2f7f0 `tbz`→NOP | （直接改 Zero 上的二进制） | 备份 `.bak` 同目录；**deb 升级会覆盖需重打** |
| 5 | **三向互通达成**：Tab5↔C6L↔CardputerZero | — | C6L 控制台 `LoRa→UART: "...CardputerZhello" RSSI=-10` 实锤 |
| 6 | **LoRa Chat 语音输入**：输入行新增「语音」按钮（大麦克风模式，识别中文填入输入框） | `4697dc4` | 已刷机，**未验收** |

- Tab5 当前固件 = `5ab4876`（voice 输入 + 白屏加固 + 白屏定性注记），ELF SHA `29a5b516d` 之后的新 build（含 hal 注记回撤）。
- C6L 当前 = sync0x34 + BW 表修正 + TX 超时 20s（OLED 应显示 `C6L 868M SF12 BW125`）。
- **C6L↔ADV Cardputer (SF7/BW500) 现在不互通**——回连 = C6L 改 `LORA_BW_KHZ 500`/`LORA_SF 7` 两常量重刷。

## ⏳ 断点：下一步（按序）

1. **语音+中文端到端验收**（白屏打断，未做）：
   - Tab5 → 工具页 → LoRa Chat → 点「语音」→ 说中文 → 停顿 2s 自动识别 → 文字落输入框 → 发送
   - 看 Tab5 聊天页中文气泡 ✓？
   - **看 CardputerZero 屏幕显示什么**（中文/乱码/方块）——Zero 官方应用字体编死在二进制里，若乱码则 Zero 端要单独处理（找 M5 源码加 CJK 字体重建，或接受 Tab5 单端中文）
   - **中文 UART 悬案**：上一轮中文消息在 C6L 端收到乱码（`\xd6k\xd3k` 两字节周期，非合法 UTF-8/GB2312）——当时 BW 表是错的，需重测；若 ASCII 干净而中文必乱 → 查 Tab5 发送编码/UART 信号
2. **白屏**（间歇性，未结）：刷机后（下载模式 30s + USB 域硬复位）偶发面板无视频=白屏（背光亮、固件正常跑、日志全对）。**恢复=按一下实体复位键（EN），今日 3/3**。已试无效：LCD_RST 双脉冲（BSP 保留）、USB 复位检测+esp_restart 软自愈（数字域清不到 DSI PHY 模拟域，已撤）。**复发先拔 Hub 隔离**（本日一次拔 Hub 实验：USB 复位仍白、EN 键恢复——外设排除）。根治方向=DSI PHY 域级复位（无公开 API，要碰 PCR 寄存器）。
3. （可选）**向 M5 反馈**：Cap-LoRa-1262 v1.0.1 的 PI4IO 寄存器映射 bug（TCA9535 图对 PI4IO 图芯片）。

## ⚠️ 现场坑（断点续作必读）

- **刷机后白屏 → 按一下复位键**（今日 3/3），别浪费轮次排查固件。
- **C6L 刷机**：USB 插 Mac 枚举在 **12401**（占 Passport 位；先 `esptool --chip esp32c6 chip_id` 验芯片 + `pkill -f usbmodem12401`（我的控制台 reader 占口挡刷机））。C6L 的 ESP_LOG 走 USB 不走 Grove；**排障利器=USB 插 Mac 读控制台**（reader 脚本模式见下）。
- **C6L UART 全断 → 先 USB 插拔重启 C6L 再深挖**（曾 wedge 一次即愈）。
- **Zero（192.168.1.115，SSH pi，密码用户提供不入档）**：sshd 限连发（Permission denied 就 sleep 几秒）；应用日志不吐 radio 层——**排障用内核 SPI 追踪**（`/sys/kernel/tracing events/spi/spi_transfer_start`，解码原始命令字节=射频参数终极真相，方法全记在 `m5tab5-cardputerzero-lora`）。
- **Tab5 串口日志**：`/tmp/radio-watch.log`（logger 已在跑）；**静默≠死机**（管道假死+积压补发假时间线）；刷机前 `pkill -f tab5_serial_log.py` + `lsof -t /dev/cu.usbmodem1301 | xargs kill`。
- **Tab5 早前遗留**：白屏案定性注记在 `platforms/tab5/main/hal/hal_esp32.cpp`（RESET REASON 附近）。
- 长期记忆索引：`MEMORY.md`（自动记忆）/ 项目 `memory/MEMORY.md`。

## 工具/文件速查

| 什么 | 哪里 |
|---|---|
| C6L 固件源（非 git） | `~/Projects/UnitC6L-LoRa`（main.c=参数/协议，sx1262.c=驱动） |
| C6L 控制台 reader | `nohup python3 -c "import serial,sys; s=serial.Serial('/dev/cu.usbmodem12401',115200,timeout=2); ..."` → `/tmp/c6l-console.log`（**占口，刷机前 pkill -f usbmodem12401**） |
| Zero 补丁二进制 | `/usr/share/Cap-LoRa-1262/bin/Cap-LoRa-1262`（备份 `.bak`；补丁=vaddr 0x2f7f0 `a0 05 00 36`→`1f 20 03 d5`） |
| Zero 昵称 | `~/.config/Cap-LoRa-1262/nickname`（**UI 编辑优先于文件**；当前=用户 UI 里自设） |
| Tab5 串口 logger | `platforms/tab5/tools/tab5_serial_log.py` → `/tmp/radio-watch.log` |
| ESP-IDF | v5.5.2 + `IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.14_env` + `ESP_IDF_VERSION=v5.5.2`；Tab5 工程 `platforms/tab5`，app 层只 app-flash |
