#!/usr/bin/env bash
set -euo pipefail

CONF_PATH="${NEURACOUST_INSTALL_CONF:-/etc/neuracoust-dsp-server/install.conf}"
if [[ -f "$CONF_PATH" ]]; then
  # shellcheck disable=SC1090
  source "$CONF_PATH"
fi

REPORT_DIR="${NEURACOUST_REPORT_DIR:-/var/log/neuracoust/dsp-server}"
REPORT_PATH="$REPORT_DIR/install-report.json"
MOUNT_ROOT="${NEURACOUST_INSTALL_MOUNT_ROOT:-/mnt/neuracoust-dsp-root}"
EFI_MOUNT="$MOUNT_ROOT/boot/efi"
REQUIRED_VALUE="${REQUIRED_ERASE_CONFIRMATION:-ERASE}"
mkdir -p "$REPORT_DIR"

fail_report() {
  local message="$1"
  cat > "$REPORT_PATH" <<JSON
{
  "product": "Neuracoust DSP Server",
  "status": "failed",
  "message": "$message",
  "generatedAt": "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
}
JSON
  echo "$message" >&2
  exit 1
}

[[ "${AUTO_INSTALL_TO_INTERNAL_SSD:-1}" == "1" ]] || {
  echo "AUTO_INSTALL_TO_INTERNAL_SSD is disabled."
  exit 0
}

[[ "${INSTALLED_TO_INTERNAL_SSD:-0}" != "1" ]] || {
  echo "Already marked as installed to internal SSD."
  exit 0
}

[[ "${NEURACOUST_CONFIRM_ERASE:-}" == "$REQUIRED_VALUE" ]] || \
  fail_report "Refusing to erase a disk because NEURACOUST_CONFIRM_ERASE is not set to $REQUIRED_VALUE."

MIN_TARGET_BYTES="${MIN_TARGET_BYTES:-32000000000}"

for required in lsblk findmnt sgdisk mkfs.vfat mkfs.ext4 rsync blkid mount umount udevadm awk; do
  command -v "$required" >/dev/null 2>&1 || fail_report "Required command missing: $required"
done

root_source="$(findmnt -no SOURCE /)"
root_pkname="$(lsblk -no PKNAME "$root_source" 2>/dev/null | head -1 || true)"
boot_disk=""
if [[ -n "$root_pkname" ]]; then
  boot_disk="/dev/$root_pkname"
fi

allowed_transports=" ${TARGET_TRANSPORTS:-nvme sata ata scsi} "
target_disk=""
candidate_log="$REPORT_DIR/install-candidate-disks.log"
: > "$candidate_log"
while read -r name type tran rm size_bytes hotplug model; do
  [[ "$type" == "disk" ]] || continue
  candidate="/dev/$name"
  id_bus="$(udevadm info --query=property --name "$candidate" 2>/dev/null | awk -F= '$1 == "ID_BUS" { print $2; exit }')"
  printf 'candidate=%s type=%s tran=%s rm=%s hotplug=%s sizeBytes=%s idBus=%s model=%s bootDisk=%s\n' \
    "$candidate" "$type" "${tran:-}" "$rm" "$hotplug" "${size_bytes:-0}" "${id_bus:-}" "${model:-}" "${boot_disk:-}" >> "$candidate_log"
  [[ "$rm" == "0" ]] || continue
  [[ "$hotplug" == "0" ]] || continue
  [[ "${id_bus:-}" != "usb" ]] || continue
  [[ "$candidate" != "$boot_disk" ]] || continue
  [[ "${size_bytes:-0}" =~ ^[0-9]+$ ]] || continue
  (( size_bytes >= MIN_TARGET_BYTES )) || continue
  [[ " $allowed_transports " == *" ${tran:-unknown} "* ]] || continue
  target_disk="$candidate"
  break
done < <(lsblk -bdn -o NAME,TYPE,TRAN,RM,SIZE,HOTPLUG,MODEL | sort)

[[ -n "$target_disk" ]] || fail_report "No eligible internal SSD/NVMe/SATA target disk was found."

echo "Installing Neuracoust DSP Server appliance to $target_disk"

swapoff -a || true
umount -R "$MOUNT_ROOT" 2>/dev/null || true

sgdisk --zap-all "$target_disk"
sgdisk -n 1:1MiB:+1024MiB -t 1:EF00 -c 1:"NEURACOUST-EFI" "$target_disk"
sgdisk -n 2:0:0 -t 2:8300 -c 2:"NEURACOUST-DSP" "$target_disk"
partprobe "$target_disk" || true
sleep 2

if [[ "$target_disk" == /dev/nvme* ]]; then
  efi_part="${target_disk}p1"
  root_part="${target_disk}p2"
else
  efi_part="${target_disk}1"
  root_part="${target_disk}2"
fi

mkfs.vfat -F32 -n NEURACOUST_EFI "$efi_part"
mkfs.ext4 -F -L NEURACOUST_DSP "$root_part"

mkdir -p "$MOUNT_ROOT" "$EFI_MOUNT"
mount "$root_part" "$MOUNT_ROOT"
mkdir -p "$EFI_MOUNT"
mount "$efi_part" "$EFI_MOUNT"

rsync -aAX --numeric-ids \
  --exclude=/dev/* \
  --exclude=/proc/* \
  --exclude=/sys/* \
  --exclude=/tmp/* \
  --exclude=/run/* \
  --exclude=/mnt/* \
  --exclude=/media/* \
  --exclude=/lost+found \
  / "$MOUNT_ROOT/"

mkdir -p "$MOUNT_ROOT"/{dev,proc,sys,tmp,run,mnt,media}
chmod 1777 "$MOUNT_ROOT/tmp"

root_uuid="$(blkid -s UUID -o value "$root_part")"
efi_uuid="$(blkid -s UUID -o value "$efi_part")"
cat > "$MOUNT_ROOT/etc/fstab" <<FSTAB
UUID=$root_uuid / ext4 defaults,noatime 0 1
UUID=$efi_uuid /boot/efi vfat umask=0077 0 1
FSTAB

if [[ -f "$MOUNT_ROOT/etc/neuracoust-dsp-server/install.conf" ]]; then
  sed -i 's/^INSTALLED_TO_INTERNAL_SSD=.*/INSTALLED_TO_INTERNAL_SSD=1/' "$MOUNT_ROOT/etc/neuracoust-dsp-server/install.conf"
fi

machine_suffix="$(cat /etc/machine-id 2>/dev/null | cut -c1-6 || printf 'node')"
echo "${HOSTNAME_PREFIX:-neuracoust-dsp}-$machine_suffix" > "$MOUNT_ROOT/etc/hostname"

for fs in dev proc sys run; do
  mount --bind "/$fs" "$MOUNT_ROOT/$fs"
done

if command -v grub-install >/dev/null 2>&1 && [[ -d "$MOUNT_ROOT/usr" ]]; then
  chroot "$MOUNT_ROOT" grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=NeuracoustDSP --recheck || \
    fail_report "grub-install failed for $target_disk"
  chroot "$MOUNT_ROOT" update-grub || true
elif command -v bootctl >/dev/null 2>&1; then
  chroot "$MOUNT_ROOT" bootctl install || fail_report "bootctl install failed for $target_disk"
else
  fail_report "No supported UEFI bootloader installer was found."
fi

systemctl --root="$MOUNT_ROOT" enable neuracoust-dsp-firstboot.service >/dev/null 2>&1 || true
systemctl --root="$MOUNT_ROOT" enable neuracoust-cpu-performance.service >/dev/null 2>&1 || true
systemctl --root="$MOUNT_ROOT" enable neuracoust-net-rt.service >/dev/null 2>&1 || true
systemctl --root="$MOUNT_ROOT" enable neuracoust-wol.service >/dev/null 2>&1 || true
systemctl --root="$MOUNT_ROOT" enable neuracoust-rt-dsp.service >/dev/null 2>&1 || true
systemctl --root="$MOUNT_ROOT" enable neuracoust-dsp-server.service >/dev/null 2>&1 || true
systemctl --root="$MOUNT_ROOT" enable neuracoust-dsp-update.timer >/dev/null 2>&1 || true

cat > "$REPORT_PATH" <<JSON
{
  "product": "Neuracoust DSP Server",
  "status": "installed",
  "targetDisk": "$target_disk",
  "efiPartition": "$efi_part",
  "rootPartition": "$root_part",
  "rootUuid": "$root_uuid",
  "efiUuid": "$efi_uuid",
  "generatedAt": "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
}
JSON

mkdir -p "$MOUNT_ROOT/var/log/neuracoust/dsp-server"
cp -f "$REPORT_PATH" "$MOUNT_ROOT/var/log/neuracoust/dsp-server/install-report.json"

sync
for fs in run sys proc dev; do
  umount "$MOUNT_ROOT/$fs" 2>/dev/null || true
done
umount "$EFI_MOUNT"
umount "$MOUNT_ROOT"

echo "Neuracoust DSP Server installed to $target_disk"
