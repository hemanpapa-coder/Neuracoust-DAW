#!/usr/bin/env bash
# Build a USB that turns a bare PC into a Neuracoust NDS appliance with no keyboard attached.
#
#   plug it in, set the BIOS to boot USB, walk away.
#
# It remasters the official Debian 12 netinst image: the preseed goes into the installer's
# initrd (so it is read before any question can be asked) AND onto the medium, both boot menus
# are rewritten to start immediately with no prompt, and the engine sources ride along so the
# node builds itself on first boot without needing the network a second time.
#
#   tools/node/usb/make-appliance-usb.sh                    # build the ISO
#   tools/node/usb/make-appliance-usb.sh --write /dev/disk4 # build, then write to a USB
#   tools/node/usb/make-appliance-usb.sh --list             # show removable disks
#
# Writing needs sudo — the script never handles a password itself, it hands the dd command to
# your shell so you type it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DW_ROOT="$(cd "$HERE/../../.." && pwd)"
RT_SRC="/Volumes/Program Dev/Linux DSP Server/rt_engine"
WORK="${TMPDIR:-/tmp}/nds-usb-build"
OUT_ISO="$HERE/dist/neuracoust-nds-installer.iso"
MIRROR="https://cdimage.debian.org/debian-cd/current/amd64/iso-cd"
SSH_KEY="${NDS_SSH_KEY:-$HOME/.ssh/id_ed25519.pub}"
WRITE_DEV=""

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
die()  { printf '\033[31m오류: %s\033[0m\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --write) WRITE_DEV="${2:-}"; shift 2 ;;
        --ssh-key) SSH_KEY="${2:-}"; shift 2 ;;
        --list)
            if [ "$(uname)" = "Darwin" ]; then diskutil list external physical; else lsblk -d -o NAME,SIZE,RM,MODEL; fi
            exit 0 ;;
        --help|-h) sed -n '2,20p' "$0"; exit 0 ;;
        *) die "알 수 없는 옵션: $1" ;;
    esac
done

command -v xorriso >/dev/null || die "xorriso 가 필요합니다:  brew install xorriso"
[ -f "$SSH_KEY" ] || die "ssh 공개키가 없습니다: $SSH_KEY  (--ssh-key 로 지정하세요)"
[ -d "$RT_SRC" ] || die "rt-engine 소스가 없습니다: $RT_SRC"

rm -rf "$WORK"; mkdir -p "$WORK/nds" "$WORK/payload" "$(dirname "$OUT_ISO")"

### 1. The Debian image ---------------------------------------------------------------------
say "1/6 Debian 12 netinst 이미지"
CACHE="$HERE/dist/debian-netinst.iso"
if [ -f "$CACHE" ]; then
    info "이미 받아둔 이미지를 씁니다: $(du -h "$CACHE" | cut -f1)"
else
    info "체크섬 목록 조회…"
    SUMS="$(curl -fsSL "$MIRROR/SHA256SUMS")" || die "미러에 연결할 수 없습니다: $MIRROR"
    ISO_NAME="$(printf '%s\n' "$SUMS" | awk '/netinst\.iso$/ {print $2; exit}')"
    ISO_SHA="$(printf '%s\n' "$SUMS" | awk '/netinst\.iso$/ {print $1; exit}')"
    [ -n "$ISO_NAME" ] || die "미러에서 netinst 이미지를 찾지 못했습니다"
    info "$ISO_NAME 내려받는 중 (약 650 MB)…"
    curl -fL --progress-bar -o "$CACHE.part" "$MIRROR/$ISO_NAME" || die "다운로드 실패"
    info "체크섬 검증…"
    GOT="$(shasum -a 256 "$CACHE.part" | awk '{print $1}')"
    [ "$GOT" = "$ISO_SHA" ] || die "체크섬 불일치 — 받은 파일을 버립니다"
    mv "$CACHE.part" "$CACHE"
    info "검증 완료: $ISO_NAME"
fi

### 2. The payload --------------------------------------------------------------------------
# Sources only, never binaries: the node compiles for its own CPU, and a macOS object would
# not load there anyway. Same file layout the DAW's update script uses, so a node built by
# this USB and a node updated over the network end up identical.
say "2/6 엔진 소스 꾸리기"
P="$WORK/payload"
mkdir -p "$P/node-module/audio" "$P/node-module/core" "$P/node-module/license" "$P/node-module/plugins"
rsync -a --exclude 'build/' --exclude 'build-macos-verify/' "$RT_SRC/" "$P/rt_engine/"
cp "$DW_ROOT/tools/node/na_console_channel.cpp" "$DW_ROOT/tools/node/na_api525a.cpp" \
   "$DW_ROOT/tools/node/na_rt_plugin.h" "$P/node-module/"
cp "$DW_ROOT/src/audio/ConsoleChannelProcessor.cpp" "$DW_ROOT/src/audio/ConsoleChannelProcessor.h" \
   "$DW_ROOT/src/audio/AudioDeviceModel.h" "$DW_ROOT/src/audio/RemoteDspServerClient.h" "$P/node-module/audio/"
cp "$DW_ROOT/src/core/DawState.h" "$DW_ROOT/src/core/AppIdentity.h" "$P/node-module/core/"
cp "$DW_ROOT/src/license/LicenseAgentClient.h" "$P/node-module/license/"
cp "$DW_ROOT/src/plugins/Vst3HostFoundation.h" "$P/node-module/plugins/"
cp "$HERE/run-engine.sh" "$P/run-engine.sh"
chmod +x "$P/run-engine.sh"
tar -czf "$WORK/nds/payload.tar.gz" -C "$P" .
info "payload.tar.gz  $(du -h "$WORK/nds/payload.tar.gz" | cut -f1)"

cp "$HERE/preseed.cfg" "$WORK/nds/preseed.cfg"
cp "$HERE/firstboot.sh" "$WORK/nds/firstboot.sh"
cp "$HERE/nds-firstboot.service" "$WORK/nds/nds-firstboot.service"
cp "$SSH_KEY" "$WORK/nds/authorized_keys"
info "관리 키: $(awk '{print $1, $3}' "$SSH_KEY")"

### 3. Preseed into the installer initrd ----------------------------------------------------
# The kernel concatenates initramfs archives, so a second cpio appended to initrd.gz adds
# /preseed.cfg without unpacking (and risking) the original. d-i reads /preseed.cfg from the
# initrd root before it can ask anything — including the questions that come before the medium
# is even mounted.
say "3/6 인스톨러 initrd 에 preseed 심기"
xorriso -osirrox on -indev "$CACHE" -extract /install.amd/initrd.gz "$WORK/initrd.orig.gz" 2>/dev/null
(cd "$WORK" && cp nds/preseed.cfg preseed.cfg && echo preseed.cfg | cpio -o -H newc -R 0:0 2>/dev/null | gzip -9 > extra.cpio.gz)
cat "$WORK/initrd.orig.gz" "$WORK/extra.cpio.gz" > "$WORK/initrd.gz"
info "initrd  $(du -h "$WORK/initrd.orig.gz" | cut -f1) → $(du -h "$WORK/initrd.gz" | cut -f1)"

### 4. Boot menus that do not wait ----------------------------------------------------------
# This is the difference between "boots the installer" and "installs by itself". Both menus
# have to be handled: an old PC in CSM mode reads isolinux, a 7th-gen board in UEFI mode reads
# grub, and the stock grub.cfg has a 5-second menu with the FIRST entry being a manual install.
say "4/6 부팅 메뉴 자동화 (BIOS · UEFI 양쪽)"
cat > "$WORK/isolinux.cfg" << 'ISOCFG'
default nds
prompt 0
timeout 1

label nds
    kernel /install.amd/vmlinuz
    append initrd=/install.amd/initrd.gz auto=true priority=critical preseed/file=/cdrom/nds/preseed.cfg quiet ---
ISOCFG

cat > "$WORK/grub.cfg" << 'GRUBCFG'
set default=0
set timeout=0
set timeout_style=hidden

menuentry "Neuracoust NDS 자동 설치" {
    linux /install.amd/vmlinuz auto=true priority=critical preseed/file=/cdrom/nds/preseed.cfg quiet ---
    initrd /install.amd/initrd.gz
}
GRUBCFG

### 5. Remaster ------------------------------------------------------------------------------
# "-boot_image any replay" carries the original hybrid MBR, El Torito catalog and EFI boot
# image across unchanged. Rebuilding those by hand is where home-made installer USBs usually
# stop booting on UEFI machines.
say "5/6 ISO 재조립"
rm -f "$OUT_ISO"
xorriso -indev "$CACHE" -outdev "$OUT_ISO" \
    -boot_image any replay \
    -volid NDSINSTALL \
    -map "$WORK/nds" /nds \
    -map "$WORK/isolinux.cfg" /isolinux/isolinux.cfg \
    -map "$WORK/grub.cfg" /boot/grub/grub.cfg \
    -map "$WORK/initrd.gz" /install.amd/initrd.gz \
    -compliance no_emul_toc 2>&1 | grep -Ev '^(xorriso : UPDATE|Drive current|Media )' || true

[ -f "$OUT_ISO" ] || die "ISO 생성 실패"
info "$OUT_ISO  ($(du -h "$OUT_ISO" | cut -f1))"

### 6. Write ---------------------------------------------------------------------------------
say "6/6 USB 쓰기"
if [ -z "$WRITE_DEV" ]; then
    cat << MANUAL
    ISO 만 만들었습니다. USB 에 쓰려면:

      tools/node/usb/make-appliance-usb.sh --list          # 디스크 확인
      tools/node/usb/make-appliance-usb.sh --write /dev/diskN

    (또는 balenaEtcher 로 위 ISO 를 구워도 됩니다.)
MANUAL
else
    [ "$(uname)" = "Darwin" ] || die "--write 는 지금 macOS 경로만 구현되어 있습니다"
    diskutil info "$WRITE_DEV" >/dev/null 2>&1 || die "그런 디스크가 없습니다: $WRITE_DEV"
    if [ "$(diskutil info "$WRITE_DEV" | awk -F': *' '/Removable Media/ {print $2}')" != "Removable" ]; then
        die "$WRITE_DEV 는 이동식 디스크가 아닙니다. 내장 디스크를 지울 뻔했습니다 — 중단합니다."
    fi
    diskutil info "$WRITE_DEV" | grep -E 'Device / Media Name|Disk Size|Removable Media'
    printf '\n  \033[31m%s 의 내용이 모두 지워집니다.\033[0m 계속하려면 yes: ' "$WRITE_DEV"
    read -r CONFIRM
    [ "$CONFIRM" = "yes" ] || { echo "취소됨."; exit 0; }
    diskutil unmountDisk "$WRITE_DEV"
    RAW="${WRITE_DEV/disk/rdisk}"
    info "sudo 비밀번호를 물어봅니다 (dd 는 관리자 권한이 필요합니다)"
    sudo dd if="$OUT_ISO" of="$RAW" bs=4m status=progress
    sync
    diskutil eject "$WRITE_DEV" || true
    info "완료 — USB 를 뽑아 대상 PC 에 꽂으세요."
fi

cat << 'NEXT'

  대상 PC 에서:
    1. CMOS 에서 USB 를 첫 부팅 장치로
    2. 전원만 켜고 그대로 두기 — 메뉴도, 질문도 없습니다
    3. 약 15분 뒤 자동 재부팅 → 첫 부팅 스크립트가 엔진을 빌드 → 다시 재부팅
    4. 모니터 스테이션의 원격 코어 → 검색 을 누르면 nds-xxxx 가 잡힙니다
       (인벤토리 행 우클릭 → "NDS 풀에 추가" 로 풀에 넣으면 스트립이 분산됩니다)

  설치 중에 인터넷이 되는 랜에 물려 있어야 합니다 (netinst 는 패키지를 미러에서 받습니다).
  그 뒤로는 직결이든 오디오 허브든 상관없습니다.
NEXT
