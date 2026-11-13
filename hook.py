#!/usr/bin/env python3
"""Claude Code hook for vigil.

Appends one JSON line per event to ~/.dodeca-core/events.jsonl:
time, session id, event name, folder name, tool name, notification type.
It never stores prompts, tool input/output or full paths in that file.

With DODECA_RICH=1 it also keeps one small file per session, ~/.dodeca-core/msg/<session id>.json,
for the floating panel's chat box and voice features: Claude's last reply, the last notification text
(e.g. a permission prompt), the iTerm2 session or tmux pane the conversation runs in (where a reply
typed in the panel goes), and the last few turns as the terminal shows them: each prompt, its reply and
its steps (a tool name with a short detail: a command's first line, a file's name without its folder,
a search pattern), plus the step running now. The text never goes into the event file; a reply only
adds "m": 1 to its Stop line. The file is deleted when the session ends.

Always exits 0 and prints nothing, so it cannot block or change Claude Code.
"""
import json
import os
import re
import sys
import time
from urllib.parse import urlparse

try:
    import fcntl
except ImportError:  # not on Windows; the lock is only a nicety
    fcntl = None

PATH = os.environ.get("DODECA_EVENTS", os.path.expanduser("~/.dodeca-core/events.jsonl"))
MSG_DIR = os.path.join(os.path.dirname(PATH), "msg")
RICH = os.environ.get("DODECA_RICH") == "1"
KNOWN = {"SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse",
         "PostToolUse", "Notification", "Stop"}
MAX_TEXT = 4000
MAX_PROMPT = 2000
TURNS = 5                  # turns kept: prompt, reply and steps; the last is the one going on
STEPS = 8                  # steps kept per turn (the count and the tally keep the rest)
MSG_BUDGET = 24000         # the oldest turns go first when the file grows past this
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


def one_line(v, n=100):
    return re.sub(r"\s+", " ", v if isinstance(v, str) else "").strip()[:n]


def step_detail(tool, ti):
    """What a step works on, short: a command's first line, a file's name (never its folder), a pattern."""
    if not isinstance(ti, dict):
        return ""
    if tool == "Bash":
        cmd = ti.get("command")
        return one_line(cmd.strip().split("\n")[0] if isinstance(cmd, str) else "")
    if tool in ("Read", "Edit", "Write", "MultiEdit", "NotebookEdit", "NotebookRead"):
        f = ti.get("file_path") or ti.get("notebook_path")
        return one_line(os.path.basename(f.rstrip("/\\")) if isinstance(f, str) else "")
    if tool in ("Grep", "Glob"):
        return one_line(ti.get("pattern"))
    if tool == "WebFetch":
        u = ti.get("url")
        return one_line(urlparse(u).netloc if isinstance(u, str) else "")
    if tool == "WebSearch":
        return one_line(ti.get("query"))
    if tool == "Skill":
        return one_line(ti.get("skill") or ti.get("name"))
    return one_line(ti.get("description"))


class Locked:
    """One writer at a time per msg folder (parallel tool calls of one session), never waiting long."""
    def __enter__(self):
        self.fd = None
        if fcntl:
            try:
                os.makedirs(MSG_DIR, mode=0o700, exist_ok=True)
                self.fd = os.open(os.path.join(MSG_DIR, ".lock"), os.O_WRONLY | os.O_CREAT, 0o600)
                for _ in range(40):
                    try:
                        fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        break
                    except OSError:
                        time.sleep(0.005)
            except OSError:
                pass
        return self

    def __exit__(self, *a):
        if self.fd is not None:
            os.close(self.fd)  # closing lets the lock go


def read_msg(sid):
    try:
        with open(msg_path(sid), encoding="utf-8") as f:
            d = json.load(f)
            return d if isinstance(d, dict) else {}
    except (OSError, ValueError, TypeError):
        return {}


def turns_of(cur):
    t = cur.get("turns")
    return [x for x in t if isinstance(x, dict)] if isinstance(t, list) else []


def fit(turns):
    """The last TURNS turns, fewer when they grow past MSG_BUDGET (the one going on always stays)."""
    turns = turns[-TURNS:]
    while len(turns) > 1 and len(json.dumps(turns, ensure_ascii=False)) > MSG_BUDGET:
        turns = turns[1:]
    return turns


BACKFILL_MAX = 4 << 20     # how far back in the transcript the first fill reads at most


def prompt_text(m):
    """A typed prompt in a transcript user line ("" for tool results, commands and the harness's own lines)."""
    c = m.get("content")
    if isinstance(c, list):
        c = "\n".join(x.get("text", "") for x in c if isinstance(x, dict) and x.get("type") == "text")
    if not isinstance(c, str):
        return ""
    t = c.strip()
    if not t or t.startswith(("<command-", "<local-command", "<system-reminder>", "<bash-", "<task-notification", "Caveat:", "[Request interrupted")):
        return ""
    return t


def backfill(path):
    """The last TURNS turns read from the transcript: for a session whose turns began before the hook kept them."""
    if not path or not os.path.isfile(path):
        return []
    size, chunk, lines = os.path.getsize(path), 1 << 20, []
    with open(path, "rb") as f:
        while True:
            start = max(0, size - chunk)
            f.seek(start)
            data = f.read(size - start)
            lines = data.split(b"\n")
            if start:
                lines = lines[1:]  # cut in the middle of a line
            if not start or chunk >= BACKFILL_MAX or data.count(b'"type":"user"') > 40 * TURNS:
                break
            chunk *= 2
    turns = []
    for raw in lines:
        try:
            e = json.loads(raw)
        except ValueError:
            continue
        if not isinstance(e, dict) or e.get("isMeta") or e.get("isSidechain") or e.get("isCompactSummary"):
            continue
        m = e.get("message")
        if not isinstance(m, dict):
            continue
        if e.get("type") == "user":
            p = prompt_text(m)
            if p:
                turns.append({"p": p[:MAX_PROMPT], "r": "", "s": [], "n": 0, "c": {}})
        elif e.get("type") == "assistant" and turns and isinstance(m.get("content"), list):
            t = turns[-1]
            for x in m["content"]:
                if not isinstance(x, dict):
                    continue
                if x.get("type") == "text" and x.get("text", "").strip():
                    t["r"] = x["text"][:MAX_TEXT]  # the last text of the turn is its reply
                elif x.get("type") == "tool_use":
                    tool = str(x.get("name", ""))[:40]
                    t["s"] = (t["s"] + [[tool, step_detail(tool, x.get("input"))]])[-STEPS:]
                    t["n"] += 1
                    t["c"][tool] = t["c"].get(tool, 0) + 1
    return turns[-TURNS:]


def on_turn(sid, ev, d):
    """The turns: a prompt starts one, each tool call is a step of it, Stop gives it its reply."""
    cur = read_msg(sid)
    turns = turns_of(cur)
    fields = {}
    if not cur.get("filled") and ev in ("UserPromptSubmit", "Stop"):  # once: the turns from before, out of the transcript
        try:
            old = backfill(d.get("transcript_path"))
            if old:
                turns = old
        except Exception:
            pass
        fields["filled"] = True
    if ev == "UserPromptSubmit":
        p = d.get("prompt")
        p = p[:MAX_PROMPT] if isinstance(p, str) else ""
        if not (turns and turns[-1].get("p") == p and not turns[-1].get("r")):  # (already in the transcript)
            turns.append({"p": p, "r": "", "s": [], "n": 0, "c": {}})
        fields["run"] = None
    else:
        if not turns:
            turns.append({"p": "", "r": "", "s": [], "n": 0, "c": {}})
        t = turns[-1]
        tool = str(d.get("tool_name", ""))[:40]
        if ev == "PreToolUse":
            step = [tool, step_detail(tool, d.get("tool_input"))]
            s = t.get("s") if isinstance(t.get("s"), list) else []
            t["s"] = (s + [step])[-STEPS:]
            t["n"] = int(t.get("n") or 0) + 1
            c = t.get("c") if isinstance(t.get("c"), dict) else {}
            c[tool] = int(c.get(tool) or 0) + 1
            t["c"] = c
            fields["run"] = step
        elif ev == "PostToolUse":
            fields["run"] = None
        elif ev == "Stop":
            t["r"] = d.get("_reply", "")[:MAX_TEXT]
            fields["run"] = None
    fields["turns"] = fit(turns)
    update_msg(sid, fields)


def drop_msg(sid):
    p = msg_path(sid)
    if p and os.path.exists(p):
        os.remove(p)


def drop_stale():
    if not os.path.isdir(MSG_DIR):
        return
    now = time.time()
    for name in os.listdir(MSG_DIR):
        if name.startswith("."):
            continue  # the lock
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
        with Locked():
            if text.strip() and update_msg(sid, {"reply": text[:MAX_TEXT], "summary": ""}):  # new reply: old summary is stale
                rec["m"] = 1
            d["_reply"] = text
            on_turn(sid, ev, d)
    elif ev in ("UserPromptSubmit", "PreToolUse", "PostToolUse"):
        with Locked():
            on_turn(sid, ev, d)
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
