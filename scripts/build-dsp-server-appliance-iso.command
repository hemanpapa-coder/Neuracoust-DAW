#!/usr/bin/env zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${NEURACOUST_VERSION:-$(date '+%y%m%d.%H%M' | sed 's/.$/0/')}"
DIST_DIR="$ROOT_DIR/dist"
WORK_DIR="$ROOT_DIR/.codex_tmp/dsp-server-debian-live-$VERSION"
PAYLOAD_DIR="$ROOT_DIR/packaging/dsp-server-usb/payload"
SUITE="${NEURACOUST_DEBIAN_SUITE:-bookworm}"
ARCH="${NEURACOUST_DEBIAN_ARCH:-amd64}"
IMAGE_NAME="Neuracoust_DSP_Server_Debian_${SUITE}_Appliance_$VERSION.iso"
OUT_ISO="$DIST_DIR/$IMAGE_NAME"

mkdir -p "$DIST_DIR"

if ! docker info >/dev/null 2>&1; then
  echo "Docker Desktop is required to build the Debian live appliance." >&2
  exit 1
fi

case "$SUITE" in
  bookworm|trixie) ;;
  *)
    echo "Unsupported NEURACOUST_DEBIAN_SUITE=$SUITE. Use bookworm or trixie." >&2
    exit 2
    ;;
esac

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/live" "$WORK_DIR/payload"
rsync -a "$PAYLOAD_DIR"/ "$WORK_DIR/payload"/

cat > "$WORK_DIR/build-in-container.sh" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail

suite="${NEURACOUST_DEBIAN_SUITE:-bookworm}"
arch="${NEURACOUST_DEBIAN_ARCH:-amd64}"
version="${NEURACOUST_VERSION:?}"

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates live-build rsync xorriso isolinux syslinux-common debootstrap cpio

rm -rf /build/live
mkdir -p /build/live
cd /build/live
lb clean --purge || true

lb config \
  --mode debian \
  --distribution "$suite" \
  --architectures "$arch" \
  --archive-areas "main contrib non-free non-free-firmware" \
  --binary-images iso-hybrid \
  --bootappend-live "boot=live components hostname=neuracoust-dsp username=neuracoust quiet" \
  --debian-installer none \
  --firmware-binary false \
  --firmware-chroot false \
  --linux-flavours "rt-amd64" \
  --iso-application "Neuracoust DSP Server Debian Appliance" \
  --iso-publisher "Neuracoust" \
  --iso-volume "NEURACOUST_DSP"

mkdir -p config/package-lists config/includes.chroot config/includes.binary/boot/grub
cat > config/package-lists/neuracoust-dsp.list.chroot <<'PACKAGES'
linux-image-rt-amd64
systemd
systemd-sysv
udev
dbus
openssh-server
network-manager
sudo
curl
rsync
ca-certificates
ethtool
iproute2
procps
psmisc
util-linux
gdisk
parted
dosfstools
e2fsprogs
grub-efi-amd64
grub-efi-amd64-bin
grub2-common
shim-signed
cpufrequtils
linux-cpupower
rtirq-init
irqbalance
pciutils
usbutils
dmidecode
lm-sensors
smartmontools
alsa-utils
jq
python3-minimal
firmware-linux-free
firmware-realtek
PACKAGES

rsync -a /work/payload/ config/includes.chroot/

cat > config/includes.chroot/etc/hostname <<'HOSTNAME'
neuracoust-dsp
HOSTNAME

mkdir -p config/includes.chroot/etc/NetworkManager/conf.d
cat > config/includes.chroot/etc/NetworkManager/conf.d/10-neuracoust-managed.conf <<'NM'
[main]
plugins=keyfile

[ifupdown]
managed=true
NM

mkdir -p config/includes.chroot/etc/systemd/system/getty@tty1.service.d
cat > config/includes.chroot/etc/systemd/system/getty@tty1.service.d/override.conf <<'GETTY'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear %I $TERM
Type=idle
GETTY

mkdir -p config/hooks/normal
cat > config/hooks/normal/0900-neuracoust-enable-services.hook.chroot <<'HOOK'
#!/bin/sh
set -e
chmod +x /opt/neuracoust/dsp-server/bin/*.sh || true
chmod +x /usr/local/bin/neuracoust-* || true
chmod +x /usr/local/sbin/neuracoust-* || true
systemctl enable NetworkManager.service
systemctl enable ssh.service
systemctl enable neuracoust-dsp-firstboot.service
systemctl enable neuracoust-cpu-performance.service
systemctl enable neuracoust-net-rt.service
systemctl enable neuracoust-wol.service
systemctl enable neuracoust-rt-dsp.service
systemctl enable neuracoust-dsp-server.service
systemctl enable neuracoust-dsp-update.timer
systemctl set-default multi-user.target
HOOK
chmod +x config/hooks/normal/0900-neuracoust-enable-services.hook.chroot

cat > config/includes.binary/NEURACOUST-USB-CHECKLIST.txt <<CHECKLIST
Neuracoust DSP Server Debian minimal live appliance
Version: ${version}
Base: Debian ${suite}

UEFI boot checklist:
1. PC firmware boot mode is UEFI.
2. Secure Boot is off unless shim/grub signing is explicitly validated.
3. Boot menu shows the USB as UEFI media.
4. Linux boot reaches multi-user target without installer screens.
5. systemctl is-active neuracoust-rt-dsp.service returns active.
6. UDP status port 20001 responds from the DAW/server discovery panel.
7. /var/log/neuracoust/dsp-server/hardware.json exists.
8. /var/log/neuracoust/dsp-server/install-candidate-disks.log contains only internal candidates.
9. If an internal SSD was eligible, /var/log/neuracoust/dsp-server/install-report.json reports installed.
10. After reboot without USB, the same DSP service is active from SSD.
CHECKLIST

lb build
cp live-image-"$arch".hybrid.iso "/work/out.iso"
SCRIPT
chmod +x "$WORK_DIR/build-in-container.sh"

docker run --rm --privileged --platform linux/amd64 \
  -e "NEURACOUST_DEBIAN_SUITE=$SUITE" \
  -e "NEURACOUST_DEBIAN_ARCH=$ARCH" \
  -e "NEURACOUST_VERSION=$VERSION" \
  -v "$WORK_DIR:/work" \
  debian:bookworm \
  /work/build-in-container.sh

cp "$WORK_DIR/out.iso" "$OUT_ISO"
shasum -a 256 "$OUT_ISO" > "$OUT_ISO.sha256"
printf '%s\n' "$OUT_ISO" > "$DIST_DIR/latest-dsp-server-appliance-image.txt"

cat > "$DIST_DIR/latest-dsp-server-appliance-image.json" <<JSON
{
  "product": "Neuracoust DSP Server",
  "version": "$VERSION",
  "baseOs": "Debian $SUITE",
  "architecture": "$ARCH",
  "image": "$OUT_ISO",
  "sha256File": "$OUT_ISO.sha256",
  "kernelPackage": "linux-image-rt-amd64",
  "runtime": "/opt/neuracoust/rt_engine/neuracoust-rt-engine",
  "module": "/opt/neuracoust/rt_engine/modules/na_4001e.so"
}
JSON

cat <<EOF
Neuracoust DSP Server Debian appliance ISO built.
base: Debian $SUITE ($ARCH)
iso: $OUT_ISO
sha256: $OUT_ISO.sha256
EOF
