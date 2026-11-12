#!/bin/sh
# Builds the Windows panel, vigil-panel.exe, with MinGW-w64. Works in WSL (sudo apt install mingw-w64)
# and when cross-compiling on macOS (brew install mingw-w64). The exe needs no DLLs: everything is linked in.
#
# Then, in WSL: ./vigil-win (the launcher next to it) starts it on the events file the hook writes.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-x86_64-w64-mingw32-gcc}
$CC -std=c11 -O2 -Wall -Wextra -mwindows -static -o vigil-panel.exe \
    win/panel.c src/events.c src/geom.c src/session.c src/motion.c src/render.c -lm -lgdi32 -luser32 -lshell32 -lole32 -luuid
cp win/vigil-win vigil-win
chmod +x vigil-win
echo "built vigil-panel.exe and ./vigil-win"
