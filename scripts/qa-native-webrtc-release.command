#!/bin/zsh
set -euo pipefail
export PATH="$HOME/.local/bin:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin"

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${NEURACOUST_DAW_BUILD_DIR:-$ROOT_DIR/build/dev-webrtc}"
APP_BUNDLE="${1:-$BUILD_DIR/Neuracoust DAW.app}"
HARNESS_DIR="${NEURACOUST_DAW_HARNESS_DIR:-/Volumes/Program Dev/Neuracoust DSP Validation Harness}"
STREAM_MS="${LISTEN_QA_STREAM_MS:-120000}"

echo "==> Native WebRTC release-readiness QA"
echo "Build dir: $BUILD_DIR"
echo "App bundle: $APP_BUNDLE"

echo "==> Building app, QA helpers, and native WebRTC smoke"
cmake --build "$BUILD_DIR" --target NeuracoustDAW neuracoust_native_webrtc_smoke neuracoust_video_render_fixture -j "${NEURACOUST_BUILD_JOBS:-6}"

echo "==> Verifying bundled Listen relay/web assets"
"$ROOT_DIR/scripts/verify-listen-relay-bundle.command" "$APP_BUNDLE"

echo "==> Running native WebRTC browser QA (${STREAM_MS} ms)"
LISTEN_QA_STREAM_MS="$STREAM_MS" "$ROOT_DIR/scripts/qa-native-webrtc-browser.command" "$APP_BUNDLE"

echo "==> Running DAW CTest suite"
ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 180

if [[ -d "$HARNESS_DIR" ]]; then
  echo "==> Running Neuracoust DAW validation harness"
  (cd "$HARNESS_DIR" && python3 apth.py --daw-suite --daw-project "$ROOT_DIR" --daw-build-dir "$BUILD_DIR" --pretty)
else
  echo "Skipping DAW validation harness; directory not found: $HARNESS_DIR" >&2
fi

echo "Native WebRTC release-readiness QA passed."
