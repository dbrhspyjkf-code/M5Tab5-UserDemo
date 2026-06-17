# Project Assistant Phase 4B Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a desktop Codex execution handoff for approved Tab5 requests, while keeping execution manual, auditable, and outside the gateway.

**Architecture:** Approved request JSON files can produce a local execution packet under `var/project_assistant/execution/`. The packet contains the request, context summary, safety rules, and verification requirements for a desktop Codex session. After the human-reviewed desktop work is done, the review script records a concise result summary and marks the request `done`.

**Tech Stack:** Python gateway helpers, Python desktop CLI script, JSON and Markdown artifacts, unittest.

---

### Task 1: Execution Packet

**Files:**
- Modify: `test/test_project_assistant.py`
- Modify: `scripts/claude_project_gateway.py`
- Modify: `.gitignore`
- Create: `var/project_assistant/execution/.gitkeep`

- [ ] **Step 1: Write failing tests**

Add tests that only `approved` requests can produce execution packets and packets include safety rules, request text, context hash, diff-before-completion, and verification requirements.

- [ ] **Step 2: Run tests to verify failure**

Run: `python3 test/test_project_assistant.py`
Expected: failure because execution packet helpers do not exist.

- [ ] **Step 3: Implement packet writer**

Add `EXECUTION_DIR` and `create_execution_packet(request_id)`. Write a Markdown packet and append an `execution_packet` audit entry.

- [ ] **Step 4: Verify tests pass**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

### Task 2: Result Recording

**Files:**
- Modify: `test/test_project_assistant.py`
- Modify: `scripts/claude_project_gateway.py`
- Modify: `scripts/project_request_review.py`

- [ ] **Step 1: Write failing tests**

Add tests that a completed desktop result records `summary`, `files_changed`, `verification`, and writes the latest result visible to Tab5.

- [ ] **Step 2: Run tests to verify failure**

Run: `python3 test/test_project_assistant.py`
Expected: failure because result recording does not exist.

- [ ] **Step 3: Implement result recording**

Add `record_request_result()` and a review script `result` subcommand. It marks an approved request `done`, writes audit, and updates `latest.json`.

- [ ] **Step 4: Verify tests pass**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

### Task 3: Desktop Script Commands

**Files:**
- Modify: `test/test_project_assistant.py`
- Modify: `scripts/project_request_review.py`

- [ ] **Step 1: Write failing tests**

Add tests that the script exposes `packet` and `result` commands and still does not import `subprocess`, `os.system`, or dangerous command strings.

- [ ] **Step 2: Run tests to verify failure**

Run: `python3 test/test_project_assistant.py`
Expected: failure until commands exist.

- [ ] **Step 3: Implement commands**

Add `packet <id>` to generate a Markdown execution packet and `result <id> --summary ... --verification ... --file ...` to record completion.

- [ ] **Step 4: Verify tests pass**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

### Task 4: Validation

**Files:**
- No source edits expected.

- [ ] **Step 1: Run tests**

Run: `python3 test/test_project_assistant.py`
Expected: all tests pass.

- [ ] **Step 2: Build simulator**

Run: `cmake --build build_simulator -j8`
Expected: build passes.

- [ ] **Step 3: Smoke test approved request**

Create or reuse an approved request, run `python3 scripts/project_request_review.py packet <id>`, then record a result with `python3 scripts/project_request_review.py result <id> --summary ... --verification ...`.
Expected: packet Markdown exists, request becomes `done`, audit is updated, and latest result contains the summary.
