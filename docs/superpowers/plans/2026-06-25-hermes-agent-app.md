# Plan: Hermes Agent app（ACP）实施计划

Spec: docs/superpowers/specs/2026-06-25-hermes-agent-app.md
日期: 2026-06-25

涉及两个仓库:
- `~/hermes-mcp-xiaozhi`（新增 Hermes ACP 桥）
- `~/Projects/M5Tab5/M5Tab5-UserDemo`（Claude app → Hermes app）

---

## Task 0 — ACP 握手验证（de-risk，先做）

**目的**: 证明能用 Python 驱动 `hermes acp`（stdio JSON-RPC）。

- [ ] 写一个一次性脚本：spawn `hermes acp`，发 `initialize`（protocol v1，client capabilities 不含 fs/terminal），收响应。
- [ ] 再发 `session/new`，拿到 session_id。
- [ ] 发一个 `session/prompt`（简单问题，如"你好"），打印收到的 `session/update` 通知，确认能拿到流式文本。
- [ ] 确认桥能用哪个 venv：优先复用 hermes venv（已装 `acp`）；否则在桥 venv `pip install acp`。

**产出**: 跑通的握手脚本 + 确定 `acp` 包可用性。这是整个方案的可行性闸口。

---

## Task 1 — Hermes 桥 MVP（对话 + 流式 + 记忆，先自动放行权限）

**文件**: `~/hermes-mcp-xiaozhi/hermes_mcp_server/hermes_bridge_acp.py`（新）+ launchd plist。

- [ ] ACP client 封装：管理 `hermes acp` 子进程生命周期（启动/崩溃重启），initialize 握手。
- [ ] 常驻 session：首次 `session/new`，session_id 存 `~/.hermes/tab5_session.json`；启动时若有则 `session/resume`。
- [ ] HTTP 服务（aiohttp，:8771）：
  - `POST /api/chat {text}` → `session/prompt`，分配 turn id，立即返回 `{id}`。
  - `GET /api/result/<id>` → 累积的 `session/update` → `{status, partial, tools}`。
  - `POST /api/approve/<id>` → 先留桩（本阶段权限自动放行）。
- [ ] `session/update` 处理：把 text delta 拼进 turn 的 partial；工具调用名进 tools[]；结束置 done。
- [ ] `session/request_permission` 处理：本阶段自动回 `approve`（allow_once），先保证能干活。
- [ ] launchd plist + 日志，注册为后台服务。

**验收**: curl `:8771/api/chat` 能拿到 Hermes 流式回复；连续两轮有记忆。

---

## Task 2 — Tab5 改造（Claude → Hermes，repoint）

**文件**: `app/apps/app_project_assistant/`（原地改）。

- [ ] `_bridgeUrl`: 端口 `8770` → `8771`。
- [ ] 品牌: app 名 "Claude"→"Hermes"、header 文案、logo（claude_logo → hermes logo 或暂用文字/复用）。
- [ ] 其余（键盘/语音/流式/历史/审批卡）**不动**。
- [ ] build + flash，验证能与 Hermes 对话、有记忆。

**注**: 类名 `AppProjectAssistant` 可保留（内部名），只改对用户可见的品牌，降低改动面。

---

## Task 3 — 权限审批接通（去掉自动放行）

- [ ] 桥: `session/request_permission` → result.status=permission_required + tool_name/tool_input + options；挂起等待。
- [ ] 桥: `POST /api/approve/<id> {option_id}` → 应答 ACP 请求（approve / approve_for_session / reject）。
- [ ] Tab5: 审批卡已存在；如需，加第 3 个按钮"本次会话都允许"(approve_for_session)。
- [ ] 验收: 触发危险操作弹卡，三种选择都正确生效。

---

## Task 4 — 定时任务验证（基本免开发）

- [ ] 对话说"每天早8点提醒我看未读邮件" → 确认 agent 调用 cron 落地（`hermes cron list` 可见）。
- [ ] 如体验需要，二期再加任务管理 UI（列出/删除 cron）——本计划暂不含。

---

## Task 5 — 语音输入 STT（单独立项，后做）

- [ ] 复用现有语音按钮，接 STT（hermes voice :8767 或小智 STT 链路），音频→文字填进输入框→当普通消息发。
- [ ] 记忆里提过此前卡在协议复用，单独排期，不阻塞主线。

---

## 风险与回滚
- ACP client 实现复杂度（Task 0 先验证）。
- `hermes acp` 进程稳定性（桥负责守护重启）。
- 回滚: Tab5 改动仅 `_bridgeUrl` + 品牌，git revert 即恢复 Claude；桥是独立新服务，停掉 launchd 即可。

## 实施顺序
Task 0（验证）→ Task 1（桥 MVP）→ Task 2（Tab5 repoint，可实测）→ Task 3（权限）→ Task 4（任务验证）→ Task 5（语音，另排）。
