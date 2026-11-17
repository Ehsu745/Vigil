# CLAUDE.md

Vigil (formerly dodeca-core) shows every running Claude Code session as a glowing polyhedron, in the terminal or in a floating panel (macOS, Windows). A Claude Code hook appends events to a JSONL file; a C program reads it, runs a state machine per session and draws the result with its own software renderer.

```
Claude Code ──hook.py──▶ ~/.dodeca-core/events.jsonl ──tail──▶ state machine ──▶ renderer ──▶ terminal / panel
```

The data folder `~/.dodeca-core` and the `DODECA_*` variables keep the old name until the hooks are reinstalled (`install_hooks.py --apply`, which needs the user's OK).

## The vault

The work log and the detail of how each part works live in the sibling vault `../vigil-folio` (private, `zelvet-velvet/vigil-folio`); the skills in `.claude/skills/` (`cmd-sync`, `test-queue`, `git-workflow`, …) are links into it.

- Ideas and open tasks: `LLM/DEV_STATUS.md` Ongoing. Decisions and why: `Notes/Vigil.md`. Tests: `Quest/Test Queue.md`.
- **How a part works**: `Notes/Vigil Code/` (Windows panel, Mac panel, Motions and states, Hook and privacy, READY and the dodge game, Web and wasm). Read the note of the part you are changing; grep it first, they are long.
- Not in Claude's own memory, which stays on one machine. Asked whether something is recorded: search the vault first.

## Build and run

```sh
make                 # ./vigil, the terminal version (C11, zlib, libm)
./vigil --demo       # fake sessions cycling through every state; q quits
sh mac/build.sh      # Vigil.app and its launcher ./vigil-panel (macOS)
sh win/build.sh      # vigil-panel.exe (MinGW-w64); start it from WSL with ./vigil-win
```

`vigil`, `Vigil.app`, `./vigil-panel` and `vigil-panel.exe` are build output and gitignored. Build with no new warnings (`-Wall -Wextra`).

## Checking a change

There is no test suite. Look at a picture instead:

- Terminal / shared renderer: `./vigil --snapshot /tmp/f.ppm --at 10 --size 720x400` (`--at`: seconds into the demo script, `SCRIPT` in `src/session.c`).
- Windows Settings window: `./vigil-win --settings --settings-tab <page> --settings-shot FILE.bmp` (more flags in the Windows panel note).
- Mac Settings / reply box: `--settings-shot FILE`, `--reply-shot FILE`.
- Cost: `./vigil-win --stats` writes `panel-stats.txt` next to the events file.

## Files

- `src/vigil.h`: every shared type and function, grouped by the file that implements it. Start here.
- `src/events.c` (reads the JSONL), `src/session.c` (states, timings, `LOOKS`, the demo), `src/geom.c` (shells), `src/motion.c` (routines and actions), `src/render.c` (the software renderer), `src/main.c` (terminal only).
- `win/panel.c`: the Windows panel. `mac/*.m`: the macOS panel. Both reuse the `src/` files except `main.c`.
- `hook.py`, `install_hooks.py`: the hook and its installer. `spinner_verbs.py`, `vigil-claude`, `open_pane.sh`, `pane_api.py`: helpers.
- `web/`, `src/wasm.c`: the Core Bestiary page and design mocks (plain HTML that opens from disk).

Where to change things: colours and looks are `LOOKS` in `src/session.c`; a new routine is an `RT_*` name, `r_name` and a `ROUTINES` row in `motion.c` (its state in its own `RoutineState` struct); a new action is a `W_*` name and a `WHIMS` row; a new state touches `vigil.h`, `session.c` and both READMEs. Details in the Motions and states note.

## Rules

- **After any change, restart the panel for the user**: stop it (`taskkill.exe /IM vigil-panel.exe /F`, or quit `Vigil.app`), rebuild, start it on the real sessions in the background (`./vigil-win` or `./vigil-panel`). A running `vigil-panel.exe` locks the file.
- **The hook must never get in Claude Code's way**: `hook.py` always exits 0, prints nothing, finishes fast, and writes each event with one `O_APPEND` write. Never call `claude` from it.
- **Privacy**: `events.jsonl` holds only time, session id, event name, folder name (not the path), tool name, notification type, and a subagent's id and type. Never prompts, tool input or output, or paths. Mode 600, folder 700. Text (replies, turns) is kept only with opt-in `DODECA_RICH=1`, in `msg/<sid>.json`, never in `events.jsonl`. The full rule is in the Hook and privacy note.
- **No dependencies**: Python uses only the standard library; C uses libc, libm, zlib (plus Cocoa on macOS and libraries that are part of Windows).
- **Ask before** running `install_hooks.py --apply` or `--remove` (they edit `~/.claude/settings.json`; no flags is a safe dry run).
- **Blades is Zaviel's; Skein is Ehsu745's.** `r_blades` stays as in `f9d16a1`; don't change it without the user's OK. Skein (`r_skein`) is off by default. Tell Zaviel when a change to the shared motions changes the Windows panel.
- The labels' 3×5 bitmap font has ASCII letters, digits and a few symbols; anything else shows as `?`.

## Docs

- **Ask before updating docs.** When a change seems worth a line in the README or a vault note, say so and wait for the user's OK; don't update them on every commit.
- **Never grow this file.** How a part works goes in its note in `../vigil-folio/Notes/Vigil Code/`, in a sentence or two. This file changes only with the user's OK.
- The README is bilingual: `README.md` and `README.zh-TW.md` change together and keep the language switch line at the top.
- `assets/` GIFs come from `--snapshot` frames or the Windows panel's `--record`; regenerate them only when the visuals change noticeably, each under about 5 MB.
- `hook.py`, `install_hooks.py` and the event reading in `src/events.c` are adapted from [zelvet-velvet/clawd-room](https://github.com/zelvet-velvet/clawd-room) (MIT): keep its copyright line in `LICENSE` and the credit in both READMEs.
