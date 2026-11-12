#!/bin/sh
# Builds the floating-panel version (macOS only). Needs the Xcode command line tools.
#
#   Vigil.app   the panel itself, with an Info.plist (no Dock icon; the microphone and speech
#                     recognition usage texts macOS shows when it asks for permission), signed
#   ./vigil-panel    a launcher script: starts the app through LaunchServices, so macOS asks for those
#                     permissions on behalf of the app instead of the terminal, and stays in the terminal
#                     pane (open_pane.sh runs it there) until the app quits
#
# Signing: ad-hoc by default. macOS remembers permissions per signature, and an ad-hoc signature changes
# with every build, so after a rebuild it asks again. To keep them, create a code-signing certificate once
# (Keychain Access > Certificate Assistant > Create a Certificate, type "Code Signing") and build with
#   DODECA_SIGN_ID="that certificate's name" sh mac/build.sh
set -e
cd "$(dirname "$0")/.."
APP=Vigil.app
ID=local.vigil.panel

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cc -O2 -fobjc-arc -Wall -Wextra -o "$APP/Contents/MacOS/vigil-panel" \
    mac/panel.m mac/speech.m mac/listen.m mac/tip.m src/events.c src/geom.c src/session.c src/render.c -framework Cocoa -framework Carbon -framework AVFoundation -framework Speech -framework CoreAudio -framework AudioToolbox -lm

cat >"$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key><string>$ID</string>
	<key>CFBundleName</key><string>Vigil</string>
	<key>CFBundleExecutable</key><string>vigil-panel</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
	<key>LSUIElement</key><true/>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSMicrophoneUsageDescription</key><string>vigil-panel listens while you hold a core down (push-to-talk), to send your spoken reply to that Claude Code session.</string>
	<key>NSAppleEventsUsageDescription</key><string>vigil-panel brings a session's iTerm2 pane to the front when you double-click its core.</string>
	<key>NSSpeechRecognitionUsageDescription</key><string>vigil-panel turns what you say during push-to-talk into text, on this Mac only.</string>
</dict>
</plist>
EOF

codesign --force --sign "${DODECA_SIGN_ID:--}" --identifier "$ID" "$APP" 2>/dev/null

cat >vigil-panel <<'EOF'
#!/bin/sh
# Launcher for Vigil.app (written by mac/build.sh). Starts the app through LaunchServices and waits
# for it. The app is told this terminal (--tty, to size itself to the pane and keep it blank) and this
# script's pid (--launcher-pid): when the pane is closed or Ctrl-C is pressed this script ends, and the app
# quits with it.
DIR=$(cd "$(dirname "$0")" && pwd)
set -- --launcher-pid $$ "$@"
[ -n "$DODECA_EVENTS" ] && set -- --events "$DODECA_EVENTS" "$@" # apps started by LaunchServices get no environment
C=$(command -v claude 2>/dev/null) && set -- --claude "$C" "$@"     # nor this shell's PATH
if [ -t 0 ] && [ -t 1 ]; then
    TTY=$(tty)
    exec_open() { open -n -W --stdout "$TTY" --stderr "$TTY" "$DIR/Vigil.app" --args --tty "$TTY" "$@"; }
else
    exec_open() { open -n -W "$DIR/Vigil.app" --args "$@"; }
fi
exec_open "$@"
EOF
chmod +x vigil-panel
echo "built $APP and ./vigil-panel"
