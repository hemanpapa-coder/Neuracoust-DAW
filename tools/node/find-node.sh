#!/bin/sh
# Locate the NDS node for MANAGEMENT (ssh/rsync/scp), wherever it is plugged in:
#   1. the LAN (192.168.0.198) — plain alias, no extra options;
#   2. any direct cable or audio-only hub — via the node's IPv6 LINK-LOCAL address, which is
#      derived from its MAC (EUI-64) and therefore IDENTICAL on every segment; only the
#      interface scope changes, and we find that by probing each active interface.
# Prints the extra ssh option to use ("" for the LAN case); exits 1 when the node is nowhere.
# The doubled %% is for ssh's percent-expansion in -o HostName.
NODE_LAN_IP="192.168.0.198"
NODE_LL="fe80::12bf:48ff:fe84:765e"   # EUI-64 of the node's MAC 10:bf:48:84:76:5e

if ping -c 1 -W 400 -q "$NODE_LAN_IP" >/dev/null 2>&1; then
    echo ""
    exit 0
fi
for IF in $(ifconfig -l 2>/dev/null | tr ' ' '\n' | grep '^en'); do
    ifconfig "$IF" 2>/dev/null | grep -q 'status: active' || continue
    if ping6 -c 1 "${NODE_LL}%${IF}" >/dev/null 2>&1; then
        echo "-oHostName=${NODE_LL}%%${IF}"
        exit 0
    fi
done
echo "노드를 찾을 수 없습니다 (LAN에도, 링크로컬 세그먼트에도 없음)" >&2
exit 1
