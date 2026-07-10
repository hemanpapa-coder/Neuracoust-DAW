#!/usr/bin/env zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${NEURACOUST_REMOTE_CORE_BUILD_DIR:-$ROOT_DIR/build/dev}"
REMOTE_CORE_DIR="${NEURACOUST_REMOTE_CORE_DIR:-$HOME/Neuracoust/RemoteCore}"
LOG_DIR="${NEURACOUST_REMOTE_CORE_LOG_DIR:-$HOME/Neuracoust/logs}"
PLIST_TEMPLATE="$ROOT_DIR/packaging/macos/com.neuracoust.remote-core.plist"
PLIST_OUT="$HOME/Library/LaunchAgents/com.neuracoust.remote-core.plist"
DEFAULT_VST3="/Library/Audio/Plug-Ins/VST3/Newacoust4001E.vst3"
VST3_PATH="${NEURACOUST_REMOTE_CORE_VST3_PATH:-}"

SERVER_SOURCE="$BUILD_DIR/neuracoust_remote_core_server"
SERVER_DEST="$REMOTE_CORE_DIR/neuracoust_remote_core_server"

if [[ ! -x "$SERVER_SOURCE" ]]; then
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
  cmake --build "$BUILD_DIR" --target neuracoust_remote_core_server -j 4
fi

mkdir -p "$REMOTE_CORE_DIR" "$LOG_DIR" "$HOME/Library/LaunchAgents"
cp "$SERVER_SOURCE" "$SERVER_DEST"
chmod +x "$SERVER_DEST"
xattr -d com.apple.quarantine "$SERVER_DEST" 2>/dev/null || true

if [[ "${NEURACOUST_REMOTE_CORE_ENABLE_VST3:-0}" == "1" && -z "$VST3_PATH" && -d "$DEFAULT_VST3" ]]; then
  VST3_PATH="$DEFAULT_VST3"
fi
if [[ -n "$VST3_PATH" ]]; then
  VST3_ARGS=$'        <string>--vst3-path</string>\n'
  VST3_ARGS+=$"        <string>$VST3_PATH</string>"$'\n'
  VST3_ARGS+=$'        <string>--vst3-name</string>\n'
  VST3_ARGS+=$"        <string>${NEURACOUST_REMOTE_CORE_VST3_NAME:-Newacoust4001E}</string>"
else
  VST3_ARGS=""
fi

export SERVER_DEST REMOTE_CORE_DIR LOG_DIR VST3_ARGS
perl -0pe 's#__REMOTE_CORE_BIN__#$ENV{SERVER_DEST}#g;
           s#__REMOTE_CORE_DIR__#$ENV{REMOTE_CORE_DIR}#g;
           s#__LOG_DIR__#$ENV{LOG_DIR}#g;
           s#__REMOTE_CORE_VST3_ARGS__#$ENV{VST3_ARGS}#g;' \
  "$PLIST_TEMPLATE" > "$PLIST_OUT"

plutil -lint "$PLIST_OUT" >/dev/null
launchctl bootout "gui/$(id -u)" "$PLIST_OUT" >/dev/null 2>&1 || true
pkill -f "$SERVER_DEST" >/dev/null 2>&1 || true
launchctl bootstrap "gui/$(id -u)" "$PLIST_OUT"
launchctl kickstart -k "gui/$(id -u)/com.neuracoust.remote-core"
launchctl print "gui/$(id -u)/com.neuracoust.remote-core" | sed -n '1,80p'
