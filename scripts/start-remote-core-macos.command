#!/usr/bin/env zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${NEURACOUST_REMOTE_CORE_BUILD_DIR:-$ROOT_DIR/build/dev}"
SERVER="$BUILD_DIR/neuracoust_remote_core_server"

if [[ ! -x "$SERVER" ]]; then
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
  cmake --build "$BUILD_DIR" --target neuracoust_remote_core_server -j 4
fi

args=(
  --bind "${NEURACOUST_REMOTE_CORE_BIND:-0.0.0.0}"
  --port "${NEURACOUST_REMOTE_CORE_PORT:-20000}"
  --status-port "${NEURACOUST_REMOTE_CORE_STATUS_PORT:-20001}"
  --module-id "${NEURACOUST_REMOTE_CORE_MODULE_ID:-na.neuracoust.monitor.speaker}"
  --module-name "${NEURACOUST_REMOTE_CORE_MODULE_NAME:-Neuracoust Monitor Speaker Remote Core}"
  --gain "${NEURACOUST_REMOTE_CORE_GAIN:-1.0}"
  --sample-rate "${NEURACOUST_REMOTE_CORE_SAMPLE_RATE:-48000}"
)

if [[ -n "${NEURACOUST_REMOTE_CORE_MODULE_PATH:-}" ]]; then
  args+=(--module-path "$NEURACOUST_REMOTE_CORE_MODULE_PATH")
fi
if [[ -n "${NEURACOUST_REMOTE_CORE_VST3_PATH:-}" ]]; then
  args+=(--vst3-path "$NEURACOUST_REMOTE_CORE_VST3_PATH")
  args+=(--vst3-name "${NEURACOUST_REMOTE_CORE_VST3_NAME:-${NEURACOUST_REMOTE_CORE_MODULE_NAME:-Neuracoust Hosted VST3 Remote Core}}")
fi
if [[ -n "${NEURACOUST_REMOTE_CORE_VST3_CLASS_ID:-}" ]]; then
  args+=(--vst3-class-id "$NEURACOUST_REMOTE_CORE_VST3_CLASS_ID")
fi
if [[ -n "${NEURACOUST_REMOTE_CORE_VST3_CLASS_NAME:-}" ]]; then
  args+=(--vst3-class-name "$NEURACOUST_REMOTE_CORE_VST3_CLASS_NAME")
fi

exec "$SERVER" "${args[@]}"
