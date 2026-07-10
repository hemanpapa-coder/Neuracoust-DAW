#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
neuracoust_date_version() {
  printf '%s%02d\n' "$(date '+%y%m%d.%H')" "$((10#$(date '+%M') / 10 * 10))"
}
VERSION="${1:-$(neuracoust_date_version)}"
BUILD_DIR="${NEURACOUST_MACOS_BUILD_DIR:-$ROOT_DIR/build/macos-release-$VERSION}"
DIST_DIR="$ROOT_DIR/dist"
PKG_ROOT="$BUILD_DIR/pkgroot"
APP_NAME="Neuracoust DAW.app"
PKG_PATH="$DIST_DIR/Neuracoust DAW ${VERSION}.pkg"
AR_WRAPPER="$BUILD_DIR/neuracoust-ar-wrapper.zsh"
ENABLE_NATIVE_WEBRTC="${NEURACOUST_DAW_ENABLE_NATIVE_WEBRTC:-ON}"
WEBRTC_ROOT="${NEURACOUST_WEBRTC_ROOT:-$ROOT_DIR/third_party/libwebrtc-src/src}"
WEBRTC_BUILD_DIR="${NEURACOUST_WEBRTC_BUILD_DIR:-$ROOT_DIR/third_party/libwebrtc-src/src/out/neuracoust-macos-release}"
PAYLOAD_LIST="$(mktemp "${TMPDIR:-/tmp}/neuracoust-daw-payload.XXXXXX")"
trap 'rm -f "$PAYLOAD_LIST"' EXIT

export COPYFILE_DISABLE=1

find_tool() {
  local explicit="$1"
  shift
  local candidate
  for candidate in "$explicit" "$@"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
    if [[ -n "$candidate" ]] && command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
}

CMAKE_BIN="$(find_tool "${NEURACOUST_CMAKE:-}" \
  "$HOME/.local/lib/python3.12/site-packages/cmake/data/bin/cmake" \
  "/opt/homebrew/bin/cmake" \
  "/usr/local/bin/cmake" \
  "/Applications/CMake.app/Contents/bin/cmake" \
  "cmake")" || {
    echo "ERROR: CMake not found. Install CMake or set NEURACOUST_CMAKE." >&2
    exit 2
  }
CTEST_BIN="${NEURACOUST_CTEST:-}"
if [[ -z "$CTEST_BIN" ]]; then
  CTEST_BIN="$(find_tool "" \
    "${CMAKE_BIN:h}/ctest" \
    "$HOME/.local/lib/python3.12/site-packages/cmake/data/bin/ctest" \
    "/opt/homebrew/bin/ctest" \
    "/usr/local/bin/ctest" \
    "/Applications/CMake.app/Contents/bin/ctest" \
    "ctest")" || {
      echo "ERROR: CTest not found. Install CMake or set NEURACOUST_CTEST." >&2
      exit 2
    }
fi

strip_package_metadata() {
  local root="$1"
  xattr -cr "$root" || true
  while IFS= read -r -d '' item; do
    xattr -d com.apple.provenance "$item" 2>/dev/null || true
    xattr -d com.apple.quarantine "$item" 2>/dev/null || true
  done < <(find "$root" -print0)
  dot_clean -m "$root" || true
  find "$root" -name '._*' -delete
}

count_provenance_xattrs() {
  xattr -lr "$1" 2>/dev/null | grep -c 'com.apple.provenance' || true
}

sanitize_pkg_payload_without_appledouble() {
  local pkg_path="$1"
  local root="$2"
  local work_dir
  local expanded_dir
  local clean_pkg
  work_dir="$(mktemp -d "${TMPDIR:-/tmp}/neuracoust-daw-pkg-sanitize.XXXXXX")"
  expanded_dir="$work_dir/expanded"
  clean_pkg="$work_dir/clean.pkg"
  rm -f "$clean_pkg"
  pkgutil --expand "$pkg_path" "$expanded_dir"
  /usr/bin/mkbom "$root" "$expanded_dir/Bom"
  (
    cd "$root"
    find . -name '._*' -prune -o -print | /usr/bin/cpio -o --format odc 2>/dev/null | /usr/bin/gzip -c > "$expanded_dir/Payload"
  )
  pkgutil --flatten "$expanded_dir" "$clean_pkg"
  cp "$clean_pkg" "$pkg_path"
  rm -rf "$work_dir"
}

patch_archive_link_script() {
  local link_script="$BUILD_DIR/CMakeFiles/neuracoust_daw_core.dir/link.txt"
  if [[ ! -f "$link_script" ]]; then
    return
  fi
  NEURACOUST_AR_WRAPPER="$AR_WRAPPER" /usr/bin/perl -0pi -e 's#/usr/bin/ar qc #"$ENV{NEURACOUST_AR_WRAPPER}" qc #g' "$link_script"
}

write_archive_wrapper() {
  cat > "$AR_WRAPPER" <<'EOF'
#!/bin/zsh
set -euo pipefail

REAL_AR="/usr/bin/ar"
if [[ "$#" -ge 3 && "$1" == q* && "$2" == *.a ]]; then
  MODE="$1"
  OUT="$2"
  shift 2
  TMP_ARCHIVE="${TMPDIR:-/tmp}/neuracoust-ar-${$}-${RANDOM}.a"
  rm -f "$TMP_ARCHIVE"
  "$REAL_AR" "$MODE" "$TMP_ARCHIVE" "$@"
  cp "$TMP_ARCHIVE" "$OUT"
  rm -f "$TMP_ARCHIVE"
  exit 0
fi

exec "$REAL_AR" "$@"
EOF
  chmod +x "$AR_WRAPPER"
}

mkdir -p "$BUILD_DIR" "$DIST_DIR"
write_archive_wrapper
echo "==> Using macOS build directory: $BUILD_DIR"
echo "==> Using CMake: $CMAKE_BIN"
if [[ "${NEURACOUST_FORCE_CONFIGURE:-0}" != "1" \
  && -f "$BUILD_DIR/CMakeCache.txt" \
  && "$(grep '^NEURACOUST_DAW_VERSION:STRING=' "$BUILD_DIR/CMakeCache.txt" | sed 's/^NEURACOUST_DAW_VERSION:STRING=//')" == "$VERSION" \
  && "$(grep '^CMAKE_BUILD_TYPE:STRING=' "$BUILD_DIR/CMakeCache.txt" | sed 's/^CMAKE_BUILD_TYPE:STRING=//')" == "Release" ]]; then
  echo "==> Reusing configured CMake cache for $VERSION"
else
  configure_args=(
    -S "$ROOT_DIR"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release
    -DNEURACOUST_DAW_VERSION="$VERSION"
    -DCMAKE_AR="$AR_WRAPPER"
    -DNEURACOUST_DAW_ENABLE_NATIVE_WEBRTC="$ENABLE_NATIVE_WEBRTC"
  )
  if [[ "$ENABLE_NATIVE_WEBRTC" == "ON" ]]; then
    configure_args+=(
      -DNEURACOUST_WEBRTC_ROOT="$WEBRTC_ROOT"
      -DNEURACOUST_WEBRTC_BUILD_DIR="$WEBRTC_BUILD_DIR"
    )
  fi
  "$CMAKE_BIN" "${configure_args[@]}"
fi
GENERATOR="$(grep '^CMAKE_GENERATOR:INTERNAL=' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | sed 's/^CMAKE_GENERATOR:INTERNAL=//')"
patch_archive_link_script
if [[ "$GENERATOR" == "Unix Makefiles" ]]; then
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_daw_core/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target NeuracoustVst3EditorHost/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target NeuracoustAuEditorHost/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_vst3_process_worker/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_vst3_host_audit/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_vst3_isolated_process_smoke/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_native_webrtc_smoke/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_remote_core_server/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_video_render/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_video_render_fixture/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target NeuracoustDAW/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_daw_core_tests/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_daw_ai_assistant_tests/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_daw_audio_tests/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_daw_desktop_wav_workflow_tests/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_daw_remote_dsp_tests/fast -j 8
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target neuracoust_dsp_manager_tests/fast -j 8
else
  "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target NeuracoustDAW neuracoust_daw_core_tests neuracoust_daw_audio_tests -j 8
fi
"$CTEST_BIN" --test-dir "$BUILD_DIR" --output-on-failure

rm -rf "$PKG_ROOT"
mkdir -p "$PKG_ROOT/Applications/Neuracoust"
ditto --noextattr --noqtn "$BUILD_DIR/Neuracoust DAW.app" "$PKG_ROOT/Applications/Neuracoust/$APP_NAME"

strip_package_metadata "$PKG_ROOT"
PROVENANCE_COUNT="$(count_provenance_xattrs "$PKG_ROOT")"
if [[ "$PROVENANCE_COUNT" != "0" ]]; then
  echo "WARNING: package root still contains $PROVENANCE_COUNT com.apple.provenance xattrs before pkgbuild." >&2
  echo "macOS may preserve those as AppleDouble payload files in app-hosted shells." >&2
fi

pkgbuild \
  --root "$PKG_ROOT" \
  --install-location "/" \
  --identifier "com.neuracoust.daw" \
  --version "$VERSION" \
  "$PKG_PATH"

pkgutil --payload-files "$PKG_PATH" > "$PAYLOAD_LIST"
grep -q "Applications/Neuracoust/Neuracoust DAW.app" "$PAYLOAD_LIST"
if grep -E -q '(^|/)\._' "$PAYLOAD_LIST"; then
  echo "WARNING: package payload contains AppleDouble sidecar files; rebuilding Payload/Bom without resource-fork sidecars." >&2
  sanitize_pkg_payload_without_appledouble "$PKG_PATH" "$PKG_ROOT"
  pkgutil --payload-files "$PKG_PATH" > "$PAYLOAD_LIST"
fi
if grep -E -q '(^|/)\._' "$PAYLOAD_LIST"; then
  echo "ERROR: package payload still contains AppleDouble sidecar files after sanitization." >&2
  echo "First AppleDouble payload entries:" >&2
  grep -E '(^|/)\._' "$PAYLOAD_LIST" | sed -n '1,40p' >&2
  echo "This usually means protected com.apple.provenance xattrs were added by the app-hosted shell." >&2
  echo "Re-run this script from a clean Terminal or SSH shell before public release." >&2
  echo "Clean-shell retry: cd '$ROOT_DIR' && scripts/build-macos-pkg.command '$VERSION'" >&2
  if [[ "${NEURACOUST_ALLOW_APP_SHELL_XATTRS:-0}" != "1" ]]; then
    exit 9
  fi
  echo "NEURACOUST_ALLOW_APP_SHELL_XATTRS=1 is set, so this development package is being kept." >&2
fi
echo "$PKG_PATH" > "$DIST_DIR/latest-pkg.txt"
echo "$BUILD_DIR/Neuracoust DAW.app" > "$DIST_DIR/latest-macos-app.txt"
echo "$PKG_PATH"
