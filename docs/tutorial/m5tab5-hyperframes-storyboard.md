# M5Tab5 HyperFrames Tutorial Storyboard

## Video Promise

把 M5Tab5 变成本地 AI 智能终端: 一块 5 寸触摸屏, 连接家里的设备、Mac 上的 Hermes、本地 Claude 工作流, 也保留真实嵌入式项目该有的稳定性边界。

## Timing Overview

| Scene | Time | Duration | Purpose |
|---|---:|---:|---|
| S1 Hook | 00:00-00:18 | 18s | Establish that this is a real local AI terminal, not a screen demo. |
| S2 System Map | 00:18-01:00 | 42s | Explain firmware, app layer, Mac services, and LAN boundary. |
| S3 Maker Feature Tour | 01:00-03:05 | 125s | Show what a maker can use directly. |
| S4 Local AI Loop | 03:05-04:35 | 90s | Explain Tab5 + Hermes + Claude request/approval/result flow. |
| S5 Embedded Pitfalls | 04:35-05:50 | 75s | Show the hard-earned engineering lessons. |
| S6 Build And Flash | 05:50-06:45 | 55s | Give the reproduction path. |
| S7 Closing | 06:45-07:00 | 15s | Restate the project identity. |

## S1 Hook: What This Device Became

**Timestamp:** 00:00-00:18

**Viewer beat:** In the first few seconds, the viewer understands that the M5Tab5 is being used as a real home/local-AI terminal.

**Narration summary:** 这不是普通屏幕 demo; 它是一个本地入口, 可以触发家里的设备、语音助手、Mac 上的 Hermes 和 Claude 工作流。

**Visual plan:**
- Open on a clean M5Tab5 device frame.
- Fast flashes: HA panel, Xiaozhi, Claude assistant, email/stock status, LED ticker.
- Four orbit nodes around the device: `Home Assistant`, `Xiaozhi`, `Hermes`, `Claude`.

**Animation notes:**
- Device enters first.
- App flashes appear as quick cards around it.
- End with title lockup: `把 M5Tab5 变成本地 AI 智能终端`.

**Source references:**
- `docs/TUTORIAL-outline.md`: hardware, framework, app list.
- Git commits `6208c17`, `349a065`, `2e1fdab`, `fb276f2`.

## S2 System Map: The System Under The Screen

**Timestamp:** 00:18-01:00

**Viewer beat:** The viewer sees that the project is split between firmware UI and Mac-side local services.

**Narration summary:** 固件侧负责触摸 UI、App 生命周期和设备交互; Mac 侧负责 Hermes、本地接口、Claude 状态和审批结果.

**Visual plan:**
- Left block: `M5Tab5 Firmware`.
- Inside it: `ESP-IDF`, `Mooncake`, `LVGL`, `AppHome`.
- Middle app row: `HA`, `Xiaozhi`, `Claude`, `Tools`, `LED`, `LoRa`, `Stocks`.
- Right block: `Mac Local Services`.
- Inside it: `Hermes`, `Mail :8768`, `Stocks :8766`, `Claude Gateway`.
- Highlight connection: `LAN HTTP JSON`.

**Animation notes:**
- Build blocks layer by layer from bottom to top.
- Boundary line appears before service arrows.
- Use a lock or shield label for `secrets stay on Mac`.

**Source references:**
- `docs/TUTORIAL-outline.md`: foundation section.
- Project memory: project assistant gateway boundary.

## S3 Maker Feature Tour: What A Maker Sees

**Timestamp:** 01:00-03:05

**Viewer beat:** The viewer sees practical, visible use cases before hearing deeper architecture.

**Narration summary:** 先看能直接用的东西: 智能家居、语音、Claude 对话、工具、邮件、灯阵、LoRa、自选股。

**Visual plan:**
- Use a horizontal app carousel or 3x3 grid.
- Each feature card contains app name, use case, and repo module.
- Cards:
  - `Home Assistant` / `app/apps/app_ha`
  - `Xiaozhi` / `app/apps/app_xiaozhi`
  - `Claude Assistant` / `app/apps/app_project_assistant`
  - `Voice Input` / `app/apps/app_voice_input`
  - `Tools` / `app/apps/app_settings`
  - `Email LED` / `app/apps/app_email_led`
  - `Unit-Puzzle LED` / `app/apps/app_unit_puzzle`
  - `LoRa Chat` / `app/apps/app_lora_chat`
  - `Stocks` / `app/apps/app_stocks`

**Animation notes:**
- Cards enter by category: home, AI, utilities, hardware extensions.
- Keep labels short; avoid long paragraphs inside cards.
- Use source path as small technical caption.

**Source references:**
- `docs/TUTORIAL-outline.md`: sections 1 through 8.
- Git commits `fde9d53`, `2e1fdab`, `1a0a65a`, `ae207a7`, `cfb51a8`, `fb276f2`.

## S4 Local AI Loop: The Memorable Technical Idea

**Timestamp:** 03:05-04:35

**Viewer beat:** The viewer understands why the Tab5-to-Claude flow is safe and interesting.

**Narration summary:** Tab5 不直接执行危险命令。它只把意图送到 Mac; Mac 侧排队、审批、生成执行包、记录结果; Tab5 再读取最新结果。

**Visual plan:**
- Seven-step sequence diagram:
  1. `Request from Tab5`
  2. `LAN HTTP JSON`
  3. `claude_project_gateway.py`
  4. `pending request`
  5. `desktop approve/reject`
  6. `Markdown execution packet`
  7. `Latest result back to Tab5`
- Quick action badges: `Context`, `Plan`, `Request`, `Latest`.
- Guardrail strip: `no secrets on device`, `no direct shell execution`, `local approval`.

**Animation notes:**
- Animate one step at a time.
- Use a hard visual stop for rejected direct execution.
- Latest-result arrow returns to the device at the end.

**Source references:**
- `scripts/claude_project_gateway.py`
- `scripts/project_request_review.py`
- `app/apps/app_project_assistant/app_project_assistant.cpp`
- Project memory: project assistant phases 1, 3, 4A, 4B.

## S5 Embedded Pitfalls: Why This Is Real Embedded Work

**Timestamp:** 04:35-05:50

**Viewer beat:** The viewer sees that the project was made robust through real constraints, not just feature stacking.

**Narration summary:** 真正的嵌入式项目, 难点常常不是能不能写出来, 而是端口、内存、字体、刷新、串口证据和服务边界能不能扛住。

**Visual plan:**
- Split cards, each with `Problem` and `Fix`.
- Include:
  - PORT A ownership conflict -> `setPortAOwnedByApp`
  - P4 memory pressure -> app suspend/resume
  - USB Host/HID instability -> disable unused host path for HA validation
  - LVGL glyph warnings -> font coverage and safer labels
  - LED double refresh flicker -> clear buffer once, refresh once
  - Eastmoney string numbers -> backend float conversion
  - PSRAM/font hazard -> small font table in rodata

**Animation notes:**
- Each problem card flips or wipes into its fix.
- End with a large line: `硬件证据 > 猜测`.

**Source references:**
- `docs/TUTORIAL-outline.md`: pitfalls table.
- Project memory: HA stability and flashing.

## S6 Build And Flash: The Reproduction Path

**Timestamp:** 05:50-06:45

**Viewer beat:** The viewer sees the rough path to reproduce without being buried in terminal detail.

**Narration summary:** 复现路线很朴素: 先用桌面模拟器看 UI, 再构建 Tab5 固件, 用 esptool 烧录, 最后用设备行为和服务状态验证.

**Visual plan:**
- Terminal panel with commands:
  - `source ~/.local/bin/idf_env.sh`
  - `cd ~/Projects/M5Tab5/M5Tab5-UserDemo/platforms/tab5`
  - `ninja -C build`
  - `python -m esptool ... write_flash ...`
- Checklist: `Boot`, `WiFi`, `HA`, `Hermes`, `Stocks`, `LED`.

**Animation notes:**
- Type commands in short chunks.
- Keep full flash command readable by using horizontal scroll-style reveal or line grouping.
- Checklist ticks after commands finish.

**Source references:**
- `docs/TUTORIAL-outline.md`: build and flash commands.
- Project memory: stable ESP-IDF build/flash validation path.

## S7 Closing: What The Project Really Is

**Timestamp:** 06:45-07:00

**Viewer beat:** The viewer remembers the project identity in one sentence.

**Narration summary:** 最有意思的不是某一个 App, 而是这块屏幕变成了本地 AI 和真实设备之间的触摸入口。

**Visual plan:**
- Return to the system map.
- Collapse service nodes into the M5Tab5 screen.
- End title: `M5Tab5 Local AI Terminal`.

**Animation notes:**
- Slow convergence animation.
- Fade to title; no extra feature list.

**Source references:**
- `docs/superpowers/specs/2026-06-25-m5tab5-hyperframes-tutorial-design.md`.
