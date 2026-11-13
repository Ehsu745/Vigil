#!/bin/sh
# Claude Code SessionStart hook: make sure the tab this session runs in has the dodeca-core side pane.
#
# Adds a narrow pane at the right edge of the tab and gives the keyboard back to Claude Code. If the
# floating panel has been built (mac/build.sh) the pane stays empty and the panel floats over it;
# otherwise the pane itself draws the sessions (dodeca-core --pane). There is one floating panel, so in
# any further tab the pane only holds the space free (spacer.sh). A tab that already has its pane is
# left alone. Does nothing outside iTerm2. Prints nothing and always exits 0, so it cannot disturb
# Claude Code.
#
# This needs iTerm2's Python API (pane_api.py): AppleScript can split a pane but cannot resize one in a
# full-screen window, and cannot tell whether a tab already has the pane. Without the Python API there
# is one pane only, at half the width. To turn the API on, once: enable it in iTerm2 (Settings >
# General > Magic) and copy the script to where iTerm2 looks for scripts:
#   mkdir -p ~/Library/Application\ Support/iTerm2/Scripts
#   cp pane_api.py ~/Library/Application\ Support/iTerm2/Scripts/dodeca_pane.py
#
#   DODECA_PANE_COLS    width of the pane in characters (default 34)
#   DODECA_PANE_ARGS    extra options for the program, e.g. "--shape bucky"
#   DODECA_PANE_MODE    "terminal" to draw in the pane even when the floating panel has been built
#   DODECA_PANE_DEBUG   set to 1 to run in the foreground and show what iTerm2 answers

[ "$TERM_PROGRAM" = "iTerm.app" ] || exit 0
[ -n "$ITERM_SESSION_ID" ] || exit 0

DIR=$(cd "$(dirname "$0")" && pwd)
PIDFILE="$HOME/.dodeca-core/pane.pid"
SID=${ITERM_SESSION_ID#*:} # "w0t0p0:UUID" -> the session's id
COLS=${DODECA_PANE_COLS:-34}
IT2RUN=/Applications/iTerm.app/Contents/Resources/it2run
API_SCRIPT="$HOME/Library/Application Support/iTerm2/Scripts/dodeca_pane.py"

pane_is_open() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; }

WAS_OPEN=0
pane_is_open && WAS_OPEN=1

if [ -x "$DIR/dodeca-panel" ] && [ "$DODECA_PANE_MODE" != "terminal" ]; then
    if [ "$WAS_OPEN" = 1 ]; then
        CMD="sh '$DIR/spacer.sh' $DODECA_PANE_ARGS" # the panel exists: this tab only needs the space kept free
    else
        CMD="'$DIR/dodeca-panel' --fit-pane --exit-when-empty $DODECA_PANE_ARGS"
    fi
elif [ -x "$DIR/dodeca-core" ]; then
    CMD="'$DIR/dodeca-core' --pane --exit-when-empty $DODECA_PANE_ARGS" # drawn in the terminal: one per tab
else
    exit 0
fi

open_pane() {
    # First choice: the Python API. it2run asks iTerm2 to start a script from its own Scripts folder and
    # returns at once; the script does nothing if this tab already has its pane.
    if [ -x "$IT2RUN" ] && [ -f "$API_SCRIPT" ]; then
        cmp -s "$DIR/pane_api.py" "$API_SCRIPT" || cp "$DIR/pane_api.py" "$API_SCRIPT" # keep the copy current
        "$IT2RUN" dodeca_pane.py "$SID" "$CMD" "$COLS"
        [ "$WAS_OPEN" = 1 ] && return 0
        i=0
        while [ "$i" -lt 20 ]; do # nothing was open before: wait and see whether the pane appears
            sleep 0.5
            pane_is_open && return 0
            i=$((i + 1))
        done
        echo "open_pane: the Python API script opened nothing within 10 s (see iTerm2 > Scripts > Manage > Console); using AppleScript" >&2
    fi
    # Otherwise AppleScript, which cannot check the tab: one pane only, at half the width.
    if pane_is_open; then
        [ -n "$DODECA_PANE_DEBUG" ] && echo "open_pane: a pane is already open (pid $(cat "$PIDFILE")) and the Python API is not set up" >&2
        return 0
    fi
    osascript - "$SID" "$CMD" <<'EOF'
on run argv
    set sid to item 1 of argv
    set cmd to item 2 of argv
    tell application "iTerm2"
        repeat with w in windows
            repeat with t in tabs of w
                repeat with s in sessions of t
                    if (id of s as text) is sid then
                        tell s to split vertically with same profile command cmd
                        tell s to select
                        return
                    end if
                end repeat
            end repeat
        end repeat
    end tell
end run
EOF
}

if [ -n "$DODECA_PANE_DEBUG" ]; then
    open_pane
else
    open_pane >/dev/null 2>&1 & # in the background: Claude Code should not wait for iTerm2 to answer
fi
exit 0
