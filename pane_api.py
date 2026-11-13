#!/usr/bin/env python3
"""Give a tab its side pane through iTerm2's Python API.

iTerm2 only starts scripts from its own Scripts folder, so open_pane.sh keeps a copy of this file there
as dodeca_pane.py and runs:  it2run dodeca_pane.py SESSION_ID COMMAND COLUMNS

Finds the tab SESSION_ID is in. If that tab has no side pane yet, splits the pane at its right edge,
runs COMMAND in the new pane, makes it COLUMNS wide, marks it, and gives the keyboard back to
SESSION_ID. Errors go to iTerm2's script console (Scripts > Manage > Console).

Needs, once: iTerm2 > Settings > General > Magic > Enable Python API.
"""
import sys

import iterm2

SID, CMD, COLS = sys.argv[1], sys.argv[2], int(sys.argv[3])
TAG = "user.dodeca"  # set on every pane this script makes, so a tab is never given two


def right_edge(tab, fallback):
    """The pane at the right edge of a tab that is split into columns; otherwise the given pane."""
    try:
        splitter = iterm2.session.Splitter
        node = tab.root
        while isinstance(node, splitter):
            if not node.vertical or not node.children:
                return fallback
            node = node.children[-1]
        return node if isinstance(node, iterm2.Session) else fallback
    except Exception:
        return fallback


async def main(connection):
    app = await iterm2.async_get_app(connection)
    session = app.get_session_by_id(SID)
    if session is None:
        return
    _, tab = app.get_window_and_tab_for_session(session)
    target = session
    if tab is not None:
        for s in tab.sessions:
            try:
                if await s.async_get_variable(TAG):
                    return  # this tab already has its pane
            except Exception:
                pass
        target = right_edge(tab, session)

    profile = iterm2.LocalWriteOnlyProfile()
    profile.set_use_custom_command("Yes")
    profile.set_command(CMD)
    pane = await target.async_split_pane(vertical=True, profile_customizations=profile)
    try:
        await pane.async_set_variable(TAG, "1")
        # the layout is adjusted from the app's own picture of the tab, so look the new pane up there
        await app.async_refresh()
        pane = app.get_session_by_id(pane.session_id) or pane
        pane.preferred_size = iterm2.util.Size(COLS, pane.grid_size.height)
        _, tab = app.get_window_and_tab_for_session(pane)
        if tab is not None:
            await tab.async_update_layout()
    finally:
        await session.async_activate()


iterm2.run_until_complete(main)
