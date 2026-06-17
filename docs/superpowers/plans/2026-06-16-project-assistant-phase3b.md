# Project Assistant Phase 3B Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let Tab5 create desktop-confirmed pending project requests without executing anything automatically.

**Architecture:** The Mac gateway owns the queue and writes one JSON file per request under `var/project_assistant/requests/`. The Tab5 app posts a task description to `/api/project/request` through a `Request` quick action. Desktop Codex can later inspect the pending JSON and ask the user for confirmation before any code changes.

**Tech Stack:** Python `http.server` gateway, C++ Mooncake/LVGL Tab5 app, nlohmann/json, unittest.

---

### Task 1: Pending Request Queue

**Files:**
- Modify: `test/test_project_assistant.py`
- Modify: `scripts/claude_project_gateway.py`
- Modify: `.gitignore`
- Create: `var/project_assistant/requests/.gitkeep`

- [ ] **Step 1: Write failing tests**

Add tests for creating a pending request JSON file with `id`, `status`, `project`, `mode`, `text`, `created_at`, `context_hash`, and `context`.

- [ ] **Step 2: Run tests to verify failure**

Run: `python3 test/test_project_assistant.py`
Expected: failure because request queue helpers do not exist.

- [ ] **Step 3: Implement queue writer**

Add `REQUESTS_DIR`, `parse_project_request()`, `create_pending_request()`, and `request_response()` helpers. Write JSON files under `var/project_assistant/requests/` and ignore generated `*.json`.

- [ ] **Step 4: Verify tests pass**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

### Task 2: Request Endpoint

**Files:**
- Modify: `test/test_project_assistant.py`
- Modify: `scripts/claude_project_gateway.py`

- [ ] **Step 1: Write failing tests**

Add tests that `POST /api/project/request` is routed separately from `/api/claude/message` and that direct command intents are rejected.

- [ ] **Step 2: Run tests to verify failure**

Run: `python3 test/test_project_assistant.py`
Expected: failure because the endpoint is not implemented.

- [ ] **Step 3: Implement endpoint**

Add `Handler._handle_project_request()` and route `/api/project/request` in `do_POST`. The endpoint writes a pending file and returns `ok`, `id`, `status`, and `path`.

- [ ] **Step 4: Verify tests pass**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

### Task 3: Tab5 Request Button

**Files:**
- Modify: `test/test_project_assistant.py`
- Modify: `app/apps/app_project_assistant/app_project_assistant.cpp`
- Modify: `app/apps/app_project_assistant/app_project_assistant.h`

- [ ] **Step 1: Write failing tests**

Add tests that the UI has a `Request` quick action, posts to `/api/project/request`, and displays the returned pending request id.

- [ ] **Step 2: Run tests to verify failure**

Run: `python3 test/test_project_assistant.py`
Expected: failure because the button and request post path are missing.

- [ ] **Step 3: Implement UI wiring**

Add a `Request` quick prompt and `_submitProjectRequest()`. Reuse the existing worker pattern and display `Pending request` plus the returned id/path.

- [ ] **Step 4: Verify tests pass**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

### Task 4: Validation

**Files:**
- No source edits expected.

- [ ] **Step 1: Run tests**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

- [ ] **Step 2: Build desktop simulator**

Run: `cmake --build build_simulator -j8`
Expected: `app_desktop_build` builds successfully.

- [ ] **Step 3: Restart gateway and smoke test**

Run: `curl -s -X POST http://127.0.0.1:8769/api/project/request -H 'Content-Type: application/json' -d '{"text":"请排队实现一个只读测试任务","mode":"request","project":"M5Tab5"}'`
Expected: JSON response with `ok: true`, `status: "pending"`, and a local request file path.

- [ ] **Step 4: Confirm no execution happened**

Inspect the pending JSON file. Expected: it contains only metadata and context; no command output, code patch, or execution result.
