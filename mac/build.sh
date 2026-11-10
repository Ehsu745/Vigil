#!/bin/sh
# Builds dodeca-panel, the floating-panel version (macOS only). Needs the Xcode command line tools.
set -e
cd "$(dirname "$0")/.."
cc -O2 -fobjc-arc -Wall -o dodeca-panel mac/panel.m src/events.c src/geom.c src/session.c src/render.c -framework Cocoa -lm
echo "built ./dodeca-panel"
