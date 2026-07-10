#!/usr/bin/env zsh
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  scripts/make-dsp-server-usb-macos.command --list
  NEURACOUST_CONFIRM_ERASE=ERASE scripts/make-dsp-server-usb-macos.command --disk /dev/diskN --image /path/to/appliance.img
  scripts/make-dsp-server-usb-macos.command --disk /dev/diskN --image /path/to/appliance.img --validate-only

Writes a Neuracoust DSP Server appliance image to an external/removable USB disk.
USAGE
}

if [[ "${1:-}" == "--list" ]]; then
  diskutil list external physical
  exit 0
fi

disk=""
image=""
validate_only=0
original_args=("$@")
while [[ $# -gt 0 ]]; do
  case "$1" in
    --disk)
      disk="${2:-}"
      shift 2
      ;;
    --image|--img)
      image="${2:-}"
      shift 2
      ;;
    --validate-only)
      validate_only=1
      shift
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

[[ -n "$disk" && -n "$image" ]] || { usage >&2; exit 2; }
[[ -f "$image" ]] || { echo "Image not found: $image" >&2; exit 1; }

log_file="${NEURACOUST_USB_LOG:-}"
if [[ -z "$log_file" ]]; then
  log_dir="$HOME/Library/Logs/Neuracoust/DSPUSBMaker"
  mkdir -p "$log_dir"
  log_file="$log_dir/write-$(date '+%Y%m%d-%H%M%S').log"
fi
mkdir -p "$(dirname "$log_file")"
exec > >(tee -a "$log_file") 2>&1

echo "===== Neuracoust DSP USB write log ====="
echo "Started: $(date '+%Y-%m-%d %H:%M:%S %z')"
echo "Log file: $log_file"
echo "Command: $0 ${original_args[*]}"
echo "User: $(id)"
echo "macOS: $(sw_vers -productVersion 2>/dev/null || true) ($(sw_vers -buildVersion 2>/dev/null || true))"
echo "Kernel: $(uname -a)"
echo "Image: $image"
echo "Image bytes: $(/usr/bin/stat -f '%z' "$image" 2>/dev/null || true)"
echo "Target disk: $disk"

disk="${disk%/}"
[[ "$disk" == /dev/disk* ]] || { echo "Disk must look like /dev/diskN" >&2; exit 1; }
[[ "$disk" != "/dev/disk0" ]] || { echo "Refusing to write /dev/disk0." >&2; exit 1; }

plist="$(diskutil info -plist "$disk")"
bus_protocol="$(printf '%s' "$plist" | plutil -extract BusProtocol raw - 2>/dev/null || true)"
internal="$(printf '%s' "$plist" | plutil -extract Internal raw - 2>/dev/null || true)"
os_internal="$(printf '%s' "$plist" | plutil -extract OSInternalMedia raw - 2>/dev/null || true)"
removable="$(printf '%s' "$plist" | plutil -extract RemovableMediaOrExternalDevice raw - 2>/dev/null || true)"
whole_disk="$(printf '%s' "$plist" | plutil -extract WholeDisk raw - 2>/dev/null || true)"
system_image="$(printf '%s' "$plist" | plutil -extract SystemImage raw - 2>/dev/null || true)"
writable="$(printf '%s' "$plist" | plutil -extract WritableMedia raw - 2>/dev/null || true)"
media_name="$(printf '%s' "$plist" | plutil -extract MediaName raw - 2>/dev/null || true)"
total_size="$(printf '%s' "$plist" | plutil -extract TotalSize raw - 2>/dev/null || true)"

echo "--- diskutil list external physical ---"
/usr/sbin/diskutil list external physical || true
echo "--- diskutil info $disk ---"
/usr/sbin/diskutil info "$disk" || true
echo "--- device nodes ---"
/bin/ls -l "$disk" "/dev/r${disk#/dev/}" 2>/dev/null || true
echo "--- parsed target facts ---"
echo "BusProtocol=$bus_protocol"
echo "Internal=$internal"
echo "OSInternalMedia=$os_internal"
echo "RemovableMediaOrExternalDevice=$removable"
echo "WholeDisk=$whole_disk"
echo "SystemImage=$system_image"
echo "WritableMedia=$writable"
echo "TotalSize=$total_size"
echo "MediaName=$media_name"

[[ "$whole_disk" == "true" ]] || {
  echo "Refusing to write $disk because it is not a whole disk." >&2
  exit 1
}
[[ "$bus_protocol" == "USB" ]] || {
  echo "Refusing to write $disk because it is not reported as USB." >&2
  exit 1
}
[[ "$internal" != "true" && "$os_internal" != "true" && "$system_image" != "true" ]] || {
  echo "Refusing to write $disk because it is an internal/system disk." >&2
  exit 1
}
[[ "$writable" == "true" ]] || {
  echo "Refusing to write $disk because it is not writable." >&2
  exit 1
}
[[ "$total_size" == <-> ]] || {
  echo "Refusing to write $disk because its size could not be read." >&2
  exit 1
}
(( total_size >= 4000000000 && total_size <= 32000000000 )) || {
  echo "Refusing to write $disk because its size is outside the supported 4 GB to 32 GB USB range." >&2
  exit 1
}

raw_disk="/dev/r${disk#/dev/}"
block_disk="$disk"
image_bytes="$(/usr/bin/stat -f '%z' "$image" 2>/dev/null || echo 0)"
echo "Validated target: $disk ${media_name:-USB media} ($bus_protocol, removable=$removable, bytes=$total_size)"
if [[ "$validate_only" == "1" ]]; then
  echo "Validation only; no write was performed."
  exit 0
fi

[[ "${NEURACOUST_CONFIRM_ERASE:-}" == "ERASE" ]] || {
  echo "Refusing to erase a disk because NEURACOUST_CONFIRM_ERASE is not ERASE." >&2
  exit 1
}

echo "Writing $image to $disk"
echo "This will erase every partition on $disk."

if [[ "$(id -u)" != "0" ]]; then
  echo "This writer must run as root. Re-run from the app and approve the macOS administrator prompt." >&2
  exit 1
fi

echo "Writer privilege check: running as root."

prepare_file_copy_fallback() {
  echo "--- file-copy fallback: format USB as single MBR/FAT32 ---"
  echo "Using a single FAT32 partition so Samsung/strict UEFI firmware can find /EFI/BOOT/BOOTX64.EFI."
  /usr/sbin/diskutil eraseDisk FAT32 NEURACOUST MBRFormat "$disk"

  local console_user
  console_user="$(/usr/bin/stat -f '%Su' /dev/console 2>/dev/null || true)"
  if [[ -z "$console_user" || "$console_user" == "root" ]]; then
    console_user="${SUDO_USER:-}"
  fi
  if [[ -z "$console_user" || "$console_user" == "root" ]]; then
    echo "Could not determine logged-in user for FAT32 file copy." >&2
    exit 1
  fi
  echo "Preparing ISO file-copy fallback for logged-in user: $console_user"

  local partition=""
  for _ in {1..20}; do
    partition="$(/usr/sbin/diskutil list -plist "$disk" 2>/dev/null | /usr/bin/plutil -extract AllDisksAndPartitions.0.Partitions raw - 2>/dev/null | /usr/bin/awk -F'DeviceIdentifier = |;' '/DeviceIdentifier = disk/ { id=$2 } /VolumeName = NEURACOUST/ { print id; exit }' || true)"
    if [[ -z "$partition" ]]; then
      partition="$(/usr/sbin/diskutil list "$disk" | /usr/bin/awk '/NEURACOUST/ { print $NF; exit }')"
    fi
    [[ -n "$partition" ]] && break
    /bin/sleep 1
  done

  if [[ -z "$partition" ]]; then
    echo "Could not find NEURACOUST FAT32 partition after formatting." >&2
    exit 1
  fi

  echo "--- unmount FAT32 volume for app/user remount ---"
  /usr/sbin/diskutil unmount "/dev/$partition" >/dev/null 2>&1 || true
  /usr/sbin/chown "$console_user":staff "$log_file" 2>/dev/null || true
  /bin/chmod 664 "$log_file" 2>/dev/null || true
  echo "NEURACOUST_COPY_FALLBACK_READY partition=/dev/$partition image=$image total_bytes=$image_bytes"
}

echo "--- unmount target ---"
/usr/sbin/diskutil unmountDisk force "$disk"
echo "--- write with dd ---"
if ! /bin/dd if="$image" of="$raw_disk" bs=4m conv=sync status=progress; then
  echo "Direct raw write to $raw_disk failed. Retrying block device $block_disk..." >&2
  /usr/sbin/diskutil unmountDisk force "$disk" || true
  echo "--- write with dd using block device ---"
  if ! /bin/dd if="$image" of="$block_disk" bs=4m conv=sync status=progress; then
    echo "Block device write also failed." >&2
    if [[ "${image:l}" == *.dmg ]]; then
      echo "Image looks like a DMG. Retrying with Apple Software Restore..." >&2
      /usr/sbin/diskutil unmountDisk force "$disk" || true
      echo "--- write with asr ---"
      /usr/sbin/asr restore --source "$image" --target "$disk" --erase --noprompt --noverify
    else
      echo "Skipping Apple Software Restore because this image is not a DMG restore image." >&2
      prepare_file_copy_fallback
      exit 0
    fi
  fi
fi
echo "--- sync/eject ---"
/usr/sbin/diskutil eject "$disk"

echo "Neuracoust DSP Server USB write complete and ejected."
