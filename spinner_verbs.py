#!/usr/bin/env python3
"""Claude Code's own spinner words ("Pondering", "Zigzagging", ...), read from the Claude Code installed on this
machine, for the panel's running line ("Pondering... (1m 5s)"). Vigil does not ship the list: it is part of Claude
Code. This reads it out of the user's own copy and keeps it in claude-verbs.txt next to the events file, one word
a line (mode 600). hook.py runs it in the background at SessionStart when Claude Code is newer than that file;
it can also be run by hand. Standard library only; it never starts Claude Code.
"""
import glob
import mmap
import os
import re
import sys

def data_dir():
    """~/.vigil; the old ~/.dodeca-core is moved there the first time (the name before Vigil)."""
    new, old = os.path.expanduser("~/.vigil"), os.path.expanduser("~/.dodeca-core")
    if not os.path.exists(new) and os.path.isdir(old):
        try:
            os.rename(old, new)
        except OSError:  # (another hook moved it at the same moment: fine)
            return new if os.path.exists(new) else old
    return new


PATH = os.environ.get("VIGIL_EVENTS") or os.environ.get("DODECA_EVENTS") or os.path.join(data_dir(), "events.jsonl")
OUT = os.path.join(os.path.dirname(PATH), "claude-verbs.txt")
WORD = re.compile(r"^[A-Z][A-Za-z'\u2019-]+$")


def installs():
    """Claude Code's program files, the newest first: the native build's versions, then an npm install's cli.js."""
    found = glob.glob(os.path.expanduser("~/.local/share/claude/versions/*"))
    for root in ("~/.npm-global/lib", "/usr/local/lib", "/usr/lib", "~/.nvm/versions/node/*/lib"):
        found += glob.glob(os.path.expanduser(root + "/node_modules/@anthropic-ai/claude-code/cli.js"))
    return sorted((p for p in found if os.path.isfile(p)), key=os.path.getmtime, reverse=True)


def verbs_in(path):
    """The longest list of quoted words that holds one of the spinner's well-known words, or []."""
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        best = []
        for anchor in (b'"Pondering"', b'"Thinking"', b'"Working"'):
            at = m.find(anchor)
            while at >= 0 and not best:
                a, b = m.rfind(b"[", max(0, at - 20000), at), m.find(b"]", at, at + 20000)
                if a >= 0 and b >= 0:
                    words = re.findall(r'"([^"\\]{2,40})"', m[a:b + 1].decode("utf-8", "replace"))
                    good = [w for w in words if WORD.match(w)]
                    if len(good) >= 30 and len(good) >= 0.9 * len(words) and sum(w.endswith("ing") for w in good) >= 0.8 * len(good):
                        best = good
                at = m.find(anchor, at + 1)
            if best:
                break
        return best


def main():
    for path in installs():
        try:
            words = verbs_in(path)
        except (OSError, ValueError):
            continue
        if words:
            os.makedirs(os.path.dirname(OUT), mode=0o700, exist_ok=True)
            tmp = OUT + ".tmp"
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write("\n".join(dict.fromkeys(words)) + "\n")
            os.replace(tmp, OUT)
            if sys.stdout.isatty():
                print(f"{len(words)} words from {path} -> {OUT}")
            return 0
    if sys.stdout.isatty():
        print("no Claude Code install with its spinner words found")
    return 1


if __name__ == "__main__":
    sys.exit(main())
