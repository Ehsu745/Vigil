# Vigil

**English** | [繁體中文](README.zh-TW.md)

*Formerly dodeca-core. The data folder (`~/.dodeca-core`) and the `DODECA_*` variables keep their old names for now.*

In your terminal, every running Claude Code session becomes a glowing polyhedron: a thin white wireframe shell, a blue glow, and a bright, sun-like core at the center. When a session's state changes, its color, spin speed and core brightness change with it.

The approach and parts of the code (`hook.py`, `install_hooks.py`, and the event reading in `src/events.c`) are adapted from [zelvet-velvet/clawd-room](https://github.com/zelvet-velvet/clawd-room) (MIT License, Copyright (c) 2026 Zaviel). The polyhedron rendering, the macOS floating panel and the pane features are new to this project. See [LICENSE](LICENSE).

![Vigil demo: three sessions changing state](assets/demo.gif)

## States

| State | Triggered by | Looks like |
|---|---|---|
| IDLE | `SessionStart`, after finishing | A calm, exact sphere: white core, blue glow, slow rotation; now and then a line of light climbs it, and every few seconds it does something on its own (glances, peeks, twirls, blinks, turns a vertex toward you, snaps a step, unfolds, scans, spikes, and rarely a whole crazy fit) |
| THINK | `UserPromptSubmit`, `PostToolUse` | Amber. Patches of the shell lift to two heights and the two layers turn against each other like a puzzle cube being solved, two quick turns and a halt, three times, then fold back |
| WORK | `PreToolUse` | An engine: a belt of triangles runs round the middle against the still caps, the patches on the caps fire like pistons in order, an inner shell counter-rotates, sparks run along the edges |
| NEEDS YOU | `Notification` (except idle reminders) | Orange, the core flickers like a glitch; two "fish" of small patches swim just under the surface |
| DONE | `Stop` | Charges, bursts open with spikes and a ring, turns a full circle open, then the patches lock back one by one round the sphere; back to IDLE after 2.5 s |
| SLEEP | IDLE for more than 90 s | Everything dims and draws in a little; a faint scan plane passes through it every 6 s |

The shell never bends or wobbles: it moves in snap turns, patches of triangles that slide out and lock back (cut at random each time, so no pattern repeats), layers turning against the rest, and spikes, where a node is pulled far out into a sharp pyramid while another is pulled in toward the centre. The panel's own states move too: SPEAKING (patches rise and fall with the voice), LISTENING (the front opens with the microphone), TRANSCRIBING (36° steps while a scan plane reads it), STANDBY (still, square to you), ERROR (goes crazy in fits). Patches need the geodesic shells (`geo2`, the default, or `geo3`); `dodeca` and `bucky` keep a rigid net with turns, spikes and light only.

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

When installed with `--pane`, `SessionStart` also runs `open_pane.sh`: it splits a pane off to the right of Claude Code's pane, runs `vigil --pane --exit-when-empty` in it, shrinks it to 34 columns, and gives keyboard focus back to Claude Code. If a pane is already open it won't open another; the pane closes itself 15 seconds after the last session ends.

- Pane width: environment variable `DODECA_PANE_COLS` (default 34).
- Shrinking the pane needs iTerm2's Python API (`pane_api.py`): AppleScript can split panes but can't resize them in a full-screen window. Two one-time steps: enable Settings > General > Magic > Enable Python API in iTerm2, then copy the script to iTerm2's scripts folder: `mkdir -p ~/Library/Application\ Support/iTerm2/Scripts && cp pane_api.py ~/Library/Application\ Support/iTerm2/Scripts/vigil_pane.py` (after that, `open_pane.sh` keeps the copy up to date). Without this the pane is half the width; you can drag the divider yourself and the display follows.
- Debugging: `DODECA_PANE_DEBUG=1 sh open_pane.sh` runs in the foreground and shows iTerm2's responses.
- Extra arguments: environment variable `DODECA_PANE_ARGS`, e.g. `"--shape geo2 --res 1200"`.
- The first time it runs, macOS asks whether to allow controlling iTerm2; click Allow.
- If you don't want it opened automatically, split a pane yourself and run `./dodeca-core --pane`.

## Floating panel (macOS)

`vigil-panel` is a native-window version using the same rendering core: a borderless panel pinned to the right edge of the screen. It doesn't take keyboard focus, mouse clicks pass through it (except on a core, see below), and dark areas are transparent, so it can float over a full-screen iTerm2. The system composites it directly, without going through the terminal.

```sh
sh mac/build.sh
./vigil-panel --demo              # try it on its own first, Ctrl-C to quit
```

`mac/build.sh` makes `Vigil.app` and a small launcher script `./vigil-panel`. The launcher starts the app through macOS (so the microphone and speech permissions used by the voice features belong to the app, not to your terminal), stays in the terminal until the app quits, and takes the same options. Ctrl-C or closing the terminal pane ends both. The app is signed ad-hoc, which changes with every build, so macOS asks for its permissions again after a rebuild; to avoid that, create a "Code Signing" certificate in Keychain Access once and build with `DODECA_SIGN_ID="certificate name" sh mac/build.sh`.

Once built, `open_pane.sh` uses it automatically: the iTerm2 pane split off to the right stays blank and just reserves the space; the panel starts inside it, measures the pane and sits right on top of it. Closing that pane ends the panel; 15 seconds after the last session ends, the panel and pane close together.

- The panel stays on the Space it was started on, i.e. the full-screen iTerm2 window that launched it; it doesn't follow you to other desktops or apps.
- `--all-spaces` makes it appear on every Space, shown only while iTerm2 is the frontmost app (add `--always` to drop that condition).
- If you switch to an iTerm2 tab without the blank pane, the panel still floats on the right edge and may cover text; use `--bg 1e1e1e` to give it a solid background if that bothers you.
- When run by hand, the panel is 17% of the screen width. Use `--width` (in points) and `--top` to position it, and `--inset` for the space above the first session (default 30). When started by `open_pane.sh` it gets `--fit-pane`, which sizes it to the blank pane.
- The font defaults to Monaco 12; use `--font` and `--font-size` to match your iTerm2 settings.
- To go back to the terminal-pane version: `export DODECA_PANE_MODE=terminal`.
- The cores react to the mouse and to a hotkey: see [Interaction and voice](#interaction-and-voice-floating-panel).

## Interaction and voice (floating panel)

The cores in `vigil-panel` react to the mouse and to a global hotkey. The panel still never takes keyboard focus from iTerm2, and clicks outside a core's round shell still go through to the terminal. Speaking and push-to-talk are off unless you start it with `--voice`; without it a click only marks the reply as read and holding a core does nothing. `./vigil-panel --demo --voice` lets you try everything below with made-up sessions (add `--away 10` to see standby quickly).

| Input | Does | The core |
|---|---|---|
| Point at it | Tooltip after 0.3 s, to the left of the panel: folder, state and the last reply laid out as in the terminal, up to 16 lines (the notification text while NEEDS YOU) | Brightens, spins slower, turns toward the cursor, and answers in its state's own way: IDLE stirs (calm spikes on your side), THINK lights the nodes under the pointer, WORK's pistons fire harder, NEEDS YOU's fish swim toward you, TRANSCRIBING's scan follows you, SLEEP and STANDBY wake, ERROR jerks away |
| Rest on it 2 s | — | Lock-on: it stops with a vertex toward you and a focus frame closes in (right-click > Settings > Focus frame: segments, lens, polar, diamond, lock, orbits or none) |
| Rush at it | — | Only from well away (over 2.2 shell radii) onto the core within 0.18 s: the patches clamp shut, a spike shoots toward where you came from, the shell jerks. Not while working |
| Unread reply | — | A slow, soft flash until you click it or send a new prompt |
| Click | Speaks the report, or stops it; marks the reply as read | The patch you clicked goes in and the press travels out through its neighbours; then SPEAKING (teal) |
| Hold (> 0.35 s), release | Push-to-talk: what you say is shown under the core for 1.5 s, then typed into that session with Return; click the core in that time to cancel | LISTENING while held (green, shell stops, glow follows the microphone), then TRANSCRIBING (violet sparks racing along the edges) |
| Double-click | Brings that session's iTerm2 pane to the front | One shockwave ring |
| Right-click | Menu: Replay, Copy reply, Mute this session, Bring to front, and **Settings** (voice, read new replies aloud, summaries by Claude, wait before sending, labels, text size, glow, shape, away time). Settings apply at once, are saved in `~/.dodeca-core/panel-settings.json` and win over the launch options; Settings > Use launch options again forgets them. With no session running, right-click shows Settings only | — |
| Drag | Turns the polyhedron by hand; let go while moving and it spins on, slowing back to its own rotation | — |
| Scroll | This core's speech volume | The shell gets thicker or thinner for a moment |
| Esc while recording | Cancels; nothing is sent | Glow shrinks and fades back |
| `⌃⌥V` (hold, with `--voice`) | Push-to-talk to the session that needs you most: NEEDS YOU, else the one that finished last, else the one active last | as holding the core |
| No keyboard or mouse input for 2 minutes | — | Every core except NEEDS YOU goes to STANDBY (dim indigo); when you come back, cores with an unread reply flare once |

- **Single-core mode** (`--single`, or `⌃⌥C` at any time; `--mode-hotkey` changes the key): one large core for one session, and the others as small cores floating around it. Each session's core is tinted with its own colour, so you can tell who is who; the shell keeps the state colours. The panel shrinks to just this cluster: ⌥-drag it anywhere on the screen (or ⌥-click it: it rides on the mouse until the next click puts it down; the pointer hides, the focus frame holds it, the core leans into the motion, lags a little and settles when you stop or turn (no bounce), the patches on the trailing side open, the small cores trail behind, and a hard shake makes it go crazy), and it comes back there next time (saved per screen size in `~/.dodeca-core/panel-position.json`; right-click > Reset position puts it back on the right edge). Click a small core and it swaps into the middle and stays there (the tooltip says "pinned"); right-click > Follow automatically lets the panel choose again. On its own the middle follows the news: a session that needs you, else the one with the newest reply; it does not jump around just because sessions are busy. Push-to-talk with the hotkey goes to the core in the middle. The right-click menu also switches modes.
- `⌥`-drag a core up or down to reorder the column (in the all-cores mode): the core follows the mouse and the others make room. An ⌥-click picks the core up without holding the button; the next click puts it down.
- `--labels hover` leaves out the two lines of text above each core, so the panel shows only the cores (larger, since they get that space) and the tooltip says which is which.
- The tooltip and the recognized-text preview appear to the left of the panel, over the terminal, and wrap their text instead of cutting it off; the preview shows everything that will be sent. The title is in the session's colour (its state colour, or its own colour in single-core mode) with a bar of that colour down the left edge; the text is in the system font at `--font-size`, laid out like Claude Code's terminal: paragraphs apart, `-` items as bullets with wrapped lines indented, **bold**, `code` in a monospaced font, code blocks indented.
- A single click waits for the system's double-click time before it acts, so a double click never starts with a single one.
- Hotkeys use `RegisterEventHotKey`, which needs no Accessibility permission. Esc is only taken while recording, so the terminal keeps it otherwise. `--hotkey ctrl+opt+v` changes the push-to-talk key (`cmd`, `opt`, `ctrl`, `shift` plus a letter, digit, `space`, `return`, `tab` or `f1`–`f12`; `none` turns it off). macOS already uses ⌃Space, ⌃⌥Space, ⌘Space and ⌥⌘Space, so the default is `⌃⌥V`.
- Away is read from the system's "seconds since last input" (no Input Monitoring permission, no key logging). `--away SECONDS` changes the 120 s; `--away 0` turns it off.
- Double-click and "Bring to front" need `DODECA_RICH` (the iTerm2 session id is kept in the msg file) and, the first time, your OK for the panel to control iTerm2 (System Settings > Privacy & Security > Automation).

**Spoken reports.** A report starts with where it comes from: "notes 完成了：…" for a reply, "notes 需要你：…" with the permission request while a session needs you. Only one session speaks at a time, others wait their turn, and a permission request goes to the front of the queue.

- By default the panel speaks only when you click a core or choose Replay. With `--auto-speak` it also reads a new reply out on its own if you used the keyboard or mouse in the last 60 seconds, and reads a new permission request out at once. Muted sessions never speak on their own.
- What is read is a summary. By default the first paragraph of the reply, at most 3 sentences or 150 characters (no cost, no delay). With `--summarize claude` the panel asks `claude -p` (model haiku, hooks and tools off) in the background for two or three conversational sentences in Traditional Chinese; if that takes more than 15 seconds it uses the local summary. The summary is kept in the msg file, so Replay doesn't ask again. This sends the reply text to Anthropic once more and uses your Claude Code quota.
- Before reading, code blocks become "（略過一段程式碼）", and Markdown symbols and URLs are removed (links keep their text).
- The voice is the **System Voice** of System Settings > Accessibility > Spoken Content. Set it to a **Siri** voice: it is the only kind that reads Chinese with English words in it naturally, and apps can reach it only this way (the panel renders each sentence with `say` and plays it, so the first sound comes about a second after the click). `--voice-id zh-TW` (or `en-US`, or a voice identifier) uses a voice apps may choose directly instead: it starts at once, and the best installed one is picked (download **Meijia (Premium)** under Manage Voices for zh-TW), but English words inside Chinese sound poor. `--rate` sets the speed (0 to 1, default 0.5). The panel's own words are in Chinese when your macOS language is Chinese.
- Speaking needs no permission.

**Voice replies.** Hold a core (or `⌃⌥V`), speak, let go.

- Recognition runs on this Mac only (`SFSpeechRecognizer` with on-device recognition required): the recording is never uploaded. If the language has no on-device model, push-to-talk is turned off and the core says why. `--listen-locale` sets the language (default `zh-TW`).
- The recognized text appears under the core. After 1.5 seconds it is typed into that session's iTerm2 pane, followed by Return; click the core before then to cancel. With `--confirm-send` it waits instead, and a second click sends it.
- If Claude is still working, the panel asks first ("還在工作中，要打斷嗎？") with a menu: send after this step (Claude Code reads it when the current step ends), interrupt and send (Esc first), or cancel.
- If the pane can't be found, the core turns red (ERROR) and says why.
- It listens with the system's input device, except a virtual one (BlackHole, Loopback, Soundflower, Teams or Zoom audio), which carries other apps' sound rather than your voice: then it uses the built-in microphone. `--mic NAME` picks a device by part of its name. If a recording is completely silent, the core says which device it used.
- `--debug` writes each voice step to stderr (device, levels, recognizer errors, send result; never what you said).
- Sending needs `DODECA_RICH` (the session's `ITERM_SESSION_ID` is kept in the msg file). It uses AppleScript, which asks once for permission to control iTerm2; if that is not allowed, it falls back to iTerm2's Python API (`pane_api.py send`).
- In `--demo` nothing is typed anywhere; the core only says what it would have sent.

**One-time setup for voice:**

1. `sh mac/build.sh`, then start the panel with `--voice` (`DODECA_PANE_ARGS="--voice"` for the pane opened by `open_pane.sh`).
2. `python3 install_hooks.py --rich --apply`, so replies and the iTerm2 session id are kept (start new Claude Code sessions afterwards).
3. The first time you hold a core, macOS asks for the microphone and for speech recognition: allow both, then hold again.
4. The first time a reply is sent or you double-click a core, macOS asks whether Vigil may control iTerm2: allow it (System Settings > Privacy & Security > Automation).
5. Optional: a better zh-TW voice (System Settings > Accessibility > Spoken Content > System Voice > Manage Voices); iTerm2 > Settings > General > Magic > Enable Python API, as the fallback for sending.
- How each reaction looks is set in `src/session.c`: `LOOKS` (the SPEAKING, LISTENING, TRANSCRIBING and STANDBY rows), `HOVER` and `TOUCH`.

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

Voice (`vigil-panel --voice`): speech is synthesized on this Mac, and push-to-talk recognition is on-device only, so recordings never leave the Mac and are not kept. The recognized text is typed into the Claude Code session it was meant for, like anything you type. The one network use is `--summarize claude`, which sends the reply text to Anthropic through `claude -p` to shorten it.

## Known limitations

- After you approve a permission request, Claude Code doesn't send another event, so the session keeps showing NEEDS YOU while that tool runs and only switches once the tool finishes.
- Labels use a built-in 3×5 bitmap font with only letters, digits and a few symbols; non-ASCII characters in folder names (e.g. Chinese) show as `?`.
