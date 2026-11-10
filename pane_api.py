#!/usr/bin/env python3
"""Open the side pane through iTerm2's Python API, which (unlike AppleScript) can set its width.

iTerm2 only starts scripts from its own Scripts folder, so open_pane.sh keeps a copy of this file there
as dodeca_pane.py and runs:  it2run dodeca_pane.py SESSION_ID COMMAND COLUMNS

Splits the given session, runs COMMAND in the new pane on the right, makes that pane COLUMNS wide and
gives the keyboard back to the original session. Errors go to iTerm2's script console
(Scripts > Manage > Console).

Needs, once: iTerm2 > Settings > General > Magic > Enable Python API.
"""
import sys

import iterm2

SID, CMD, COLS = sys.argv[1], sys.argv[2], int(sys.argv[3])
opened = False


async def main(connection):
    global opened
    app = await iterm2.async_get_app(connection)
    session = app.get_session_by_id(SID)
    if session is None:
        return
    profile = iterm2.LocalWriteOnlyProfile()
    profile.set_use_custom_command("Yes")
    profile.set_command(CMD)
    pane = await session.async_split_pane(vertical=True, profile_customizations=profile)
    opened = True
    try:
        # the layout is adjusted from the app's own picture of the tab, so look the new pane up there
        await app.async_refresh()
        pane = app.get_session_by_id(pane.session_id) or pane
        pane.preferred_size = iterm2.util.Size(COLS, pane.grid_size.height)
        _, tab = app.get_window_and_tab_for_session(pane)
        if tab is not None:
            await tab.async_update_layout()
    finally:
        await session.async_activate()


try:
    iterm2.run_until_complete(main)
except Exception as e:  # the API is off, the runtime is missing, ...
    print("pane_api:", e, file=sys.stderr)
sys.exit(0 if opened else 1)
