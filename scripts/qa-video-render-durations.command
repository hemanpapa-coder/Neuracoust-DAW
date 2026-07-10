#!/bin/zsh
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${1:-$repo_root/build/dev}"
out_dir="${2:-/tmp/neuracoust-video-duration-qa}"
durations=(${=NEURACOUST_VIDEO_QA_DURATIONS:-180 600 1800})
preset="${NEURACOUST_VIDEO_QA_PRESET:-share-preview-720p}"
timeout_seconds="${NEURACOUST_VIDEO_QA_TIMEOUT_SECONDS:-900}"
fixture="$build_dir/neuracoust_video_render_fixture"
renderer="$build_dir/neuracoust_video_render"

if [[ ! -x "$fixture" || ! -x "$renderer" ]]; then
  echo "Missing video QA tools under $build_dir" >&2
  exit 2
fi

rm -rf "$out_dir"
mkdir -p "$out_dir"
report="$out_dir/report.tsv"
printf "duration_seconds\tpreset\tstatus\toutput_bytes\trender_seconds\tmanifest\n" > "$report"

for duration in "${durations[@]}"; do
  case_dir="$out_dir/${duration}s"
  mkdir -p "$case_dir"
  "$fixture" "$case_dir/fixture" --duration "$duration" >"$case_dir/fixture.log"
  start_epoch="$(date +%s)"
  "$renderer" \
    --project "$case_dir/fixture/Video Render Fixture.ndaw" \
    --output "$case_dir/output.mp4" \
    --preset "$preset" >"$case_dir/render.log" 2>&1 &
  render_pid="$!"
  render_status="running"
  while kill -0 "$render_pid" 2>/dev/null; do
    now="$(date +%s)"
    elapsed="$((now - start_epoch))"
    if [[ "$elapsed" -ge "$timeout_seconds" ]]; then
      render_status="timeout"
      kill "$render_pid" 2>/dev/null || true
      wait "$render_pid" 2>/dev/null || true
      break
    fi
    sleep 2
  done
  if [[ "$render_status" == "running" ]]; then
    if wait "$render_pid"; then
      render_status="passed"
    else
      render_status="failed"
    fi
  fi
  end_epoch="$(date +%s)"
  bytes="0"
  if [[ -f "$case_dir/output.mp4" ]]; then
    bytes="$(/usr/bin/stat -f%z "$case_dir/output.mp4")"
  fi
  if [[ "$render_status" == "passed" ]]; then
    if ! /usr/bin/grep -q '"ok": true' "$case_dir/output.render.json"; then
      render_status="failed-validation"
    fi
  fi
  printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$duration" "$preset" "$render_status" "$bytes" "$((end_epoch - start_epoch))" "$case_dir/output.render.json" >> "$report"
done

cat "$report"
