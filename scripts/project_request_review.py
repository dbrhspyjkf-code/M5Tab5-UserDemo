#!/usr/bin/env python3
"""Desktop approval helper for Tab5 Project Assistant requests.

This tool only reads and updates queue JSON files. It never executes queued
work. Real implementation still has to happen in a separate desktop Codex
session after the user reviews and confirms the request.
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GATEWAY = ROOT / "scripts" / "claude_project_gateway.py"


def _gateway():
    spec = importlib.util.spec_from_file_location("claude_project_gateway", GATEWAY)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _print_request(item: dict) -> None:
    print(f"{item.get('id')} [{item.get('status')}] {item.get('project')}")
    print(f"  created: {item.get('created_at', '')}")
    print(f"  text: {item.get('text', '')}")
    print(f"  path: {item.get('path', '')}")


def list_requests(args: argparse.Namespace) -> int:
    gw = _gateway()
    for item in gw.list_project_requests(status=args.status):
        _print_request(item)
    return 0


def show_request(args: argparse.Namespace) -> int:
    gw = _gateway()
    item = gw.load_project_request(args.id)
    _print_request(item)
    print("\nContext:")
    print(item.get("context", ""))
    print("\nAudit:")
    for audit in item.get("audit", []):
        print(f"  {audit.get('updated_at')} {audit.get('actor')} -> {audit.get('status')}: {audit.get('note', '')}")
    return 0


def approve_request(args: argparse.Namespace) -> int:
    gw = _gateway()
    item = gw.update_project_request_status(args.id, "approved", note=args.note)
    print(f"approved {item['id']}")
    return 0


def reject_request(args: argparse.Namespace) -> int:
    gw = _gateway()
    item = gw.update_project_request_status(args.id, "rejected", note=args.note)
    print(f"rejected {item['id']}")
    return 0


def mark_done(args: argparse.Namespace) -> int:
    gw = _gateway()
    item = gw.update_project_request_status(args.id, "done", note=args.note)
    print(f"done {item['id']}")
    return 0


def create_packet(args: argparse.Namespace) -> int:
    gw = _gateway()
    item = gw.create_execution_packet(args.id)
    print(item["packet_path"])
    return 0


def record_result(args: argparse.Namespace) -> int:
    gw = _gateway()
    item = gw.record_request_result(
        args.id,
        summary=args.summary,
        files_changed=args.file,
        verification=args.verification,
        next_step=args.next_step,
    )
    print(f"result recorded {item['id']}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Review Tab5 Project Assistant requests")
    sub = parser.add_subparsers(dest="command", required=True)

    list_parser = sub.add_parser("list", help="list queued requests")
    list_parser.add_argument("--status", choices=["pending", "approved", "rejected", "done"])
    list_parser.set_defaults(func=list_requests)

    show_parser = sub.add_parser("show", help="show one request")
    show_parser.add_argument("id")
    show_parser.set_defaults(func=show_request)

    approve_parser = sub.add_parser("approve", help="mark a pending request approved")
    approve_parser.add_argument("id")
    approve_parser.add_argument("--note", default="approved on desktop")
    approve_parser.set_defaults(func=approve_request)

    reject_parser = sub.add_parser("reject", help="mark a pending request rejected")
    reject_parser.add_argument("id")
    reject_parser.add_argument("--note", default="rejected on desktop")
    reject_parser.set_defaults(func=reject_request)

    done_parser = sub.add_parser("done", help="mark an approved request done")
    done_parser.add_argument("id")
    done_parser.add_argument("--note", default="marked done on desktop")
    done_parser.set_defaults(func=mark_done)

    packet_parser = sub.add_parser("packet", help="create a desktop Codex execution packet")
    packet_parser.add_argument("id")
    packet_parser.set_defaults(func=create_packet)

    result_parser = sub.add_parser("result", help="record the reviewed desktop execution result")
    result_parser.add_argument("id")
    result_parser.add_argument("--summary", required=True)
    result_parser.add_argument("--file", action="append", default=[])
    result_parser.add_argument("--verification", action="append", default=[])
    result_parser.add_argument("--next-step", default="")
    result_parser.set_defaults(func=record_result)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
