#!/bin/zsh
set -euo pipefail
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin"

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_BUNDLE="${1:-$ROOT_DIR/build/dev/Neuracoust DAW.app}"
RELAY_SCRIPT="$APP_BUNDLE/Contents/Resources/start-relay.sh"
RELAY_DIR="$APP_BUNDLE/Contents/relay"
WEB_DIR="$APP_BUNDLE/Contents/web"
SESSION="bundle-verify"
TOKEN="bundle-token"
HOST="127.0.0.1"
HTTP_PORT="${LISTEN_VERIFY_HTTP_PORT:-8787}"
PYTHON_BIN="${PYTHON:-/usr/bin/python3}"

required=(
  "$RELAY_SCRIPT"
  "$RELAY_DIR/listen_relay.py"
  "$RELAY_DIR/protocol.py"
  "$RELAY_DIR/opus_transcode.py"
  "$WEB_DIR/index.html"
  "$WEB_DIR/pcm-player-processor.js"
)

for path in "${required[@]}"; do
  if [[ ! -e "$path" ]]; then
    echo "Missing bundled Listen file: $path" >&2
    exit 2
  fi
done
if [[ "$HTTP_PORT" != "8787" ]]; then
  echo "Bundled Listen start-relay.sh currently serves HTTP on fixed port 8787; got LISTEN_VERIFY_HTTP_PORT=$HTTP_PORT" >&2
  exit 2
fi
if [[ ! -x "$PYTHON_BIN" ]]; then
  echo "Python not found: $PYTHON_BIN" >&2
  exit 2
fi

"$PYTHON_BIN" -m py_compile "$RELAY_DIR/listen_relay.py" "$RELAY_DIR/protocol.py" "$RELAY_DIR/opus_transcode.py"

cleanup() {
  if [[ -n "${RELAY_PID:-}" ]] && kill -0 "$RELAY_PID" 2>/dev/null; then
    kill "$RELAY_PID" 2>/dev/null || true
    wait "$RELAY_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

LISTEN_ACCESS_TOKEN="$TOKEN" \
"$RELAY_SCRIPT" --host "$HOST" --session "$SESSION" > /tmp/neuracoust-listen-bundle-verify.log 2>&1 &
RELAY_PID=$!

ready=0
for _ in {1..80}; do
  if /usr/bin/curl -fsS "http://$HOST:$HTTP_PORT/api/stats" > /tmp/neuracoust-listen-bundle-stats.json 2>/dev/null; then
    ready=1
    break
  fi
  /bin/sleep 0.25
done

if [[ "$ready" != "1" ]]; then
  echo "Bundled Listen relay did not expose /api/stats" >&2
  exit 3
fi

/usr/bin/curl -fsS "http://$HOST:$HTTP_PORT/api/capabilities?token=$TOKEN" > /tmp/neuracoust-listen-bundle-capabilities.json
/usr/bin/curl -fsS "http://$HOST:$HTTP_PORT/?session=$SESSION&token=$TOKEN" > /tmp/neuracoust-listen-bundle-index.html
offer_json='{"type":"offer","sdp":"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n","quality":"lan","latency":"low"}'
/usr/bin/curl -fsS \
  -H "Content-Type: application/json" \
  --data "$offer_json" \
  "http://$HOST:$HTTP_PORT/api/native-webrtc/offer?session=$SESSION&token=$TOKEN" \
  > /tmp/neuracoust-listen-native-offer.json
/usr/bin/curl -fsS "http://$HOST:$HTTP_PORT/api/native-webrtc/offer?session=$SESSION&token=$TOKEN" \
  > /tmp/neuracoust-listen-native-offer-readback.json
native_id="$(/usr/bin/sed -n 's/.*"id": *"\([^"]*\)".*/\1/p' /tmp/neuracoust-listen-native-offer.json)"
answer_json="{\"id\":\"$native_id\",\"type\":\"answer\",\"sdp\":\"v=0\\r\\no=- 2 1 IN IP4 127.0.0.1\\r\\ns=-\\r\\nt=0 0\\r\\n\"}"
/usr/bin/curl -fsS \
  -H "Content-Type: application/json" \
  --data "$answer_json" \
  "http://$HOST:$HTTP_PORT/api/native-webrtc/answer?token=$TOKEN" \
  > /tmp/neuracoust-listen-native-answer.json
/usr/bin/curl -fsS "http://$HOST:$HTTP_PORT/api/native-webrtc/answer?id=$native_id&token=$TOKEN" \
  > /tmp/neuracoust-listen-native-answer-readback.json

if ! /usr/bin/grep -q '"serverFallback"' /tmp/neuracoust-listen-bundle-capabilities.json; then
  echo "Bundled Listen capabilities response is incomplete" >&2
  exit 3
fi

if ! /usr/bin/grep -q 'Listen' /tmp/neuracoust-listen-bundle-index.html; then
  echo "Bundled Listen web page did not render expected content" >&2
  exit 4
fi

if ! /usr/bin/grep -q '"mode": "native-webrtc"' /tmp/neuracoust-listen-native-answer-readback.json; then
  echo "Bundled Listen native WebRTC signaling did not round-trip" >&2
  exit 5
fi

echo "Listen relay bundle verified: $APP_BUNDLE"
