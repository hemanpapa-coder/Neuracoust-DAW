#!/bin/sh
# Start the NDS engine with every module the DAW expects.
#
# Two callers, two modes:
#   --foreground   systemd's ExecStart. Execs the engine, so systemd owns the process and
#                  Restart=always applies.
#   (no argument)  the DAW's update path (tools/node/update-node-engine.sh), which has just
#                  killed the old process. Under systemd the kill already triggered a restart
#                  with the freshly built binary, so this only waits for the engine to answer
#                  and returns. On a node with no unit installed it starts one itself, which is
#                  how the hand-built node has always worked.
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ENGINE="$DIR/rt_engine/build/neuracoust-rt-engine"
AUDIO_PORT=20002
MONITOR_PORT=20003
PIDFILE=/tmp/neuracoust-engine.pid
LOGFILE=/tmp/neuracoust-engine.log

# Every module that exists gets loaded — a missing .so must not stop the rest from coming up.
MODULES=""
for m in "$DIR/rt_engine/build/na_4001e.so" \
         "$DIR/rt_engine/build/na_mirage8.so" \
         "$DIR/node-module/na_console_channel.so" \
         "$DIR/node-module/na_api525a.so"; do
    [ -f "$m" ] && MODULES="$MODULES --module $m"
done

[ -x "$ENGINE" ] || { echo "엔진 바이너리가 없습니다: $ENGINE" >&2; exit 1; }

if [ "${1:-}" = "--foreground" ]; then
    echo $$ > "$PIDFILE"
    # shellcheck disable=SC2086
    exec "$ENGINE" $MODULES --port "$AUDIO_PORT" --monitor-port "$MONITOR_PORT"
fi

engine_answers() {
    ss -lun 2>/dev/null | grep -q ":$MONITOR_PORT\b"
}

if systemctl is-enabled neuracoust-nds.service >/dev/null 2>&1; then
    i=0
    while [ $i -lt 20 ]; do
        engine_answers && { echo "엔진 가동 중 (systemd, UDP $AUDIO_PORT/$MONITOR_PORT)"; exit 0; }
        i=$((i + 1))
        sleep 1
    done
    echo "엔진이 20초 안에 뜨지 않았습니다 — journalctl -u neuracoust-nds" >&2
    exit 1
fi

# No unit: run it detached, the way the hand-built node does — which is also driven by a
# once-a-minute cron watchdog there, so this path must be safe to call when the engine is
# ALREADY up. Without this guard every watchdog tick would start another engine, and the copies
# would fight over the ports.
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
    echo "엔진이 이미 가동 중입니다 (pid $(cat "$PIDFILE"))"
    exit 0
fi
# shellcheck disable=SC2086
setsid "$ENGINE" $MODULES --port "$AUDIO_PORT" --monitor-port "$MONITOR_PORT" \
    < /dev/null >> "$LOGFILE" 2>&1 &
echo $! > "$PIDFILE"
sleep 1
echo "엔진 시작 (pid $(cat "$PIDFILE"), UDP $AUDIO_PORT/$MONITOR_PORT)"
