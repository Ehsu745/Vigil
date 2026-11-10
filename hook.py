#!/usr/bin/env python3
"""Claude Code hook for dodeca-core.

Appends one JSON line per event to ~/.dodeca-core/events.jsonl:
time, session id, event name, folder name, tool name, notification type.
It never stores prompts, tool input/output or full paths.
Always exits 0 and prints nothing, so it cannot block or change Claude Code.
"""
import json
import os
import sys
import time

PATH = os.environ.get("DODECA_EVENTS", os.path.expanduser("~/.dodeca-core/events.jsonl"))
KNOWN = {"SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse",
         "PostToolUse", "Notification", "Stop"}


def main():
    d = json.load(sys.stdin)
    ev = d.get("hook_event_name", "")
    if ev not in KNOWN:
        return
    cwd = (d.get("cwd") or "").rstrip("/\\")
    rec = {
        "t": time.time(),
        "sid": str(d.get("session_id", "?"))[:64],
        "ev": ev,
        "cwd": os.path.basename(cwd)[:32],
        "tool": str(d.get("tool_name", ""))[:32],
        "n": str(d.get("notification_type", ""))[:24],
    }
    os.makedirs(os.path.dirname(PATH), mode=0o700, exist_ok=True)
    # one O_APPEND write per line: concurrent sessions never interleave
    fd = os.open(PATH, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
    try:
        os.write(fd, (json.dumps(rec) + "\n").encode())
    finally:
        os.close(fd)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
    sys.exit(0)
