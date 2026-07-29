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
RT_SRC="/Volumes/Program Dev/Linux DSP Server/rt_engine"

say() { printf '==> %s\n' "$*"; }

[ -d "$RT_SRC" ] || { echo "rt-engine 소스가 없습니다: $RT_SRC"; exit 1; }

say "1/4 소스 동기화"
# build/ excluded WHOLE: the Mac side may hold Mach-O artifacts from parity runs, and one of
# them once clobbered the node's .so files ("invalid ELF header" at dlopen) — sources travel,
# artifacts never do.
rsync -az -e "ssh -o BatchMode=yes" "$RT_SRC/" "$HOST:neuracoust-node/rt_engine/" \
    --exclude 'build/'

say "2/4 노드에서 재빌드"
ssh -o BatchMode=yes "$HOST" 'cd ~/neuracoust-node/rt_engine && make build/neuracoust-rt-engine build/na_4001e.so build/na_mirage8.so 2>&1 | tail -2'
"$SOURCE_DIR/tools/node/build-console-module.sh" "$HOST" > /dev/null

say "3/4 엔진 재시작"
ssh -o BatchMode=yes "$HOST" 'kill "$(cat /tmp/neuracoust-engine.pid 2>/dev/null)" 2>/dev/null || true
rm -f /tmp/neuracoust-engine.pid
~/neuracoust-node/run-engine.sh'

say "4/4 확인"
# $HOST may be an ssh-config alias (linux-dsp), which python's resolver knows nothing about —
# ask ssh what it actually connects to.
PROBE_HOST=$(ssh -G "$HOST" 2>/dev/null | awk '/^hostname /{print $2}')
PROBE_HOST=${PROBE_HOST:-$HOST}
for _ in 1 2 3 4 5; do
    sleep 1
    # No `timeout` on macOS — python's own socket timeout bounds the wait.
    REPLY=$(python3 - "$PROBE_HOST" <<'PY' 2>/dev/null || true
import socket, sys
host = sys.argv[1] if len(sys.argv) > 1 else "linux-dsp"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(1.5)
s.sendto(b"NA_STATUS", (host, 20003))
data, _ = s.recvfrom(65535)
for line in data.decode(errors="replace").splitlines():
    if line.startswith("plugin_ids="):
        print(line)
PY
)
    if [ -n "$REPLY" ]; then
        say "완료: $REPLY"
        exit 0
    fi
done
echo "엔진이 응답하지 않습니다 — /tmp/neuracoust-engine.log 를 확인하세요"
exit 1
