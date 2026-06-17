# Project Assistant Phase 2 Design

## Goal

Make the Tab5 Claude app able to hand Claude a useful read-only snapshot of the current M5Tab5 project, so the user can ask for project guidance without typing repository context by hand.

## Scope

Phase 2 adds a project-context path:

- The Mac gateway exposes `GET /api/project/context`.
- The gateway builds a bounded, read-only context summary from local project state.
- The Tab5 app adds a `Context` quick button.
- Pressing `Context` sends a project handoff request to Claude that includes the gateway-generated context.

Phase 2 remains read-only. It does not edit files, run flash commands, commit, push, stream responses, or let the device invoke arbitrary shell commands.

## Context Contents

The gateway context includes:

- repository path
- current branch
- short git status
- latest commit
- key project files and directories
- available Project Assistant phase docs

All command execution stays inside the gateway on the Mac. Commands are fixed and bounded; user text is never interpolated into shell commands.

## App Flow

The app keeps the Phase 1 text prompt flow. `Context` is added beside the existing quick prompt buttons. It sends a canned request asking Claude to review the current project state, risks, and next steps. The device does not fetch or display the raw context directly; it posts a `mode: "context"` request and lets the gateway attach the context.

## Safety

The existing write-intent blocker remains active. `mode: "context"` is accepted by the gateway, but it is still read-only. Context output is truncated to keep Tab5 responses responsive and avoid sending very large project data to Claude.

## Validation

- `python3 test/test_project_assistant.py`
- `bash scripts/check_ha_stability.sh`
- `cmake --build build-mac-desktop -j4`
- Restart the desktop simulator and verify the Claude app opens.
