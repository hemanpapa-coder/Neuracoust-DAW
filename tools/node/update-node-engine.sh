#!/usr/bin/env bash
# Update the NDS appliance engine from the DAW — the SoundGrid firmware-update flow, over ssh
# key auth: NO login prompt, NO root, ever. The monitor station's 업데이트 button runs this.
#
# What it does, in order, loudly failing on any step:
#   1. rsync the rt-engine sources and the console-module sources to the node
#   2. rebuild there (cc for the C engine + modules, the console script for the C++ strip)
#   3. restart the user-owned engine on the canonical ports (20002/20003)
#   4. probe NA_STATUS until the new engine answers with its module list
#
#   tools/node/update-node-engine.sh            # update linux-dsp
#   tools/node/update-node-engine.sh <host>     # or name the host
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HOST="${1:-linux-dsp}"
# Locate the node wherever it is plugged in (LAN, direct cable, audio hub): find-node.sh emits
# an extra ssh option ("" on the LAN) or fails when the node is nowhere.
NODE_OPT="$("$(dirname "$0")/find-node.sh" "$HOST")" || exit 1
RT_SRC="/Volumes/Program Dev/Linux DSP Server/rt_engine"

say() { printf '==> %s\n' "$*"; }

[ -d "$RT_SRC" ] || { echo "rt-engine 소스가 없습니다: $RT_SRC"; exit 1; }

say "1/4 소스 동기화"
# build/ excluded WHOLE: the Mac side may hold Mach-O artifacts from parity runs, and one of
# them once clobbered the node's .so files ("invalid ELF header" at dlopen) — sources travel,
# artifacts never do.
rsync -az -e "ssh -o BatchMode=yes $NODE_OPT" "$RT_SRC/" "$HOST:neuracoust-node/rt_engine/" \
    --exclude 'build/'

say "2/4 노드에서 재빌드"
ssh -o BatchMode=yes $NODE_OPT "$HOST" 'cd ~/neuracoust-node/rt_engine && make build/neuracoust-rt-engine build/na_4001e.so build/na_mirage8.so 2>&1 | tail -2'
"$SOURCE_DIR/tools/node/build-console-module.sh" "$HOST" > /dev/null

say "3/4 엔진 재시작"
ssh -o BatchMode=yes $NODE_OPT "$HOST" 'kill "$(cat /tmp/neuracoust-engine.pid 2>/dev/null)" 2>/dev/null || true
rm -f /tmp/neuracoust-engine.pid
~/neuracoust-node/run-engine.sh'

say "4/4 확인"
# Probe ON THE NODE (loopback): a probe from the Mac over IPv4 falls into the multi-interface
# link-local routing trap on a direct cable / audio hub, and loopback is correct everywhere.
for _ in 1 2 3 4 5; do
    sleep 1
    REPLY=$(ssh -o BatchMode=yes $NODE_OPT "$HOST" 'python3 - <<PY 2>/dev/null || true
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(1.5)
s.sendto(b"NA_STATUS", ("127.0.0.1", 20003))
data, _ = s.recvfrom(65535)
for line in data.decode(errors="replace").splitlines():
    if line.startswith("plugin_ids="):
        print(line)
PY' || true)
    if [ -n "$REPLY" ]; then
        say "완료: $REPLY"
        exit 0
    fi
done
echo "엔진이 응답하지 않습니다 — /tmp/neuracoust-engine.log 를 확인하세요"
exit 1
