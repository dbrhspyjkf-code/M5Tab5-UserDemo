#!/usr/bin/env python3
"""Local read-only Claude gateway for the Tab5 Project Assistant.

This service intentionally keeps Claude/Feishu/claude-to-im credentials on the
Mac. The device only sends plain HTTP JSON to this LAN service.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import threading
import time
import unicodedata
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROJECT = "M5Tab5"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8769
MAX_TEXT_LEN = 2000
MAX_CONTEXT_LEN = 6000
DEFAULT_ASSISTANT_TIMEOUT = 210
DEVICE_HTTP_TIMEOUT_SECONDS = 240
VARIATION_SELECTOR_START = 0xFE00
VARIATION_SELECTOR_END = 0xFE0F
RESULTS_DIR = PROJECT_ROOT / "var" / "project_assistant" / "results"
LATEST_RESULT_FILE = RESULTS_DIR / "latest.json"
REQUESTS_DIR = PROJECT_ROOT / "var" / "project_assistant" / "requests"
EXECUTION_DIR = PROJECT_ROOT / "var" / "project_assistant" / "execution"
LATEST_RESULT: dict[str, Any] | None = None
_CLAUDE_SEM = threading.Semaphore(1)  # one claude subprocess at a time
VALID_REQUEST_STATUSES = {"pending", "approved", "rejected", "done"}
REQUEST_TRANSITIONS = {
    "pending": {"approved", "rejected"},
    "approved": {"done"},
    "rejected": set(),
    "done": set(),
}


@dataclass
class AssistantRequest:
    text: str
    project: str = DEFAULT_PROJECT
    mode: str = "ask"


@dataclass
class ProjectRequest:
    text: str
    project: str = DEFAULT_PROJECT
    mode: str = "request"


def parse_request(raw: bytes) -> AssistantRequest:
    try:
        payload = json.loads(raw.decode("utf-8"))
    except Exception as exc:
        raise ValueError("invalid json") from exc
    if not isinstance(payload, dict):
        raise ValueError("json object is required")

    text = str(payload.get("text", "")).strip()
    if not text:
        raise ValueError("text is required")
    if len(text) > MAX_TEXT_LEN:
        raise ValueError(f"text must be <= {MAX_TEXT_LEN} chars")

    project = str(payload.get("project", DEFAULT_PROJECT)).strip() or DEFAULT_PROJECT
    mode = str(payload.get("mode", "ask")).strip() or "ask"
    if mode not in {"ask", "plan", "context"}:
        raise ValueError("mode must be ask, plan, or context")

    return AssistantRequest(text=text, project=project, mode=mode)


def parse_project_request(raw: bytes) -> ProjectRequest:
    try:
        payload = json.loads(raw.decode("utf-8"))
    except Exception as exc:
        raise ValueError("invalid json") from exc
    if not isinstance(payload, dict):
        raise ValueError("json object is required")

    text = str(payload.get("text", "")).strip()
    if not text:
        raise ValueError("text is required")
    if len(text) > MAX_TEXT_LEN:
        raise ValueError(f"text must be <= {MAX_TEXT_LEN} chars")

    project = str(payload.get("project", DEFAULT_PROJECT)).strip() or DEFAULT_PROJECT
    mode = str(payload.get("mode", "request")).strip() or "request"
    if mode not in {"request", "ask", "plan", "context"}:
        raise ValueError("mode must be request, ask, plan, or context")

    return ProjectRequest(text=text, project=project, mode=mode)


_BLOCKED_TERMS = [
    "apply patch",
    "commit",
    "git push",
    "git reset",
    "idf.py flash",
    "rm -rf",
    "sudo ",
    "执行命令",
    "运行命令",
    "修改代码",
    "提交代码",
    "刷机",
]


def is_write_intent(text: str) -> bool:
    lowered = text.lower()
    return any(term in lowered for term in _BLOCKED_TERMS)


# Alias used by the request queue path — same rules apply.
is_direct_execution_intent = is_write_intent


def _run_readonly_command(argv: list[str], timeout: int = 3) -> str:
    try:
        proc = subprocess.run(
            argv,
            cwd=str(PROJECT_ROOT),
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
    except Exception as exc:
        return f"unavailable: {exc}"
    output = (proc.stdout or proc.stderr).strip()
    return output or "none"


def collect_project_context() -> str:
    key_paths = [
        "app/apps/app_project_assistant",
        "scripts/claude_project_gateway.py",
        "test/test_project_assistant.py",
        "docs/superpowers/specs",
        "docs/superpowers/plans",
        "platforms/desktop/hal/components/hal_http.cpp",
        "platforms/tab5/main/hal/components/hal_http.cpp",
    ]
    existing_paths = [path for path in key_paths if (PROJECT_ROOT / path).exists()]
    docs = sorted(
        path.name
        for folder in ("docs/superpowers/specs", "docs/superpowers/plans")
        for path in (PROJECT_ROOT / folder).glob("*project-assistant*.md")
    )

    context = "\n".join([
        f"Repository: {PROJECT_ROOT}",
        f"Branch: {_run_readonly_command(['git', 'branch', '--show-current'])}",
        "Recent commits:",
        _run_readonly_command(['git', 'log', '--oneline', '-5']),
        "Git status:",
        _run_readonly_command(['git', 'status', '--short']),
        "Key files:",
        "\n".join(f"- {path}" for path in existing_paths) or "- none",
        "Project Assistant docs:",
        "\n".join(f"- {name}" for name in docs) or "- none",
    ])
    return context[:MAX_CONTEXT_LEN]


def build_claude_prompt(
    text: str,
    project: str = DEFAULT_PROJECT,
    mode: str = "ask",
    context: str | None = None,
) -> str:
    prompt = (
        "You are helping from a local Tab5 Project Assistant.\n"
        f"Project: {project}\n"
        f"Repository path: {PROJECT_ROOT}\n\n"
        "Safety rules:\n"
        "- This is a read-only request.\n"
        "- Do not modify files.\n"
        "- Do not run shell commands that change system, device, Git, or project state.\n"
        "- Give concise project guidance in Chinese unless the user asks otherwise.\n\n"
        "Display rules for the Tab5 screen:\n"
        "- Use plain text only.\n"
        "- Do not use emoji, rare symbols, markdown tables, or decorative bullets.\n\n"
    )
    if mode in {"context", "plan"}:
        prompt += (
            "Project context snapshot:\n"
            f"{context or collect_project_context()}\n\n"
        )
    if mode == "context":
        prompt += (
            "Use this snapshot to explain the current project state, risks, and next steps.\n\n"
        )
    elif mode == "plan":
        prompt += (
            "Create a concise read-only implementation plan for the Tab5 screen.\n"
            "Do not execute the plan.\n"
            "Include these short sections in Chinese:\n"
            "1. Current project state\n"
            "2. Next implementation task\n"
            "3. files likely to change\n"
            "4. verification commands\n"
            "5. Main risks\n\n"
        )
    prompt += f"User request:\n{text}\n"
    return prompt


def strip_markdown(text: str) -> str:
    text = re.sub(r'\*{1,3}(.+?)\*{1,3}', r'\1', text)     # **bold**, *em*, ***both***
    text = re.sub(r'^#{1,6}\s+', '', text, flags=re.MULTILINE)  # ## headings
    text = re.sub(r'^>\s*', '', text, flags=re.MULTILINE)   # > blockquote
    text = re.sub(r'`{1,3}(.+?)`{1,3}', r'\1', text, flags=re.DOTALL)  # `code`
    return text


def sanitize_display_text(text: str) -> str:
    text = strip_markdown(text)
    cleaned = []
    for char in text:
        codepoint = ord(char)
        if codepoint > 0xFFFF:
            continue
        if VARIATION_SELECTOR_START <= codepoint <= VARIATION_SELECTOR_END:
            continue
        if unicodedata.category(char) in {"Sk", "So"}:
            continue
        cleaned.append(char)
    return re.sub(r"[ \t]{2,}", " ", "".join(cleaned)).strip()


def json_response(ok: bool, **fields: Any) -> bytes:
    body = {"ok": ok, **fields}
    return json.dumps(body, ensure_ascii=False).encode("utf-8")


def record_latest_result(req: AssistantRequest, reply: str) -> None:
    global LATEST_RESULT
    LATEST_RESULT = {
        "mode": req.mode,
        "project": req.project,
        "text": req.text,
        "reply": reply,
        "updated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    LATEST_RESULT_FILE.write_text(
        json.dumps(LATEST_RESULT, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def load_latest_result() -> dict[str, Any] | None:
    if not LATEST_RESULT_FILE.exists():
        return None
    try:
        data = json.loads(LATEST_RESULT_FILE.read_text(encoding="utf-8"))
    except Exception:
        return None
    if not isinstance(data, dict):
        return None
    required = {"mode", "project", "text", "reply", "updated_at"}
    if not required.issubset(data):
        return None
    return data


def latest_result_response() -> dict[str, Any]:
    global LATEST_RESULT
    if not LATEST_RESULT:
        LATEST_RESULT = load_latest_result()
    if not LATEST_RESULT:
        return {"ok": True, "has_result": False}
    return {"ok": True, "has_result": True, **LATEST_RESULT}


def create_pending_request(req: ProjectRequest, context: str | None = None) -> dict[str, Any]:
    if is_direct_execution_intent(req.text):
        raise PermissionError("request queue only accepts pending tasks; direct execution intents are blocked")

    created_at = time.strftime("%Y-%m-%d %H:%M:%S")
    context_text = context if context is not None else collect_project_context()
    context_hash = hashlib.sha256(context_text.encode("utf-8")).hexdigest()[:16]
    request_id = f"{time.strftime('%Y%m%d-%H%M%S')}-{context_hash}"
    body = {
        "id": request_id,
        "status": "pending",
        "source": "tab5",
        "project": req.project,
        "mode": req.mode,
        "text": req.text,
        "created_at": created_at,
        "context_hash": context_hash,
        "context": context_text,
        "audit": [{
            "status": "pending",
            "actor": "tab5",
            "note": "queued from Tab5",
            "updated_at": created_at,
        }],
    }

    REQUESTS_DIR.mkdir(parents=True, exist_ok=True)
    path = REQUESTS_DIR / f"{request_id}.json"
    path.write_text(json.dumps(body, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {**body, "path": str(path)}


def _request_file(request_id: str) -> Path:
    safe_id = re.sub(r"[^A-Za-z0-9_.-]", "", request_id)
    if not safe_id or safe_id != request_id:
        raise ValueError("invalid request id")
    return REQUESTS_DIR / f"{safe_id}.json"


def load_project_request(request_id: str) -> dict[str, Any]:
    path = _request_file(request_id)
    if not path.exists():
        raise FileNotFoundError(f"request not found: {request_id}")
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("request file is invalid")
    return {**data, "path": str(path)}


def list_project_requests(status: str | None = None, limit: int = 50) -> list[dict[str, Any]]:
    if status is not None and status not in VALID_REQUEST_STATUSES:
        raise ValueError("invalid status")
    if not REQUESTS_DIR.exists():
        return []

    items: list[dict[str, Any]] = []
    for path in REQUESTS_DIR.glob("*.json"):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            continue
        if not isinstance(data, dict):
            continue
        if status is not None and data.get("status") != status:
            continue
        items.append({**data, "path": str(path)})

    items.sort(key=lambda item: (item.get("created_at", ""), item.get("id", "")), reverse=True)
    return items[:limit]


def update_project_request_status(
    request_id: str,
    status: str,
    note: str = "",
    actor: str = "desktop",
) -> dict[str, Any]:
    if status not in VALID_REQUEST_STATUSES - {"pending"}:
        raise ValueError("status must be approved, rejected, or done")

    data = load_project_request(request_id)
    current = str(data.get("status", "pending"))
    if status not in REQUEST_TRANSITIONS.get(current, set()):
        raise ValueError(f"invalid transition: {current} -> {status}")

    updated_at = time.strftime("%Y-%m-%d %H:%M:%S")
    data["status"] = status
    data["updated_at"] = updated_at
    audit = data.setdefault("audit", [])
    if not isinstance(audit, list):
        audit = []
        data["audit"] = audit
    audit.append({
        "status": status,
        "actor": actor,
        "note": note,
        "updated_at": updated_at,
    })

    path = _request_file(request_id)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {**data, "path": str(path)}


def _append_request_audit(data: dict[str, Any], status: str, note: str, actor: str = "desktop") -> dict[str, Any]:
    updated_at = time.strftime("%Y-%m-%d %H:%M:%S")
    data["updated_at"] = updated_at
    audit = data.setdefault("audit", [])
    if not isinstance(audit, list):
        audit = []
        data["audit"] = audit
    audit.append({
        "status": status,
        "actor": actor,
        "note": note,
        "updated_at": updated_at,
    })
    return data


def create_execution_packet(request_id: str) -> dict[str, Any]:
    data = load_project_request(request_id)
    if data.get("status") != "approved":
        raise ValueError("execution packet requires an approved request")

    EXECUTION_DIR.mkdir(parents=True, exist_ok=True)
    packet_path = EXECUTION_DIR / f"{request_id}.md"
    content = "\n".join([
        f"# Project Assistant Execution Packet {request_id}",
        "",
        "## Request",
        f"Project: {data.get('project', DEFAULT_PROJECT)}",
        f"Status: {data.get('status')}",
        f"Context hash: {data.get('context_hash', '')}",
        "",
        str(data.get("text", "")),
        "",
        "## Context",
        str(data.get("context", "")),
        "",
        "## Safety Rules",
        "- Desktop Codex may handle this only after human review.",
        "- Show diff before completion.",
        "- Run verification before completion.",
        "- Do not flash devices.",
        "- Do not run Git push, reset, destructive shell, or sudo without explicit user approval.",
        "- Record concise result summary after work is done.",
        "",
        "## Completion Requirements",
        "- Files changed",
        "- Verification commands and pass/fail result",
        "- Short next step",
        "",
    ])
    packet_path.write_text(content, encoding="utf-8")

    data = _append_request_audit(
        data,
        "execution_packet",
        f"created execution packet: {packet_path}",
        actor="desktop",
    )
    path = _request_file(request_id)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {**data, "path": str(path), "packet_path": str(packet_path)}


def _format_request_result_reply(result: dict[str, Any]) -> str:
    files = result.get("files_changed", [])
    verification = result.get("verification", [])
    return "\n".join([
        f"Request result: {result.get('summary', '')}",
        "Files changed:",
        "\n".join(f"- {item}" for item in files) or "- none",
        "Verification:",
        "\n".join(f"- {item}" for item in verification) or "- none",
        f"Next: {result.get('next_step', '')}",
    ]).strip()


def record_request_result(
    request_id: str,
    summary: str,
    files_changed: list[str] | None = None,
    verification: list[str] | None = None,
    next_step: str = "",
) -> dict[str, Any]:
    if not summary.strip():
        raise ValueError("summary is required")

    data = load_project_request(request_id)
    if data.get("status") != "approved":
        raise ValueError("result recording requires an approved request")

    result = {
        "summary": summary.strip(),
        "files_changed": files_changed or [],
        "verification": verification or [],
        "next_step": next_step.strip(),
        "updated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    data["status"] = "done"
    data["result"] = result
    data = _append_request_audit(data, "done", summary.strip(), actor="desktop")

    path = _request_file(request_id)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    record_latest_result(
        AssistantRequest(text=str(data.get("text", "")), project=str(data.get("project", DEFAULT_PROJECT)), mode="request_result"),
        _format_request_result_reply(result),
    )
    return {**data, "path": str(path)}


def run_assistant(req: AssistantRequest, mock: bool = False, timeout: int = 90) -> str:
    if is_write_intent(req.text):
        raise PermissionError("phase 1 is read-only; write or command intents are blocked")

    prompt = build_claude_prompt(req.text, req.project, mode=req.mode)
    if mock:
        return f"[mock] {req.project}: {req.text}"

    if not _CLAUDE_SEM.acquire(blocking=True, timeout=5):
        raise RuntimeError("another Claude request is already running; please wait")

    command = os.environ.get("CLAUDE_PROJECT_GATEWAY_CMD", "claude --model claude-sonnet-4-6 -p")
    argv = command.split()
    try:
        proc = subprocess.run(
            argv + [prompt],
            cwd=str(PROJECT_ROOT),
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
    except FileNotFoundError as exc:
        raise RuntimeError("claude command not found; set CLAUDE_PROJECT_GATEWAY_CMD") from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"claude command timed out after {timeout}s") from exc
    finally:
        _CLAUDE_SEM.release()

    output = proc.stdout.strip()
    if proc.returncode != 0:
        err = proc.stderr.strip() or output or f"exit code {proc.returncode}"
        raise RuntimeError(err[:500])
    if not output:
        raise RuntimeError("claude returned an empty response")
    return sanitize_display_text(output)


class Handler(BaseHTTPRequestHandler):
    mock = False
    timeout = DEFAULT_ASSISTANT_TIMEOUT

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"[gateway] {self.address_string()} {fmt % args}")

    def _send(self, status: int, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/health":
            self._send(200, json_response(True, service="claude_project_gateway"))
        elif parsed.path == "/api/project/context":
            self._send(200, json_response(True, context=collect_project_context()))
        elif parsed.path == "/api/project/results/latest":
            self._send(200, json.dumps(latest_result_response(), ensure_ascii=False).encode("utf-8"))
        elif parsed.path == "/api/project/requests":
            query = parse_qs(parsed.query)
            status = query.get("status", [None])[0]
            try:
                requests = list_project_requests(status=status)
                self._send(200, json_response(True, requests=requests))
            except ValueError as exc:
                self._send(400, json_response(False, error=str(exc)))
        else:
            self._send(404, json_response(False, error="not found"))

    def do_POST(self) -> None:
        if self.path == "/api/project/request":
            self._handle_project_request()
            return
        if self.path == "/api/project/request/status":
            self._handle_project_request_status()
            return
        if self.path != "/api/claude/message":
            self._send(404, json_response(False, error="not found"))
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            req = parse_request(self.rfile.read(length))
            reply = run_assistant(req, mock=self.mock, timeout=self.timeout)
            record_latest_result(req, reply)
            self._send(200, json_response(True, reply=reply))
        except ValueError as exc:
            self._send(400, json_response(False, error=str(exc)))
        except PermissionError as exc:
            self._send(403, json_response(False, error=str(exc)))
        except Exception as exc:
            self._send(502, json_response(False, error=str(exc)))

    def _handle_project_request_status(self) -> None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(payload, dict):
                raise ValueError("json object is required")
            updated = update_project_request_status(
                str(payload.get("id", "")).strip(),
                str(payload.get("status", "")).strip(),
                note=str(payload.get("note", "")).strip(),
                actor="desktop",
            )
            self._send(
                200,
                json_response(
                    True,
                    id=updated["id"],
                    status=updated["status"],
                    path=updated["path"],
                    audit=updated.get("audit", []),
                ),
            )
        except (ValueError, FileNotFoundError) as exc:
            self._send(400, json_response(False, error=str(exc)))
        except Exception as exc:
            self._send(502, json_response(False, error=str(exc)))

    def _handle_project_request(self) -> None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
            req = parse_project_request(self.rfile.read(length))
            pending = create_pending_request(req)
            self._send(
                200,
                json_response(
                    True,
                    id=pending["id"],
                    status=pending["status"],
                    path=pending["path"],
                    context_hash=pending["context_hash"],
                ),
            )
        except ValueError as exc:
            self._send(400, json_response(False, error=str(exc)))
        except PermissionError as exc:
            self._send(403, json_response(False, error=str(exc)))
        except Exception as exc:
            self._send(502, json_response(False, error=str(exc)))


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the Tab5 Claude project gateway")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", default=DEFAULT_PORT, type=int)
    parser.add_argument("--mock", action="store_true")
    parser.add_argument("--timeout", default=DEFAULT_ASSISTANT_TIMEOUT, type=int)
    args = parser.parse_args()

    Handler.mock = args.mock
    Handler.timeout = args.timeout
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"claude project gateway listening on http://{args.host}:{args.port}")
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
