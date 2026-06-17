# Project Assistant Phase 4A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a desktop-only approval state machine for queued Tab5 project requests, without executing code.

**Architecture:** The gateway remains the queue owner and exposes read/status endpoints for local desktop review. Request JSON files keep their full audit trail. A local desktop script reads pending requests and marks them `approved`, `rejected`, or `done`; it never runs shell commands, applies patches, flashes devices, or touches Git.

**Tech Stack:** Python gateway helpers, Python desktop CLI script, JSON files, unittest.

---

### Task 1: Request Listing

**Files:**
- Modify: `test/test_project_assistant.py`
- Modify: `scripts/claude_project_gateway.py`

- [ ] **Step 1: Write failing tests**

Add tests that list request JSON files by status and return newest first.

- [ ] **Step 2: Run tests to verify failure**

Run: `python3 test/test_project_assistant.py`
Expected: failure because list helpers do not exist.

- [ ] **Step 3: Implement list helpers and GET endpoint**

Add `list_project_requests()` and route `GET /api/project/requests?status=pending`.

- [ ] **Step 4: Verify tests pass**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

### Task 2: Approval State Machine

**Files:**
- Modify: `test/test_project_assistant.py`
- Modify: `scripts/claude_project_gateway.py`

- [ ] **Step 1: Write failing tests**

Add tests for `pending -> approved`, `pending -> rejected`, `approved -> done`, invalid transitions, and audit entries.

- [ ] **Step 2: Run tests to verify failure**

Run: `python3 test/test_project_assistant.py`
Expected: failure because status update helpers do not exist.

- [ ] **Step 3: Implement state transitions**

Add `update_project_request_status()` and `POST /api/project/request/status`.

- [ ] **Step 4: Verify tests pass**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

### Task 3: Desktop Review Script

**Files:**
- Create: `scripts/project_request_review.py`
- Modify: `test/test_project_assistant.py`

- [ ] **Step 1: Write failing tests**

Add tests that the script exists, supports `list/show/approve/reject/done`, and does not import subprocess or call execution APIs.

- [ ] **Step 2: Run tests to verify failure**

Run: `python3 test/test_project_assistant.py`
Expected: failure because the script does not exist.

- [ ] **Step 3: Implement script**

Create a small desktop CLI that reads and updates request JSON files through gateway helper functions.

- [ ] **Step 4: Verify tests pass**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

### Task 4: Validation

**Files:**
- No source edits expected.

- [ ] **Step 1: Run tests**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

- [ ] **Step 2: Smoke test local approval**

Run: `python3 scripts/project_request_review.py list` and approve/reject one generated test request.
Expected: request JSON status changes and audit trail records the action.

- [ ] **Step 3: Confirm no execution path exists**

Inspect the script and gateway status update code.
Expected: no shell execution, no Git operation, no flash operation.
