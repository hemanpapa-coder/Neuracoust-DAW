#!/usr/bin/env bash
# Build and install neuracoust_remote_core_server on a DSP node.
#
# The node runs the SAME processing code as the DAW — it links neuracoust_daw_core — which is what
# makes a channel sound identical whether its strip runs here or there. So the node is not given a
# binary: it is given the source and builds it, against its own compiler and its own CPU.
#
#   tools/deploy-node.sh linux-dsp          # host, or an ssh config alias
#   tools/deploy-node.sh user@192.168.0.198
#
# Needs key-based ssh to the node (ssh-copy-id once) and, on the node, a C++20 compiler, cmake,
# ninja and rsync. Everything else it needs is in this repo.
set -euo pipefail

HOST="${1:-linux-dsp}"
REMOTE_DIR="${2:-~/neuracoust-node}"
SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

say "Checking $HOST"
ssh -o BatchMode=yes -o ConnectTimeout=8 "$HOST" 'uname -srm; nproc; cmake --version | head -1' \
  || { echo "Cannot reach $HOST with key-based ssh. Run: ssh-copy-id $HOST" >&2; exit 1; }

# Only what the server target needs. The VST3 SDK, the design files and the Swift app are not
# built on the node — it hosts Neuracoust modules, not third-party plug-ins, and has no UI.
say "Syncing source"
rsync -az --delete \
  --exclude 'build/' --exclude '.git/' --exclude 'design/' --exclude 'third_party/' \
  --exclude '*.app/' --exclude 'src/app/swift/' \
  "$SOURCE_DIR/" "$HOST:$REMOTE_DIR/"

# No VST3 SDK on the node, and none wanted: NEURACOUST_HAS_VST3_SDK is already optional and plug-in
# hosting stubs out without it. The node runs Neuracoust modules, which are compiled in.
say "Building on the node"
ssh "$HOST" "cd $REMOTE_DIR && \
  cmake -S . -B build-node -DCMAKE_BUILD_TYPE=Release && \
  cmake --build build-node --target neuracoust_remote_core_server -j\$(nproc)"

say "Self-test"
ssh "$HOST" "$REMOTE_DIR/build-node/neuracoust_remote_core_server --self-test"

cat <<EOF

Built at $REMOTE_DIR/build-node/neuracoust_remote_core_server on $HOST.

Start it (audio on UDP 20000, status/discover on 20001):

    ssh $HOST '$REMOTE_DIR/build-node/neuracoust_remote_core_server'

Then in the DAW: 모니터 독 → 원격 코어 → NDS 켜기, 주소 확인, 역할 배정.
The 부하 meter answers within two seconds once it is running.
EOF
