#!/usr/bin/env python3
"""Add or remove the dodeca-core hook in ~/.claude/settings.json.

  python3 install_hooks.py            # dry run: print the hooks section that would be written
  python3 install_hooks.py --apply    # back up settings.json, then merge the hook in
  python3 install_hooks.py --remove   # take the hook out again (other hooks are kept)

Add --pane to the first two to also open the side pane in iTerm2 whenever a session starts.
"""
import json
import os
import shlex
import shutil
import sys
import time

SETTINGS = os.path.expanduser("~/.claude/settings.json")
HOOK_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hook.py")
COMMAND = "python3 " + shlex.quote(HOOK_FILE)
PANE_COMMAND = "sh " + shlex.quote(os.path.join(os.path.dirname(HOOK_FILE), "open_pane.sh"))
EVENTS = ["SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse",
          "PostToolUse", "Notification", "Stop"]
TOOL_EVENTS = {"PreToolUse", "PostToolUse"}


def is_ours(entry):
    cmds = [h.get("command", "") for h in entry.get("hooks", [])]
    return any(c in (COMMAND, PANE_COMMAND) or ("dodeca-core" in c and ("hook.py" in c or "open_pane.sh" in c)) for c in cmds)


def merge(cfg, remove, pane):
    hooks = cfg.setdefault("hooks", {})
    for ev in EVENTS:
        entries = [e for e in hooks.get(ev, []) if not is_ours(e)]
        if not remove:
            entry = {"hooks": [{"type": "command", "command": COMMAND}]}
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
    new = merge(json.loads(json.dumps(cfg)), remove, "--pane" in sys.argv)
    if not (apply or remove):
        print(json.dumps({"hooks": new.get("hooks", {})}, indent=2, ensure_ascii=False))
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
