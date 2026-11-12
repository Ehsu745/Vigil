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
- `win/panel.c`: the Windows panel (`vigil-panel.exe`, built with MinGW-w64 by `win/build.sh`, started from WSL by `win/vigil-win`). Claude Code and the hook run in WSL; the exe reads the events file through `\\wsl.localhost` and draws the shared renderer's cores into a layered window. Every core is its own small window in both modes (`g_pane[i]` shows session i; `g_cluster` is only the hidden owner of the clock, hotkey and menus). Single-core mode (the default; `--no-others` hides the satellites): the main core sits at `g_mx, g_my` and each satellite is a bead on a ring round it (`draw_single`): gravity (Settings > Gravity, `g_gravity`) holds it on the orbit and a weaker spring draws it along to its slot; while the main core is carried that spring is off, so the satellites slide round to the trailing side and push each other apart, and a very fast carry breaks gravity and flings them off with their speed, bouncing off the screen edges (`on_work`: not where another monitor goes on), until the main core comes close; all-cores mode (`--all`): Alt-drag places each core, kept by folder name. Positions are desktop coordinates keyed by the monitor layout, every clamp uses the work area of the monitor the core is on (`work_at`); per-monitor DPI is not handled yet. Alt does what ⌥ does on the Mac (Alt-drag move, Alt-click carry, Ctrl+Alt+C mode); the Settings window (right-click > Settings..., `settings_open`; `--settings` opens it at start, `--settings-shot FILE` saves it as a BMP after 2 s and quits, the way to check a change to it) is drawn antialiased into its own layered window (`set_build` for the still layer, `settings_tick` for the live preview, which reuses the orbit physics `bead_step` / `fly_step`), and its choices are kept in `panel-settings-win.json` (the text box's colours are `g_tip_bg` / `g_text_col`, with a custom colour each from Windows' colour window, `pick_colour`; the wheel's spin is `g_wheel_spin`); the focus frame and the labels' dot are drawn with its own antialiased helpers (`aa_*`), label text with GDI through a scratch bitmap (`text_into`), since layered windows need real alpha; voice is not ported yet. It keeps each core's picture (`Pic`) and renders it again only at `session_rate` × 1.5 (60 while in the hand), pushes a window's bitmap only when a picture in it changed, ticks on a high-resolution waitable timer (30 Hz, 60 while dragging) and reads the events file 4 times a second; `--record reply|input|carry FILE` plays a scripted demo scene and writes its frames raw (the README's animations), `--settings-zoom X` starts the preview zoomed (for the Settings screenshots); `--on X,Y` puts those windows on the monitor with that desktop point, and screenshot and recording runs never take the foreground (a game may be running on another monitor); `--stats` writes ticks, pictures and window updates a second to `panel-stats.txt` next to the events file, the way to check a change costs no more; the text box is a second window, `g_tip`, fed from `msg\<sid>.json`. Like `panel.m` it reuses `events.c`, `geom.c`, `session.c`, `motion.c` and `render.c`, not `main.c`. Build it with no new warnings too.
- `mac/tip.m`: the box beside the panel (hover tooltip, voice-reply preview): wraps its text, system font, title and edge bar in the session's colour.
- `mac/speech.m`: the panel's voice output: Markdown cleanup, the local summary, the one-at-a-time `Speaker` (`AVSpeechSynthesizer`), and the background `claude -p` summary (hooks off via `--settings`, so it never shows up as a session). Never call `claude` from the hook.
- `mac/listen.m`: push-to-talk recognition (`SFSpeechRecognizer`, `requiresOnDeviceRecognition` always on: never fall back to server recognition). The panel types the result into the session's iTerm2 pane with AppleScript, or `pane_api.py send` as the fallback; Claude Code needs the text and Return sent separately.
- `hook.py`, `install_hooks.py`: the Claude Code hook and its installer (`--rich` sets `DODECA_RICH=1`).
- `open_pane.sh`, `pane_api.py`: open and size the iTerm2 side pane on `SessionStart`; `pane_api.py send` also types a voice reply into a session.

To change colors, edit `LOOKS` in `src/session.c`; how each state moves is a routine in `src/motion.c`. A new routine is an `RT_*` name in `vigil.h`, a function `r_name(Ctx *c)` in `motion.c` and its row of `ROUTINES` there (name, the state it is for, run, optional enter, description); its working state is its own struct in the `RoutineState` union (`Motion.rs`, zeroed as it starts), never shared fields. A new action is a `W_*` name, its row of `WHIMS` in `session.c` (with its description and weights), and for a geometric one a function `w_name` plus its row of `WHIM_RUN` in `motion.c`; actions keep their state in `Motion.ws`. `MOTION_MAP` says which routines and actions each state may use; its defaults are worked out from the tables (`motion_map_default`: the routines whose `home` is the state, the actions `WHIMS` weights for it). A state picks one of its routines at random each time it starts, and the Windows Settings window's Motions page edits the map (kept in `panel-settings-win.json` as `mapRoutines` / `mapActions`; 32-bit masks, checked at compile time). Everything here is shared C, so the Mac panel runs the same motions with the default map; only the Motions page is Windows-only so far. A routine sets per-frame effects that the renderer reads from `Motion`: patch lifts, per-triangle matrices (`tmi`/`tmx`), carried offsets (`tt`), see-through (`talpha`), blade tips, squash, sparks, trails, rings, the black hole. How a core reacts to the mouse (panel only) is the `HOVER`, `TOUCH` and `CARRY` tables next to it, and what it does on its own when left alone (idle or asleep) is `WHIMS` and `WHIM_GAP`, and the panel's own states (SPEAKING, LISTENING, TRANSCRIBING, STANDBY) are rows of `LOOKS` too: the panel sets them as `Session.overlay`, which `session_shown` puts first; the terminal never sets it. Don't hard-code looks in `panel.m`. The panel's hit test (`coreAt:`) uses the shell outline `SHELL_HIT` from `vigil.h`; gestures are told apart in `PanelView` (press / drag / click / double-click) and acted on by `AppDelegate` through the `CoreActions` protocol. A new state needs changes in `vigil.h` (the enum), `session.c` (`STATE_NAME`, `STATE_TEXT`, `LOOKS`, the transitions), and both READMEs.

## Rules

- **After any change, restart the panel for the user.** Stop the running Vigil build (Windows: `taskkill.exe /IM vigil-panel.exe /F`; macOS: quit `Vigil.app`), rebuild it (`sh win/build.sh` or `sh mac/build.sh`), and start it again on the real sessions in the background (`./vigil-win`, or `./vigil-panel`), so the user can try the change at once. A running `vigil-panel.exe` locks the file, so the build fails until it is stopped.
- **The hook must never get in Claude Code's way.** `hook.py` always exits 0, prints nothing and finishes fast. It writes each event with a single `O_APPEND` write so concurrent sessions don't interleave lines.
- **Privacy:** the event file holds only time, session id, event name, folder name (not the full path), tool name and notification type. Never add prompts, tool input/output or paths. The file is mode 600 and its directory 700. The one exception is opt-in: with `DODECA_RICH=1` the hook keeps the last reply and notification text, the iTerm2 session id and the tmux pane id (`%N`, where a reply typed in the Windows panel goes) in `~/.dodeca-core/msg/<sid>.json` (600, written to a temp file and renamed, at most 4000 characters per text, deleted on `SessionEnd`); the event line only gets `"m": 1`. Never put that text in `events.jsonl`.
- **No dependencies:** the Python scripts use only the standard library. The C code uses only libc, libm and zlib (plus Cocoa for the panel).
- **Don't run `python3 install_hooks.py --apply` or `--remove` without asking.** They edit the user's `~/.claude/settings.json`. Running it without flags is a dry run and is safe.
- The labels use a built-in 3×5 bitmap font (ASCII letters, digits, a few symbols). Anything else renders as `?`.

## Docs

- The README is bilingual: `README.md` (English) and `README.zh-TW.md` (Traditional Chinese). Any README change goes into both, and both keep the language switch line at the top.
- `assets/demo.gif` is rendered from `--snapshot` frames (10 fps, `--at` 8 to 20 s, `--size 640x340`), laid over the terminal background `#212121` in a rounded card (black counts as see-through: colour + bg × (1 − brightest channel)), and assembled with Pillow using one shared 128-color palette and frame differencing. `assets/states/*.gif` are one session forced into each state and routine (`motion_play`), on the same card. `assets/demos/*.gif` come from the Windows panel's `--record reply|input|carry FILE` (a scripted pointer on a fixed clock; the real mouse is untouched; raw frames of its own windows over `#212121`). Regenerate them if the visuals change noticeably, and keep each under about 5 MB.
- `hook.py`, `install_hooks.py` and the event reading in `src/events.c` are adapted from [zelvet-velvet/clawd-room](https://github.com/zelvet-velvet/clawd-room) (MIT). Keep his copyright line in `LICENSE` and the credit in both READMEs.
