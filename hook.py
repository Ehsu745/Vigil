#!/usr/bin/env python3
"""Claude Code hook for vigil.

Appends one JSON line per event to ~/.dodeca-core/events.jsonl:
time, session id, event name, folder name, tool name, notification type.
It never stores prompts, tool input/output or full paths in that file.

With DODECA_RICH=1 it also keeps one small file per session, ~/.dodeca-core/msg/<session id>.json,
for the floating panel's tooltip and voice features: Claude's last reply, the last notification text
(e.g. a permission prompt) and the iTerm2 session or tmux pane the conversation runs in (where a reply
typed in the panel goes). The text never goes into the
event file; a reply only adds "m": 1 to its Stop line. The file is deleted when the session ends.

Always exits 0 and prints nothing, so it cannot block or change Claude Code.
"""
import json
import os
import re
import sys
import time

PATH = os.environ.get("DODECA_EVENTS", os.path.expanduser("~/.dodeca-core/events.jsonl"))
MSG_DIR = os.path.join(os.path.dirname(PATH), "msg")
RICH = os.environ.get("DODECA_RICH") == "1"
KNOWN = {"SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse",
         "PostToolUse", "Notification", "Stop"}
MAX_TEXT = 4000
STALE = 24 * 3600          # msg files of sessions that never sent SessionEnd
TRANSCRIPT_TAIL = 262144   # how much of the transcript the fallback reads, from the end


def msg_path(sid):
    return os.path.join(MSG_DIR, sid + ".json") if re.fullmatch(r"[A-Za-z0-9_-]{1,64}", sid) else None


def last_reply_from_transcript(path):
    """The text of the last assistant message in the transcript (only if Stop did not include it)."""
    if not path or not os.path.isfile(path):
        return ""
    with open(path, "rb") as f:
        f.seek(max(0, os.path.getsize(path) - TRANSCRIPT_TAIL))
        lines = f.read().decode("utf-8", "replace").splitlines()
    for line in reversed(lines):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if d.get("type") != "assistant":
            continue
        content = (d.get("message") or {}).get("content")
        if isinstance(content, str):
            return content
        text = "\n".join(c.get("text", "") for c in content or [] if isinstance(c, dict) and c.get("type") == "text")
        if text.strip():
            return text
    return ""


def update_msg(sid, fields):
    """Merge fields into the session's msg file; written to a temp file and renamed, so a reader never sees half."""
    p = msg_path(sid)
    if not p:
        return False
    os.makedirs(MSG_DIR, mode=0o700, exist_ok=True)
    cur = {}
    try:
        with open(p, encoding="utf-8") as f:
            cur = json.load(f)
    except (OSError, ValueError):
        pass
    cur.update(fields)
    cur["t"] = time.time()
    tmp = "%s.%d.tmp" % (p, os.getpid())
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(fd, json.dumps(cur, ensure_ascii=False).encode())
    finally:
        os.close(fd)
    os.replace(tmp, p)
    return True


def drop_msg(sid):
    p = msg_path(sid)
    if p and os.path.exists(p):
        os.remove(p)


def drop_stale():
    if not os.path.isdir(MSG_DIR):
        return
    now = time.time()
    for name in os.listdir(MSG_DIR):
        p = os.path.join(MSG_DIR, name)
        try:
            if now - os.path.getmtime(p) > STALE:
                os.remove(p)
        except OSError:
            pass


def rich(d, ev, sid, rec):
    if ev == "SessionStart":
        drop_stale()
        pane = os.environ.get("TMUX_PANE", "")
        update_msg(sid, {"iterm": os.environ.get("ITERM_SESSION_ID", "")[:80],
                         "tmux": pane if re.fullmatch(r"%\d{1,6}", pane) else ""})
    elif ev == "SessionEnd":
        drop_msg(sid)
    elif ev == "Stop":
        text = d.get("last_assistant_message")
        if not isinstance(text, str):
            text = last_reply_from_transcript(d.get("transcript_path"))
        if text.strip() and update_msg(sid, {"reply": text[:MAX_TEXT], "summary": ""}):  # new reply: old summary is stale
            rec["m"] = 1
    elif ev == "Notification":
        text = d.get("message")
        if isinstance(text, str) and text.strip() and d.get("notification_type") != "idle_prompt":
            update_msg(sid, {"notice": text[:MAX_TEXT]})


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
    if RICH:
        try:
            rich(d, ev, rec["sid"], rec)
        except Exception:
            pass  # the event line is written regardless
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
