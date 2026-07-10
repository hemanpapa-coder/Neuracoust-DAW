#!/usr/bin/env zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -n "${NEURACOUST_VERSION:-}" ]]; then
  VERSION="$NEURACOUST_VERSION"
else
  RAW_VERSION="$(date '+%y%m%d.%H%M')"
  VERSION="${RAW_VERSION%?}0"
fi
BUILD_DIR="$ROOT_DIR/build/dsp-usb-maker-macos-$VERSION"
APP_DIR="$BUILD_DIR/Neuracoust DSP USB Maker.app"
CONTENTS_DIR="$APP_DIR/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"
RESOURCES_DIR="$CONTENTS_DIR/Resources"

rm -rf "$BUILD_DIR"
mkdir -p "$MACOS_DIR" "$RESOURCES_DIR"

cat > "$CONTENTS_DIR/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>NeuracoustDspUsbMaker</string>
  <key>CFBundleIdentifier</key>
  <string>com.neuracoust.dsp-usb-maker</string>
  <key>CFBundleName</key>
  <string>Neuracoust DSP USB Maker</string>
  <key>CFBundleDisplayName</key>
  <string>Neuracoust DSP USB Maker</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleIconFile</key>
  <string>NeuracoustDspUsbMaker</string>
  <key>CFBundleShortVersionString</key>
  <string>$VERSION</string>
  <key>CFBundleVersion</key>
  <string>$VERSION</string>
  <key>LSMinimumSystemVersion</key>
  <string>13.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
PLIST

swiftc \
  -O \
  -parse-as-library \
  "$ROOT_DIR/packaging/dsp-server-usb/macos/NeuracoustDspUsbMaker.swift" \
  -o "$MACOS_DIR/NeuracoustDspUsbMaker"

cp -R "$ROOT_DIR/scripts" "$RESOURCES_DIR/scripts"
cp "$ROOT_DIR/resources/icons/dsp-usb-maker/NeuracoustDspUsbMaker.icns" "$RESOURCES_DIR/NeuracoustDspUsbMaker.icns"
mkdir -p "$RESOURCES_DIR/packaging"
cp -R "$ROOT_DIR/packaging/dsp-server-usb" "$RESOURCES_DIR/packaging/dsp-server-usb"
cp "$ROOT_DIR/packaging/dsp-server-usb/README.md" "$RESOURCES_DIR/README.md"
cp "$ROOT_DIR/packaging/dsp-server-usb/docs/pc-boot-test-checklist.md" "$RESOURCES_DIR/PC-Boot-Test-Checklist.md"
cp "$ROOT_DIR/packaging/dsp-server-usb/docs/current-server-extract-summary.md" "$RESOURCES_DIR/Current-Debian-DSP-Server-Extract-Summary.md"
cat > "$RESOURCES_DIR/RELEASE.txt" <<EOF
Neuracoust DSP Server USB Maker for macOS
Version: $VERSION
Base OS: Debian 12 bookworm minimal live appliance
Kernel family: Debian PREEMPT_RT
Visibility: administrator-only Neuracoust utility release
Copyright (C) 2026 Neuracoust. All rights reserved.
EOF
chmod +x "$RESOURCES_DIR/scripts/make-dsp-server-usb-macos.command" \
  "$RESOURCES_DIR/scripts/make-dsp-server-usb-windows-remote.command" \
  "$RESOURCES_DIR/scripts/build-dsp-server-appliance-iso.command" \
  "$RESOURCES_DIR/packaging/dsp-server-usb/payload/opt/neuracoust/dsp-server/bin/"*.sh \
  "$RESOURCES_DIR/packaging/dsp-server-usb/payload/usr/local/bin/neuracoust-"* \
  "$RESOURCES_DIR/packaging/dsp-server-usb/payload/usr/local/sbin/neuracoust-"*

mkdir -p "$RESOURCES_DIR/appliance-images"
LATEST_IMAGE=""
if [[ -f "$ROOT_DIR/dist/latest-dsp-server-appliance-image.txt" ]]; then
  CANDIDATE="$(tr -d '\r\n' < "$ROOT_DIR/dist/latest-dsp-server-appliance-image.txt")"
  if [[ -n "$CANDIDATE" && -f "$CANDIDATE" ]]; then
    LATEST_IMAGE="$CANDIDATE"
  elif [[ -n "$CANDIDATE" && -f "$ROOT_DIR/dist/$CANDIDATE" ]]; then
    LATEST_IMAGE="$ROOT_DIR/dist/$CANDIDATE"
  fi
fi
if [[ -z "$LATEST_IMAGE" ]]; then
  LATEST_IMAGE="$(find "$ROOT_DIR/dist" -maxdepth 1 -type f \( -iname '*dsp*.img' -o -iname '*dsp*.iso' -o -iname '*appliance*.img' -o -iname '*appliance*.iso' -o -iname '*neuracoust*.img' -o -iname '*neuracoust*.iso' \) -print0 2>/dev/null | xargs -0 ls -t 2>/dev/null | head -1 || true)"
fi
if [[ -n "$LATEST_IMAGE" && -f "$LATEST_IMAGE" ]]; then
  cp "$LATEST_IMAGE" "$RESOURCES_DIR/appliance-images/$(basename "$LATEST_IMAGE")"
  echo "$(basename "$LATEST_IMAGE")" > "$RESOURCES_DIR/appliance-images/latest-dsp-server-appliance-image.txt"
  echo "$LATEST_IMAGE" > "$ROOT_DIR/dist/latest-dsp-server-appliance-image.txt"
else
  cat > "$RESOURCES_DIR/appliance-images/README.txt" <<'README'
Place the latest Neuracoust DSP Server appliance .img or .iso here, or write
its absolute path to dist/latest-dsp-server-appliance-image.txt before building.
README
fi

if [[ "${NEURACOUST_SKIP_CODESIGN:-0}" != "1" ]]; then
  codesign --force --deep --sign - "$APP_DIR" >/dev/null
else
  rm -rf "$APP_DIR/Contents/_CodeSignature"
fi
xattr -cr "$APP_DIR" 2>/dev/null || true

mkdir -p "$ROOT_DIR/dist"
rm -rf "$ROOT_DIR/dist/Neuracoust DSP USB Maker.app"
ditto "$APP_DIR" "$ROOT_DIR/dist/Neuracoust DSP USB Maker.app"
xattr -cr "$ROOT_DIR/dist/Neuracoust DSP USB Maker.app" 2>/dev/null || true
ditto -c -k --keepParent "$APP_DIR" "$ROOT_DIR/dist/Neuracoust_DSP_USB_Maker_Mac_$VERSION.zip"
echo "$APP_DIR" > "$ROOT_DIR/dist/latest-dsp-usb-maker-app.txt"

cat <<EOF
Neuracoust DSP USB Maker built.
app: $APP_DIR
zip: $ROOT_DIR/dist/Neuracoust_DSP_USB_Maker_Mac_$VERSION.zip
EOF
