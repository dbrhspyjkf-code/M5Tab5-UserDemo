# M5Tab5 HyperFrames Tutorial Design

## Goal

Create a HyperFrames-ready tutorial for the M5Tab5 project aimed at a mixed audience:

- Creators and makers who want to see what the device can do and how to reproduce the path.
- Local AI workflow users who care about the Tab5 + Hermes + Claude loop.

The video should feel like a project story, not a feature checklist. The central message is:

> M5Tab5 can become a local AI smart terminal: a touch entry point for Home Assistant, voice, local services, device status, and safe Claude handoff workflows.

## Source Material

Primary source:

- `docs/TUTORIAL-outline.md`

Project evidence:

- Git history from `2026-06-13` through `2026-06-23`, especially the commits that introduced the multi-app architecture, Xiaozhi, Claude assistant, tools, Unit-Puzzle LED matrix, LoRa chat, email LED, and stocks app.
- Project memory for Home Assistant stability and flashing, including the serial-debugging path.
- Project memory for the phased Tab5-to-local-Claude project assistant, including the LAN gateway, approval queue, execution packet, and latest-result feedback loop.

## Audience And Tone

The voice should be Chinese, practical, and maker-native. It should avoid sounding like a marketing page or a dry firmware lecture.

The viewer should leave with three impressions:

1. This is a real personal hardware project, not a mockup.
2. The most interesting part is the local loop between a small touch device and Mac-side AI services.
3. Embedded projects become good when stability, memory, ports, and recovery paths are designed deliberately.

## Recommended Video Shape

Format:

- 16:9 landscape.
- 5 to 7 minutes for the first full version.
- Built in HyperFrames as a multi-scene composition.
- Use motion graphics, UI screenshots, code callouts, terminal snippets, and short device-demo clips or stills.

Recommended title:

> 把 M5Tab5 变成本地 AI 智能终端

Alternative titles:

- M5Tab5 本地 AI 控制台改造记录
- 用 M5Tab5 做一个桌面上的 AI + 智能家居终端

## Visual Direction

The visual identity should be defined in a later `DESIGN.md` before any HyperFrames HTML is written.

Recommended style:

- Dark technical canvas, but not generic cyberpunk.
- Accent colors should distinguish hardware, local service, AI handoff, and smart-home control.
- Use crisp diagrams, real UI frames, terminal blocks, and small animated labels.
- Avoid heavy decorative gradients, vague stock imagery, and oversized marketing hero sections.

Suggested visual motifs:

- A device at center, with local Mac/Hermes/Claude and Home Assistant as surrounding nodes.
- Commit timeline as a living build history.
- LAN gateway as a guarded bridge rather than a magic black box.
- Pitfalls as compact warning cards with the fix shown immediately beside each one.

## Narrative Arc

### 1. Hook: What This Device Became

Duration: 15 to 20 seconds.

Viewer experience:

- Rapid montage of M5Tab5 roles: Home Assistant panel, Xiaozhi voice, Claude project assistant, email/stock status, LED ticker.
- The narration frames the device as a local AI terminal, not just a screen demo.

Core message:

> 这块 5 寸屏幕最后变成的是一个本地入口: 家里的设备、Mac 上的 Hermes、Claude 工作流, 都能从它这里被触发和查看。

Visuals:

- Device silhouette or real photo if available.
- Four labeled nodes: HA, Xiaozhi, Hermes, Claude.
- Quick flashes of app names from the repo.

### 2. Project Map: The System Under The Screen

Duration: 35 to 45 seconds.

Viewer experience:

- A simple architecture map explains the layers without diving into source code.

Core message:

> 固件侧负责 UI 和设备交互; Mac 侧负责 Hermes、本地接口和 Claude 状态; 两边通过局域网 HTTP 连接。

Visuals:

- M5Tab5 firmware block: ESP-IDF, Mooncake, LVGL.
- App layer: Home, HA, Xiaozhi, Claude assistant, Settings, LED, LoRa, Stocks.
- Mac service block: Hermes, mail, stocks API, Claude gateway.
- A highlighted boundary: Tab5 sends requests; Mac owns secrets, approvals, and execution artifacts.

### 3. Feature Tour: What A Maker Sees

Duration: 100 to 130 seconds.

Viewer experience:

- Friendly tour of major apps, each tied to a use case.
- Avoid explaining every field. Show what each app is for.

Scene beats:

- Home Assistant: one touch controls real entities and shows dedicated device cards.
- Xiaozhi: voice assistant embedded inside the Tab5 firmware.
- Claude assistant: talk to local project context and read results back later.
- Voice input: waveform recording UI and keyboard support.
- Settings tools: calculator, currency, unit conversion, unread email list.
- Email LED: unread mail becomes visible even when not looking at the screen.
- Unit-Puzzle LED matrix: text and patterns on a 40x8 LED strip.
- LoRa chat: local message bubbles over Unit C6L.
- Stocks: portfolio table, conclusion line, market-hours refresh, LED ticker.

Visuals:

- One compact screen card per app.
- Each card shows: app name, real use case, repo module path.
- Use icon or UI screenshot when available.

### 4. Local AI Loop: The Memorable Technical Idea

Duration: 75 to 95 seconds.

Viewer experience:

- Slow down and make the local AI workflow understandable.

Core message:

> Tab5 不是一台会偷偷执行命令的设备。它只负责把意图送到 Mac; Mac 侧保存队列、审批、执行包和结果; Tab5 再读取最新结果。

Flow:

1. User sends a project request from Tab5.
2. `scripts/claude_project_gateway.py` receives LAN HTTP JSON.
3. Gateway stores a pending request.
4. Desktop review approves or rejects it.
5. Approved request becomes a Markdown execution packet.
6. Result is recorded and written to latest-result storage.
7. Tab5 reads back the latest result.

Visuals:

- Sequence diagram with seven steps.
- Guardrail labels: no secrets on device, no direct shell execution from Tab5, local approval required.
- Highlight `Context`, `Plan`, `Request`, and `Latest` quick actions.

### 5. Pitfalls: Why This Is Real Embedded Work

Duration: 65 to 80 seconds.

Viewer experience:

- Fast, satisfying list of hard-earned engineering lessons.

Pitfalls to include:

- PORT A ownership conflict across LED matrix, LoRa, and email LED.
- P4 memory pressure and app suspend/resume.
- USB Host/HID instability during HA runtime validation.
- LVGL font and glyph problems under real refresh conditions.
- LED refresh flicker caused by clearing and refreshing twice.
- Eastmoney API returning string numbers that need conversion.
- PSRAM/font data hazards solved by keeping the small dot-matrix font in rodata.

Visuals:

- Split cards: problem on the left, fix on the right.
- Use concise labels, not long paragraphs.
- Show the most important lesson last: hardware evidence beats guessing.

### 6. Build And Flash: The Reproduction Path

Duration: 45 to 60 seconds.

Viewer experience:

- Give a practical path without turning the video into a terminal tutorial.

Steps:

1. Run or inspect the desktop simulator when UI changes do not need hardware.
2. Build the Tab5 target from `platforms/tab5`.
3. Use the project’s stable flashing command path.
4. Confirm runtime behavior on the device.
5. For Hermes features, make sure the Mac-side service is running.

Visuals:

- Terminal block with the short command sequence from `docs/TUTORIAL-outline.md`.
- Device validation checklist: boots, WiFi, HA, Hermes, stocks, LED.

### 7. Closing: What The Project Really Is

Duration: 10 to 15 seconds.

Core message:

> 最有意思的不是某一个 app, 而是这块屏幕变成了本地 AI 和真实设备之间的入口。

Visuals:

- Return to the architecture map.
- Collapse the nodes into the M5Tab5 screen.
- End on project title and repo path.

## HyperFrames Production Requirements

Before writing composition HTML:

1. Create `DESIGN.md` with exact colors, typography, and motion rules.
2. Decide whether the video will use real device footage, simulator screenshots, generated device mockups, or a mix.
3. Convert this design into a storyboard file with columns: timestamp, scene, narration, visual assets, animation notes, source references.
4. Prepare a small asset inventory:
   - Tab5 device photo or generated product-style render.
   - App screenshots or simulator captures.
   - Terminal snippets.
   - Simplified architecture diagram data.
   - Optional voiceover script.

HyperFrames constraints:

- Use scene transitions between all major scenes.
- Use entrance animation for each scene element.
- Keep text large enough for 1080p video.
- Avoid dense code blocks; use short highlighted snippets.
- Use `npx hyperframes lint`, `validate`, and `inspect` before rendering.

## Non-Goals

- Do not make a full firmware programming course.
- Do not explain every commit in chronological order.
- Do not present the local Claude workflow as automatic shell execution.
- Do not fake unavailable live metrics or screenshots.
- Do not generate HyperFrames HTML until visual identity and asset choices are approved.

## Success Criteria

- A maker can understand what the project does and why it is useful.
- A local-AI user can understand the Tab5-to-Hermes-to-Claude workflow and its safety boundary.
- The tutorial has enough structure to become a HyperFrames storyboard without another full repo archaeology pass.
- The video emphasizes real project evidence: git history, app modules, gateway design, and hardware-debugging lessons.
