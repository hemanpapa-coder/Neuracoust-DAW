#!/usr/bin/env zsh
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  NEURACOUST_CONFIRM_ERASE=ERASE scripts/make-dsp-server-usb-windows-remote.command \
    --host windows11-server \
    --disk-number 3 \
    --image /path/to/appliance.img

Writes a Neuracoust DSP Server appliance image to a USB drive attached to a
Windows machine reachable by SSH.
USAGE
}

host=""
disk_number=""
image=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      host="${2:-}"
      shift 2
      ;;
    --disk-number)
      disk_number="${2:-}"
      shift 2
      ;;
    --image|--img)
      image="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

[[ -n "$host" && -n "$disk_number" && -n "$image" ]] || { usage >&2; exit 2; }
[[ -f "$image" ]] || { echo "Image not found: $image" >&2; exit 1; }
[[ "${NEURACOUST_CONFIRM_ERASE:-}" == "ERASE" ]] || {
  echo "Refusing to erase a disk because NEURACOUST_CONFIRM_ERASE is not ERASE." >&2
  exit 1
}

remote_dir='C:\Neuracoust\DspUsbMaker'
remote_scp_dir='/C:/Neuracoust/DspUsbMaker'
remote_script="$remote_dir\\Write-NeuracoustDspUsb.ps1"
remote_image="$remote_dir\\$(basename "$image")"
remote_scp_script="$remote_scp_dir/Write-NeuracoustDspUsb.ps1"
remote_scp_image="$remote_scp_dir/$(basename "$image")"
local_script="packaging/dsp-server-usb/windows/Write-NeuracoustDspUsb.ps1"

ssh "$host" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"New-Item -ItemType Directory -Force '$remote_dir' | Out-Null\""
scp "$local_script" "$host:$remote_scp_script"
scp "$image" "$host:$remote_scp_image"

ssh "$host" "powershell -NoProfile -ExecutionPolicy Bypass -File '$remote_script' -DiskNumber $disk_number -ImagePath '$remote_image' -ConfirmErase ERASE"

echo "Remote Windows USB write complete on $host disk $disk_number."
