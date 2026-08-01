#!/bin/sh
# Locate an NDS node for MANAGEMENT (ssh/rsync/scp), wherever it is plugged in.
# Prints the extra ssh option to use ("" when the plain name already works); exits 1 when the
# node is nowhere to be found.
#
#   tools/node/find-node.sh                # the original appliance (linux-dsp / .198)
#   tools/node/find-node.sh nds-765e       # any node built by tools/node/usb
#
# Three ways in, tried in order:
#   1. the name or address as given — the studio LAN case, nothing special needed;
#   2. mDNS (<name>.local) on each active interface — this is what covers a direct cable and an
#      audio-only switch, where there is no DHCP and therefore no routable address. Both avahi
#      (node) and Bonjour (Mac) answer over link-local, so nothing has to be known in advance.
#      That is what lets a pool of any size stay reachable;
#   3. the original node's IPv6 link-local address, hardcoded because that box predates the USB
#      installer and runs no avahi. EUI-64 is MAC-derived, so it is identical on every segment;
#      only the interface scope changes.
# The doubled %% is for ssh's percent-expansion in -o HostName.
TARGET="${1:-linux-dsp}"
LEGACY_LAN_IP="192.168.0.198"
LEGACY_LL="fe80::12bf:48ff:fe84:765e"   # EUI-64 of the first node's MAC 10:bf:48:84:76:5e

active_interfaces() {
    for IF in $(ifconfig -l 2>/dev/null | tr ' ' '\n' | grep '^en'); do
        ifconfig "$IF" 2>/dev/null | grep -q 'status: active' && echo "$IF"
    done
}

# 1. As given. An ssh alias resolves through ~/.ssh/config, so its HostName is worth a ping too.
ALIAS_HOST=$(ssh -G "$TARGET" 2>/dev/null | awk '/^hostname /{print $2; exit}')
for CANDIDATE in "$TARGET" "$ALIAS_HOST"; do
    [ -n "$CANDIDATE" ] || continue
    if ping -c 1 -W 400 -q "$CANDIDATE" >/dev/null 2>&1; then
        echo ""
        exit 0
    fi
done

# 2. mDNS, per interface.
case "$TARGET" in
    *.local)   MDNS_NAME="$TARGET" ;;
    linux-dsp) MDNS_NAME="" ;;            # legacy node: no avahi, skip to step 3
    *)         MDNS_NAME="$TARGET.local" ;;
esac
if [ -n "$MDNS_NAME" ]; then
    if ping6 -c 1 "$MDNS_NAME" >/dev/null 2>&1 || ping -c 1 -W 800 -q "$MDNS_NAME" >/dev/null 2>&1; then
        echo "-oHostName=$MDNS_NAME"
        exit 0
    fi
    for IF in $(active_interfaces); do
        if ping6 -c 1 "${MDNS_NAME}%${IF}" >/dev/null 2>&1; then
            echo "-oHostName=${MDNS_NAME}%%${IF}"
            exit 0
        fi
    done
fi

# 3. The first appliance, by its fixed link-local address.
if [ "$TARGET" = "linux-dsp" ] || [ "$TARGET" = "$LEGACY_LAN_IP" ]; then
    for IF in $(active_interfaces); do
        if ping6 -c 1 "${LEGACY_LL}%${IF}" >/dev/null 2>&1; then
            echo "-oHostName=${LEGACY_LL}%%${IF}"
            exit 0
        fi
    done
fi

echo "노드를 찾을 수 없습니다: $TARGET (LAN에도, mDNS에도, 링크로컬 세그먼트에도 없음)" >&2
exit 1
