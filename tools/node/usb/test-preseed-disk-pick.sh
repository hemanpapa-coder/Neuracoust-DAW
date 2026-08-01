#!/bin/sh
# Run the shipped preseed's disk-selection snippet against fake machines.
#
# This is the one piece of the unattended install that has to reason rather than answer: an
# unset partman-auto/disk on a machine with more than one drive is an interactive question, and
# the USB the installer booted from is itself one of the drives. Picking it would install
# Debian onto the installer.
#
# The snippet is extracted from preseed.cfg rather than copied here, so the test cannot drift
# away from what actually ships.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
PRESEED="$HERE/preseed.cfg"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
FAILED=0

# The preseed line is "d-i partman/early_command string <shell>", continued with backslashes.
extract_snippet() {
    awk '/^d-i partman\/early_command string/ {found=1; sub(/^[^ ]+ [^ ]+ string[ ]*/, ""); }
         found { line=$0; sub(/[ ]*\\$/, "", line); printf "%s\n", line;
                 if ($0 !~ /\\$/) exit }' "$PRESEED"
}

run_case() {
    NAME="$1"; EXPECT="$2"; shift 2
    ROOT="$WORK/$NAME"; rm -rf "$ROOT"; mkdir -p "$ROOT/bin" "$ROOT/sys/block"

    DEVLIST=""
    for SPEC in "$@"; do          # each spec is name:removable
        DEV="${SPEC%%:*}"; REM="${SPEC##*:}"
        mkdir -p "$ROOT/sys/block/$DEV"
        printf '%s\n' "$REM" > "$ROOT/sys/block/$DEV/removable"
        DEVLIST="$DEVLIST /dev/$DEV"
    done

    cat > "$ROOT/bin/list-devices" << LD
#!/bin/sh
for d in $DEVLIST; do echo "\$d"; done
LD
    # debconf-set does not exist off the installer; record what it was asked to set.
    cat > "$ROOT/bin/debconf-set" << 'DS'
#!/bin/sh
echo "$1=$2" >> "$RESULT"
DS
    chmod +x "$ROOT/bin/list-devices" "$ROOT/bin/debconf-set"

    RESULT="$ROOT/result"; : > "$RESULT"
    # The snippet reads /sys/block directly, so the fake tree is spliced in by running from a
    # copy that has $ROOT prefixed onto that one path.
    extract_snippet | sed "s#/sys/block#$ROOT/sys/block#g" > "$ROOT/snippet.sh"
    ( PATH="$ROOT/bin:$PATH"; RESULT="$RESULT"; export RESULT; sh "$ROOT/snippet.sh" )

    GOT_DISK="$(awk -F= '/^partman-auto\/disk=/ {print $2}' "$RESULT")"
    GOT_BOOT="$(awk -F= '/^grub-installer\/bootdev=/ {print $2}' "$RESULT")"
    if [ "$GOT_DISK" = "$EXPECT" ] && [ "$GOT_BOOT" = "$EXPECT" ]; then
        printf '  ok    %-38s → %s\n' "$NAME" "$GOT_DISK"
    else
        printf '  FAIL  %-38s → disk=%s boot=%s (expected %s)\n' "$NAME" "$GOT_DISK" "$GOT_BOOT" "$EXPECT"
        FAILED=1
    fi
}

echo "preseed 디스크 선택 검증"
# The real case: the installer USB enumerates FIRST, before the internal drive.
run_case "usb-first-then-sata"      /dev/sdb  sda:1 sdb:0
run_case "sata-first-then-usb"      /dev/sda  sda:0 sdb:1
run_case "nvme-plus-usb"            /dev/nvme0n1  sdb:1 nvme0n1:0
run_case "two-internal-disks"       /dev/sda  sda:0 sdb:0
run_case "only-the-usb-is-visible"  /dev/sda  sda:1
echo
[ "$FAILED" = 0 ] && echo "전부 통과" || echo "실패 있음"
exit "$FAILED"
