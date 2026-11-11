# dodeca-core

**English** | [繁體中文](README.zh-TW.md)

In your terminal, every running Claude Code session becomes a glowing polyhedron: a thin white wireframe shell, a blue glow, and a bright, sun-like core at the center. When a session's state changes, its color, spin speed and core brightness change with it.

The approach and parts of the code (`hook.py`, `install_hooks.py`, and the event reading in `src/events.c`) are adapted from [zelvet-velvet/clawd-room](https://github.com/zelvet-velvet/clawd-room) (MIT License, Copyright (c) 2026 Zaviel). The polyhedron rendering, the macOS floating panel and the pane features are new to this project. See [LICENSE](LICENSE).

![dodeca-core demo: three sessions changing state](assets/demo.gif)

## States

| State | Triggered by | Looks like |
|---|---|---|
| IDLE | `SessionStart`, after finishing | White core, blue glow, slow rotation |
| THINK | `UserPromptSubmit`, `PostToolUse` | Yellow lines and glow, core pulses like breathing |
| WORK | `PreToolUse` | White core, blue glow, faster rotation, sparks travel along the edges |
| NEEDS YOU | `Notification` (except idle reminders) | Red lines, white core with red glow, core flickers like a glitch |
| DONE | `Stop` | Core flares with a shockwave ring, back to IDLE after 2.5 s |
| SLEEP | IDLE for more than 90 s | Everything dims; the solid core stays visible |

`SessionEnd` removes that session's polyhedron. Sessions with no events for 30 minutes are removed too.

## Requirements

- A C compiler and `make` (macOS: `xcode-select --install`)
- zlib (built into macOS)
- Python 3 (only for the hook and install script; standard library only)
- A truecolor terminal. To see the thin lines and glow you need a terminal that can display images (iTerm2, kitty, Ghostty, WezTerm)

## Usage

```sh
make
./dodeca-core --demo          # try it with fake sessions first, q to quit

python3 install_hooks.py          # only prints what would be written to ~/.claude/settings.json
python3 install_hooks.py --apply  # actually write it (makes a backup first)
./dodeca-core                     # show your real sessions
```

- Don't move this folder after installing the hook; the hook is called by absolute path. If you do move it, run `--apply` again.
- To remove the hook: `python3 install_hooks.py --remove`.
- The hook only applies to Claude Code sessions started after installing it.
- `--rich` (with or without `--apply`) runs the hook with `DODECA_RICH=1`, which also keeps the text of Claude's last reply for the floating panel's tooltip and voice features. Off by default; see [Privacy](#privacy). The dry run shows exactly which lines of your settings would change.

## Side pane (iTerm2)

`--pane` is a vertical layout for narrow panes: sessions stack top to bottom, each with two lines of text (name, state) above its polyhedron. The text uses the terminal's own font, so it's the same size as other panes using the same profile.

```sh
./dodeca-core --pane --demo                # try it in a narrow pane first
python3 install_hooks.py --pane --apply    # open the pane automatically whenever Claude Code starts
```

When installed with `--pane`, `SessionStart` also runs `open_pane.sh`: it splits a pane off to the right of Claude Code's pane, runs `dodeca-core --pane --exit-when-empty` in it, shrinks it to 34 columns, and gives keyboard focus back to Claude Code. If a pane is already open it won't open another; the pane closes itself 15 seconds after the last session ends.

- Pane width: environment variable `DODECA_PANE_COLS` (default 34).
- Shrinking the pane needs iTerm2's Python API (`pane_api.py`): AppleScript can split panes but can't resize them in a full-screen window. Two one-time steps: enable Settings > General > Magic > Enable Python API in iTerm2, then copy the script to iTerm2's scripts folder: `mkdir -p ~/Library/Application\ Support/iTerm2/Scripts && cp pane_api.py ~/Library/Application\ Support/iTerm2/Scripts/dodeca_pane.py` (after that, `open_pane.sh` keeps the copy up to date). Without this the pane is half the width; you can drag the divider yourself and the display follows.
- Debugging: `DODECA_PANE_DEBUG=1 sh open_pane.sh` runs in the foreground and shows iTerm2's responses.
- Extra arguments: environment variable `DODECA_PANE_ARGS`, e.g. `"--shape geo2 --res 1200"`.
- The first time it runs, macOS asks whether to allow controlling iTerm2; click Allow.
- If you don't want it opened automatically, split a pane yourself and run `./dodeca-core --pane`.

## Floating panel (macOS)

`dodeca-panel` is a native-window version using the same rendering core: a borderless panel pinned to the right edge of the screen. It doesn't take keyboard focus, mouse clicks pass through it (except on a core, see below), and dark areas are transparent, so it can float over a full-screen iTerm2. The system composites it directly, without going through the terminal.

```sh
sh mac/build.sh
./dodeca-panel --demo              # try it on its own first, Ctrl-C to quit
```

`mac/build.sh` makes `DodecaPanel.app` and a small launcher script `./dodeca-panel`. The launcher starts the app through macOS (so the microphone and speech permissions used by the voice features belong to the app, not to your terminal), stays in the terminal until the app quits, and takes the same options. Ctrl-C or closing the terminal pane ends both. The app is signed ad-hoc, which changes with every build, so macOS asks for its permissions again after a rebuild; to avoid that, create a "Code Signing" certificate in Keychain Access once and build with `DODECA_SIGN_ID="certificate name" sh mac/build.sh`.

Once built, `open_pane.sh` uses it automatically: the iTerm2 pane split off to the right stays blank and just reserves the space; the panel starts inside it, measures the pane and sits right on top of it. Closing that pane ends the panel; 15 seconds after the last session ends, the panel and pane close together.

- The panel stays on the Space it was started on, i.e. the full-screen iTerm2 window that launched it; it doesn't follow you to other desktops or apps.
- `--all-spaces` makes it appear on every Space, shown only while iTerm2 is the frontmost app (add `--always` to drop that condition).
- If you switch to an iTerm2 tab without the blank pane, the panel still floats on the right edge and may cover text; use `--bg 1e1e1e` to give it a solid background if that bothers you.
- When run by hand, the panel is 17% of the screen width. Use `--width` (in points) and `--top` to position it, and `--inset` for the space above the first session (default 30). When started by `open_pane.sh` it gets `--fit-pane`, which sizes it to the blank pane.
- The font defaults to Monaco 12; use `--font` and `--font-size` to match your iTerm2 settings.
- To go back to the terminal-pane version: `export DODECA_PANE_MODE=terminal`.
- Point at a core and it reacts: it brightens, slows down and turns toward the cursor. After a moment a tooltip to the left of the panel shows the folder, the state and the first two lines of the last reply (the reply needs `DODECA_RICH`, see below). Only the round shell counts as the core: clicks next to it still reach the terminal underneath.

## Output modes

The program first draws an RGB image in memory, then sends it to the terminal in one of three ways, detected automatically at startup:

| Mode | Description |
|---|---|
| `--kitty` | kitty graphics protocol, real pixels. Preferred when the terminal reports support |
| `--iterm` | iTerm2 inline images (PNG), real pixels. Used inside iTerm2 when the above isn't supported |
| `--cells` | Half-block characters `▀`, two pixels per character cell. Works in any truecolor terminal but is coarse; always used inside tmux |

If auto-detection picks poorly, force a mode with the flag.

## Options

```
--pane            vertical layout for a side pane
--exit-when-empty exit after 15 s with no sessions
--demo [N]        use N fake sessions (1-6, default 3)
--shape NAME      dodeca | bucky | geo2 (default) | geo3
--res N           pixels on the image's longest side (default 960, 420 with --pane; higher is finer but heavier on the terminal)
--fps N           max frames per second (default 20; calm sessions redraw less often automatically)
--glow X          glow strength (default 1.0)
--core X          core radius as a fraction of the shell's inscribed-sphere radius (default 0.35)
--events PATH     event file (default ~/.dodeca-core/events.jsonl, or env var DODECA_EVENTS)
--snapshot FILE   write one demo frame as a PPM file and exit (with --at seconds, --size WxH)
```

## Architecture

```
Claude Code ──hook.py──▶ ~/.dodeca-core/events.jsonl ──tail──▶ state machine ──▶ software renderer ──▶ terminal
```

| File | Contents |
|---|---|
| `mac/panel.m` | Floating panel: Cocoa window, text labels, turning the canvas into a transparent image |
| `pane_api.py` | Splits panes and sets their width via the iTerm2 Python API |
| `open_pane.sh` | `SessionStart` hook: automatically splits off a side pane in iTerm2 |
| `hook.py` | Appends one JSON line per event: time, session id, event name, folder name, tool name, notification type |
| `src/events.c` | Follows the event file and reads new lines |
| `src/session.c` | Maps events to states, plus the per-state appearance table `LOOKS` |
| `src/geom.c` | Generates vertices and edges for the dodecahedron, truncated icosahedron and geodesic spheres |
| `src/render.c` | Draws lines, glow, core and labels into a float RGB canvas |
| `src/main.c` | Main loop, terminal setup, the three output modes, demo |

To change colors or motion, just edit the `LOOKS` table in `src/session.c`.

## Privacy

The event file contains only time, session id, event name, folder name (no path), tool name and notification type. Prompts and tool input/output are never recorded. No network connections, no background daemon, and the file's permissions are 600.

With `DODECA_RICH=1` (`install_hooks.py --rich`) the hook also keeps one file per session in `~/.dodeca-core/msg/<session id>.json` (mode 600, folder 700): **the text of Claude's last reply** and of the last notification (e.g. a permission prompt), at most 4000 characters each, and the `ITERM_SESSION_ID` of the terminal it runs in. Each new reply or notice replaces the old one, the file is deleted when the session ends, and files left by sessions that never ended are deleted after 24 hours. The event file stays the same apart from `"m": 1` on a `Stop` line. Since reply text is written to disk, turn this on only on your own machine.

## Known limitations

- After you approve a permission request, Claude Code doesn't send another event, so the session keeps showing NEEDS YOU while that tool runs and only switches once the tool finishes.
- Labels use a built-in 3×5 bitmap font with only letters, digits and a few symbols; non-ASCII characters in folder names (e.g. Chinese) show as `?`.
