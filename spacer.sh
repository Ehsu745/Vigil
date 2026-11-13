#!/bin/sh
# Keeps a pane empty under the floating panel, in a tab other than the one the panel was started from.
# Closes when the panel is gone and no session is running. If the panel is gone but sessions are still
# running (its own tab was closed), this pane becomes the panel's new home.

DIR=$(cd "$(dirname "$0")" && pwd)
PIDFILE="$HOME/.dodeca-core/pane.pid"
panel_alive() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; }

stty -echo 2>/dev/null
printf '\033[?25l\033[2J\033[H' # no cursor, empty screen
while :; do
    while panel_alive; do sleep 2; done
    sleep "0.$(($$ % 10))" # several spacers may notice at once: do not all act in the same instant
    panel_alive && continue
    n=$("$DIR/dodeca-core" --count 2>/dev/null)
    [ "${n:-0}" -gt 0 ] 2>/dev/null || exit 0
    exec "$DIR/dodeca-panel" --fit-pane --exit-when-empty "$@"
done
