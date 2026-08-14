# Tab5 Reachy 控制与聊天功能设计

日期：2026-08-14

## 目标

扩展现有 Tab5 Reachy app，使其能够安全调用 YRobot Dashboard 已有 API：

- 在“音频”Tab 控制 Mic、音量、静音和 VAD 灵敏度。
- 新增“控制”Tab，管理对话后端、视频上传、QWEN 音色、摄像头和 Reachy daemon。
- 新增“聊天”Tab，显示 XIAOZHI/QWEN 最近的用户语音与机器人回复。
- 保持后台网络请求不阻塞 LVGL，并避免 app 退出后继续更新 UI。

本功能只修改 `M5Tab5-UserDemo`。不修改或部署 YRobot，不直接操作官方 Reachy daemon 端口，也不引入新的第三方依赖。

## 已验证的服务端接口

Tab5 直接复用机器人 `http://<reachy_host>:8042` 上已有的 YRobot API：

| 功能 | 方法与路径 | 请求体 |
| --- | --- | --- |
| 音量 | `PUT /api/volume` | `{"percent": <0..100>}` |
| Mic 上传 | `PUT /api/audio/input` | `{"enabled": <bool>}` |
| VAD | `PUT /api/audio/vad` | `{"rms_min": <number>}` |
| 对话后端 | `PUT /api/conversation/backend` | `{"backend": "xiaozhi"|"qwen"}` |
| 视频上传 | `PUT /api/conversation/video` | `{"enabled": <bool>}` |
| QWEN 音色 | `PUT /api/conversation/voice` | `{"voice": <available voice>}` |
| 摄像头 | `PUT /api/camera/state` | `{"running": <bool>}` |
| Daemon | `POST /api/reachy-daemon/action` | `{"action": "wake"|"sleep"|"restart"}` |
| 聊天记录 | `GET /api/logs?filter=chat&limit=200` | 无 |
| YRobot 重启 | `POST /api/system/restart` | 无 |

后端切换接口只保存配置并返回 `restart_required: true`。Tab5 必须在用户确认后调用 YRobot 重启接口，并轮询状态直到目标后端运行或超时。QWEN 失败时保留 QWEN 配置并显示失败，不得静默切回 XIAOZHI。

## 架构

### `reachy_client.h`

扩展现有固定用途客户端，不增加通用 REST 抽象。客户端负责：

- 读取并解析音频、控制和聊天状态。
- 发送固定路径、固定字段的 PUT/POST 请求。
- 将服务端错误转换为可展示的简短结果。
- 把聊天日志中的 `xz stt:`、`xz tts text:`、`qwen stt:` 和 `qwen response:` 转换为用户/机器人消息。

### `AppReachy`

`AppReachy` 负责 UI、轮询策略和操作状态：

- 继续使用 `GetHAL()->tryRunDetached()` 执行 HTTP 请求。
- 同一时间只允许一个 Reachy 网络 worker，避免内存碎片和请求乱序。
- worker 只写缓存和完成标记，LVGL 线程读取缓存后更新控件。
- 每个操作保留提交前值；失败后恢复服务端真实值并显示错误。
- app 不在前台时停止轮询和渲染。异步结果必须通过生命周期标记确认当前 UI 仍有效。

## 页面设计

顶部导航调整为七个 Tab：

`状态 / 运动 / 音频 / 控制 / 聊天 / 系统 / 日志`

### 音频

- Mic 启用/禁用开关。
- 音量滑杆、百分比和静音/恢复按钮。
- VAD 灵敏度滑杆和三位小数 RMS 数值。
- 当前 Mic 电平只读显示。
- 音量与 VAD 在拖动期间只更新本地数值；松手时发送一次请求。
- 静音时记住本次 app 运行期最后一个非零音量；恢复时使用该值，若不存在则使用 50%。

### 控制

- XIAOZHI / QWEN 双按钮。
- 视频上传开关。
- QWEN 音色下拉框。非 QWEN 模式也允许预先选择音色。
- 摄像头启动/停止开关。
- Daemon 唤醒、休眠和重启按钮。

模式切换、Daemon 休眠和 Daemon 重启必须弹确认框。Daemon 唤醒、视频、摄像头和音色选择直接执行并显示结果。

### 聊天

- 用户消息靠右，Reachy 回复靠左。
- 展示最近六轮对话并自动滚动到底部。
- 每三秒读取一次聊天日志，仅内容变化时重绘。
- 工具调用和技术日志不显示。
- 连续的机器人回复片段按时间顺序展示；首版不在 Tab5 端猜测语义边界或合并改写文本。

### 系统与日志

保留现有系统和技术日志页面。聊天记录不混入技术日志页面。

## 操作状态与错误处理

- 用户提交操作后禁用对应控件并显示“处理中”。
- 成功时使用服务端响应更新 UI，不能仅相信本地目标值。
- 网络错误、HTTP 错误或 JSON 错误均恢复真实状态并显示简短错误。
- 模式切换流程为：确认、保存后端、重启 YRobot、轮询 `/api/status`、确认 `configured_backend` 与运行后端均为目标值。
- 模式切换轮询超时为 30 秒；超时后显示当前配置、运行后端和错误，不自动回退。
- YRobot 重启只影响 YRobot 对话层，不调用 Reachy daemon 重启。
- Daemon 操作必须使用现有 `/api/reachy-daemon/action` 白名单动作，不接受任意 action 字符串。

## 轮询策略

- 激活任意 Tab 时立即读取该 Tab 的真实状态。
- 状态、运动、音频、控制、系统和日志沿用五秒周期。
- 聊天使用三秒周期。
- 非当前 Tab 不发起周期请求。
- 写操作进行时不启动同类轮询；完成后立即刷新一次。

## 测试与验收

### 自动测试

- 客户端 JSON 解析：音频、控制状态、可用音色和聊天消息。
- 请求契约：所有路径、方法和请求字段固定且正确。
- UI 生命周期：后台 app 不轮询、不渲染，旧回调不访问已失效对象。
- 滑杆：只在释放时提交。
- 聊天：只接受四种对话标记，限制最近六轮，内容不变时不重绘。
- 后端状态机：保存、重启、成功、失败、超时和不回退。
- 现有 Reachy 生命周期及图标测试继续通过。
- ESP-IDF 5.5.2 固件完整构建通过。

### 真机验收

- Mic、音量、静音和 VAD 在 Dashboard 与 Tab5 之间状态一致。
- 视频、音色、摄像头和 Daemon 操作成功且错误可见。
- XIAOZHI/QWEN 切换必须经过确认；重启后显示的配置与实际运行模式一致。
- 两种后端的聊天记录都能显示并自动刷新。
- Reachy app 可反复打开、上滑退出和重新进入，不闪屏、不重启。

烧录前必须再次确认 Tab5 串口为 ESP32-P4 `/dev/cu.usbmodem112401`，并获得用户明确许可。

## 不在本次范围

- 修改 YRobot API 或 Dashboard。
- 自动发现任意 Dashboard 控件。
- 在 Tab5 发送文字或语音聊天消息。
- QWEN 连接失败时自动回退 XIAOZHI。
- 修改官方 Reachy daemon、固件或媒体资源所有权。
