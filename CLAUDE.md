# CLAUDE.md

The data folder `~/.dodeca-core` and the `DODECA_*` environment variables keep the old name until the hooks are reinstalled; renaming them means running `install_hooks.py --apply`, which needs the user's OK.

Vigil (formerly dodeca-core) shows every running Claude Code session as a glowing polyhedron, either in the terminal or in a floating macOS panel. A Claude Code hook appends events to a JSONL file, and a C program reads that file, runs a state machine per session and draws the result with its own software renderer.

## Build and run

```sh
make                          # builds ./dodeca-core (C11, links zlib and libm)
./dodeca-core --demo          # fake sessions cycling through every state; q quits
sh mac/build.sh               # builds Vigil.app and its launcher script ./vigil-panel (macOS floating panel)
./vigil-panel --demo
```

`vigil`, `Vigil.app` and the `./vigil-panel` launcher are build output and are gitignored. Don't commit them. The launcher starts the app with `open -n -W` so macOS attributes microphone and speech permissions to the app (a binary started straight from the terminal would count as iTerm2); it passes `--tty` and `--launcher-pid`, and the app quits when the launcher ends.

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

- `src/vigil.h`: every shared type and function, grouped by the file that implements it. Start here.
- `src/events.c`: follows the JSONL file and parses lines into `Event`.
- `src/session.c`: event → state mapping, timings (`HOLD_WORK`, `CHEER_FOR`, `SLEEP_AFTER`, `DROP_AFTER`), the per-state appearance table `LOOKS`, and the built-in demo.
- `src/geom.c`: polyhedron shells (`dodeca`, `bucky`, `geo2`, `geo3`).
- `src/motion.c`: how the shell moves (the "Core Bestiary" design): snap turns (`Motion.q`), random patch cuts and their offsets, layer turns, spikes, scan plane, inner shell, and one routine per state (`think`, `work`, `swim`, `unlock`, the cases in `motion_tick`), plus the geometric whims. Nothing overshoots: fixed easing curves or critically damped following. Patches need the triangles of the geodesic shells.
- `src/render.c`: draws into a float RGB `Canvas`; draws the shell per triangle when patches have moved.
- `src/main.c`: terminal main loop, the output modes (kitty / iTerm2 inline PNG / half-block cells), options, `--snapshot`.
- `mac/panel.m`: the floating panel. It reuses `events.c`, `geom.c`, `session.c` and `render.c`, but not `main.c`.
- `mac/tip.m`: the box beside the panel (hover tooltip, voice-reply preview): wraps its text, system font, title and edge bar in the session's colour.
- `mac/speech.m`: the panel's voice output: Markdown cleanup, the local summary, the one-at-a-time `Speaker` (`AVSpeechSynthesizer`), and the background `claude -p` summary (hooks off via `--settings`, so it never shows up as a session). Never call `claude` from the hook.
- `mac/listen.m`: push-to-talk recognition (`SFSpeechRecognizer`, `requiresOnDeviceRecognition` always on: never fall back to server recognition). The panel types the result into the session's iTerm2 pane with AppleScript, or `pane_api.py send` as the fallback; Claude Code needs the text and Return sent separately.
- `hook.py`, `install_hooks.py`: the Claude Code hook and its installer (`--rich` sets `DODECA_RICH=1`).
- `open_pane.sh`, `pane_api.py`: open and size the iTerm2 side pane on `SessionStart`; `pane_api.py send` also types a voice reply into a session.

To change colors, edit `LOOKS` in `src/session.c`; how each state moves is its routine in `src/motion.c`. How a core reacts to the mouse (panel only) is the `HOVER`, `TOUCH` and `CARRY` tables next to it, and what it does on its own when left alone (idle or asleep) is `WHIMS` and `WHIM_GAP`, and the panel's own states (SPEAKING, LISTENING, TRANSCRIBING, STANDBY) are rows of `LOOKS` too: the panel sets them as `Session.overlay`, which `session_shown` puts first; the terminal never sets it. Don't hard-code looks in `panel.m`. The panel's hit test (`coreAt:`) uses the shell outline `SHELL_HIT` from `vigil.h`; gestures are told apart in `PanelView` (press / drag / click / double-click) and acted on by `AppDelegate` through the `CoreActions` protocol. A new state needs changes in `vigil.h` (the enum), `session.c` (`STATE_NAME`, `STATE_TEXT`, `LOOKS`, the transitions), and both READMEs.

## Rules

- **The hook must never get in Claude Code's way.** `hook.py` always exits 0, prints nothing and finishes fast. It writes each event with a single `O_APPEND` write so concurrent sessions don't interleave lines.
- **Privacy:** the event file holds only time, session id, event name, folder name (not the full path), tool name and notification type. Never add prompts, tool input/output or paths. The file is mode 600 and its directory 700. The one exception is opt-in: with `DODECA_RICH=1` the hook keeps the last reply and notification text and the iTerm2 session id in `~/.dodeca-core/msg/<sid>.json` (600, written to a temp file and renamed, at most 4000 characters per text, deleted on `SessionEnd`); the event line only gets `"m": 1`. Never put that text in `events.jsonl`.
- **No dependencies:** the Python scripts use only the standard library. The C code uses only libc, libm and zlib (plus Cocoa for the panel).
- **Don't run `python3 install_hooks.py --apply` or `--remove` without asking.** They edit the user's `~/.claude/settings.json`. Running it without flags is a dry run and is safe.
- The labels use a built-in 3×5 bitmap font (ASCII letters, digits, a few symbols). Anything else renders as `?`.

## Docs

- The README is bilingual: `README.md` (English) and `README.zh-TW.md` (Traditional Chinese). Any README change goes into both, and both keep the language switch line at the top.
- `assets/demo.gif` is rendered from `--snapshot` frames (10 fps, `--at` 8 to 20 s, `--size 640x340`) and assembled with Pillow using one shared 128-color palette. Regenerate it if the visuals change noticeably, and keep it under about 5 MB.
- `hook.py`, `install_hooks.py` and the event reading in `src/events.c` are adapted from [zelvet-velvet/clawd-room](https://github.com/zelvet-velvet/clawd-room) (MIT). Keep his copyright line in `LICENSE` and the credit in both READMEs.
