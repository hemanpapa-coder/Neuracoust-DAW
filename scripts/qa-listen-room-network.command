#!/bin/zsh
set -euo pipefail
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin"

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_BUNDLE="${LISTEN_QA_APP_BUNDLE:-$ROOT_DIR/build/dev/Neuracoust DAW.app}"
RELAY_SCRIPT="${LISTEN_QA_RELAY_SCRIPT:-$APP_BUNDLE/Contents/Resources/start-relay.sh}"
SESSION="${LISTEN_QA_SESSION:-qa-soak}"
TOKEN="${LISTEN_QA_TOKEN:-qa-token}"
HOST="${LISTEN_QA_HOST:-127.0.0.1}"
HTTP_PORT="${LISTEN_QA_HTTP_PORT:-8787}"
DURATION_SECONDS="${LISTEN_QA_SECONDS:-30}"
INTERVAL_SECONDS="${LISTEN_QA_INTERVAL_SECONDS:-2}"
RUN_ROOT="$ROOT_DIR/build/listen-room-qa/$(date '+%Y%m%d-%H%M%S')"
STATS_LOG="$RUN_ROOT/stats.jsonl"
SUMMARY="$RUN_ROOT/summary.txt"

mkdir -p "$RUN_ROOT"

if [[ ! -x "$RELAY_SCRIPT" ]]; then
  echo "Relay start script not found or not executable: $RELAY_SCRIPT" >&2
  exit 2
fi
if [[ "$HTTP_PORT" != "8787" ]]; then
  echo "Bundled Listen start-relay.sh currently serves HTTP on fixed port 8787; got LISTEN_QA_HTTP_PORT=$HTTP_PORT" >&2
  exit 2
fi

cleanup() {
  if [[ -n "${RELAY_PID:-}" ]] && kill -0 "$RELAY_PID" 2>/dev/null; then
    kill "$RELAY_PID" 2>/dev/null || true
    wait "$RELAY_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "==> Starting Listen Room relay for network QA"
LISTEN_ACCESS_TOKEN="$TOKEN" \
"$RELAY_SCRIPT" --host "$HOST" --session "$SESSION" > "$RUN_ROOT/relay.log" 2>&1 &
RELAY_PID=$!

ready=0
for _ in {1..80}; do
  if /usr/bin/curl -fsS "http://$HOST:$HTTP_PORT/api/stats" > "$RUN_ROOT/initial-stats.json" 2>/dev/null; then
    ready=1
    break
  fi
  /bin/sleep 0.25
done

if [[ "$ready" != "1" || ! -s "$RUN_ROOT/initial-stats.json" ]]; then
  echo "Relay did not expose /api/stats. See $RUN_ROOT/relay.log" >&2
  exit 3
fi

/usr/bin/curl -fsS "http://$HOST:$HTTP_PORT/api/capabilities?token=$TOKEN" > "$RUN_ROOT/capabilities.json"

deadline=$((SECONDS + DURATION_SECONDS))
samples=0
failures=0
while (( SECONDS < deadline )); do
  timestamp="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  if body="$(/usr/bin/curl -fsS "http://$HOST:$HTTP_PORT/api/stats" 2>> "$RUN_ROOT/curl-errors.log")"; then
    printf '{"timestamp":"%s","stats":%s}\n' "$timestamp" "$body" >> "$STATS_LOG"
    samples=$((samples + 1))
  else
    printf '{"timestamp":"%s","error":"stats request failed"}\n' "$timestamp" >> "$STATS_LOG"
    failures=$((failures + 1))
  fi
  /bin/sleep "$INTERVAL_SECONDS"
done

{
  echo "Listen Room network QA complete"
  echo "session=$SESSION"
  echo "durationSeconds=$DURATION_SECONDS"
  echo "samples=$samples"
  echo "failures=$failures"
  echo "statsLog=$STATS_LOG"
  echo "capabilities=$RUN_ROOT/capabilities.json"
  echo "relayLog=$RUN_ROOT/relay.log"
} | tee "$SUMMARY"

if (( samples == 0 || failures > 0 )); then
  exit 4
fi
