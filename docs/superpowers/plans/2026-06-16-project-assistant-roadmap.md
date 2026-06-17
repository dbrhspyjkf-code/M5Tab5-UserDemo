# Tab5 Claude Project Assistant Roadmap

## Goal

Turn the Tab5 Claude app into a safe project-control surface: first ask Claude, then hand over project context, then request plans, then review execution results. Tab5 should stay a client. The Mac gateway and desktop Codex/Claude environment remain the only place that can inspect the repository or run local tooling.

## Safety Boundary

- Tab5 never receives Claude, Feishu, or local shell credentials.
- Tab5 only sends HTTP JSON to the local Mac gateway on port `8769`.
- Gateway project inspection uses fixed read-only commands only.
- Write, flash, Git, and shell execution stay blocked from Tab5 until there is a separate confirmation flow.
- Any future execution should be queued for desktop confirmation, not run directly from the device.

## Phase 1: Text Claude App

Status: implemented.

Purpose:
- Add a `Claude` home entry.
- Let Tab5 send text prompts to the local gateway.
- Keep requests read-only.
- Display Claude replies on the Tab5 screen.

Implemented pieces:
- `scripts/claude_project_gateway.py`
- `app/apps/app_project_assistant/`
- `app/apps/app_installer.h`
- desktop and Tab5 HTTP timeout/proxy handling
- XiaoZhi Puhui font integration for Chinese replies
- keyboard double-click toggle and reply-panel resizing

Validation:
- `python3 test/test_project_assistant.py`
- `bash scripts/check_ha_stability.sh`
- `cmake --build build-mac-desktop -j4`
- restart `./build/desktop/app_desktop_build`

## Phase 2: Project Context Handoff

Status: implemented.

Purpose:
- Let Tab5 hand Claude a current project snapshot without manually typing context.
- Keep all project inspection on the Mac gateway.

Implemented pieces:
- `GET /api/project/context`
- gateway `mode: "context"`
- `Context` quick action in the Tab5 Claude app
- bounded context collection:
  - repository path
  - branch
  - latest commit
  - `git status --short`
  - key files
  - Project Assistant phase docs
- longer Claude and device HTTP timeouts for context analysis

Validation:
- `python3 test/test_project_assistant.py`
- `bash scripts/check_ha_stability.sh`
- `cmake --build build-mac-desktop -j4`
- `curl http://127.0.0.1:8769/api/project/context`
- real `mode: "context"` request through `/api/claude/message`

## Phase 3A: Plan Mode

Status: proposed next step.

Purpose:
- Add a `Plan` quick action that asks Claude to turn current project context into an implementation plan.
- Display the plan on Tab5.
- Do not execute the plan.

Implementation steps:
- Extend gateway parsing to accept `mode: "plan"` as a context-aware mode.
- Build a plan-specific Claude prompt:
  - summarize project state
  - identify next implementation task
  - list files likely to change
  - list verification commands
  - keep output concise for Tab5
- Add a `Plan` quick button in `AppProjectAssistant`.
- Send the same project context snapshot with a plan-specific user request.
- Add tests for plan prompt construction and Tab5 `Plan` button wiring.

Acceptance criteria:
- Pressing `Plan` returns a concise implementation plan.
- No files are modified by the gateway.
- Existing `Ask` and `Context` flows keep working.
- Tests and desktop simulator build pass.

## Phase 3B: Desktop-Confirmed Execution Queue

Status: future.

Purpose:
- Let Tab5 request work without directly granting execution power.
- Store requested tasks as pending local files for desktop review.

Implementation steps:
- Add a local queue directory, for example `var/project_assistant/requests/`.
- Add gateway endpoint `POST /api/project/request`.
- Request payload includes:
  - task text
  - mode
  - project
  - timestamp
  - current context snapshot hash or short context
- Gateway writes a pending JSON file.
- Tab5 adds a `Request` button or mode.
- Desktop Codex reads the pending request and asks the user for confirmation before doing any code changes.

Acceptance criteria:
- Tab5 can create a pending task.
- The pending task is visible as a local JSON file.
- Nothing executes automatically.
- Invalid or write-like direct commands are still blocked.

## Phase 3C: Result Review

Status: future.

Purpose:
- Let Tab5 see what happened after desktop Codex handled a requested task.

Implementation steps:
- Add queue result files under `var/project_assistant/results/`.
- Add gateway endpoint `GET /api/project/results/latest`.
- Result summary includes:
  - status
  - files changed
  - verification commands
  - pass/fail result
  - short next step
- Add a `Result` quick action in the Tab5 app.
- Keep result text short and display-safe.

Acceptance criteria:
- Tab5 can display the latest desktop-reviewed result.
- Results never include secrets.
- Large logs are summarized rather than sent raw.

## Phase 4: Optional Controlled Execution

Status: later, only after Phase 3B/3C are stable.

Purpose:
- Allow limited execution only after explicit desktop confirmation.

Possible scope:
- A desktop approval file or UI action marks one queued request as approved.
- Codex executes from the desktop session, not from Tab5.
- Tab5 can observe status but cannot bypass approval.

Hard requirements:
- Show diff before commit.
- Run verification before completion.
- Never run flash, Git push, reset, or destructive shell commands without explicit user approval.
- Keep a local audit trail for each request.

## Recommended Next Work

Start with Phase 3A.

Why:
- It upgrades Tab5 from “ask and context” to “project planning”.
- It remains read-only.
- It uses the context work already built in Phase 2.
- It avoids the security complexity of execution queues until the planning UX feels good.

Suggested first task:
- Add `Plan` quick action and plan-specific gateway prompt.
