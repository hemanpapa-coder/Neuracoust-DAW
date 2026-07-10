#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_BUNDLE="${1:-$ROOT_DIR/build/dev-webrtc/Neuracoust DAW.app}"
SMOKE_BIN="${SMOKE_BIN:-$ROOT_DIR/build/dev-webrtc/neuracoust_native_webrtc_smoke}"
NODE_BIN="${NODE_BIN:-/Users/hasnagm4pro/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node}"
NODE_MODULES="${NODE_MODULES:-/Users/hasnagm4pro/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules}"
HOST="${LISTEN_QA_HOST:-127.0.0.1}"
PORT="${LISTEN_QA_PORT:-8787}"
SESSION="${LISTEN_QA_SESSION:-native-smoke}"
TOKEN="${LISTEN_QA_TOKEN:-native-token}"
WAIT_OFFER_MS="${LISTEN_QA_WAIT_OFFER_MS:-15000}"
STREAM_MS="${LISTEN_QA_STREAM_MS:-18000}"
SENDER_STREAM_MS="${LISTEN_QA_SENDER_STREAM_MS:-$((STREAM_MS + WAIT_OFFER_MS + 5000))}"
RELAY_SCRIPT="$APP_BUNDLE/Contents/Resources/start-relay.sh"
QA_JS="/tmp/neuracoust-native-webrtc-browser-qa.mjs"
RELAY_LOG="/tmp/neuracoust-native-webrtc-relay.log"
SENDER_LOG="/tmp/neuracoust-native-webrtc-sender.log"

if [[ ! -x "$RELAY_SCRIPT" ]]; then
  echo "Missing relay launcher: $RELAY_SCRIPT" >&2
  exit 2
fi
if [[ ! -x "$SMOKE_BIN" ]]; then
  echo "Missing native WebRTC smoke binary: $SMOKE_BIN" >&2
  exit 2
fi
if [[ ! -x "$NODE_BIN" ]]; then
  echo "Missing Node runtime: $NODE_BIN" >&2
  exit 2
fi

cleanup() {
  if [[ -n "${SENDER_PID:-}" ]] && kill -0 "$SENDER_PID" 2>/dev/null; then
    kill "$SENDER_PID" 2>/dev/null || true
    wait "$SENDER_PID" 2>/dev/null || true
  fi
  if [[ -n "${RELAY_PID:-}" ]] && kill -0 "$RELAY_PID" 2>/dev/null; then
    kill "$RELAY_PID" 2>/dev/null || true
    wait "$RELAY_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

LISTEN_ACCESS_TOKEN="$TOKEN" "$RELAY_SCRIPT" --host "$HOST" --session "$SESSION" > "$RELAY_LOG" 2>&1 &
RELAY_PID=$!

for _ in {1..80}; do
  if /usr/bin/curl -fsS "http://$HOST:$PORT/api/stats" >/tmp/neuracoust-native-webrtc-stats.json 2>/dev/null; then
    break
  fi
  /bin/sleep 0.25
done
/usr/bin/curl -fsS "http://$HOST:$PORT/api/stats" >/tmp/neuracoust-native-webrtc-stats.json

"$SMOKE_BIN" --require-available --wait-offer-ready-ms "$WAIT_OFFER_MS" --stream-ms "$SENDER_STREAM_MS" > "$SENDER_LOG" 2>&1 &
SENDER_PID=$!

for _ in {1..80}; do
  offer_status="$(/usr/bin/curl -sS -o /tmp/neuracoust-native-webrtc-offer.json -w "%{http_code}" "http://$HOST:$PORT/api/native-webrtc/offer?session=$SESSION&token=$TOKEN" 2>/dev/null || true)"
  if [[ "$offer_status" == "200" ]] && /usr/bin/grep -q '"id"' /tmp/neuracoust-native-webrtc-offer.json; then
    break
  fi
  /bin/sleep 0.25
done
if ! /usr/bin/grep -q '"id"' /tmp/neuracoust-native-webrtc-offer.json 2>/dev/null; then
  echo "Native WebRTC offer was not published." >&2
  cat "$SENDER_LOG" >&2 || true
  exit 3
fi

cat > "$QA_JS" <<'JS'
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const { chromium } = require("playwright");

const [host, port, session, token, observeMsArg] = process.argv.slice(2);
const observeMs = Math.max(1000, Number.parseInt(observeMsArg || "18000", 10) || 18000);
const url = `http://${host}:${port}/?session=${encodeURIComponent(session)}&profile=external&connect=direct&token=${encodeURIComponent(token)}&bufferMs=120`;
const chromePath = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const browser = await chromium.launch({
  headless: true,
  executablePath: chromePath,
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage();
page.on("console", (msg) => {
  if (msg.type() === "error") console.error(`[browser] ${msg.text()}`);
});
await page.goto(url, { waitUntil: "domcontentloaded" });
await page.waitForFunction(() => Boolean(window.__neuracoustListenTest), null, { timeout: 5000 });
await page.click("#playBtn");
try {
  await page.waitForFunction(() => {
    const state = window.__neuracoustListenTest?.state?.();
    return state && state.activeTransport === "native-webrtc" && state.rxRaw > 0 && state.rxPackets > 0;
  }, null, { timeout: 14000 });
} catch (err) {
  const state = await page.evaluate(() => window.__neuracoustListenTest?.state?.() || null).catch(() => null);
  console.error(JSON.stringify({ error: String(err?.message || err), state }, null, 2));
  await browser.close();
  process.exit(4);
}
const firstState = await page.evaluate(() => window.__neuracoustListenTest.state());
const deadline = Date.now() + Math.max(1000, observeMs - 2000);
let state = firstState;
while (Date.now() < deadline) {
  await page.waitForTimeout(Math.min(1000, Math.max(100, deadline - Date.now())));
  state = await page.evaluate(() => window.__neuracoustListenTest.state());
  if (state.rxBadParse > 0 || state.activeTransport !== "native-webrtc") {
    break;
  }
}
await browser.close();
if (state.rxPackets <= firstState.rxPackets || state.rxBadParse > 0 || state.activeTransport !== "native-webrtc") {
  console.error(JSON.stringify(state, null, 2));
  process.exit(3);
}
console.log(JSON.stringify(state, null, 2));
JS

NODE_PATH="$NODE_MODULES" "$NODE_BIN" "$QA_JS" "$HOST" "$PORT" "$SESSION" "$TOKEN" "$STREAM_MS"
echo "Native WebRTC browser QA passed."
