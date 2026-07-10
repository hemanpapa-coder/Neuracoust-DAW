#!/usr/bin/env bash
set -euo pipefail

CONF_PATH="${NEURACOUST_UPDATE_CONF:-/etc/neuracoust-dsp-server/update.conf}"
if [[ -f "$CONF_PATH" ]]; then
  # shellcheck disable=SC1090
  source "$CONF_PATH"
fi

STATE_DIR="${UPDATE_STATE_DIR:-/var/lib/neuracoust/dsp-server/update}"
LOG_DIR="${UPDATE_LOG_DIR:-/var/log/neuracoust/dsp-server}"
CURRENT_VERSION_FILE="${CURRENT_VERSION_FILE:-/etc/neuracoust-dsp-server/version}"
MANIFEST_URL="${UPDATE_MANIFEST_URL:-}"
AUTO_APPLY="${AUTO_APPLY_UPDATES:-0}"
UPDATE_CONTROLLER="${UPDATE_CONTROLLER:-self-scheduled}"
mkdir -p "$STATE_DIR" "$LOG_DIR"

log() {
  printf '%s %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*" | tee -a "$LOG_DIR/update.log" >&2
}

usage() {
  cat <<'USAGE'
Usage:
  neuracoust-dsp-update.sh --check
  neuracoust-dsp-update.sh --check --manifest-url URL
  neuracoust-dsp-update.sh --scheduled
  neuracoust-dsp-update.sh --package-url URL --sha256 HASH
  neuracoust-dsp-update.sh --apply /path/to/firmware.tar.gz --sha256 HASH

Firmware package format:
  firmware.tar.gz
  ├─ manifest.json
  └─ install.sh
USAGE
}

json_field() {
  local file="$1"
  local key="$2"
  python3 - "$file" "$key" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    data = json.load(fh)
value = data
for part in sys.argv[2].split("."):
    value = value.get(part, "") if isinstance(value, dict) else ""
print(value if value is not None else "")
PY
}

current_version() {
  if [[ -f "$CURRENT_VERSION_FILE" ]]; then
    tr -d ' \n\r\t' < "$CURRENT_VERSION_FILE"
  else
    printf '0'
  fi
}

download_manifest() {
  [[ -n "$MANIFEST_URL" ]] || {
    log "No UPDATE_MANIFEST_URL configured."
    return 1
  }
  command -v curl >/dev/null 2>&1 || {
    log "curl is required for update checks."
    return 1
  }
  curl -fsSL "$MANIFEST_URL" -o "$STATE_DIR/latest.json"
  log "Downloaded update manifest from $MANIFEST_URL"
}

report_available() {
  local manifest="$STATE_DIR/latest.json"
  [[ -f "$manifest" ]] || return 1
  local available package_url sha
  available="$(json_field "$manifest" version)"
  package_url="$(json_field "$manifest" packageUrl)"
  sha="$(json_field "$manifest" sha256)"
  cat > "$LOG_DIR/update-status.json" <<JSON
{
  "product": "Neuracoust DSP Server",
  "channel": "${UPDATE_CHANNEL:-stable}",
  "controller": "$UPDATE_CONTROLLER",
  "currentVersion": "$(current_version)",
  "availableVersion": "$available",
  "packageUrl": "$package_url",
  "sha256": "$sha",
  "checkedAt": "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
}
JSON
  log "Current version $(current_version), available version ${available:-unknown}"
}

download_package_from_manifest() {
  local manifest="$STATE_DIR/latest.json"
  local package_url sha package_path
  package_url="$(json_field "$manifest" packageUrl)"
  sha="$(json_field "$manifest" sha256)"
  [[ -n "$package_url" ]] || {
    log "Manifest has no packageUrl."
    return 1
  }
  package_path="$STATE_DIR/$(basename "$package_url")"
  curl -fsSL "$package_url" -o "$package_path"
  verify_sha256 "$package_path" "$sha"
  printf '%s\n' "$package_path"
}

download_package_url() {
  local package_url="$1"
  local expected_sha="$2"
  [[ -n "$package_url" ]] || {
    log "No package URL provided."
    return 1
  }
  command -v curl >/dev/null 2>&1 || {
    log "curl is required for package URL updates."
    return 1
  }
  local package_path="$STATE_DIR/$(basename "$package_url")"
  curl -fsSL "$package_url" -o "$package_path"
  verify_sha256 "$package_path" "$expected_sha"
  printf '%s\n' "$package_path"
}

verify_sha256() {
  local package="$1"
  local expected="$2"
  [[ -n "$expected" ]] || {
    log "No sha256 provided for $package."
    return 1
  }
  local actual
  actual="$(sha256sum "$package" | awk '{print $1}')"
  [[ "$actual" == "$expected" ]] || {
    log "Checksum mismatch for $package"
    log "expected=$expected actual=$actual"
    return 1
  }
}

apply_package() {
  local package="$1"
  local expected_sha="${2:-}"
  [[ -f "$package" ]] || {
    log "Package not found: $package"
    return 1
  }
  if [[ -n "$expected_sha" ]]; then
    verify_sha256 "$package" "$expected_sha"
  fi

  local stage="$STATE_DIR/stage"
  rm -rf "$stage"
  mkdir -p "$stage"
  tar -xzf "$package" -C "$stage"
  [[ -f "$stage/manifest.json" ]] || {
    log "Package is missing manifest.json"
    return 1
  }
  [[ -x "$stage/install.sh" ]] || chmod +x "$stage/install.sh" 2>/dev/null || true
  [[ -x "$stage/install.sh" ]] || {
    log "Package is missing executable install.sh"
    return 1
  }

  local new_version
  new_version="$(json_field "$stage/manifest.json" version)"
  log "Applying Neuracoust DSP Server firmware package ${new_version:-unknown}"
  NEURACOUST_DSP_UPDATE_STAGE="$stage" "$stage/install.sh"
  if [[ -n "$new_version" ]]; then
    printf '%s\n' "$new_version" > "$CURRENT_VERSION_FILE"
  fi
  systemctl restart neuracoust-dsp-server.service >/dev/null 2>&1 || true
  log "Firmware update applied: ${new_version:-unknown}"
}

mode=""
apply_path=""
apply_sha=""
package_url=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --check)
      mode="check"
      shift
      ;;
    --scheduled)
      mode="scheduled"
      shift
      ;;
    --manifest-url)
      MANIFEST_URL="${2:-}"
      shift 2
      ;;
    --controller)
      UPDATE_CONTROLLER="${2:-}"
      shift 2
      ;;
    --package-url)
      mode="package-url"
      package_url="${2:-}"
      shift 2
      ;;
    --apply)
      mode="apply"
      apply_path="${2:-}"
      shift 2
      ;;
    --sha256)
      apply_sha="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

case "$mode" in
  check)
    download_manifest
    report_available
    ;;
  scheduled)
    download_manifest || exit 0
    report_available || exit 0
    if [[ "$AUTO_APPLY" == "1" ]]; then
      package_path="$(download_package_from_manifest)"
      apply_package "$package_path"
    fi
    ;;
  apply)
    [[ -n "$apply_path" ]] || { usage >&2; exit 2; }
    apply_package "$apply_path" "$apply_sha"
    ;;
  package-url)
    [[ -n "$package_url" ]] || { usage >&2; exit 2; }
    package_path="$(download_package_url "$package_url" "$apply_sha")"
    apply_package "$package_path"
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
