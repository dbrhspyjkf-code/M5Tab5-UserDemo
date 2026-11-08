# HANDOFF — SSH 终端语音输入：✅ 已修复并真机验收通过（2026-10-30）

> 遗留问题关闭。订阅制修复（commit 6964242）真机验收数据：
> - 会话 1：86 opus 包 / 12530 字节 → STT "老安。"（毫秒级返回）
> - 会话 2：66 opus 包 / 9588 字节 → STT "今天星期几？"
> - 对照修复前：2 包 / 16 字节 / 10s 超时
> 本文档留作根因分析与排查参考（tap 断流看门狗/失败模式含义仍在下方）。

---

## 根因（2026-10-29 23:25 串口日志实锤）

录音期间每次 `esp_codec_dev_read` 报：

```
E (125820) i2s_common: i2s_channel_read(1367): The channel is not enabled
（每 1026ms 一次 × 3，然后 silence timeout 2052ms，只录 2 包 16 字节）
```

IDF 5.5.2 源码里这条错误的**真实语义是 I2S 读序列化二值信号量 1000ms 抢不到**
（`xSemaphoreTake(handle->binary, timeout)`），不一定是通道真被禁用。时间线精确重构：
前 2 帧拿到 DMA 积压旧数据，之后每次读超时返回、缓冲保持全零 → 被静音检测当
"没说话" → 2s 静音超时 → STT 收 16 字节垃圾必然超时。

「挂起 xiaozhi + 另一任务直读 codec」架构天生不可修：
- `xiaozhi_suspend` 异步 Schedule，`Stop(kAll)` 用 `vTaskDelete` 硬杀正在读麦克风
  的 input task（死时可能带着 I2S 读锁 → 永久死锁）
- 全双工 I2S 的 TX/RX 时钟联动，任何一侧被 esp_codec_dev 内部 set_fmt 禁用都会停摆
- `Tab5AudioCodec::Read` 无论成败都返回 samples，错误全被吞（这次日志能定位纯属
  ESP_ERROR_CHECK_WITHOUT_ABORT 恰好打印）
- sleep 1s 只能等 task 退出，等不回被带走的锁/停掉的时钟

## 修复（commit 6964242，2026-10-30，已刷机）

**订阅制**（原步骤 5 备选架构落地）——xiaozhi 不再挂起，其他 app 向它的输入管线"借"麦克风：

| 文件 | 改动 |
|---|---|
| `audio_service.{h,cc}` | 新增 `SetInputTap(cb)` + `AS_EVENT_INPUT_TAP_RUNNING` 事件位；AudioInputTask 新分支把读到的 **16kHz mono**（与唤醒词同源、已重采样）分发出去；input task 保持 codec **唯一读者** |
| `xiaozhi_ctl.{h,cc}` | 导出 `xiaozhi_set_input_tap(cb/nullptr)`：注册时 `Start(kInputOnly)` 确保 input task 存活 + **暂停唤醒词**（防说话误唤醒、保 tap 全速 10ms 节奏）；注销时恢复唤醒词 |
| `voice_input_service.{h,cc}` | 采集路径重写：tap 帧 → pcm_backlog（1s 上限，超限丢弃）→ 消费者按 60ms 帧编码；新增 **2s 断流看门狗**（tap 无数据明确报错，不再假装静音）；删掉 suspend/resume/直读 |

顺带修掉：原实现把 24kHz TDM 当 16kHz 喂 opus（1.5 倍速变调）；现在 tap 拿到的就是 16kHz。

启动日志已验证：AFE 唤醒词常开正常、无 I2S 错误。

## 验收要点（看 /tmp/radio-watch.log）

正常应看到：
```
voice_input: starting voice input session
xiaozhi_ctl: input tap armed (wake word paused)
AudioService: Input tap registered
voice_input: recording started (max 10000 ms)
（说话期间不应有 i2s_common 错误）
voice_input: user-stopped recording with N packets   ← N 应为几十上百
voice_input: recorded N opus packets (M bytes total) ← M 应为几千字节
xiaozhi_ctl: input tap released (wake word resumed)
voice_input: STT result: '...'
```

失败模式的含义：
- `mic tap starved for 2000 ms` = tap 没来数据（xiaozhi input task 没跑或被停）→ 查 audio service 状态
- `recorded 0/2 packets` 复现 = tap 通了但数据是零 → 查 ES7210/通道增益
- STT 超时但包数正常 → 服务端问题，与采集无关

## 已知边界（未处理，按需再改）

- 录音期间若恰有 xiaozhi 对话进行中（audio processor 在跑），tap 与 processor 分时
  各半 → 断流看门狗 2s 会明确报错退出（不静默失败）。可接受；要更稳可在 tap 注册
  前先 AbortSpeaking
- 回调在 input task（优先级 8）执行，当前只做锁+拷贝，勿在 tap 回调里做重活

## 若验收失败的新排查入口

1. `voice_input` 日志的 packets 数 + bytes 数（区分"没数据"vs"数据是零"）
2. `AudioService: Input tap registered` 是否出现（没出现=xiaozhi 未初始化）
3. tap 期间 `Enabling/Disabling wake word detection` 成对出现（唤醒词暂停/恢复）

---

## 常用命令（详见 memory/m5tab5-ssh-app.md 与 cockpit 记忆）

```bash
# 构建（platforms/tab5，ESP_IDF_VERSION 必须设）
source ~/esp-idf-v5.5.2/export.sh
export ESP_IDF_VERSION=v5.5.2 IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.14_env
export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
idf.py build && idf.py -p /dev/cu.usbmodem1301 app-flash   # 组件改动也编进 app 镜像，app-flash 即可
# 刷机前 pkill -f tab5_serial_log.py；刷后必须 grep "Hash of data verified"；刷完 nohup 拉回日志脚本
# res 分区（改字库/资源后）：先 python3 tools/pack_assets.py，再 esptool write-flash 0x910000 build/assets.img
# 串口日志：/tmp/tab5_serial_log.py → /tmp/radio-watch.log（追加写！查崩溃只看最后一个 watcher start 段）
# 注意：shell 里 grep 被 ugrep 包装带 -I 会跳过含乱码字节的串口日志，查日志用 /usr/bin/grep
```

## 提醒

- OrangePi 服务改动：本地改 → commit push → Pi 拉取（禁止线上直改）
- worktree：`/Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo-yrobot-cockpit`（分支 tab5-batch-20261026，推 origin/main）
- 长期记忆：`~/.zcode/cli/memories/projects/m5tab5-userdemo-e3295864c3893a58/memory/`（SSH 详见 m5tab5-ssh-app.md）
- 2026-10-30 上午 github.com 曾不可达：commit 6964242 已本地提交，push 由后台重试任务补推（/tmp/push_retry.log）
