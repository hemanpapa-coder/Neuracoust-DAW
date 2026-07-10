#!/usr/bin/env zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -n "${NEURACOUST_VERSION:-}" ]]; then
  VERSION="$NEURACOUST_VERSION"
else
  RAW_VERSION="$(date '+%y%m%d.%H%M')"
  VERSION="${RAW_VERSION%?}0"
fi

BUILD_DIR="$ROOT_DIR/build/dsp-usb-maker-windows-$VERSION"
PACKAGE_DIR="$BUILD_DIR/Neuracoust DSP USB Maker Windows"
RESOURCES_DIR="$PACKAGE_DIR/appliance-images"
DIST_DIR="$ROOT_DIR/dist"
ZIP_PATH="$DIST_DIR/Neuracoust_DSP_USB_Maker_Windows_$VERSION.zip"

rm -rf "$BUILD_DIR"
mkdir -p "$RESOURCES_DIR" "$DIST_DIR"

cp "$ROOT_DIR/packaging/dsp-server-usb/windows/Write-NeuracoustDspUsb.ps1" "$PACKAGE_DIR/Write-NeuracoustDspUsb.ps1"
cp "$ROOT_DIR/packaging/dsp-server-usb/windows/Start-NeuracoustDspUsbMaker.cmd" "$PACKAGE_DIR/Start-NeuracoustDspUsbMaker.cmd"
cp "$ROOT_DIR/packaging/dsp-server-usb/windows/README-Windows.txt" "$PACKAGE_DIR/README-Windows.txt"
cp "$ROOT_DIR/packaging/dsp-server-usb/docs/pc-boot-test-checklist.md" "$PACKAGE_DIR/PC-Boot-Test-Checklist.md"
cp "$ROOT_DIR/packaging/dsp-server-usb/docs/current-server-extract-summary.md" "$PACKAGE_DIR/Current-Debian-DSP-Server-Extract-Summary.md"

LATEST_IMAGE=""
if [[ -f "$DIST_DIR/latest-dsp-server-appliance-image.txt" ]]; then
  CANDIDATE="$(tr -d '\r\n' < "$DIST_DIR/latest-dsp-server-appliance-image.txt")"
  if [[ -n "$CANDIDATE" && -f "$CANDIDATE" ]]; then
    LATEST_IMAGE="$CANDIDATE"
  elif [[ -n "$CANDIDATE" && -f "$DIST_DIR/$CANDIDATE" ]]; then
    LATEST_IMAGE="$DIST_DIR/$CANDIDATE"
  fi
fi
if [[ -z "$LATEST_IMAGE" ]]; then
  LATEST_IMAGE="$(find "$DIST_DIR" -maxdepth 1 -type f \( -iname '*dsp*.iso' -o -iname '*appliance*.iso' -o -iname '*neuracoust*.iso' \) -print0 2>/dev/null | xargs -0 ls -t 2>/dev/null | head -1 || true)"
fi
if [[ -z "$LATEST_IMAGE" || ! -f "$LATEST_IMAGE" ]]; then
  echo "No Debian appliance image found. Build it first with scripts/build-dsp-server-appliance-iso.command." >&2
  exit 1
fi

cp "$LATEST_IMAGE" "$RESOURCES_DIR/$(basename "$LATEST_IMAGE")"
if [[ -f "$LATEST_IMAGE.sha256" ]]; then
  cp "$LATEST_IMAGE.sha256" "$RESOURCES_DIR/$(basename "$LATEST_IMAGE").sha256"
else
  shasum -a 256 "$RESOURCES_DIR/$(basename "$LATEST_IMAGE")" > "$RESOURCES_DIR/$(basename "$LATEST_IMAGE").sha256"
fi
printf '%s\n' "$(basename "$LATEST_IMAGE")" > "$RESOURCES_DIR/latest-dsp-server-appliance-image.txt"

cat > "$PACKAGE_DIR/RELEASE.txt" <<EOF
Neuracoust DSP Server USB Maker for Windows
Version: $VERSION
Base image: $(basename "$LATEST_IMAGE")
Base OS: Debian 12 bookworm minimal live appliance
Kernel family: Debian PREEMPT_RT
Visibility: administrator-only Neuracoust utility release
Copyright (C) 2026 Neuracoust. All rights reserved.
EOF

rm -f "$ZIP_PATH"
(cd "$BUILD_DIR" && zip -qry "$ZIP_PATH" "Neuracoust DSP USB Maker Windows")
printf '%s\n' "$ZIP_PATH" > "$DIST_DIR/latest-dsp-usb-maker-windows-package.txt"

cat <<EOF
Neuracoust DSP USB Maker Windows package built.
zip: $ZIP_PATH
image: $LATEST_IMAGE
EOF
