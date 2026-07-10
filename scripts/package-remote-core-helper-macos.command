#!/usr/bin/env zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${NEURACOUST_REMOTE_CORE_BUILD_DIR:-$ROOT_DIR/build/dev}"
DIST_DIR="$ROOT_DIR/dist"
APP_DIR="$DIST_DIR/Neuracoust Remote Core.app"
MACOS_DIR="$APP_DIR/Contents/MacOS"
RESOURCES_DIR="$APP_DIR/Contents/Resources"
SERVER="$BUILD_DIR/neuracoust_remote_core_server"

if [[ ! -x "$SERVER" ]]; then
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
  cmake --build "$BUILD_DIR" --target neuracoust_remote_core_server -j 4
fi

rm -rf "$APP_DIR"
mkdir -p "$MACOS_DIR" "$RESOURCES_DIR"
cp "$SERVER" "$MACOS_DIR/neuracoust_remote_core_server"

cat > "$MACOS_DIR/Neuracoust Remote Core" <<'EOF'
#!/usr/bin/env zsh
set -euo pipefail
APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
args=(
  --bind "${NEURACOUST_REMOTE_CORE_BIND:-0.0.0.0}"
  --port "${NEURACOUST_REMOTE_CORE_PORT:-20000}"
  --status-port "${NEURACOUST_REMOTE_CORE_STATUS_PORT:-20001}"
  --module-id "${NEURACOUST_REMOTE_CORE_MODULE_ID:-na.neuracoust.4001e}"
  --module-name "${NEURACOUST_REMOTE_CORE_MODULE_NAME:-Neuracoust 4001E Remote Core}"
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
exec "$APP_DIR/MacOS/neuracoust_remote_core_server" "${args[@]}"
EOF

chmod +x "$MACOS_DIR/Neuracoust Remote Core" "$MACOS_DIR/neuracoust_remote_core_server"

cat > "$APP_DIR/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>Neuracoust Remote Core</string>
  <key>CFBundleIdentifier</key>
  <string>com.neuracoust.remote-core</string>
  <key>CFBundleName</key>
  <string>Neuracoust Remote Core</string>
  <key>CFBundleDisplayName</key>
  <string>Neuracoust Remote Core</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>0.1.0</string>
  <key>CFBundleVersion</key>
  <string>0.1.0</string>
  <key>LSMinimumSystemVersion</key>
  <string>13.0</string>
</dict>
</plist>
EOF

ditto -c -k --keepParent "$APP_DIR" "$DIST_DIR/Neuracoust_Remote_Core_Mac.zip"
echo "$DIST_DIR/Neuracoust_Remote_Core_Mac.zip" > "$DIST_DIR/latest-remote-core-macos.txt"
echo "Remote Core helper app: $APP_DIR"
echo "Remote Core helper ZIP: $DIST_DIR/Neuracoust_Remote_Core_Mac.zip"
