# CLAUDE.md

dodeca-core shows every running Claude Code session as a glowing polyhedron, either in the terminal or in a floating macOS panel. A Claude Code hook appends events to a JSONL file, and a C program reads that file, runs a state machine per session and draws the result with its own software renderer.

## Build and run

```sh
make                          # builds ./dodeca-core (C11, links zlib and libm)
./dodeca-core --demo          # fake sessions cycling through every state; q quits
sh mac/build.sh               # builds ./dodeca-panel, the macOS floating panel (Cocoa)
./dodeca-panel --demo
```

Both binaries are build output and are gitignored. Don't commit them.

## Checking a change

There is no test suite. To check a visual change without a terminal, render one demo frame to a file and look at it:

```sh
./dodeca-core --snapshot /tmp/f.ppm --at 10 --size 720x400   # state of the 3-session demo 10 s in
python3 -c "from PIL import Image; Image.open('/tmp/f.ppm').save('/tmp/f.png')"
```

`--at` picks the moment in the demo script (`SCRIPT` in `src/session.c`): session N starts 3.7·N s in, and one cycle through all states takes about 27 s. Build with no new warnings (`-Wall -Wextra`).

## Layout

```
Claude Code ──hook.py──▶ ~/.dodeca-core/events.jsonl ──tail──▶ state machine ──▶ renderer ──▶ terminal / panel
```

- `src/dodeca.h`: every shared type and function, grouped by the file that implements it. Start here.
- `src/events.c`: follows the JSONL file and parses lines into `Event`.
- `src/session.c`: event → state mapping, timings (`HOLD_WORK`, `CHEER_FOR`, `SLEEP_AFTER`, `DROP_AFTER`), the per-state appearance table `LOOKS`, and the built-in demo.
- `src/geom.c`: polyhedron shells (`dodeca`, `bucky`, `geo2`, `geo3`).
- `src/render.c`: draws into a float RGB `Canvas`.
- `src/main.c`: terminal main loop, the output modes (kitty / iTerm2 inline PNG / half-block cells), options, `--snapshot`.
- `mac/panel.m`: the floating panel. It reuses `events.c`, `geom.c`, `session.c` and `render.c`, but not `main.c`.
- `hook.py`, `install_hooks.py`: the Claude Code hook and its installer.
- `open_pane.sh`, `pane_api.py`: open and size the iTerm2 side pane on `SessionStart`.

To change colors or motion, edit `LOOKS` in `src/session.c`. How a core reacts to the mouse (panel only) is the `HOVER` table next to it; the panel's hit test (`coreAt:` in `mac/panel.m`) uses the shell outline `SHELL_HIT` from `dodeca.h`. A new state needs changes in `dodeca.h` (the enum), `session.c` (`STATE_NAME`, `STATE_TEXT`, `LOOKS`, the transitions), and both READMEs.

## Rules

- **The hook must never get in Claude Code's way.** `hook.py` always exits 0, prints nothing and finishes fast. It writes each event with a single `O_APPEND` write so concurrent sessions don't interleave lines.
- **Privacy:** the event file holds only time, session id, event name, folder name (not the full path), tool name and notification type. Never add prompts, tool input/output or paths. The file is mode 600 and its directory 700.
- **No dependencies:** the Python scripts use only the standard library. The C code uses only libc, libm and zlib (plus Cocoa for the panel).
- **Don't run `python3 install_hooks.py --apply` or `--remove` without asking.** They edit the user's `~/.claude/settings.json`. Running it without flags is a dry run and is safe.
- The labels use a built-in 3×5 bitmap font (ASCII letters, digits, a few symbols). Anything else renders as `?`.

## Docs

- The README is bilingual: `README.md` (English) and `README.zh-TW.md` (Traditional Chinese). Any README change goes into both, and both keep the language switch line at the top.
- `assets/demo.gif` is rendered from `--snapshot` frames (10 fps, `--at` 8 to 20 s, `--size 640x340`) and assembled with Pillow using one shared 128-color palette. Regenerate it if the visuals change noticeably, and keep it under about 5 MB.
- `hook.py`, `install_hooks.py` and the event reading in `src/events.c` are adapted from [zelvet-velvet/clawd-room](https://github.com/zelvet-velvet/clawd-room) (MIT). Keep his copyright line in `LICENSE` and the credit in both READMEs.
