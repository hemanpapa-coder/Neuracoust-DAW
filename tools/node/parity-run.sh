#!/usr/bin/env bash
# Restart the node's console module on a scratch port, then compare it against the local strip.
#
# The restart is the point. The module keeps its filter memories, detectors and ramping
# coefficients for the life of its process, while the local reference starts from reset every run
# — so without it the two sides sit at different places on the same convergence curve and the
# answer moves by two orders of magnitude between runs for no reason at all.
#
#   tools/node/parity-run.sh                 # every module, one at a time, then the whole strip
#   tools/node/parity-run.sh eq              # just one
set -euo pipefail

HOST="${NODE_HOST:-linux-dsp}"
IP="${NODE_IP:-192.168.0.198}"
PORT="${NODE_PORT:-20010}"
STATUS_PORT=$((PORT + 1))
SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODULE="\$HOME/neuracoust-node/node-module/na_console_channel.so"
ENGINE=/opt/neuracoust/rt_engine/neuracoust-rt-engine

# Two things this has to get right, both of which cost a debugging round when they are wrong:
#
#  - Kill by RECORDED PID, never `pkill -f "port $PORT"`. The remote ssh command line contains that
#    string too, so pkill matches the shell running it and kills the session — ssh just returns 255
#    with no output.
#  - Detach completely (setsid, every fd redirected) or ssh waits for the engine to exit and this
#    script hangs instead of returning.
PIDFILE="/tmp/console-module-\$PORT.pid"
restart_node() {
    ssh -o BatchMode=yes "$HOST" \
        "[ -f /tmp/console-module-$PORT.pid ] && kill \$(cat /tmp/console-module-$PORT.pid) 2>/dev/null; \
         sleep 0.5; \
         setsid $ENGINE --module $MODULE --port $PORT --monitor-port $STATUS_PORT \
             </dev/null >/tmp/console-module-$PORT.log 2>&1 & \
         echo \$! > /tmp/console-module-$PORT.pid; \
         exit 0" </dev/null >/dev/null 2>&1
    sleep 2
}

modes=("$@")
if [ ${#modes[@]} -eq 0 ]; then
    modes=(none filter eq comp gate sat bias full)
fi

for mode in "${modes[@]}"; do
    restart_node
    printf '%-8s ' "$mode"
    # A mismatch must not stop the sweep — the point is to see WHICH modules differ, and set -e
    # would report only the first.
    ("$SOURCE_DIR/build/neuracoust_node_console_parity_check" "$IP" "$PORT" "$STATUS_PORT" "$mode" \
        || true) | grep -E 'worst sample difference|skipping' | tr '\n' ' '
    echo
done
