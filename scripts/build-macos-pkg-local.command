#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
neuracoust_date_version() {
  printf '%s%02d\n' "$(date '+%y%m%d.%H')" "$((10#$(date '+%M') / 10 * 10))"
}
VERSION="${1:-$(neuracoust_date_version)}"
SCRATCH_PARENT="${NEURACOUST_MACOS_LOCAL_BUILD_PARENT:-${TMPDIR:-/tmp}}"
LOCAL_ROOT="$SCRATCH_PARENT/neuracoust-daw-src-$VERSION"
LOCAL_BUILD="$SCRATCH_PARENT/neuracoust-daw-build-$VERSION"
LOCAL_DIST="$LOCAL_ROOT/dist"
DIST_DIR="$ROOT_DIR/dist"
APP_STAGE="$ROOT_DIR/build/macos-local-package-$VERSION"
APP_NAME="Neuracoust DAW.app"
PKG_NAME="Neuracoust DAW ${VERSION}.pkg"

export COPYFILE_DISABLE=1

rm -rf "$LOCAL_ROOT" "$LOCAL_BUILD"
mkdir -p "$LOCAL_ROOT" "$DIST_DIR" "$APP_STAGE"

echo "==> Copying source to local macOS build scratch: $LOCAL_ROOT"
for item in CMakeLists.txt cmake docs packaging resources scripts src tests; do
  if [[ -e "$ROOT_DIR/$item" ]]; then
    ditto --noextattr --noqtn "$ROOT_DIR/$item" "$LOCAL_ROOT/$item"
  fi
done
mkdir -p "$LOCAL_ROOT/third_party/vst3sdk/public.sdk/source/vst"
ditto --noextattr --noqtn "$ROOT_DIR/third_party/vst3sdk/pluginterfaces" "$LOCAL_ROOT/third_party/vst3sdk/pluginterfaces"
ditto --noextattr --noqtn "$ROOT_DIR/third_party/vst3sdk/public.sdk/source/vst/vstinitiids.cpp" \
  "$LOCAL_ROOT/third_party/vst3sdk/public.sdk/source/vst/vstinitiids.cpp"

echo "==> Building local-scratch macOS package"
(
  cd "$LOCAL_ROOT"
  NEURACOUST_MACOS_BUILD_DIR="$LOCAL_BUILD" \
    NEURACOUST_FORCE_CONFIGURE=1 \
    NEURACOUST_ALLOW_APP_SHELL_XATTRS="${NEURACOUST_ALLOW_APP_SHELL_XATTRS:-0}" \
    scripts/build-macos-pkg.command "$VERSION"
)

echo "==> Copying local-scratch artifacts back to project"
ditto --noextattr --noqtn "$LOCAL_DIST/$PKG_NAME" "$DIST_DIR/$PKG_NAME"
rm -rf "$APP_STAGE/$APP_NAME"
ditto --noextattr --noqtn "$LOCAL_BUILD/$APP_NAME" "$APP_STAGE/$APP_NAME"

echo "$DIST_DIR/$PKG_NAME" > "$DIST_DIR/latest-pkg.txt"
echo "$APP_STAGE/$APP_NAME" > "$DIST_DIR/latest-macos-app.txt"
echo "$DIST_DIR/$PKG_NAME"
