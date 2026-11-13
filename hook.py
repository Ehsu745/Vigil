#!/usr/bin/env python3
"""Claude Code hook for dodeca-core.

Appends one JSON line per event to ~/.dodeca-core/events.jsonl:
time, session id, event name, folder name, tool name, notification type, and (when a session starts or a
prompt is sent) the process id of Claude Code, so the display can tell when that process is gone.
It never stores prompts, tool input/output or full paths.
Always exits 0 and prints nothing, so it cannot block or change Claude Code.
"""
import json
import os
import subprocess
import sys
import time

PATH = os.environ.get("DODECA_EVENTS", os.path.expanduser("~/.dodeca-core/events.jsonl"))
KNOWN = {"SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse",
         "PostToolUse", "Notification", "Stop"}


def claude_pid():
    """The nearest ancestor process whose command line mentions claude, or 0 if there is none to be sure of."""
    try:
        out = subprocess.run(["ps", "-axo", "pid=,ppid=,command="], capture_output=True, text=True, timeout=2).stdout
    except Exception:
        return 0
    procs = {}
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0].isdigit() and parts[1].isdigit():
            procs[int(parts[0])] = (int(parts[1]), parts[2])
    pid = os.getppid()
    for _ in range(6):
        if pid not in procs:
            return 0
        ppid, cmd = procs[pid]
        if "claude" in cmd.lower() and "hook.py" not in cmd:  # not the shell that was started to run this hook
            return pid
        pid = ppid
    return 0


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
    if ev in ("SessionStart", "UserPromptSubmit"):
        pid = claude_pid()
        if pid:
            rec["pid"] = pid
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
