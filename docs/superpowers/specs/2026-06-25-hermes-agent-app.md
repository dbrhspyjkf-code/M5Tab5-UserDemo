# Spec: 把 Claude app 改造成 Hermes Agent app（ACP）

日期: 2026-06-25
状态: 设计已确认，待实施

## 目标

把 Tab5 现有的 "Claude" app（`app_project_assistant`）**原地改造**成 "Hermes" app：
- 复用现有 UI（聊天气泡、流式回复、SPIFFS 历史、**权限审批卡**、语音按钮、屏幕键盘）——一行交互代码不重写。
- 后端从 claude-to-im 守护进程（`:8770`）换成 **Hermes Agent**。
- 能力：与 Hermes 对话、**记忆**（多轮 + 长期 memory）、**安排定时任务**（cron）、**语音输入文字**（后续阶段）。
- **权限**：Hermes 执行危险操作时，在 Tab5 触屏弹审批卡，用户点同意/拒绝（替代不安全的 `--yolo`）。

## 为什么不重做 / 不复制

- 现有 app 已是通用 agent 触屏客户端（流式 + 历史 + 审批卡 + 语音 + 键盘），键盘/语音最费功夫的部分都打磨好了。
- flash 分区只剩 ~5%，复制成第二个 app（claude_logo 46KB + key_icon 130KB + mic_icon 41KB 再来一份）会爆分区。
- 结论：**原地改造**（改品牌 + 换后端），Claude 入口变成 Hermes。

## 现状事实（已核实）

### Tab5 app（`app/apps/app_project_assistant/`）
- 协议：`POST /api/chat` → 轮询 `GET /api/result/<id>` → `POST /api/approve/<id>`。
- `_bridgeUrl()` → `http://{svc_host=192.168.1.142}:8770{path}`。
- 已有状态机 `UpdateKind { Progress, Done, Error, PermissionRequired }` + `_showPermissionCard` + `/api/approve`。
- 历史持久化 SPIFFS（`_saveHistory/_loadHistory`，MAX_HISTORY=50）。

### Hermes 侧（已核实）
- `hermes acp` = 以 ACP 模式启动 agent（供编辑器集成）。ACP 库已装在 hermes venv：`acp` v0.11.2，PROTOCOL_VERSION=1。
- ACP 方法（agent 方向）：`initialize` / `session/new` / `session/load` / `session/resume` / `session/prompt` / `session/cancel` / `session/close` …
- ACP 回调（client 方向，桥要实现）：`session/update`（流式）、`session/request_permission`（权限）、`fs/read_text_file`、`fs/write_text_file`、`terminal/*`。
- 权限选项标准集：`approve`(allow_once) / `approve_for_session`(allow_always) / `reject`(reject_once)。
- 记忆：`session/resume`/`session/load` 续接会话 + 自动注入 AGENTS.md/SOUL.md/memory。
- 定时任务：`hermes cron`（agent 自带工具，用户用自然语言安排即可）。

## 目标架构

```
Tab5 Hermes app ──HTTP轮询──► Hermes Bridge (:8771, Python, launchd)
  /api/chat /result /approve        │  ACP over stdio (JSON-RPC, protocol v1)
                                    ▼
                              `hermes acp`  (agent: 记忆/工具/cron)
```

桥 = **ACP client**。发：`initialize` → `session/new`（首次）/`session/resume`（续接）→ `session/prompt`（每轮）。
收：`session/update`（累积成 partial/工具进度）、`session/request_permission`（转成审批卡）。

### 关键简化
桥在 `initialize` 时**不声明** fs/terminal 客户端能力 → Hermes agent 用**自己的**工具执行，只在危险操作前发 `session/request_permission`。
→ 桥不必充当文件/终端执行器，只处理 `session/update` + `session/request_permission` 两类回调。工作量大幅减少。

## 端点契约（Tab5 协议保持不变）

| Tab5 调用 | 桥行为 |
|---|---|
| `POST /api/chat {text}` | 对该设备常驻 session 发 `session/prompt`，返回 `{id}`（本轮 turn id） |
| `GET /api/result/<id>` | 返回 `{status: progress\|done\|error\|permission_required, partial, tools[], perm_*}`，由 `session/update` 累积 |
| `POST /api/approve/<id> {approved 或 option_id}` | 用选中的 option_id 应答挂起的 `session/request_permission` |

## 记忆 / 会话语义
- 每台设备一个常驻 ACP session：首次 `session/new`，session_id 存盘；桥重启后 `session/resume`。
- Tab5 SPIFFS 历史仅用于 UI 回显，不参与 agent 上下文（上下文由 Hermes session 保证）。
- 「清空聊天」= 关旧 session 开新 session（断开记忆，重新开始）。

## 权限（解决既有痛点）
- agent `session/request_permission(tool_call, options)` → 桥置 result.status=permission_required + tool 名/入参 + options → Tab5 弹卡。
- 用户点：同意 / 本次会话都允许 / 拒绝 → `/api/approve` 带 option_id → 桥回 ACP 响应。
- 小智语音那条（无屏幕）仍另行处理（`--yolo` 或 toolset 白名单），与本 app 无关。

## 范围外（本 spec 不含）
- 语音 STT 链路（P3 单独立项）。
- 小智语音侧的 hermes 工具权限（另一条线）。
- 桥的多设备并发（先支持单会话，够用再扩）。

## 验收标准
1. Tab5 打开 Hermes app，发消息能收到 Hermes 流式回复。
2. 连续多轮对话有记忆（第二轮能引用第一轮）。
3. 说"每天早上8点提醒我看未读邮件"→ Hermes 用 cron 落地，可在 `hermes cron list` 看到。
4. 触发危险操作时 Tab5 弹审批卡，点同意后任务继续、点拒绝则中止。
5. 桥重启后再对话能 resume 上原会话（记忆不丢）。
