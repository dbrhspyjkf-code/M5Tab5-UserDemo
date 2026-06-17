# Project Assistant Phase 1 Design

## Goal

Add a first-phase Project Assistant for M5Tab5 that lets the device ask a local Mac service for read-only Claude project guidance.

## Scope

Phase 1 is text-only and read-only:

- Tab5 shows a new home entry named `Claude`.
- The app provides a text box, quick prompt buttons, a send button, and a response area.
- The app posts messages to a local gateway at `http://<ha_host>:8769/api/claude/message`.
- The gateway binds requests to this project checkout and rejects command-like or write-intent prompts.
- The gateway returns one JSON reply string for display.
- The app uses the XiaoZhi Puhui 20 font for project text, so Chinese prompts and Claude replies can render without the old narrow CJK subset.
- The on-screen keyboard is hidden by default. Double-clicking the input toggles it; hiding the keyboard expands the reply panel, and showing it shrinks the reply panel.

Phase 1 does not implement voice input, streaming, code edits, flashing, Git operations, or Feishu protocol emulation.

## Architecture

The device should not know Claude, Feishu, or claude-to-im secrets. It only knows a LAN HTTP endpoint. The Mac gateway owns the Claude/Codex runtime boundary and can later route through claude-to-im internals if needed.

```text
AppProjectAssistant -> HalBase::httpPost -> Mac gateway :8769 -> Claude CLI/session -> JSON reply
```

## Components

- `scripts/claude_project_gateway.py`: small Python HTTP gateway. It exposes `/health` and `/api/claude/message`, validates request shape, blocks write/command intents, and invokes a local assistant backend.
- `app/apps/app_project_assistant/`: Mooncake/LVGL app. It owns the UI, request state, and response rendering.
- `app/apps/app_installer.h`: installs the new app and adds it to the home launcher.
- `test/test_project_assistant.py`: source-level tests for the first-phase contract.
- `platforms/*/hal/components/hal_http.cpp`: gives Claude gateway requests a longer timeout and keeps desktop LAN calls out of proxy routing.
- `platforms/tab5/managed_components/78__xiaozhi-fonts/`: provides the Puhui font used by the Project Assistant display.

## Safety

The gateway is the safety boundary:

- It accepts only loopback/LAN HTTP requests.
- It never exposes secrets in responses.
- It rejects prompts containing clear write/execute intents such as `commit`, `push`, `flash`, `rm -rf`, `apply patch`, or `修改代码`.
- It constrains Claude with a system prompt that says read-only project analysis only.
- It asks Claude for plain text and strips emoji or rare symbols before returning the text to Tab5, reducing missing-glyph boxes.

The firmware remains simple and cannot trigger local shell commands directly.

## Validation

- Run `python3 test/test_project_assistant.py`.
- Run `bash scripts/check_ha_stability.sh`.
- Build the desktop simulator when dependencies are available.
