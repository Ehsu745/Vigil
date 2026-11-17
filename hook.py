#!/usr/bin/env python3
"""Claude Code hook for vigil.

Appends one JSON line per event to ~/.dodeca-core/events.jsonl:
time, session id, event name, folder name, tool name, notification type, and for a subagent's events its
id and type (e.g. "Explore"), so it can be shown as a core of its own.
It never stores prompts, tool input/output or full paths in that file.

With DODECA_RICH=1 it also keeps one small file per session, ~/.dodeca-core/msg/<session id>.json,
for the floating panel's chat box and voice features: Claude's last reply, the last notification text
(e.g. a permission prompt), the iTerm2 session, Windows Terminal tab, tmux pane or vigil-claude launcher the
conversation runs in (where a reply typed in the panel goes), and the last few turns as the terminal shows them: each prompt, its reply and
its steps (a tool name with a short detail: a command's first line, a file's name without its folder,
a search pattern), plus the step running now. The text never goes into the event file; a reply only
adds "m": 1 to its Stop line. A subagent's final reply goes into a file of its own,
msg/<session id>-<agent id>.json. The files are deleted when the session ends.

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
         "PostToolUse", "Notification", "Stop", "SubagentStart", "SubagentStop"}
MAX_TEXT = 4000
MAX_PROMPT = 2000
TURNS = 5                  # turns kept: prompt, reply and steps; the last is the one going on
STEPS = 16                 # steps kept per turn, Claude's words between them too (the count and the tally keep the rest)
MSG_BUDGET = 24000         # the oldest turns go first when the file grows past this
STALE = 24 * 3600          # msg files of sessions that never sent SessionEnd
LIVE_FOR = 600             # a turn with no step for this long (no Stop came: an error) takes no more of your messages
TRANSCRIPT_TAIL = 262144   # how much of the transcript the fallback reads, from the end


def sub_sid(sid, aid):
    """A subagent's own id, as the panel and the terminal make it: its session's id and its agent id."""
    return (sid + "-" + aid)[:64]


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
    if tool == "Bash":  # what Claude says the command does, else its first line
        if isinstance(ti.get("description"), str) and ti["description"].strip():
            return one_line(ti["description"])
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


HARNESS = ("<command-", "<local-command", "<system-reminder>", "<bash-", "<task-notification", "Caveat:", "[Request interrupted")


def prompt_text(m):
    """A typed prompt in a transcript user line ("" for tool results, commands and the harness's own lines)."""
    c = m.get("content")
    if isinstance(c, list):
        c = "\n".join(x.get("text", "") for x in c if isinstance(x, dict) and x.get("type") == "text")
    if not isinstance(c, str):
        return ""
    t = c.strip()
    if not t or t.startswith(HARNESS):
        return ""
    return t


def notice_step(p):
    """A background task's notice as a step of the turn it came in ("" for the harness's other lines)."""
    if not p.startswith("<task-notification"):
        return None
    m = re.search(r"<summary>(.*?)</summary>", p, re.S)
    return ["@notice", " ".join((m.group(1) if m else "background task").split())[:80]]


def tail_entries(path):
    """The transcript's last entries, newest first (its tail only)."""
    if not path or not os.path.isfile(path):
        return []
    with open(path, "rb") as f:
        f.seek(max(0, os.path.getsize(path) - TRANSCRIPT_TAIL))
        lines = f.read().decode("utf-8", "replace").splitlines()
    out = []
    for line in reversed(lines):
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if isinstance(e, dict) and not e.get("isSidechain") and not e.get("isMeta") and isinstance(e.get("message"), dict):
            out.append(e)
    return out


def turn_scan(path, sent=()):
    """Since your prompt (messages you sent during the turn, `sent`, do not end it): Claude's newest words ("" if none
    yet) and the output tokens of its messages (a message's usage is repeated on each of its blocks: counted once)."""
    said, tok = "", {}
    for e in tail_entries(path):
        m = e["message"]
        if e.get("type") == "user":
            p = prompt_text(m)
            if p and p[:300] not in sent:
                break  # back to your prompt
            continue
        if e.get("type") != "assistant":
            continue
        u = m.get("usage")
        if isinstance(u, dict) and isinstance(u.get("output_tokens"), int):
            k = m.get("id") or id(m)
            tok[k] = max(tok.get(k, 0), u["output_tokens"])
        if not said and isinstance(m.get("content"), list):
            said = "\n".join(x.get("text", "") for x in m["content"] if isinstance(x, dict) and x.get("type") == "text").strip()
    return said, sum(tok.values())


def words_for(path, tid):
    """What Claude said in the message that makes tool call tid (its text before the call, as the terminal shows it):
    "" when it said nothing, None when the message is not in the transcript yet (Claude Code writes it a moment after
    the tool starts, so at PreToolUse it is often still missing)."""
    if not tid:
        return None
    es = tail_entries(path)
    mid = None
    for e in es:
        c = e["message"].get("content")
        if e.get("type") == "assistant" and isinstance(c, list) and any(
                isinstance(x, dict) and x.get("type") == "tool_use" and x.get("id") == tid for x in c):
            mid = e["message"].get("id") or id(e)
            break
    if mid is None:
        return None
    said = []
    for e in reversed(es):  # oldest first: its blocks in order (one transcript line each, the same message id)
        m = e["message"]
        if e.get("type") == "assistant" and (m.get("id") or None) == mid and isinstance(m.get("content"), list):
            said += [x.get("text", "") for x in m["content"] if isinstance(x, dict) and x.get("type") == "text"]
    return "\n".join(x for x in said if x.strip()).strip()


def say_before(steps, i, said):
    """Claude's words put in before step i (once: not when the same words are there already)."""
    said = one_line(said, 300)
    if not said or any(isinstance(x, list) and len(x) > 1 and x[0] == "@say" and x[1] == said for x in steps):
        return steps
    return steps[:i] + [["@say", said]] + steps[i:]


def interrupted(path, p):
    """Whether the turn before prompt p was stopped by you (Esc): the transcript's "[Request interrupted" line comes
    after its last prompt."""
    for e in tail_entries(path):
        if e.get("type") != "user":
            continue
        c = e["message"].get("content")
        if isinstance(c, list):
            c = "\n".join(x.get("text", "") for x in c if isinstance(x, dict) and x.get("type") == "text")
        if isinstance(c, str) and c.strip().startswith("[Request interrupted"):
            return True
        q = prompt_text(e["message"])
        if q and q != p:
            return False
    return False


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
        if p.lstrip().startswith(HARNESS):  # the harness's own line (a background task's notice): no turn of its own;
            step = notice_step(p.lstrip())  # the notice is a step of the turn going on, which keeps your prompt
            if step and turns:
                s = turns[-1].get("s") if isinstance(turns[-1].get("s"), list) else []
                turns[-1]["s"] = (s + [step])[-STEPS:]
        elif turns and turns[-1].get("p") == p and not turns[-1].get("r"):
            turns[-1].setdefault("t0", int(time.time()))  # (already in the transcript: read in by the first fill)
        elif turns and turns[-1].get("t0") and not turns[-1].get("d") and not turns[-1].get("r") \
                and time.time() - (turns[-1].get("ts") or turns[-1]["t0"]) < LIVE_FOR \
                and not interrupted(d.get("transcript_path"), p):
            t = turns[-1]  # sent while the turn runs: a line of that turn, as the terminal shows it
            s = t.get("s") if isinstance(t.get("s"), list) else []
            t["s"] = (s + [["@you", p[:300]]])[-STEPS:]
            t["you"] = (t.get("you") or [])[-7:] + [p[:300]]
            update_msg(sid, {"turns": fit(turns)})
            return
        else:
            last = turns[-1] if turns else None
            if last and not last.get("n") and not last.get("r") and p.startswith(last.get("p") or "\0"):
                turns.pop()  # stopped before any step and sent again, longer: the new one only
            turns.append({"p": p, "r": "", "s": [], "n": 0, "c": {}, "t0": int(time.time())})
        fields["run"] = None
    else:
        if not turns:
            turns.append({"p": "", "r": "", "s": [], "n": 0, "c": {}})
        t = turns[-1]
        tool = str(d.get("tool_name", ""))[:40]
        if ev == "PreToolUse":
            tid = str(d.get("tool_use_id") or "")[:80]
            step = [tool, step_detail(tool, d.get("tool_input"))] + ([tid] if tid else [])  # (the id: its words may come later)
            s = t.get("s") if isinstance(t.get("s"), list) else []
            try:  # the turn's tokens so far, and what Claude said in the message making this call (if written yet)
                _, tok = turn_scan(d.get("transcript_path"), list(t.get("you") or []))
                if tok > int(t.get("k") or 0):
                    t["k"] = tok
                said = words_for(d.get("transcript_path"), tid)
            except Exception:
                said = None
            s = s + [step]
            if said:
                s = say_before(s, len(s) - 1, said)
            t["s"] = s[-STEPS:]
            t["n"] = int(t.get("n") or 0) + 1
            t["ts"] = int(time.time())
            c = t.get("c") if isinstance(t.get("c"), dict) else {}
            c[tool] = int(c.get(tool) or 0) + 1
            t["c"] = c
            fields["run"] = step
        elif ev == "PostToolUse":
            t["ts"] = int(time.time())  # (the last sign of life: the running line reddens after a long quiet)
            fields["run"] = None
            tid = str(d.get("tool_use_id") or "")[:80]
            s = t.get("s") if isinstance(t.get("s"), list) else []
            at = next((i for i, x in enumerate(s) if isinstance(x, list) and len(x) > 2 and x[2] == tid), -1) if tid else -1
            if at >= 0 and not (at > 0 and isinstance(s[at - 1], list) and s[at - 1][:1] == ["@say"]):
                try:  # its words were not written yet when it started: now they are, before it
                    said = words_for(d.get("transcript_path"), tid)
                except Exception:
                    said = None
                if said:
                    t["s"] = say_before(s, at, said)[-STEPS:]
        elif ev == "Stop":
            t["r"] = d.get("_reply", "")[:MAX_TEXT]
            t["d"] = 1
            fields["run"] = None
    fields["turns"] = fit(turns)
    update_msg(sid, fields)


def drop_msg(sid):
    p = msg_path(sid)
    if p and os.path.exists(p):
        os.remove(p)
    if p and os.path.isdir(MSG_DIR):  # its subagents' replies too
        for name in os.listdir(MSG_DIR):
            if name.startswith(sid + "-") and name.endswith(".json"):
                try:
                    os.remove(os.path.join(MSG_DIR, name))
                except OSError:
                    pass


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


def verbs_refresh():
    """Claude Code updated since its spinner words were read (or never read): spinner_verbs.py reads them again, in the
    background, so this returns at once."""
    try:
        import glob
        import subprocess
        out = os.path.join(os.path.dirname(PATH), "claude-verbs.txt")
        cc = glob.glob(os.path.expanduser("~/.local/share/claude/versions/*"))
        newest = max((os.path.getmtime(p) for p in cc), default=0)
        if os.path.exists(out) and os.path.getmtime(out) >= newest:
            return
        script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "spinner_verbs.py")
        subprocess.Popen([sys.executable, script], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True, close_fds=True)
    except Exception:
        pass


def rich(d, ev, sid, rec):
    aid = rec.get("aid")
    if aid:  # a subagent's: only its final reply is kept, in a file of its own (its steps stay out of the session's turns)
        sub = sub_sid(sid, aid)
        if ev == "PreToolUse" and d.get("tool_name") == "SubagentHandback":  # its report, handed back as a tool call
            text = (d.get("tool_input") or {}).get("message")
            if isinstance(text, str) and text.strip():
                with Locked():
                    update_msg(sub, {"reply": text[:MAX_TEXT], "hb": 1})
        elif ev == "SubagentStop":
            text = d.get("last_assistant_message")
            with Locked():
                if read_msg(sub).get("hb"):  # (its closing words after the report: the report stays)
                    rec["m"] = 1
                elif isinstance(text, str) and text.strip() and update_msg(sub, {"reply": text[:MAX_TEXT]}):
                    rec["m"] = 1
        return
    if ev == "SessionStart":
        drop_stale()
        verbs_refresh()
        pane = os.environ.get("TMUX_PANE", "")
        inbox = os.environ.get("DODECA_INBOX", "")  # started by vigil-claude: the pid whose pipe takes typed replies
        fields = {"iterm": os.environ.get("ITERM_SESSION_ID", "")[:80],
                  "tmux": pane if re.fullmatch(r"%\d{1,6}", pane) else "",
                  "inbox": inbox if re.fullmatch(r"\d{1,9}", inbox) else "",
                  # the Windows Terminal tab it runs in (WT passes its id into WSL): where the Windows panel types a reply
                  "wt": wt if re.fullmatch(r"[0-9A-Fa-f-]{36}", wt := os.environ.get("WT_SESSION", "")) else ""}
        with Locked():
            if d.get("source") == "resume" and not turns_of(read_msg(sid)):
                # resumed (its file went when it ended): its turns and last reply again, from the transcript, at once
                try:
                    old = backfill(d.get("transcript_path"))
                except Exception:
                    old = []
                if old:  # (none yet in the transcript: left to the next prompt or Stop, as before)
                    fields.update(turns=fit(old), filled=True,
                                  reply=next((t["r"] for t in reversed(old) if t.get("r")), ""))
            update_msg(sid, fields)
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
        asks = d.get("notification_type") in (None, "", "permission_prompt", "elicitation_dialog")  # (a login or a quota notice asks nothing)
        if isinstance(text, str) and text.strip() and asks:
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
    aid = d.get("agent_id")
    if isinstance(aid, str) and re.fullmatch(r"[A-Za-z0-9_-]{1,64}", aid):  # fired in (or for) a subagent
        rec["aid"] = aid
        rec["at"] = re.sub(r"[^A-Za-z0-9_ .:-]", "", str(d.get("agent_type") or ""))[:32]
    if ev in ("SubagentStart", "SubagentStop") and not rec.get("at"):
        return  # Claude Code's own internal agents (no type): not shown
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
