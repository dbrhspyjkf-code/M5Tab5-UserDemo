# M5Tab5 HyperFrames Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a first playable HyperFrames tutorial video project from the approved M5Tab5 storyboard, voiceover, asset inventory, and visual identity.

**Architecture:** Create an isolated HyperFrames project under `docs/tutorial/video/m5tab5-local-ai-terminal/` so video source files do not mix with firmware code. Use `index.html` as the root composition, local CSS/JS modules for reusable scene data and timeline helpers, and generated/diagrammatic placeholders where real screenshots are missing. The first implementation should be reviewable in the HyperFrames Studio before final rendering.

**Tech Stack:** HyperFrames CLI (`npx hyperframes`), HTML/CSS, GSAP timelines, Markdown preproduction docs, generated SVG/HTML diagram assets, optional real screenshots later.

## Global Constraints

- Source preproduction files are `docs/tutorial/m5tab5-hyperframes-storyboard.md`, `docs/tutorial/m5tab5-hyperframes-voiceover.md`, `docs/tutorial/m5tab5-hyperframes-assets.md`, and `docs/tutorial/DESIGN.md`.
- Target audience is A+C mixed: creators/makers plus local AI workflow users.
- The video should feel like a project story, not a feature checklist.
- Title is `把 M5Tab5 变成本地 AI 智能终端`.
- First full version target length is exactly 420 seconds (`00:00` through `07:00`).
- Format target is 16:9 landscape at `1920x1080`.
- Do not fake unavailable live metrics or screenshots.
- Missing real assets must be represented as clearly labeled generated diagrams, device frames, or placeholder cards, not fake UI screenshots.
- Local Claude workflow must be represented as request, approval, execution packet, and latest-result readback, not automatic shell execution.
- Every multi-scene composition must use scene transitions and entrance animations.
- Every scene element must be readable at 1080p; avoid dense paragraphs on screen.
- Run `npx hyperframes lint`, `npx hyperframes validate`, and `npx hyperframes inspect` before calling the composition ready for preview/render.

---

## File Structure

- Create `docs/tutorial/video/m5tab5-local-ai-terminal/`
  - HyperFrames project root.

- Create `docs/tutorial/video/m5tab5-local-ai-terminal/index.html`
  - Root HyperFrames composition.
  - Owns the seven timed scene clips and composition registration.

- Create `docs/tutorial/video/m5tab5-local-ai-terminal/src/styles.css`
  - Visual system from `docs/tutorial/DESIGN.md`.
  - Scene layout, typography, cards, device frame, diagrams, timeline ribbon, pitfall cards.

- Create `docs/tutorial/video/m5tab5-local-ai-terminal/src/story-data.js`
  - Scene timing, feature cards, AI loop steps, pitfall data, build commands.
  - Keeps content data separate from animation logic.

- Create `docs/tutorial/video/m5tab5-local-ai-terminal/src/timeline.js`
  - GSAP timeline construction and scene transition choreography.
  - Registers `window.__timelines["m5tab5-local-ai-terminal"]`.

- Create `docs/tutorial/video/m5tab5-local-ai-terminal/assets/`
  - Generated device-frame SVG, placeholder app panels, and diagram labels.
  - Real screenshots can be dropped here later without changing scene structure.

- Create `docs/tutorial/video/m5tab5-local-ai-terminal/README.md`
  - How to preview, validate, inspect, and render.
  - Documents which assets are placeholders and which are evidence-backed diagrams.

- Modify `docs/tutorial/m5tab5-hyperframes-assets.md`
  - Add implementation status notes only if new generated asset files are created.

---

### Task 1: Scaffold The HyperFrames Project Shell

**Files:**
- Create: `docs/tutorial/video/m5tab5-local-ai-terminal/index.html`
- Create: `docs/tutorial/video/m5tab5-local-ai-terminal/src/styles.css`
- Create: `docs/tutorial/video/m5tab5-local-ai-terminal/src/story-data.js`
- Create: `docs/tutorial/video/m5tab5-local-ai-terminal/src/timeline.js`
- Create: `docs/tutorial/video/m5tab5-local-ai-terminal/README.md`

**Interfaces:**
- Consumes: `docs/tutorial/DESIGN.md` and the HyperFrames root composition contract.
- Produces: A minimal 420-second, 1920x1080 HyperFrames composition shell with registered timeline and no blank render.

- [ ] **Step 1: Check HyperFrames environment**

Run:

```bash
npx hyperframes doctor
```

Expected:

```text
Doctor reports usable Node, Chrome, and FFmpeg. If a tool is missing, record the exact failure in the task report and stop before editing.
```

- [ ] **Step 2: Create project directories**

Run:

```bash
mkdir -p docs/tutorial/video/m5tab5-local-ai-terminal/src
mkdir -p docs/tutorial/video/m5tab5-local-ai-terminal/assets
```

Expected:

```text
Both directories exist.
```

- [ ] **Step 3: Create data module**

Create `docs/tutorial/video/m5tab5-local-ai-terminal/src/story-data.js`:

```js
export const duration = 420;

export const scenes = [
  { id: "s1", title: "Hook", start: 0, duration: 18 },
  { id: "s2", title: "System Map", start: 18, duration: 42 },
  { id: "s3", title: "Maker Feature Tour", start: 60, duration: 125 },
  { id: "s4", title: "Local AI Loop", start: 185, duration: 90 },
  { id: "s5", title: "Embedded Pitfalls", start: 275, duration: 75 },
  { id: "s6", title: "Build And Flash", start: 350, duration: 55 },
  { id: "s7", title: "Closing", start: 405, duration: 15 },
];

export const featureCards = [
  ["Home Assistant", "直连家里的真实实体", "app/apps/app_ha"],
  ["Xiaozhi", "固件内的语音助手", "app/apps/app_xiaozhi"],
  ["Claude Assistant", "项目请求与结果回读", "app/apps/app_project_assistant"],
  ["Voice Input", "录音波形与键盘输入", "app/apps/app_voice_input"],
  ["Tools", "计算器 / 汇率 / 单位 / 邮件", "app/apps/app_settings"],
  ["Email LED", "未读邮件外接提示", "app/apps/app_email_led"],
  ["Unit-Puzzle LED", "40x8 灯阵与 ticker", "app/apps/app_unit_puzzle"],
  ["LoRa Chat", "本地消息气泡", "app/apps/app_lora_chat"],
  ["Stocks", "自选股行情与结论", "app/apps/app_stocks"],
];

export const aiLoopSteps = [
  "Request from Tab5",
  "LAN HTTP JSON",
  "claude_project_gateway.py",
  "Pending request",
  "Desktop approve / reject",
  "Markdown execution packet",
  "Latest result back to Tab5",
];

export const pitfalls = [
  ["PORT A 抢占", "setPortAOwnedByApp 仲裁"],
  ["P4 内存压力", "App 挂起 / 恢复"],
  ["USB Host 不稳", "HA 验证禁用无用路径"],
  ["LVGL 缺字", "字体覆盖与安全标签"],
  ["LED 双刷闪烁", "只清缓冲, 末尾刷新"],
  ["行情字符串数字", "Hermes 端转 float"],
  ["PSRAM 字形风险", "小点阵字体进 rodata"],
];

export const buildCommands = [
  "source ~/.local/bin/idf_env.sh",
  "cd ~/Projects/M5Tab5/M5Tab5-UserDemo/platforms/tab5",
  "ninja -C build",
  "python -m esptool --chip esp32p4 ... write_flash ...",
];
```

- [ ] **Step 4: Create CSS visual system**

Create `docs/tutorial/video/m5tab5-local-ai-terminal/src/styles.css` with:

```css
:root {
  --canvas: #0B0F14;
  --panel: #141B24;
  --text: #F4F7FA;
  --muted: #A9B4C0;
  --hardware: #35D0BA;
  --ai: #8B7CFF;
  --service: #FFB020;
  --home: #46A6FF;
  --warning: #FF5C5C;
}

* {
  box-sizing: border-box;
}

body {
  margin: 0;
  background: var(--canvas);
  color: var(--text);
  font-family: "Source Han Sans SC", "Inter", sans-serif;
}

[data-composition-id="m5tab5-local-ai-terminal"] {
  width: 1920px;
  height: 1080px;
  overflow: hidden;
  background: var(--canvas);
  position: relative;
}

.scene {
  position: absolute;
  inset: 0;
  padding: 72px 96px;
  display: flex;
  flex-direction: column;
  gap: 32px;
  background: radial-gradient(circle at 50% 40%, rgba(53, 208, 186, 0.10), transparent 36%), var(--canvas);
}

.eyebrow {
  color: var(--hardware);
  font-size: 28px;
  font-weight: 700;
}

.title {
  font-size: 78px;
  line-height: 1.08;
  margin: 0;
  max-width: 1320px;
}

.subtitle {
  color: var(--muted);
  font-size: 34px;
  line-height: 1.35;
  max-width: 1180px;
}

.device-map,
.feature-grid,
.ai-flow,
.pitfall-grid,
.terminal-wrap {
  flex: 1;
  min-height: 0;
}

.device {
  width: 520px;
  aspect-ratio: 16 / 9;
  border: 14px solid #202B36;
  border-radius: 34px;
  background: linear-gradient(135deg, #101820, #17222d);
  box-shadow: 0 24px 80px rgba(0, 0, 0, 0.45), 0 0 60px rgba(53, 208, 186, 0.16);
}

.panel,
.card,
.step,
.pitfall,
.terminal {
  background: var(--panel);
  border: 1px solid rgba(244, 247, 250, 0.10);
  border-radius: 8px;
}
```

Then continue in the same file with focused classes for `.node`, `.timeline-ribbon`, `.feature-grid`, `.ai-flow`, `.pitfall-grid`, `.terminal`, and `.transition-wipe`. Keep every text container width-constrained and readable.

- [ ] **Step 5: Create root composition HTML**

Create `docs/tutorial/video/m5tab5-local-ai-terminal/index.html` as a standalone HyperFrames composition without a `<template>` wrapper:

```html
<!doctype html>
<html lang="zh-CN">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>把 M5Tab5 变成本地 AI 智能终端</title>
    <link rel="stylesheet" href="./src/styles.css" />
  </head>
  <body>
    <div
      data-composition-id="m5tab5-local-ai-terminal"
      data-width="1920"
      data-height="1080"
      data-duration="420"
    >
      <section id="s1" class="scene scene-hook">
        <div class="eyebrow">M5Tab5 Local AI Terminal</div>
        <h1 class="title">把 M5Tab5 变成本地 AI 智能终端</h1>
        <p class="subtitle">一块 5 寸触摸屏, 连接 Home Assistant、Hermes、Claude 和真实设备。</p>
        <div class="device-map" data-layout-allow-overflow></div>
      </section>
      <section id="s2" class="scene scene-map"></section>
      <section id="s3" class="scene scene-features"></section>
      <section id="s4" class="scene scene-ai"></section>
      <section id="s5" class="scene scene-pitfalls"></section>
      <section id="s6" class="scene scene-build"></section>
      <section id="s7" class="scene scene-close"></section>
      <div class="transition-wipe" data-layout-ignore></div>
    </div>
    <script src="https://cdn.jsdelivr.net/npm/gsap@3.14.2/dist/gsap.min.js"></script>
    <script type="module" src="./src/timeline.js"></script>
  </body>
</html>
```

- [ ] **Step 6: Create timeline registration**

Create `docs/tutorial/video/m5tab5-local-ai-terminal/src/timeline.js` with a minimal registered timeline that reveals each scene and transition:

```js
import { scenes } from "./story-data.js";

window.__timelines = window.__timelines || {};

const tl = gsap.timeline({ paused: true });

scenes.forEach((scene, index) => {
  const selector = `#${scene.id}`;
  tl.set(selector, { opacity: 1 }, scene.start);
  tl.from(`${selector} .eyebrow, ${selector} .title, ${selector} .subtitle`, {
    y: 36,
    opacity: 0,
    duration: 0.7,
    stagger: 0.12,
    ease: "power3.out",
  }, scene.start + 0.2);

  if (index > 0) {
    tl.fromTo(".transition-wipe", {
      xPercent: -110,
      opacity: 1,
    }, {
      xPercent: 110,
      duration: 0.65,
      ease: "power2.inOut",
    }, scene.start - 0.35);
  }
});

tl.to("#s7", { opacity: 0, duration: 0.6, ease: "power2.in" }, 419.2);

window.__timelines["m5tab5-local-ai-terminal"] = tl;
```

- [ ] **Step 7: Create README**

Create `docs/tutorial/video/m5tab5-local-ai-terminal/README.md`:

```markdown
# M5Tab5 Local AI Terminal HyperFrames Video

Source composition for the M5Tab5 tutorial video.

## Commands

```bash
npx hyperframes lint
npx hyperframes validate
npx hyperframes inspect --samples 15
npx hyperframes preview
npx hyperframes render --quality draft --output renders/m5tab5-local-ai-terminal-draft.mp4
```

## Source Docs

- `../../m5tab5-hyperframes-storyboard.md`
- `../../m5tab5-hyperframes-voiceover.md`
- `../../m5tab5-hyperframes-assets.md`
- `../../DESIGN.md`

## Asset Policy

Real screenshots or footage can replace placeholder panels later. Placeholder diagrams must stay clearly generated and must not fake live metrics or real app screenshots.
```
```

- [ ] **Step 8: Run shell validation**

Run:

```bash
cd docs/tutorial/video/m5tab5-local-ai-terminal
npx hyperframes lint
npx hyperframes validate
```

Expected:

```text
Both commands exit 0. If validate emits contrast warnings, fix colors before committing.
```

- [ ] **Step 9: Commit shell**

Run:

```bash
git add docs/tutorial/video/m5tab5-local-ai-terminal
git commit -m "feat(tutorial): scaffold M5Tab5 HyperFrames video"
```

Expected:

```text
Commit succeeds with the HyperFrames project shell.
```

---

### Task 2: Implement Story Scenes And Data-Driven Layouts

**Files:**
- Modify: `docs/tutorial/video/m5tab5-local-ai-terminal/index.html`
- Modify: `docs/tutorial/video/m5tab5-local-ai-terminal/src/styles.css`
- Modify: `docs/tutorial/video/m5tab5-local-ai-terminal/src/story-data.js`
- Modify: `docs/tutorial/video/m5tab5-local-ai-terminal/src/timeline.js`

**Interfaces:**
- Consumes: Project shell from Task 1.
- Produces: Seven fully populated scenes matching storyboard S1 through S7.

- [ ] **Step 1: Populate HTML scene containers**

Add semantic scene content for:

- S1: centered M5Tab5 device with service nodes.
- S2: firmware/app/Mac service map.
- S3: 3x3 maker feature grid using the nine feature cards.
- S4: seven-step local AI loop with guardrail strip.
- S5: seven problem/fix pitfall cards.
- S6: terminal command panel plus validation checklist.
- S7: closing node convergence and final title.

Use generated panels and labels only; do not fake real screenshots.

- [ ] **Step 2: Add render helpers in `timeline.js`**

Implement small synchronous helper functions:

```js
function createFeatureCards(container, cards) { /* append static cards */ }
function createAiSteps(container, steps) { /* append numbered steps */ }
function createPitfalls(container, pitfalls) { /* append problem/fix pairs */ }
function createBuildCommands(container, commands) { /* append terminal rows */ }
```

These helpers must run synchronously at module load before the GSAP timeline is registered.

- [ ] **Step 3: Expand CSS layouts**

Add responsive fixed-format layouts for:

- `.device-map` with center device and nodes.
- `.feature-grid` with 3 columns and 3 rows.
- `.ai-flow` with seven steps and directional connectors.
- `.pitfall-grid` with readable paired problem/fix panels.
- `.terminal-wrap` with monospaced command lines and checklist badges.

Keep all text 20px or larger except source captions, which must be at least 16px.

- [ ] **Step 4: Implement scene-specific animation choreography**

Update `timeline.js` to animate each scene’s actual elements:

- S1: title, device, service nodes, title lockup.
- S2: layered map blocks and guarded bridge.
- S3: feature cards entering by category.
- S4: AI loop steps one at a time, blocked direct-execution label, latest-result return arrow.
- S5: problem then fix for each pitfall.
- S6: terminal lines and checklist ticks.
- S7: service nodes converging into the device and final title.

Do not use exit animations before transitions except final fade.

- [ ] **Step 5: Validate**

Run:

```bash
cd docs/tutorial/video/m5tab5-local-ai-terminal
npx hyperframes lint
npx hyperframes validate
npx hyperframes inspect --samples 15
```

Expected:

```text
All commands exit 0. Any layout overflow or contrast warning is fixed before commit.
```

- [ ] **Step 6: Commit populated scenes**

Run:

```bash
git add docs/tutorial/video/m5tab5-local-ai-terminal
git commit -m "feat(tutorial): implement M5Tab5 HyperFrames scenes"
```

Expected:

```text
Commit succeeds with populated scene layouts and animation choreography.
```

---

### Task 3: Add Voiceover Captions And Evidence Labels

**Files:**
- Modify: `docs/tutorial/video/m5tab5-local-ai-terminal/index.html`
- Modify: `docs/tutorial/video/m5tab5-local-ai-terminal/src/styles.css`
- Modify: `docs/tutorial/video/m5tab5-local-ai-terminal/src/story-data.js`
- Modify: `docs/tutorial/video/m5tab5-local-ai-terminal/src/timeline.js`

**Interfaces:**
- Consumes: Voiceover script and populated scenes from Tasks 1-2.
- Produces: Scene-level narration captions and compact evidence labels.

- [ ] **Step 1: Add caption data**

In `story-data.js`, add scene-level caption groups derived from `docs/tutorial/m5tab5-hyperframes-voiceover.md`. Use 1-3 short captions per scene, not full paragraphs.

Example:

```js
export const captions = {
  s1: ["这不是普通屏幕 demo", "它是本地系统的入口"],
  s4: ["Tab5 只发送意图", "Mac 负责审批、执行包和结果"],
};
```

- [ ] **Step 2: Add evidence label data**

Add evidence labels:

```js
export const evidenceLabels = {
  s1: ["docs/TUTORIAL-outline.md", "git: 2026-06-13 -> 2026-06-23"],
  s4: ["claude_project_gateway.py", "approval / handoff memory"],
  s5: ["HA stability memory", "hardware evidence > guessing"],
};
```

- [ ] **Step 3: Render captions and evidence labels**

Add caption and evidence containers to each scene. Keep captions large, sparse, and below primary diagrams.

- [ ] **Step 4: Animate captions without clutter**

Captions should enter after the primary visual structure and avoid overlapping feature cards or diagrams. Use `gsap.from()` entrance animations only.

- [ ] **Step 5: Validate captions**

Run:

```bash
cd docs/tutorial/video/m5tab5-local-ai-terminal
npx hyperframes lint
npx hyperframes validate
npx hyperframes inspect --samples 20
```

Expected:

```text
No text overflow. Contrast warnings fixed.
```

- [ ] **Step 6: Commit captions**

Run:

```bash
git add docs/tutorial/video/m5tab5-local-ai-terminal
git commit -m "feat(tutorial): add captions and evidence labels"
```

Expected:

```text
Commit succeeds with caption and evidence overlays.
```

---

### Task 4: Add Generated Placeholder Assets And Asset Status Notes

**Files:**
- Create: `docs/tutorial/video/m5tab5-local-ai-terminal/assets/device-frame.svg`
- Create: `docs/tutorial/video/m5tab5-local-ai-terminal/assets/placeholder-panels.svg`
- Modify: `docs/tutorial/video/m5tab5-local-ai-terminal/index.html`
- Modify: `docs/tutorial/video/m5tab5-local-ai-terminal/README.md`
- Modify: `docs/tutorial/m5tab5-hyperframes-assets.md`

**Interfaces:**
- Consumes: Asset inventory.
- Produces: Clearly labeled generated placeholders and updated asset status notes.

- [ ] **Step 1: Create generated device frame**

Create a simple SVG frame for M5Tab5. It must not show fake app UI. It may show the title `M5Tab5` and abstract interface lines.

- [ ] **Step 2: Create placeholder panels**

Create generated placeholder panels for missing screenshots:

- HA
- Xiaozhi
- Claude Assistant
- Voice Input
- Tools
- Email LED
- Unit-Puzzle LED
- LoRa Chat
- Stocks

Every panel must include a label such as `generated placeholder` or `diagram placeholder`.

- [ ] **Step 3: Wire placeholders into scenes**

Use the generated assets in S1 and S3 where real screenshots are missing. Keep labels visible enough that no viewer mistakes them for live screenshots.

- [ ] **Step 4: Update asset inventory**

In `docs/tutorial/m5tab5-hyperframes-assets.md`, add an implementation note that A001 and A003-A010/A013 are currently represented by generated placeholders until real captures are supplied.

- [ ] **Step 5: Validate**

Run:

```bash
cd docs/tutorial/video/m5tab5-local-ai-terminal
npx hyperframes lint
npx hyperframes validate
npx hyperframes inspect --samples 20
```

Expected:

```text
All commands exit 0. Placeholder labels are readable.
```

- [ ] **Step 6: Commit placeholders**

Run:

```bash
git add docs/tutorial/video/m5tab5-local-ai-terminal docs/tutorial/m5tab5-hyperframes-assets.md
git commit -m "feat(tutorial): add generated video placeholders"
```

Expected:

```text
Commit succeeds with placeholder assets and asset status notes.
```

---

### Task 5: Preview, Inspect, And Render Draft

**Files:**
- Modify: `docs/tutorial/video/m5tab5-local-ai-terminal/README.md`
- Create: `docs/tutorial/video/m5tab5-local-ai-terminal/renders/` if rendering succeeds.

**Interfaces:**
- Consumes: Complete composition from Tasks 1-4.
- Produces: Verified preview URL instructions and a draft MP4 render if the environment supports rendering.

- [ ] **Step 1: Run full HyperFrames verification**

Run:

```bash
cd docs/tutorial/video/m5tab5-local-ai-terminal
npx hyperframes lint
npx hyperframes validate
npx hyperframes inspect --samples 25
npx hyperframes inspect --at 0.5,18,60,185,275,350,405,419
```

Expected:

```text
All commands exit 0. Any warnings are either fixed or explicitly documented in README with reason.
```

- [ ] **Step 2: Start preview server**

Run:

```bash
cd docs/tutorial/video/m5tab5-local-ai-terminal
npx hyperframes preview --port 3017
```

Expected:

```text
Preview server starts and reports a usable local URL.
```

If the server stays running, keep the session open for the user and report:

```text
http://localhost:3017/#project/m5tab5-local-ai-terminal
```

- [ ] **Step 3: Render draft**

Run:

```bash
cd docs/tutorial/video/m5tab5-local-ai-terminal
mkdir -p renders
npx hyperframes render --quality draft --output renders/m5tab5-local-ai-terminal-draft.mp4
```

Expected:

```text
Render succeeds and writes the MP4. If FFmpeg/Chrome/rendering fails, record the exact failure in README and do not claim a video render exists.
```

- [ ] **Step 4: Update README with verified commands**

Update README with:

- Commands that passed.
- Preview URL if server started.
- Draft render path if render succeeded.
- Any environment limitations.

- [ ] **Step 5: Commit verification notes and render metadata**

Run:

```bash
git add docs/tutorial/video/m5tab5-local-ai-terminal/README.md
git add docs/tutorial/video/m5tab5-local-ai-terminal/renders/m5tab5-local-ai-terminal-draft.mp4 || true
git commit -m "docs(tutorial): verify M5Tab5 HyperFrames draft"
```

Expected:

```text
Commit succeeds if README changed or a draft render was produced. If no files changed because preview/render was not possible, do not create an empty commit.
```

---

## Self-Review Checklist

- The composition is a standalone root composition and does not use `<template>`.
- `window.__timelines["m5tab5-local-ai-terminal"]` is registered synchronously.
- The composition duration is 420 seconds.
- The output dimensions are 1920x1080.
- Scenes use transitions and entrance animations.
- Final scene is the only scene with a fade-out exit.
- Placeholder visuals are clearly labeled and do not fake live screenshots or metrics.
- Local Claude flow is request/approval/execution packet/result, not automatic shell execution.
- `npx hyperframes lint`, `npx hyperframes validate`, and `npx hyperframes inspect` pass before preview/render claims.
