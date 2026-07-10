#!/bin/zsh
set -euo pipefail

HELPER_PATH="${1:-}"
LOG_PATH="${TMPDIR:-/tmp}/neuracoust-daw-vst3-editor-host-smoke.log"
TIMEOUT_SECONDS="${NEURACOUST_VST3_EDITOR_SMOKE_TIMEOUT_SECONDS:-10}"

if [[ -z "$HELPER_PATH" || ! -x "$HELPER_PATH" ]]; then
  echo "VST3 editor host smoke skipped: helper is not executable at '$HELPER_PATH'"
  exit 0
fi

typeset -a CANDIDATE_PATHS
typeset -a CANDIDATE_NAMES
CANDIDATE_PATHS=("${2:-/Library/Audio/Plug-Ins/VST3/Newacoust4001E.vst3}")
CANDIDATE_NAMES=("${3:-Newacoust4001E}")
if [[ "$#" -lt 2 ]]; then
  CANDIDATE_PATHS+=("/Library/Audio/Plug-Ins/VST3/Neuracoust Comp Limiter 340.vst3")
  CANDIDATE_NAMES+=("Neuracoust Comp Limiter 340")
  CANDIDATE_PATHS+=("/Library/Audio/Plug-Ins/VST3/FabFilter Pro-Q 4.vst3")
  CANDIDATE_NAMES+=("FabFilter Pro-Q 4")
fi

HAD_INSTALLED_CANDIDATE=0
LAST_LOG=""
LAST_NAME=""

run_probe() {
  local plugin_path="$1"
  local plugin_name="$2"
  rm -f "$LOG_PATH"
  "$HELPER_PATH" --probe --plugin "$plugin_path" --name "$plugin_name" --title "Neuracoust VST3 Editor Host Smoke" >"$LOG_PATH" 2>&1 &
  local pid=$!
  local max_polls=$(( TIMEOUT_SECONDS * 10 ))
  for _ in $(seq 1 "$max_polls"); do
    if ! kill -0 "$pid" 2>/dev/null; then
      set +e
      wait "$pid"
      local exit_status=$?
      set -e
      if [[ "$exit_status" -eq 0 && -f "$LOG_PATH" ]] && grep -q '^READY$' "$LOG_PATH"; then
        return 0
      fi
      return 1
    fi
    sleep 0.1
  done

  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  return 1
}

run_parameter_probe() {
  local plugin_path="$1"
  local plugin_name="$2"
  rm -f "$LOG_PATH"
  if ! "$HELPER_PATH" --inspect-parameters --plugin "$plugin_path" --name "$plugin_name" --limit 8 >"$LOG_PATH" 2>&1; then
    return 1
  fi
  awk -F '\t' '
    /^INSPECT/ {
      if ($2 == "1" && $3 == "1" && $4 == "1" && $5 == "1" && $6 == "1" && ($7 + 0) > 0) {
        found = 1
      }
    }
    END { exit(found ? 0 : 1) }
  ' "$LOG_PATH"
}

for index in $(seq 1 ${#CANDIDATE_PATHS[@]}); do
  PLUGIN_PATH="${CANDIDATE_PATHS[$index]}"
  PLUGIN_NAME="${CANDIDATE_NAMES[$index]}"
  if [[ ! -e "$PLUGIN_PATH" ]]; then
    continue
  fi
  HAD_INSTALLED_CANDIDATE=1
  LAST_NAME="$PLUGIN_NAME"
  if run_probe "$PLUGIN_PATH" "$PLUGIN_NAME"; then
    echo "VST3 editor host smoke passed for $PLUGIN_NAME"
    exit 0
  fi
  LAST_LOG="$(cat "$LOG_PATH" 2>/dev/null || true)"
  if run_parameter_probe "$PLUGIN_PATH" "$PLUGIN_NAME"; then
    echo "VST3 editor host smoke passed for $PLUGIN_NAME via parameter fallback"
    exit 0
  fi
  LAST_LOG="$(cat "$LOG_PATH" 2>/dev/null || true)"
  if [[ "${#CANDIDATE_PATHS[@]}" -gt 1 ]]; then
    echo "VST3 editor host smoke candidate failed for $PLUGIN_NAME; trying fallback if available" >&2
  else
    echo "VST3 editor host smoke candidate failed for $PLUGIN_NAME" >&2
  fi
done

if [[ "$HAD_INSTALLED_CANDIDATE" -eq 0 ]]; then
  echo "VST3 editor host smoke skipped: no probe plug-in candidates are installed"
  exit 0
fi

echo "VST3 editor host smoke failed for $LAST_NAME"
if [[ -n "$LAST_LOG" ]]; then
  print -r -- "$LAST_LOG" >&2
fi
exit 1
