# Project Assistant Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a read-only project-context handoff path to the Tab5 Claude app.

**Architecture:** The Mac gateway owns project inspection through fixed local read-only commands. The Tab5 app adds a `Context` quick action that sends `mode: "context"` to the existing Claude message endpoint, and the gateway attaches the generated context to Claude's prompt.

**Tech Stack:** Python 3 standard library, Mooncake/LVGL C++17, nlohmann/json, existing source-level unittest checks.

---

### Task 1: Gateway Context Helpers

**Files:**
- Modify: `scripts/claude_project_gateway.py`
- Modify: `test/test_project_assistant.py`

- [x] Add tests for `collect_project_context()` and `mode: "context"` parsing.
- [x] Implement a bounded read-only context collector using fixed Git commands and file listings.
- [x] Make `build_claude_prompt()` include context only for context mode.

### Task 2: Gateway HTTP Surface

**Files:**
- Modify: `scripts/claude_project_gateway.py`
- Modify: `test/test_project_assistant.py`

- [x] Add `GET /api/project/context`.
- [x] Return JSON with `ok: true` and a `context` string.
- [x] Keep JSON errors bounded and plain text.

### Task 3: Tab5 Context Quick Action

**Files:**
- Modify: `app/apps/app_project_assistant/app_project_assistant.cpp`
- Modify: `app/apps/app_project_assistant/app_project_assistant.h`
- Modify: `test/test_project_assistant.py`

- [x] Add a `Context` quick button.
- [x] Add per-button mode metadata.
- [x] Send context requests as `mode: "context"` with a canned Chinese project handoff prompt.

### Task 4: Verification

**Files:**
- Modify: `test/test_project_assistant.py`

- [x] Run `python3 test/test_project_assistant.py`.
- [x] Run `bash scripts/check_ha_stability.sh`.
- [x] Run `cmake --build build-mac-desktop -j4`.
- [x] Restart `./build/desktop/app_desktop_build`.
