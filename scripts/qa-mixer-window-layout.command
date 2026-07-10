#!/bin/zsh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build/dev}"
APP="$BUILD_DIR/Neuracoust DAW.app"
CAPTURE_DIR="${TMPDIR:-/tmp}"
CAPTURES=("$CAPTURE_DIR/neuracoust-mixer-window-normal-qa.png" "$CAPTURE_DIR/neuracoust-mixer-window-narrow-qa.png")

cmake --build "$BUILD_DIR" --target NeuracoustDAW -j6

capture_mode() {
  local mode="$1"
  local capture="$2"
  if [[ "$mode" == "narrow" ]]; then
    defaults write com.neuracoust.daw NeuracoustDAWMixNarrowStrips -bool true
  else
    defaults write com.neuracoust.daw NeuracoustDAWMixNarrowStrips -bool false
  fi
  while IFS= read -r pid; do
    if [[ -n "$pid" && "$pid" != "$$" ]]; then
      kill "$pid" 2>/dev/null || true
    fi
  done < <(pgrep -f "$APP/Contents/MacOS/Neuracoust DAW" || true)
  open -n "$APP"
  sleep 3
  osascript <<'APPLESCRIPT' || true
tell application "Neuracoust DAW" to activate
delay 0.4
tell application "System Events"
  tell process "Neuracoust DAW"
    set frontmost to true
    try
      click button "Mix" of window 1
    end try
  end tell
end tell
APPLESCRIPT
  sleep 1
  screencapture -x "$capture"
}

capture_mode normal "${CAPTURES[1]}"
capture_mode narrow "${CAPTURES[2]}"

python3 - "${CAPTURES[@]}" <<'PY'
import struct
import sys
import zlib

for path in sys.argv[1:]:
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"not a PNG: {path}")

    pos = 8
    width = height = bit = color = None
    raw = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        pos += 4
        chunk_type = data[pos:pos + 4]
        pos += 4
        chunk = data[pos:pos + length]
        pos += length + 4
        if chunk_type == b"IHDR":
            width, height, bit, color, _, _, _ = struct.unpack(">IIBBBBB", chunk)
        elif chunk_type == b"IDAT":
            raw += chunk
        elif chunk_type == b"IEND":
            break

    channels = {2: 3, 6: 4}.get(color)
    if bit != 8 or channels not in (3, 4):
        raise SystemExit(f"unsupported PNG format: bit={bit} color={color}")

    pixels = zlib.decompress(raw)
    stride = width * channels
    previous = [0] * stride
    total = 0
    count = 0
    nonzero = 0
    index = 0

    for _ in range(height):
        filter_type = pixels[index]
        index += 1
        row = list(pixels[index:index + stride])
        index += stride
        out = [0] * stride
        for column, byte in enumerate(row):
            left = out[column - channels] if column >= channels else 0
            up = previous[column]
            upper_left = previous[column - channels] if column >= channels else 0
            if filter_type == 0:
                value = byte
            elif filter_type == 1:
                value = (byte + left) & 255
            elif filter_type == 2:
                value = (byte + up) & 255
            elif filter_type == 3:
                value = (byte + ((left + up) // 2)) & 255
            elif filter_type == 4:
                predictor = left + up - upper_left
                choices = (abs(predictor - left), abs(predictor - up), abs(predictor - upper_left))
                predicted = left if choices[0] <= choices[1] and choices[0] <= choices[2] else (up if choices[1] <= choices[2] else upper_left)
                value = (byte + predicted) & 255
            else:
                raise SystemExit(f"unsupported PNG filter: {filter_type}")
            out[column] = value
        previous = out
        for column in range(0, stride, channels):
            r, g, b = out[column], out[column + 1], out[column + 2]
            total += r + g + b
            count += 3
            if r or g or b:
                nonzero += 1

    mean = total / max(1, count)
    coverage = nonzero / max(1, width * height)
    if mean < 8.0 or coverage < 0.15:
        raise SystemExit(f"capture looks blank: mean={mean:.2f} coverage={coverage:.3f}")
    print(f"mixer window capture OK: {path} {width}x{height} mean={mean:.2f} coverage={coverage:.3f}")
PY

echo "QA captures: ${CAPTURES[*]}"
