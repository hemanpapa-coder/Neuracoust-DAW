#!/bin/zsh
set -euo pipefail
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin"

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WEBRTC_ROOT="${NEURACOUST_WEBRTC_ROOT:-$ROOT_DIR/third_party/libwebrtc-src}"
DEPOT_TOOLS_DIR="${NEURACOUST_DEPOT_TOOLS_DIR:-$ROOT_DIR/third_party/depot_tools}"
OUT_DIR="${NEURACOUST_WEBRTC_OUT:-out/neuracoust-macos-release}"
TARGET_CPU="${NEURACOUST_WEBRTC_TARGET_CPU:-arm64}"
GCLIENT_JOBS="${NEURACOUST_GCLIENT_JOBS:-4}"
GCLIENT_EXTRA_ARGS_TEXT="${NEURACOUST_GCLIENT_EXTRA_ARGS:---reset --delete_unversioned_trees}"
GCLIENT_EXTRA_ARGS=(${=GCLIENT_EXTRA_ARGS_TEXT})

mkdir -p "$(dirname "$WEBRTC_ROOT")"

if [[ ! -d "$DEPOT_TOOLS_DIR/.git" ]]; then
  echo "==> Fetching depot_tools"
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "$DEPOT_TOOLS_DIR"
fi

export PATH="$DEPOT_TOOLS_DIR:$PATH"

if [[ ! -d "$WEBRTC_ROOT/src" ]]; then
  echo "==> Fetching WebRTC source into $WEBRTC_ROOT"
  mkdir -p "$WEBRTC_ROOT"
  (
    cd "$WEBRTC_ROOT"
    fetch --nohooks webrtc
  )
fi

cd "$WEBRTC_ROOT/src"
echo "==> Syncing WebRTC dependencies (jobs=$GCLIENT_JOBS ${GCLIENT_EXTRA_ARGS[*]})"
gclient sync --jobs "$GCLIENT_JOBS" "${GCLIENT_EXTRA_ARGS[@]}"

GN_ARGS=(
  'is_debug=false'
  'is_component_build=false'
  'rtc_include_tests=false'
  'rtc_build_examples=false'
  'rtc_use_h264=false'
  'proprietary_codecs=false'
  'ffmpeg_branding="Chromium"'
  'use_custom_libcxx=false'
  'use_custom_libcxx_for_host=false'
  'clang_use_unsafe_buffers_plugin=false'
  'treat_warnings_as_errors=false'
  'use_rtti=true'
  'rtc_enable_symbol_export=true'
  "target_cpu=\"$TARGET_CPU\""
)

echo "==> Generating GN project: $OUT_DIR"
gn gen "$OUT_DIR" --args="${(j: :)GN_ARGS}"

echo "==> Building libwebrtc"
ninja -C "$OUT_DIR" webrtc

cat <<MSG
libwebrtc build complete.
source: $WEBRTC_ROOT/src
out:    $WEBRTC_ROOT/src/$OUT_DIR

Configure DAW with:
cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build/dev-webrtc" \\
  -DNEURACOUST_DAW_ENABLE_NATIVE_WEBRTC=ON \\
  -DNEURACOUST_WEBRTC_ROOT="$WEBRTC_ROOT/src" \\
  -DNEURACOUST_WEBRTC_BUILD_DIR="$WEBRTC_ROOT/src/$OUT_DIR"
MSG
