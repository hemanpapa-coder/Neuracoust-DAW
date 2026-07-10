#!/usr/bin/env bash
set -euo pipefail

REPORT_DIR="${NEURACOUST_REPORT_DIR:-/var/log/neuracoust/dsp-server}"
REPORT_PATH="$REPORT_DIR/hardware.json"
mkdir -p "$REPORT_DIR"

json_escape() {
  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'
  else
    awk 'BEGIN { printf "\"" } { gsub(/\\/,"\\\\"); gsub(/"/,"\\\""); gsub(/\r/,"\\r"); printf "%s\\n", $0 } END { printf "\"" }'
  fi
}

command_text() {
  local cmd="$1"
  if command -v "${cmd%% *}" >/dev/null 2>&1; then
    sh -c "$cmd" 2>&1 || true
  else
    printf 'command not found: %s\n' "${cmd%% *}"
  fi
}

cpu_json="$(command_text 'lscpu')"
mem_json="$(command_text 'free -m')"
disk_json="$(command_text 'lsblk -J -o NAME,TYPE,SIZE,MODEL,SERIAL,TRAN,RM,MOUNTPOINTS')"
pci_json="$(command_text 'lspci -nn')"
net_json="$(command_text 'ip -j address')"
dmi_json="$(command_text 'dmidecode -t system -t baseboard -t processor -t memory')"
temp_json="$(command_text 'sensors')"

cat > "$REPORT_PATH" <<JSON
{
  "product": "Neuracoust DSP Server",
  "generatedAt": "$(date -u '+%Y-%m-%dT%H:%M:%SZ')",
  "hostname": "$(hostname)",
  "kernel": "$(uname -a | sed 's/"/\\"/g')",
  "cpuText": $(printf '%s' "$cpu_json" | json_escape),
  "memoryText": $(printf '%s' "$mem_json" | json_escape),
  "blockDevicesJson": $(printf '%s' "$disk_json" | json_escape),
  "pciText": $(printf '%s' "$pci_json" | json_escape),
  "networkJson": $(printf '%s' "$net_json" | json_escape),
  "dmiText": $(printf '%s' "$dmi_json" | json_escape),
  "temperatureText": $(printf '%s' "$temp_json" | json_escape)
}
JSON

chmod 0644 "$REPORT_PATH"
printf '%s\n' "$REPORT_PATH"
