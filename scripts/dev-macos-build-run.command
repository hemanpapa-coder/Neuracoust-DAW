#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

neuracoust_date_version() {
  printf '%s%02d\n' "$(date '+%y%m%d.%H')" "$((10#$(date '+%M') / 10 * 10))"
}

find_tool() {
  local explicit="$1"
  shift
  local candidate
  for candidate in "$explicit" "$@"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
    if [[ -n "$candidate" ]] && command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
}

VERSION="${1:-$(neuracoust_date_version)}"
BUILD_DIR="${NEURACOUST_MACOS_DEV_BUILD_DIR:-$ROOT_DIR/build/macos-ui-260701.0000}"
DIST_DIR="$ROOT_DIR/dist"
APP_PATH="$BUILD_DIR/Neuracoust DAW.app"

CMAKE_BIN="$(find_tool "${NEURACOUST_CMAKE:-}" \
  "$HOME/.local/lib/python3.12/site-packages/cmake/data/bin/cmake" \
  "/opt/homebrew/bin/cmake" \
  "/usr/local/bin/cmake" \
  "/Applications/CMake.app/Contents/bin/cmake" \
  "cmake")" || {
    echo "ERROR: CMake not found. Install CMake or set NEURACOUST_CMAKE." >&2
    exit 2
  }

echo "==> Neuracoust DAW dev version: $VERSION"
echo "==> Build directory: $BUILD_DIR"
"$CMAKE_BIN" -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DNEURACOUST_DAW_VERSION="$VERSION"
"$CMAKE_BIN" --build "$BUILD_DIR" \
  --target NeuracoustDAW neuracoust_daw_core_tests neuracoust_daw_audio_tests neuracoust_daw_desktop_wav_workflow_tests \
  -j "${NEURACOUST_BUILD_JOBS:-6}"

"$BUILD_DIR/neuracoust_daw_core_tests"
"$BUILD_DIR/neuracoust_daw_audio_tests"
"$BUILD_DIR/neuracoust_daw_desktop_wav_workflow_tests"
"$ROOT_DIR/scripts/smoke-vst3-editor-host.command" "$APP_PATH/Contents/MacOS/Neuracoust VST3 Editor Host"

mkdir -p "$DIST_DIR"
echo "$APP_PATH" > "$DIST_DIR/latest-macos-app.txt"

if [[ "${NEURACOUST_DAW_SKIP_OPEN:-0}" != "1" ]]; then
  pkill -x "Neuracoust DAW" 2>/dev/null || true
  sleep 1
  open -n "$APP_PATH"
  sleep 2
  osascript -e "tell application POSIX file \"$APP_PATH\" to activate" >/dev/null 2>&1 || true
fi

BUNDLE_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$APP_PATH/Contents/Info.plist")"
if [[ "$BUNDLE_VERSION" != "$VERSION" ]]; then
  echo "ERROR: app CFBundleVersion is $BUNDLE_VERSION, expected $VERSION." >&2
  exit 3
fi

echo "==> Built and verified $APP_PATH ($BUNDLE_VERSION)"
