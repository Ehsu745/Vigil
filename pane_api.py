#!/usr/bin/env python3
"""Open the side pane through iTerm2's Python API, which (unlike AppleScript) can set its width.
Also sends a voice reply from the floating panel to a session.

iTerm2 only starts scripts from its own Scripts folder, so open_pane.sh (and the panel) keep a copy of this
file there as dodeca_pane.py and run:

  it2run dodeca_pane.py SESSION_ID COMMAND COLUMNS
      Splits the given session, runs COMMAND in the new pane on the right, makes that pane COLUMNS wide and
      gives the keyboard back to the original session.

  it2run dodeca_pane.py send SESSION_ID TEXT RESULT_FILE INTERRUPT
      Types TEXT into the session and presses Return (with INTERRUPT=1, Esc first, which stops Claude).
      Writes "ok" or "missing" (no such session) to RESULT_FILE, since it2run itself returns at once.

Errors go to iTerm2's script console (Scripts > Manage > Console).

Needs, once: iTerm2 > Settings > General > Magic > Enable Python API.
"""
import asyncio
import os
import sys

import iterm2

SEND = len(sys.argv) > 1 and sys.argv[1] == "send"
done = False


def write_result(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)  # the panel never reads half a file


async def send(connection):
    global done
    _, sid, text, result, interrupt = sys.argv[1:6]
    app = await iterm2.async_get_app(connection)
    session = app.get_session_by_id(sid)
    if session is None:
        write_result(result, "missing")
        return
    if interrupt == "1":
        await session.async_send_text("\x1b")
        await asyncio.sleep(0.4)
    # the text, then Return on its own: sent together, Claude Code would take the newline as part of a paste
    await session.async_send_text(text)
    await asyncio.sleep(0.15)
    await session.async_send_text("\r")
    write_result(result, "ok")
    done = True


async def open_pane(connection):
    global done
    sid, cmd, cols = sys.argv[1], sys.argv[2], int(sys.argv[3])
    app = await iterm2.async_get_app(connection)
    session = app.get_session_by_id(sid)
    if session is None:
        return
    profile = iterm2.LocalWriteOnlyProfile()
    profile.set_use_custom_command("Yes")
    profile.set_command(cmd)
    pane = await session.async_split_pane(vertical=True, profile_customizations=profile)
    done = True
    try:
        # the layout is adjusted from the app's own picture of the tab, so look the new pane up there
        await app.async_refresh()
        pane = app.get_session_by_id(pane.session_id) or pane
        pane.preferred_size = iterm2.util.Size(cols, pane.grid_size.height)
        _, tab = app.get_window_and_tab_for_session(pane)
        if tab is not None:
            await tab.async_update_layout()
    finally:
        await session.async_activate()


try:
    iterm2.run_until_complete(send if SEND else open_pane)
except Exception as e:  # the API is off, the runtime is missing, ...
    print("pane_api:", e, file=sys.stderr)
    if SEND and len(sys.argv) > 4:
        write_result(sys.argv[4], "error: %s" % e)
sys.exit(0 if done else 1)
