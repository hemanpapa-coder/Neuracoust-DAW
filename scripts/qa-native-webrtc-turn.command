#!/bin/zsh
set -euo pipefail
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin"

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_BUNDLE="${1:-$ROOT_DIR/build/dev-webrtc/Neuracoust DAW.app}"

if [[ -z "${NEURACOUST_LISTEN_TURN_URL:-}" ]]; then
  echo "NEURACOUST_LISTEN_TURN_URL is required for TURN-configured QA." >&2
  exit 2
fi
if [[ -z "${NEURACOUST_LISTEN_TURN_USERNAME:-}" ]]; then
  echo "NEURACOUST_LISTEN_TURN_USERNAME is required for TURN-configured QA." >&2
  exit 2
fi
if [[ -z "${NEURACOUST_LISTEN_TURN_PASSWORD:-}" ]]; then
  echo "NEURACOUST_LISTEN_TURN_PASSWORD is required for TURN-configured QA." >&2
  exit 2
fi

echo "==> Running native WebRTC QA with TURN configuration"
echo "TURN URL: ${NEURACOUST_LISTEN_TURN_URL}"
echo "TURN user: ${NEURACOUST_LISTEN_TURN_USERNAME}"
echo "TURN password: [redacted]"

LISTEN_QA_STREAM_MS="${LISTEN_QA_STREAM_MS:-30000}" \
"$ROOT_DIR/scripts/qa-native-webrtc-browser.command" "$APP_BUNDLE"
