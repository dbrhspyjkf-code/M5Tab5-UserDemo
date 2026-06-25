# M5Tab5 HyperFrames Preproduction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the preproduction package for the M5Tab5 HyperFrames tutorial: storyboard, narration script, asset inventory, and visual identity draft.

**Architecture:** Keep preproduction as plain Markdown under `docs/tutorial/` so the video structure can be reviewed before any HyperFrames HTML is written. The storyboard is the source of truth; the narration script and asset inventory are derived companion documents that reference the same scene numbers.

**Tech Stack:** Markdown, Git, existing project docs, HyperFrames design rules, future HyperFrames HTML composition.

## Global Constraints

- Target audience is A+C mixed: creators/makers plus local AI workflow users.
- The video should feel like a project story, not a feature checklist.
- Recommended title is `把 M5Tab5 变成本地 AI 智能终端`.
- First full version target length is 5 to 7 minutes.
- Format target is 16:9 landscape.
- Do not write HyperFrames HTML until `DESIGN.md`, storyboard, and asset choices are approved.
- Do not present the local Claude workflow as automatic shell execution.
- Do not fake unavailable live metrics or screenshots.
- Source evidence must include `docs/TUTORIAL-outline.md`, git history, HA stability/flashing memory, and Tab5-to-local-Claude memory.

---

## File Structure

- Create `docs/tutorial/m5tab5-hyperframes-storyboard.md`
  - Owns the timecoded scene plan.
  - Uses scene IDs `S1` through `S7`.
  - Each scene includes timestamp, viewer beat, narration summary, visual plan, animation notes, and source references.

- Create `docs/tutorial/m5tab5-hyperframes-voiceover.md`
  - Owns the Chinese narration script.
  - Uses the same scene IDs as the storyboard.
  - Keeps phrasing maker-native and non-marketing.

- Create `docs/tutorial/m5tab5-hyperframes-assets.md`
  - Owns the asset inventory.
  - Separates required, optional, generated, and missing assets.
  - Maps every asset back to scene IDs.

- Create `docs/tutorial/DESIGN.md`
  - Owns the visual identity draft required before HyperFrames HTML.
  - Includes style prompt, colors, typography, motion, and what not to do.
  - Stays a draft until the user approves it for composition work.

- Read-only reference: `docs/superpowers/specs/2026-06-25-m5tab5-hyperframes-tutorial-design.md`
  - Source design approved by the user.

- Read-only reference: `docs/TUTORIAL-outline.md`
  - Primary tutorial content outline.

---

### Task 1: Create The Timecoded Storyboard

**Files:**
- Create: `docs/tutorial/m5tab5-hyperframes-storyboard.md`
- Read: `docs/superpowers/specs/2026-06-25-m5tab5-hyperframes-tutorial-design.md`
- Read: `docs/TUTORIAL-outline.md`

**Interfaces:**
- Consumes: Approved seven-scene design from `docs/superpowers/specs/2026-06-25-m5tab5-hyperframes-tutorial-design.md`.
- Produces: Scene IDs `S1` through `S7`, timestamps, visual plan, animation notes, and source references for Tasks 2 and 3.

- [ ] **Step 1: Read the approved design and outline**

Run:

```bash
sed -n '1,280p' docs/superpowers/specs/2026-06-25-m5tab5-hyperframes-tutorial-design.md
sed -n '1,220p' docs/TUTORIAL-outline.md
```

Expected:

```text
The design includes seven scenes, A+C audience, and HyperFrames production requirements.
The outline lists architecture, HA, Xiaozhi, Claude, tools, email, LED, LoRa, stocks, pitfalls, and flash commands.
```

- [ ] **Step 2: Create storyboard document**

Create `docs/tutorial/m5tab5-hyperframes-storyboard.md` with this exact structure and filled content:

```markdown
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

**Narration summary:** 固件侧负责触摸 UI、App 生命周期和设备交互; Mac 侧负责 Hermes、本地接口、Claude 状态和审批结果。

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

**Narration summary:** 复现路线很朴素: 先用桌面模拟器看 UI, 再构建 Tab5 固件, 用 esptool 烧录, 最后用设备行为和服务状态验证。

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
```

- [ ] **Step 3: Verify scene coverage**

Run:

```bash
rg -n "## S[1-7]|Timestamp:|Source references:" docs/tutorial/m5tab5-hyperframes-storyboard.md
```

Expected:

```text
Each scene S1 through S7 has a timestamp and source references.
```

- [ ] **Step 4: Commit storyboard**

Run:

```bash
git add docs/tutorial/m5tab5-hyperframes-storyboard.md
git commit -m "docs(tutorial): add M5Tab5 HyperFrames storyboard"
```

Expected:

```text
Commit succeeds with one new storyboard file.
```

---

### Task 2: Create The Chinese Voiceover Script

**Files:**
- Create: `docs/tutorial/m5tab5-hyperframes-voiceover.md`
- Read: `docs/tutorial/m5tab5-hyperframes-storyboard.md`

**Interfaces:**
- Consumes: Scene IDs and narration summaries from Task 1.
- Produces: Chinese voiceover text keyed by scene ID for later TTS or human recording.

- [ ] **Step 1: Read storyboard**

Run:

```bash
sed -n '1,260p' docs/tutorial/m5tab5-hyperframes-storyboard.md
```

Expected:

```text
Storyboard includes S1 through S7 and timing from 00:00 through 07:00.
```

- [ ] **Step 2: Create voiceover document**

Create `docs/tutorial/m5tab5-hyperframes-voiceover.md` with this structure and filled script:

```markdown
# M5Tab5 HyperFrames Tutorial Voiceover

## Voice Direction

中文口播。语气像项目作者在复盘一次真实改造: 直接、具体、不端着。少用宣传词, 多用场景词。

## S1 Hook

这块 5 寸屏幕, 一开始只是 M5Tab5 的用户 demo。

但我后来把它改成了一个本地 AI 智能终端。

它可以控制 Home Assistant, 可以跑小智语音, 可以把请求交给 Mac 上的 Hermes 和 Claude, 还能显示邮件、自选股、LoRa 消息, 甚至把股票和邮件状态滚到外接 LED 灯阵上。

所以这期不只是看功能, 我想讲的是: 这个小屏幕怎么变成家里本地系统的入口。

## S2 System Map

先看结构。

固件侧是 ESP-IDF, 上面跑 Mooncake 的多 App 生命周期, UI 用 LVGL 做。

每个功能都是一个 App: Home Assistant、小智、Claude 助手、工具页、灯阵、LoRa、自选股。

Mac 侧跑的是本地服务: Hermes、邮件接口、股票接口, 还有 Claude project gateway。

Tab5 和 Mac 之间只走局域网 HTTP JSON。设备负责触摸和展示, Mac 负责密钥、状态、审批和结果。

这个边界很重要, 后面讲 Claude 工作流的时候会用到。

## S3 Maker Feature Tour

先看一个普通玩家能直接感受到的部分。

Home Assistant 面板可以直连家里的实体, 灯光、家电、门锁、鱼缸、扫地机、打印机这些都能做专门卡片。

小智语音是嵌进固件里的, 不是网页套壳。它有真实音频、在线模式、本地唤醒词和 tap-to-talk。

Claude 助手是另一个方向: 它不是聊天玩具, 而是能围绕这个项目本身提问、看上下文、发请求、读最新结果。

工具页里放了计算器、汇率、单位换算和邮件未读列表。邮件还有第二条通道: 状态栏一个图标, 外接 LED 矩阵还能滚动 NEW EMAIL。

外设这边, Unit-Puzzle 灯阵是 40x8 的 WS2812, 可以跑图案, 也能滚动自定义文字。LoRa Chat 则是用 Unit C6L 做收发聊天气泡。

最后是自选股: 它不是只显示价格, 还有 7 列行情、一句话结论、交易时段刷新, 以及外接 LED ticker。

这些功能堆在一起, 它就不再像一个 demo, 更像桌面上的个人控制台。

## S4 Local AI Loop

这个项目最有意思的部分, 是 Tab5 和本地 AI 工作流的关系。

我没有让设备直接执行命令。Tab5 只负责把意图发给 Mac。

Mac 上的 `claude_project_gateway.py` 收到请求以后, 会把它变成 pending request。

桌面侧再决定是 approve 还是 reject。

批准以后, 它生成的是 Markdown execution packet, 也就是给人或给 Codex 看的执行说明, 不是让设备自己偷偷跑 shell。

执行完以后, 结果会被记录下来, 写进 latest result。

Tab5 再通过 Latest 读取回来。

所以这个闭环是: 小屏幕负责入口, Mac 负责判断和状态, Claude/Codex 负责真正的项目工作。这样既方便, 又不会把密钥和危险执行权塞进设备里。

## S5 Embedded Pitfalls

做这个项目最真实的部分, 不是加了多少 App, 而是每个 App 背后都有坑。

PORT A 会被灯阵、LoRa、邮件 LED 抢, 所以要做 ownership 仲裁。

ESP32-P4 的内存也不是无限的, 小智和多个 UI App 放在一起, 就要做挂起和恢复。

Home Assistant 调试时, 重启问题不能靠猜。真正有用的是串口证据: 什么时候断、是不是 panic、USB 设备有没有从 macOS 消失。

LVGL 字体也会暴露问题, 缺字、刷新、PSRAM 覆盖, 都会在真实设备上出来。

LED 灯阵闪烁, 最后也不是换动画解决, 而是发现 clear 里面已经 refresh 过, 双刷导致闪。

自选股那边, 后端还要把东方财富返回的字符串数字转成 float, 不然固件侧 JSON 解析会崩。

这些坑放在一起, 其实就是一句话: 嵌入式项目要相信硬件证据, 不要只相信代码看起来没问题。

## S6 Build And Flash

复现路线可以分成几层。

UI 改动先看桌面模拟器, 不用每次都烧设备。

真正上 Tab5 的时候, 进入 `platforms/tab5`, 用项目里的 ESP-IDF 环境构建。

如果 `idf.py` 卡住, 这个项目里用 ninja 更稳。

烧录时找 `/dev/cu.usbmodem*`, 然后用 esptool 写 bootloader、partition table、主固件和 srmodels。

最后不要只看烧录成功。要看设备能不能连 WiFi, HA 能不能刷新, Hermes 能不能通, 股票和 LED 有没有真实更新。

## S7 Closing

所以这个项目最有意思的, 不是某一个 App。

而是 M5Tab5 这块屏幕, 变成了本地 AI 和真实设备之间的触摸入口。

它一边连着家里的灯、邮件、股票和外设, 一边连着 Mac 上的 Hermes 和 Claude。

这就是我想要的本地 AI 智能终端。
```

- [ ] **Step 3: Verify script has all scenes**

Run:

```bash
rg -n "^## S[1-7]" docs/tutorial/m5tab5-hyperframes-voiceover.md
```

Expected:

```text
Seven scene headings are printed: S1 through S7.
```

- [ ] **Step 4: Commit voiceover**

Run:

```bash
git add docs/tutorial/m5tab5-hyperframes-voiceover.md
git commit -m "docs(tutorial): add M5Tab5 voiceover script"
```

Expected:

```text
Commit succeeds with one new voiceover file.
```

---

### Task 3: Create Asset Inventory

**Files:**
- Create: `docs/tutorial/m5tab5-hyperframes-assets.md`
- Read: `docs/tutorial/m5tab5-hyperframes-storyboard.md`

**Interfaces:**
- Consumes: Scene IDs and visual plans from Task 1.
- Produces: Asset checklist for HyperFrames production and capture work.

- [ ] **Step 1: Read storyboard visual plans**

Run:

```bash
rg -n "Visual plan:|^- " docs/tutorial/m5tab5-hyperframes-storyboard.md
```

Expected:

```text
The command prints visual bullets for S1 through S7.
```

- [ ] **Step 2: Create asset inventory**

Create `docs/tutorial/m5tab5-hyperframes-assets.md` with this content:

```markdown
# M5Tab5 HyperFrames Tutorial Asset Inventory

## Asset Policy

Use real project evidence first. Use generated or diagrammatic assets only when real screenshots or footage are unavailable. Do not fake live values.

## Required Assets

| Asset ID | Scenes | Type | Source Or Capture Path | Status | Notes |
|---|---|---|---|---|---|
| A001 | S1, S7 | Device hero image | Real M5Tab5 photo or generated product-style render | Missing | Needs clean front-facing frame. |
| A002 | S2, S4 | Architecture diagram data | Derived from repo and gateway memory | Ready to draw | Firmware, app layer, Mac services, LAN HTTP JSON. |
| A003 | S3 | HA app screenshot or simulator capture | `app/apps/app_ha` runtime | Missing | Prefer real device or desktop simulator. |
| A004 | S3 | Xiaozhi app screenshot or still | `app/apps/app_xiaozhi` runtime | Missing | Include idle face or voice state. |
| A005 | S3, S4 | Claude assistant screenshot | `app/apps/app_project_assistant` runtime | Missing | Show quick actions if possible. |
| A006 | S3 | Voice input screenshot | `app/apps/app_voice_input` runtime | Missing | Show waveform UI. |
| A007 | S3 | Tools/settings screenshot | `app/apps/app_settings` runtime | Missing | Calculator, currency, units, mail list. |
| A008 | S3, S5 | LED matrix photo or generated strip visualization | `app/apps/app_unit_puzzle` plus external LED | Missing | Show NEW EMAIL or ticker if available. |
| A009 | S3 | LoRa chat screenshot | `app/apps/app_lora_chat` runtime | Missing | Show chat bubbles and input. |
| A010 | S3 | Stocks screenshot | `app/apps/app_stocks` runtime | Missing | Show table and conclusion line. |
| A011 | S5 | Pitfall cards | Drawn in HyperFrames | Ready to draw | Problem/fix cards. |
| A012 | S6 | Terminal command snippets | `docs/TUTORIAL-outline.md` | Ready | Use shortened command reveal. |

## Optional Assets

| Asset ID | Scenes | Type | Source Or Capture Path | Status | Notes |
|---|---|---|---|---|---|
| O001 | S1 | Short real device montage | Phone footage | Optional | Useful if available. |
| O002 | S6 | Flashing or serial monitor footage | Terminal recording | Optional | Use only if readable. |
| O003 | S5 | Git timeline mini-map | `git log --oneline` | Optional | Helps show project evolution. |

## Generated Assets Allowed

| Asset ID | Scenes | Description | Constraints |
|---|---|---|---|
| G001 | S1, S7 | Clean M5Tab5 device render | Must look like a hardware product frame, not a fake UI screenshot. |
| G002 | S2, S4 | System diagrams | Must use project-accurate labels and boundaries. |
| G003 | S5 | Pitfall icon set | Use simple technical icons, no decorative clutter. |

## Capture Checklist

- [ ] Capture or provide M5Tab5 hero photo.
- [ ] Capture HA screen.
- [ ] Capture Xiaozhi screen.
- [ ] Capture Claude assistant screen.
- [ ] Capture Voice Input screen.
- [ ] Capture Settings tools screen.
- [ ] Capture LED matrix or decide to draw it.
- [ ] Capture LoRa chat screen.
- [ ] Capture Stocks screen.
- [ ] Confirm terminal snippets for build and flash.

## Scene Coverage Check

- S1 has A001 plus app flashes.
- S2 has A002.
- S3 has A003 through A010.
- S4 has A002 and A005.
- S5 has A011.
- S6 has A012.
- S7 has A001 and A002.
```

- [ ] **Step 3: Verify asset IDs are unique**

Run:

```bash
rg -o "^[|] [A-Z][0-9]{3}" docs/tutorial/m5tab5-hyperframes-assets.md | sort | uniq -d
```

Expected:

```text
No output.
```

- [ ] **Step 4: Commit asset inventory**

Run:

```bash
git add docs/tutorial/m5tab5-hyperframes-assets.md
git commit -m "docs(tutorial): add M5Tab5 video asset inventory"
```

Expected:

```text
Commit succeeds with one new asset inventory file.
```

---

### Task 4: Create Visual Identity Draft

**Files:**
- Create: `docs/tutorial/DESIGN.md`
- Read: `docs/superpowers/specs/2026-06-25-m5tab5-hyperframes-tutorial-design.md`

**Interfaces:**
- Consumes: Visual direction from approved design.
- Produces: HyperFrames-ready visual identity draft for later composition work.

- [ ] **Step 1: Read visual direction**

Run:

```bash
sed -n '/## Visual Direction/,/## Narrative Arc/p' docs/superpowers/specs/2026-06-25-m5tab5-hyperframes-tutorial-design.md
```

Expected:

```text
The visual direction calls for a dark technical canvas, crisp diagrams, real UI frames, terminal blocks, and small animated labels.
```

- [ ] **Step 2: Create DESIGN.md**

Create `docs/tutorial/DESIGN.md` with this content:

```markdown
# M5Tab5 Tutorial Visual Identity

## Status

Draft for review before HyperFrames HTML composition.

## Style Prompt

Dark technical maker documentary. The canvas should feel like a precise local control room: crisp device frames, readable diagrams, terminal snippets, and compact UI cards. Use color to separate hardware, local services, AI handoff, and smart-home control. Keep the style practical, modern, and evidence-driven rather than futuristic for its own sake.

## Colors

| Role | Hex | Use |
|---|---|---|
| Canvas | `#0B0F14` | Main background. |
| Panel | `#141B24` | Diagram blocks and app cards. |
| Text Primary | `#F4F7FA` | Titles and main labels. |
| Text Secondary | `#A9B4C0` | Captions and source paths. |
| Hardware Accent | `#35D0BA` | M5Tab5 and firmware layer. |
| AI Accent | `#8B7CFF` | Claude and AI workflow. |
| Service Accent | `#FFB020` | Hermes and Mac services. |
| Smart Home Accent | `#46A6FF` | Home Assistant and device control. |
| Warning Accent | `#FF5C5C` | Pitfalls and blocked execution. |

## Typography

- Display: `Inter` or `Source Han Sans SC`.
- Body: `Source Han Sans SC`.
- Mono: `JetBrains Mono`.
- Keep Chinese narration captions large and sparse. Do not place dense paragraphs on screen.

## Layout Rules

- Use 16:9 landscape at 1920x1080.
- Keep scene content inside generous margins.
- Prefer diagrams, cards, and source captions over long text.
- Source paths should be small captions, not the primary focus.
- App tour cards should show one use case per card.

## Motion Rules

- Scene transitions are required between all major scenes.
- Use entrance animations on every major element.
- Favor precise slides, fades, wipes, and diagram reveals.
- Avoid chaotic movement; the video should feel controlled and useful.
- Local AI sequence should animate one step at a time.
- Pitfall cards should reveal problem first, fix second.

## What NOT To Do

- Do not use generic cyberpunk neon styling.
- Do not use fake dashboards, fake metrics, or fake screenshots.
- Do not overload the screen with code.
- Do not imply Tab5 directly executes shell commands.
- Do not make it look like a product landing page.
```

- [ ] **Step 3: Verify required HyperFrames visual identity sections**

Run:

```bash
rg -n "^## (Style Prompt|Colors|Typography|What NOT To Do)" docs/tutorial/DESIGN.md
```

Expected:

```text
All four required headings are present.
```

- [ ] **Step 4: Commit visual identity draft**

Run:

```bash
git add docs/tutorial/DESIGN.md
git commit -m "docs(tutorial): add M5Tab5 visual identity draft"
```

Expected:

```text
Commit succeeds with one new DESIGN.md file.
```

---

### Task 5: Cross-Check Preproduction Package

**Files:**
- Modify: `docs/tutorial/m5tab5-hyperframes-storyboard.md` if consistency fixes are needed.
- Modify: `docs/tutorial/m5tab5-hyperframes-voiceover.md` if consistency fixes are needed.
- Modify: `docs/tutorial/m5tab5-hyperframes-assets.md` if consistency fixes are needed.
- Modify: `docs/tutorial/DESIGN.md` if consistency fixes are needed.

**Interfaces:**
- Consumes: Outputs from Tasks 1 through 4.
- Produces: A reviewed preproduction package ready for user review and later HyperFrames planning.

- [ ] **Step 1: Verify expected files exist**

Run:

```bash
test -f docs/tutorial/m5tab5-hyperframes-storyboard.md
test -f docs/tutorial/m5tab5-hyperframes-voiceover.md
test -f docs/tutorial/m5tab5-hyperframes-assets.md
test -f docs/tutorial/DESIGN.md
```

Expected:

```text
All commands exit with status 0.
```

- [ ] **Step 2: Scan for placeholders**

Run:

```bash
rg -n "[T]BD|[T]ODO|[P]LACEHOLDER|待[定]|以后[再]说|\\?\\?" docs/tutorial docs/superpowers/plans/2026-06-25-m5tab5-hyperframes-preproduction.md
```

Expected:

```text
No output.
```

- [ ] **Step 3: Verify all scene IDs appear across storyboard, voiceover, and assets**

Run:

```bash
for scene in S1 S2 S3 S4 S5 S6 S7; do
  rg -q "$scene" docs/tutorial/m5tab5-hyperframes-storyboard.md
  rg -q "$scene" docs/tutorial/m5tab5-hyperframes-voiceover.md
  rg -q "$scene" docs/tutorial/m5tab5-hyperframes-assets.md
done
```

Expected:

```text
All commands exit with status 0.
```

- [ ] **Step 4: Review git diff**

Run:

```bash
git status --short
git diff -- docs/tutorial docs/superpowers/plans/2026-06-25-m5tab5-hyperframes-preproduction.md
```

Expected:

```text
Only the preproduction files and this plan are changed.
```

- [ ] **Step 5: Commit any consistency fixes**

If Step 4 shows uncommitted consistency fixes, run:

```bash
git add docs/tutorial docs/superpowers/plans/2026-06-25-m5tab5-hyperframes-preproduction.md
git commit -m "docs(tutorial): polish M5Tab5 preproduction package"
```

Expected:

```text
Commit succeeds, or there is nothing to commit because earlier tasks already committed all files.
```

---

## Self-Review Checklist

- The plan covers storyboard, voiceover, asset inventory, and visual identity draft.
- The plan does not write HyperFrames HTML before storyboard and visual identity approval.
- The local Claude workflow is described as request, approval, execution packet, and result readback, not direct shell execution.
- The asset policy forbids fake live metrics and fake screenshots.
- Each task produces an independently reviewable document and commit.
- The plan leaves final video implementation for a later HyperFrames-specific plan.
