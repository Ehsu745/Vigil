#!/bin/sh
# Claude Code SessionStart hook: open the vigil side pane next to this session, once.
#
# Splits the iTerm2 pane Claude Code is running in, narrows the new pane on the right, and gives the
# keyboard back to Claude Code. If the floating panel has been built (mac/build.sh), the new pane stays
# empty and the panel floats over it; otherwise the pane itself draws the sessions (vigil --pane).
# Does nothing outside iTerm2 or when a pane is already open. Prints nothing and always exits 0, so it
# cannot disturb Claude Code.
#
# Narrowing the pane needs iTerm2's Python API (pane_api.py); AppleScript can split a pane but cannot
# resize one in a full-screen window. Without the Python API the new pane is half the width.
# To turn it on, once: enable the Python API in iTerm2 (Settings > General > Magic) and copy the script
# to where iTerm2 looks for scripts:
#   mkdir -p ~/Library/Application\ Support/iTerm2/Scripts
#   cp pane_api.py ~/Library/Application\ Support/iTerm2/Scripts/vigil_pane.py
#
#   DODECA_PANE_COLS    width of the pane in characters (default 34)
#   DODECA_PANE_ARGS    extra options for the program, e.g. "--shape bucky"
#   DODECA_PANE_MODE    "terminal" to draw in the pane even when the floating panel has been built
#   DODECA_PANE_DEBUG   set to 1 to run in the foreground and show what iTerm2 answers

[ "$TERM_PROGRAM" = "iTerm.app" ] || exit 0
[ -n "$ITERM_SESSION_ID" ] || exit 0

DIR=$(cd "$(dirname "$0")" && pwd)
if [ -x "$DIR/vigil-panel" ] && [ "$DODECA_PANE_MODE" != "terminal" ]; then
    CMD="'$DIR/vigil-panel' --fit-pane --exit-when-empty $DODECA_PANE_ARGS"
elif [ -x "$DIR/dodeca-core" ]; then
    CMD="'$DIR/dodeca-core' --pane --exit-when-empty $DODECA_PANE_ARGS"
else
    exit 0
fi

PIDFILE="$HOME/.dodeca-core/pane.pid"
SID=${ITERM_SESSION_ID#*:} # "w0t0p0:UUID" -> the session's id
COLS=${DODECA_PANE_COLS:-34}
IT2RUN=/Applications/iTerm.app/Contents/Resources/it2run
API_SCRIPT="$HOME/Library/Application Support/iTerm2/Scripts/vigil_pane.py"

pane_is_open() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; }

open_pane() {
    # First choice: the Python API, which can also set the width. it2run asks iTerm2 to start a script
    # from its own Scripts folder and returns at once, so wait and see whether the pane appears.
    if [ -x "$IT2RUN" ] && [ -f "$API_SCRIPT" ]; then
        cmp -s "$DIR/pane_api.py" "$API_SCRIPT" || cp "$DIR/pane_api.py" "$API_SCRIPT" # keep the copy current
        "$IT2RUN" vigil_pane.py "$SID" "$CMD" "$COLS"
        i=0
        while [ "$i" -lt 20 ]; do
            sleep 0.5
            pane_is_open && return 0
            i=$((i + 1))
        done
        echo "open_pane: the Python API script opened nothing within 10 s (see iTerm2 > Scripts > Manage > Console); using AppleScript" >&2
    fi
    # Otherwise AppleScript: the pane opens, at half the width.
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

if pane_is_open; then
    [ -n "$DODECA_PANE_DEBUG" ] && echo "open_pane: a pane is already open (pid $(cat "$PIDFILE")); close it first" >&2
    exit 0
fi

if [ -n "$DODECA_PANE_DEBUG" ]; then
    open_pane
else
    open_pane >/dev/null 2>&1 & # in the background: Claude Code should not wait for iTerm2 to answer
fi
exit 0
