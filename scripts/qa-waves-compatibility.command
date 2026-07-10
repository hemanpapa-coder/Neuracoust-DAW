#!/bin/zsh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build/dev}"
INPUT="${INPUT:-test_audio/1kHz_3s_minus18dBFS_stereo_44k1_16bit.wav}"
OUT_DIR="${OUT_DIR:-runs/waves-overnight-$(date +%Y%m%d-%H%M%S)}"
LIMIT="${LIMIT:-24}"
PARAM_LIMIT="${PARAM_LIMIT:-5}"
INSPECT_LIMIT="${INSPECT_LIMIT:-260}"
START_INDEX="${START_INDEX:-0}"
MAX_BATCHES="${MAX_BATCHES:-0}"
RUN_GATE="${RUN_GATE:-1}"

mkdir -p "$OUT_DIR"

if [[ "$RUN_GATE" != "0" ]]; then
  echo "== Building DAW and VST3 editor host =="
  cmake --build "$BUILD_DIR" --target NeuracoustDAW -j "${NEURACOUST_BUILD_JOBS:-8}"

  echo "== Running DAW smoke tests =="
  ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 240

  echo "== Running installed Waves realtime insert smoke =="
  NEURACOUST_CORE_SMOKE_SCAN_INSTALLED_VST3=1 \
    ctest --test-dir "$BUILD_DIR" -R '^audio_engine_smoke$' --output-on-failure --timeout 240

  echo "== Clearing stale DAW plug-in editor hosts before audit =="
  pkill -x 'Neuracoust VST3 Editor Host' 2>/dev/null || true
  pkill -x 'Neuracoust AU Editor Host' 2>/dev/null || true
fi

batch=0
while true; do
  batch=$((batch + 1))
  echo "== Waves compatibility batch ${batch}, start index ${START_INDEX} =="
  python3 tools/waves_compat_audit.py \
    --build-dir "$BUILD_DIR" \
    --input "$INPUT" \
    --input-gain-db 0 \
    --out-dir "$OUT_DIR" \
    --filter Waves \
    --limit "$LIMIT" \
    --param-limit "$PARAM_LIMIT" \
    --inspect-limit "$INSPECT_LIMIT" \
    --start-index "$START_INDEX" \
    --resume \
    --no-renders \
    --fail-on-problem

  read -r NEXT_INDEX FILTERED_COUNT < <(python3 - "$OUT_DIR/report.json" <<'PY'
import json
import sys
from pathlib import Path

report = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
print(report.get("next_start_index", 0), report.get("filtered_plugin_count", 0))
PY
)
  python3 - "$OUT_DIR/report.json" <<'PY'
import json
import sys
from pathlib import Path

report = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
status_counts = report.get("status_counts", {})
status_text = ", ".join(f"{key}={value}" for key, value in sorted(status_counts.items())) or "none"
print(
    "== Summary: tested={tested} problems={problems} statuses: {statuses} ==".format(
        tested=report.get("tested_count", len(report.get("plugins", []))),
        problems=report.get("problem_candidate_count", 0),
        statuses=status_text,
    )
)
PY

  echo "== Completed through index ${NEXT_INDEX} of ${FILTERED_COUNT} =="
  if (( NEXT_INDEX >= FILTERED_COUNT )); then
    echo "== Waves compatibility audit complete: ${OUT_DIR}/report.md =="
    exit 0
  fi
  if (( MAX_BATCHES > 0 && batch >= MAX_BATCHES )); then
    echo "== Stopping after MAX_BATCHES=${MAX_BATCHES}. Resume from index ${NEXT_INDEX}. =="
    echo "== Report: ${OUT_DIR}/report.md =="
    exit 0
  fi
  START_INDEX="$NEXT_INDEX"
done
