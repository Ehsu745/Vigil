#!/usr/bin/env python3
"""Add or remove the vigil hook in ~/.claude/settings.json.

  python3 install_hooks.py            # dry run: print the hooks section that would be written
  python3 install_hooks.py --apply    # back up settings.json, then merge the hook in
  python3 install_hooks.py --remove   # take the hook out again (other hooks are kept)

Add --pane to the first two to also open the side pane in iTerm2 whenever a session starts.
Add --rich to run the hook with DODECA_RICH=1: it then also keeps Claude's last reply, the last notification
text and the iTerm2 session id in ~/.dodeca-core/msg/ for the floating panel (see the README, Privacy).
"""
import difflib
import json
import os
import shlex
import shutil
import sys
import time

SETTINGS = os.path.expanduser("~/.claude/settings.json")
HOOK_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hook.py")
COMMAND = "python3 " + shlex.quote(HOOK_FILE)
RICH_COMMAND = "DODECA_RICH=1 " + COMMAND
PANE_COMMAND = "sh " + shlex.quote(os.path.join(os.path.dirname(HOOK_FILE), "open_pane.sh"))
EVENTS = ["SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse",
          "PostToolUse", "Notification", "Stop"]
TOOL_EVENTS = {"PreToolUse", "PostToolUse"}


def is_ours(entry):
    cmds = [h.get("command", "") for h in entry.get("hooks", [])]
    return any(c in (COMMAND, RICH_COMMAND, PANE_COMMAND) or (("vigil" in c or "dodeca-core" in c) and ("hook.py" in c or "open_pane.sh" in c))  # (dodeca-core: the old name) for c in cmds)


def merge(cfg, remove, pane, rich):
    hooks = cfg.setdefault("hooks", {})
    for ev in EVENTS:
        entries = [e for e in hooks.get(ev, []) if not is_ours(e)]
        if not remove:
            entry = {"hooks": [{"type": "command", "command": RICH_COMMAND if rich else COMMAND}]}
            if ev in TOOL_EVENTS:
                entry["matcher"] = "*"
            entries.append(entry)
            if pane and ev == "SessionStart":
                entries.append({"hooks": [{"type": "command", "command": PANE_COMMAND}]})
        if entries:
            hooks[ev] = entries
        else:
            hooks.pop(ev, None)
    if not hooks:
        cfg.pop("hooks")
    return cfg


def main():
    apply, remove = "--apply" in sys.argv, "--remove" in sys.argv
    cfg = {}
    if os.path.exists(SETTINGS):
        with open(SETTINGS, encoding="utf-8") as f:
            cfg = json.load(f)
    new = merge(json.loads(json.dumps(cfg)), remove, "--pane" in sys.argv, "--rich" in sys.argv)
    if not (apply or remove):
        print(json.dumps({"hooks": new.get("hooks", {})}, indent=2, ensure_ascii=False))
        before = json.dumps({"hooks": cfg.get("hooks", {})}, indent=2, ensure_ascii=False).splitlines()
        after = json.dumps({"hooks": new.get("hooks", {})}, indent=2, ensure_ascii=False).splitlines()
        diff = list(difflib.unified_diff(before, after, "settings.json now", "after --apply", lineterm="", n=1))
        print("\nChanges against your settings now:" if diff else "\nNo changes against your settings now.")
        for line in diff:
            print(line)
        print("\n(dry run: nothing written. Run with --apply to merge this into %s)" % SETTINGS)
        return
    os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
    if os.path.exists(SETTINGS):
        backup = SETTINGS + ".bak-" + time.strftime("%Y%m%d-%H%M%S")
        shutil.copy2(SETTINGS, backup)
        print("backup:", backup)
    with open(SETTINGS, "w", encoding="utf-8") as f:
        f.write(json.dumps(new, indent=2, ensure_ascii=False) + "\n")
    print("removed from" if remove else "installed into", SETTINGS)


if __name__ == "__main__":
    main()
