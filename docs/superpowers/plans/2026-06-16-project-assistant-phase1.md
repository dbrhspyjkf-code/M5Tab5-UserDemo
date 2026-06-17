# Project Assistant Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a text-only, read-only Project Assistant path from Tab5 to a local Mac Claude gateway.

**Architecture:** Add a Python gateway on port 8769 and a Mooncake/LVGL app that posts user text to it. Keep Claude/Feishu/claude-to-im secrets off-device; the device only speaks HTTP JSON to the local gateway.

**Tech Stack:** ESP-IDF/Mooncake/LVGL C++17, existing `HalBase::httpPost`, Python 3 standard library, source-level unittest checks.

**Run Gateway:** `python3 scripts/claude_project_gateway.py --host 0.0.0.0 --port 8769`

**Mock Gateway:** `python3 scripts/claude_project_gateway.py --host 0.0.0.0 --port 8769 --mock`

---

### Task 1: Gateway Contract

**Files:**
- Create: `scripts/claude_project_gateway.py`
- Create: `test/test_project_assistant.py`

- [x] Write tests that import the gateway helpers and assert read-only prompt validation.
- [x] Implement `parse_request`, `is_write_intent`, and `build_claude_prompt`.
- [x] Run `python3 test/test_project_assistant.py` and confirm the tests pass.

### Task 2: Gateway HTTP Server

**Files:**
- Modify: `scripts/claude_project_gateway.py`

- [x] Add `/health`.
- [x] Add `POST /api/claude/message`.
- [x] Return JSON errors for invalid JSON, empty text, and blocked write intent.
- [x] Add a `--mock` mode so the service can be tested without invoking Claude.

### Task 3: Tab5 App Skeleton

**Files:**
- Create: `app/apps/app_project_assistant/app_project_assistant.h`
- Create: `app/apps/app_project_assistant/app_project_assistant.cpp`
- Modify: `app/apps/app_installer.h`

- [x] Add `AppProjectAssistant` lifecycle.
- [x] Build a dedicated LVGL screen with input, quick prompts, send, back, status, and response labels.
- [x] Register the app on the home launcher as `Claude`.
- [x] Wire the screen to the XiaoZhi Puhui font for Chinese project text.
- [x] Keep the keyboard hidden by default, toggle it on input double-click, and resize the reply panel with keyboard state.

### Task 4: Device HTTP Flow

**Files:**
- Modify: `app/apps/app_project_assistant/app_project_assistant.cpp`

- [x] Build JSON request body.
- [x] Post to `http://<ha_host>:8769/api/claude/message`.
- [x] Parse `"reply"` from the response.
- [x] Keep LVGL updates on the main app loop via cached strings and flags.
- [x] Give Claude gateway requests a longer timeout and bypass local proxy routing in the desktop simulator.

### Task 5: Verification

**Files:**
- Modify: `test/test_project_assistant.py`

- [x] Assert installer includes and registers the new app.
- [x] Run `python3 test/test_project_assistant.py`.
- [x] Run `bash scripts/check_ha_stability.sh`.
- [x] Attempt desktop simulator build with `cmake --build build-mac-desktop`.
