#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
neuracoust_date_version() {
  printf '%s%02d\n' "$(date '+%y%m%d.%H')" "$((10#$(date '+%M') / 10 * 10))"
}
VERSION="${1:-$(neuracoust_date_version)}"
DIST_DIR="$ROOT_DIR/dist"
PKG_PATH="${NEURACOUST_DAW_PKG:-$DIST_DIR/Neuracoust DAW ${VERSION}.pkg}"
WINDOWS_ZIP="${NEURACOUST_DAW_WINDOWS_ZIP:-$DIST_DIR/Neuracoust_DAW_Windows_x64_${VERSION}_app.zip}"
WINDOWS_INSTALLER_ZIP="${NEURACOUST_DAW_WINDOWS_INSTALLER_ZIP:-$DIST_DIR/Neuracoust_DAW_Windows_x64_${VERSION}_installer.zip}"
if [[ -n "${NEURACOUST_DAW_APP:-}" ]]; then
  APP_PATH="$NEURACOUST_DAW_APP"
elif [[ -f "$DIST_DIR/latest-macos-app.txt" ]]; then
  APP_PATH="$(<"$DIST_DIR/latest-macos-app.txt")"
else
  APP_PATH="$ROOT_DIR/build/macos-release-$VERSION/Neuracoust DAW.app"
fi
PAYLOAD_LIST="$(mktemp "${TMPDIR:-/tmp}/neuracoust-daw-payload.XXXXXX")"
ZIP_LIST="$(mktemp "${TMPDIR:-/tmp}/neuracoust-daw-zip.XXXXXX")"
INSTALLER_ZIP_LIST="$(mktemp "${TMPDIR:-/tmp}/neuracoust-daw-installer-zip.XXXXXX")"
WINDOW_LIST_SOURCE="$(mktemp "${TMPDIR:-/tmp}/neuracoust-daw-window-source.XXXXXX")"
WINDOW_LIST_TOOL="$(mktemp "${TMPDIR:-/tmp}/neuracoust-daw-window.XXXXXX")"
WINDOW_LIST_OUT="$(mktemp "${TMPDIR:-/tmp}/neuracoust-daw-window-out.XXXXXX")"
PKG_EXPAND_PARENT="$(mktemp -d "${TMPDIR:-/tmp}/neuracoust-daw-pkgexpand.XXXXXX")"
PKG_EXPAND_ROOT="$PKG_EXPAND_PARENT/pkg"
trap 'rm -f "$PAYLOAD_LIST" "$ZIP_LIST" "$INSTALLER_ZIP_LIST" "$WINDOW_LIST_SOURCE" "$WINDOW_LIST_TOOL" "$WINDOW_LIST_OUT"; rm -rf "$PKG_EXPAND_PARENT"; killall "Neuracoust DAW" 2>/dev/null || true' EXIT

require_file() {
  local path="$1"
  local label="$2"
  if [[ ! -e "$path" ]]; then
    echo "ERROR: missing $label: $path" >&2
    exit 3
  fi
}

expect_payload_entry() {
  local pattern="$1"
  local label="$2"
  if ! grep -q "$pattern" "$PAYLOAD_LIST"; then
    echo "ERROR: package payload is missing $label." >&2
    exit 4
  fi
}

expect_zip_entry() {
  local entry="$1"
  local label="$2"
  if ! grep -Fxq "$entry" "$ZIP_LIST"; then
    echo "ERROR: Windows artifact is missing $label ($entry)." >&2
    exit 5
  fi
}

expect_installer_zip_entry() {
  local entry="$1"
  local label="$2"
  if ! grep -Fxq "$entry" "$INSTALLER_ZIP_LIST"; then
    echo "ERROR: Windows installer ZIP is missing $label ($entry)." >&2
    exit 5
  fi
}

echo "==> Validating Neuracoust DAW artifacts ($VERSION)"
require_file "$APP_PATH" "macOS app"
require_file "$PKG_PATH" "macOS package"
require_file "$WINDOWS_ZIP" "Windows validation ZIP"
require_file "$WINDOWS_INSTALLER_ZIP" "Windows installer ZIP"

echo "==> Checking macOS bundle identity"
BUNDLE_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$APP_PATH/Contents/Info.plist")"
BUNDLE_SHORT_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP_PATH/Contents/Info.plist")"
BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP_PATH/Contents/Info.plist")"
if [[ "$BUNDLE_VERSION" != "$VERSION" ]]; then
  echo "ERROR: CFBundleVersion is $BUNDLE_VERSION, expected $VERSION." >&2
  exit 6
fi
if [[ "$BUNDLE_SHORT_VERSION" != "0.1.0" ]]; then
  echo "ERROR: CFBundleShortVersionString is $BUNDLE_SHORT_VERSION, expected 0.1.0." >&2
  exit 6
fi
if [[ "$BUNDLE_ID" != "com.neuracoust.daw" ]]; then
  echo "ERROR: CFBundleIdentifier is $BUNDLE_ID, expected com.neuracoust.daw." >&2
  exit 6
fi

echo "==> Checking visible macOS app window through WindowServer"
cat > "$WINDOW_LIST_SOURCE" <<'OBJC'
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

int main(void) {
    @autoreleasepool {
        NSArray* windows = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
        BOOL found = NO;
        for (NSDictionary* window in windows) {
            NSString* owner = window[(id)kCGWindowOwnerName];
            if (![owner isEqualToString:@"Neuracoust DAW"]) {
                continue;
            }
            id name = window[(id)kCGWindowName] ?: @"";
            id bounds = window[(id)kCGWindowBounds] ?: @{};
            printf("window=%s bounds=%s\n",
                [[name description] UTF8String],
                [[bounds description] UTF8String]);
            found = YES;
        }
        if (!found) {
            puts("NO_WINDOW");
        }
    }
    return 0;
}
OBJC
/usr/bin/clang -x objective-c -fobjc-arc "$WINDOW_LIST_SOURCE" -framework Foundation -framework CoreGraphics -o "$WINDOW_LIST_TOOL"
killall "Neuracoust DAW" 2>/dev/null || true
open -n "$APP_PATH"
WINDOW_FOUND=0
for attempt in {1..30}; do
  sleep 1
  osascript -e 'tell application id "com.neuracoust.daw" to activate' >/dev/null 2>&1 || true
  "$WINDOW_LIST_TOOL" > "$WINDOW_LIST_OUT"
  if ! grep -q '^NO_WINDOW$' "$WINDOW_LIST_OUT" && grep -q 'Neuracoust DAW - Untitled' "$WINDOW_LIST_OUT"; then
    WINDOW_FOUND=1
    break
  fi
done
cat "$WINDOW_LIST_OUT"
if [[ "$WINDOW_FOUND" != "1" ]] && grep -q '^NO_WINDOW$' "$WINDOW_LIST_OUT"; then
  echo "Running Neuracoust DAW processes:" >&2
  pgrep -fl "Neuracoust DAW" >&2 || true
  echo "ERROR: Neuracoust DAW did not expose a visible WindowServer window." >&2
  exit 7
fi
if [[ "$WINDOW_FOUND" != "1" ]]; then
  echo "ERROR: visible app window does not have the expected project title." >&2
  exit 7
fi
killall "Neuracoust DAW" 2>/dev/null || true

echo "==> Checking macOS package payload"
pkgutil --payload-files "$PKG_PATH" > "$PAYLOAD_LIST"
expect_payload_entry 'Applications/Neuracoust/Neuracoust DAW.app' "install app path"
expect_payload_entry 'Applications/Neuracoust/Neuracoust DAW.app/Contents/MacOS/Neuracoust DAW' "app executable"
APPLEDOUBLE_COUNT="$(grep -E '(^|/)\._' "$PAYLOAD_LIST" | wc -l | tr -d ' ')"
if [[ "$APPLEDOUBLE_COUNT" != "0" ]]; then
  echo "WARNING: package payload contains $APPLEDOUBLE_COUNT AppleDouble sidecar files." >&2
  if [[ "${NEURACOUST_ALLOW_APP_SHELL_XATTRS:-0}" != "1" ]]; then
    echo "ERROR: public release packages must be rebuilt from a clean Terminal/SSH shell." >&2
    exit 8
  fi
  echo "NEURACOUST_ALLOW_APP_SHELL_XATTRS=1 is set, so this development package is accepted." >&2
fi
echo "==> Expanding macOS package payload into temporary root"
pkgutil --expand-full "$PKG_PATH" "$PKG_EXPAND_ROOT" >/dev/null
EXPANDED_APP="$(find "$PKG_EXPAND_ROOT" -path "*/Applications/Neuracoust/Neuracoust DAW.app" -type d -print -quit)"
if [[ -z "$EXPANDED_APP" ]]; then
  echo "ERROR: expanded package payload does not contain /Applications/Neuracoust/Neuracoust DAW.app." >&2
  exit 10
fi
require_file "$EXPANDED_APP/Contents/MacOS/Neuracoust DAW" "expanded macOS app executable"
require_file "$EXPANDED_APP/Contents/Resources/bundled_plugins.json" "expanded bundled plugin catalog"
require_file "$EXPANDED_APP/Contents/Resources/license_policy.json" "expanded license policy"
EXPANDED_BUNDLE_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$EXPANDED_APP/Contents/Info.plist")"
if [[ "$EXPANDED_BUNDLE_VERSION" != "$VERSION" ]]; then
  echo "ERROR: expanded app CFBundleVersion is $EXPANDED_BUNDLE_VERSION, expected $VERSION." >&2
  exit 10
fi

echo "==> Checking Windows validation ZIP"
unzip -Z1 "$WINDOWS_ZIP" > "$ZIP_LIST"
expect_zip_entry "Neuracoust DAW.exe" "Windows app executable"
expect_zip_entry "Neuracoust DSP Manager.exe" "Windows DSP Manager executable"
expect_zip_entry "bundled_plugins.json" "bundled plugin catalog"
expect_zip_entry "license_policy.json" "license policy"
expect_zip_entry "neuracoust_daw_core_tests.exe" "Windows core smoke executable"
expect_zip_entry "neuracoust_daw_audio_tests.exe" "Windows audio smoke executable"
expect_zip_entry "PRODUCT_BRIEF.md" "product brief"
expect_zip_entry "ARCHITECTURE.md" "architecture notes"
if ! unzip -p "$WINDOWS_ZIP" PRODUCT_BRIEF.md | grep -q "version format: YYMMDD.HHMM"; then
  echo "ERROR: Windows ZIP PRODUCT_BRIEF.md does not describe the Neuracoust date build version format." >&2
  exit 9
fi

echo "==> Checking Windows installer ZIP"
unzip -Z1 "$WINDOWS_INSTALLER_ZIP" > "$INSTALLER_ZIP_LIST"
expect_installer_zip_entry "Neuracoust DAW.exe" "Windows app executable"
expect_installer_zip_entry "Neuracoust DSP Manager.exe" "Windows DSP Manager executable"
expect_installer_zip_entry "bundled_plugins.json" "bundled plugin catalog"
expect_installer_zip_entry "license_policy.json" "license policy"
expect_installer_zip_entry "install-neuracoust-daw.ps1" "installer script"
expect_installer_zip_entry "uninstall-neuracoust-daw.ps1" "uninstaller script"
expect_installer_zip_entry "README-Windows-Install.txt" "installer instructions"

cat <<MSG
Validation complete.
macOS app: $APP_PATH
macOS pkg: $PKG_PATH
Windows zip: $WINDOWS_ZIP
Windows installer zip: $WINDOWS_INSTALLER_ZIP
AppleDouble payload entries: $APPLEDOUBLE_COUNT
MSG
