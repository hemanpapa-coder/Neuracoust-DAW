#!/usr/bin/env bash
# Build the console-channel module for a DSP node's rt-engine, and install it.
#
# Two translation units, one g++ call each — no cmake, which the node does not have. That is
# possible because ConsoleChannelProcessor.cpp depends on nothing but headers, so the node can
# compile the DAW's actual strip code without the rest of the engine.
#
#   tools/node/build-console-module.sh              # build + install on linux-dsp
#   tools/node/build-console-module.sh linux-dsp    # or name the host
#   tools/node/build-console-module.sh --local      # just check it compiles here
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HOST="${1:-linux-dsp}"
# Resolved on the node, not here: scp does not expand $HOME remotely, so the path has to be real
# by the time it reaches the command line.
REMOTE_DIR="${2:-}"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

if [ "$HOST" = "--local" ]; then
    say "Compiling here (syntax + link check only — a macOS .dylib, not for the node)"
    OUT="$(mktemp -d)"
    c++ -std=c++20 -O2 -fPIC -shared \
        -I"$SOURCE_DIR/src" -I"$SOURCE_DIR/tools/node" \
        "$SOURCE_DIR/tools/node/na_console_channel.cpp" \
        "$SOURCE_DIR/src/audio/ConsoleChannelProcessor.cpp" \
        -o "$OUT/na_console_channel.dylib"
    nm -gU "$OUT/na_console_channel.dylib" | grep -q "na_rt_get_plugin" \
        && echo "OK — exports na_rt_get_plugin"
    rm -rf "$OUT"
    exit 0
fi

if [ -z "$REMOTE_DIR" ]; then
    REMOTE_DIR="$(ssh -o BatchMode=yes "$HOST" 'echo $HOME')/neuracoust-node"
fi

say "Sending sources to $HOST:$REMOTE_DIR"
ssh -o BatchMode=yes "$HOST" "mkdir -p $REMOTE_DIR/node-module/audio $REMOTE_DIR/node-module/core"
# Only what the module needs. The node gets source, never a binary: it has to compile the strip
# with its own compiler for its own CPU, and a macOS object could not be loaded there anyway.
scp -q "$SOURCE_DIR/tools/node/na_console_channel.cpp" \
       "$SOURCE_DIR/tools/node/na_rt_plugin.h" \
       "$HOST:$REMOTE_DIR/node-module/"
scp -q "$SOURCE_DIR/src/audio/ConsoleChannelProcessor.cpp" \
       "$SOURCE_DIR/src/audio/ConsoleChannelProcessor.h" \
       "$HOST:$REMOTE_DIR/node-module/audio/"
scp -q "$SOURCE_DIR/src/core/DawState.h" "$HOST:$REMOTE_DIR/node-module/core/"
# DawState.h includes these; they are declaration-only headers, so the module needs them present
# but links nothing from them.
for header in audio/AudioDeviceModel.h audio/RemoteDspServerClient.h core/AppIdentity.h \
              license/LicenseAgentClient.h plugins/Vst3HostFoundation.h; do
    ssh -o BatchMode=yes "$HOST" "mkdir -p $REMOTE_DIR/node-module/$(dirname "$header")"
    scp -q "$SOURCE_DIR/src/$header" "$HOST:$REMOTE_DIR/node-module/$header"
done

say "Compiling on $HOST"
ssh -o BatchMode=yes "$HOST" "cd $REMOTE_DIR/node-module && \
  g++ -std=c++20 -O3 -fPIC -shared -fno-math-errno -DNDEBUG \
      -I. na_console_channel.cpp audio/ConsoleChannelProcessor.cpp \
      -o na_console_channel.so -lm && \
  ls -la na_console_channel.so && \
  nm -D na_console_channel.so | grep na_rt_get_plugin"

cat <<EOF

Built: $REMOTE_DIR/node-module/na_console_channel.so

The rt-engine hosts ONE module per instance and the running one is na_4001e, started as root on
ports 20000/20001. Swapping it needs root on the node:

    sudo systemctl stop <the rt-engine unit>          # or kill the process
    sudo /opt/neuracoust/rt_engine/neuracoust-rt-engine \\
         --module $REMOTE_DIR/node-module/na_console_channel.so

Self-test first, no root needed:

    /opt/neuracoust/rt_engine/neuracoust-rt-engine \\
        --module $REMOTE_DIR/node-module/na_console_channel.so --self-test
EOF
