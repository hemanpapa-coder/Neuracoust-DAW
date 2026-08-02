#!/usr/bin/env bash
# Stop the NDS appliance from going quiet, and record why it did.
#
#   tools/node/node-power-fix.sh            # diagnose, then apply
#   tools/node/node-power-fix.sh --diagnose # look only, change nothing
#
# The appliance was measured disappearing from the network for minutes at a time — not just the
# audio port: IPv6 neighbour discovery and ssh died with it, which means the machine itself went
# away, not a socket. Three things on a Debian box do that, and all three are power management:
#
#   1. the NIC's Energy-Efficient Ethernet negotiating the link down and back up,
#   2. PCI runtime power management parking the network card,
#   3. the system suspending on idle — a box with no screen and nobody typing is idle by
#      definition, and a DSP node must never take that as permission to sleep.
#
# Everything here needs root ON THE NODE, so it asks for the node's password once. Nothing is
# typed for you: ssh -t hands the prompt straight to your terminal.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST="${NDS_HOST:-linux-dsp}"
DIAGNOSE_ONLY=false
[ "${1:-}" = "--diagnose" ] && DIAGNOSE_ONLY=true

NODE_OPT="$("$HERE/find-node.sh" "$HOST")" || {
    echo "노드에 닿지 않습니다. 전원과 랜 케이블부터 확인하세요." >&2
    exit 1
}
SSH=(ssh -t -o ConnectTimeout=8 $NODE_OPT "$HOST")

echo "==> 진단 (변경 없음)"
"${SSH[@]}" 'set -u
IF=$(ip -o link show | awk -F": " "\$2 !~ /^(lo|docker|veth)/ {print \$2; exit}")
echo "인터페이스: $IF"
echo "가동 시간:  $(uptime -p 2>/dev/null || uptime)"
echo "부팅 시각:  $(uptime -s 2>/dev/null)"
echo "링크 변화:  $(cat /sys/class/net/$IF/carrier_changes 2>/dev/null) 회   (많으면 케이블/포트/EEE)"
echo "드라이버:   $(basename "$(readlink -f /sys/class/net/$IF/device/driver 2>/dev/null)" 2>/dev/null)"
echo "속도:       $(cat /sys/class/net/$IF/speed 2>/dev/null) Mb/s"
echo "PCI 전원:   $(cat /sys/class/net/$IF/device/power/control 2>/dev/null)   (auto 면 카드가 잠들 수 있음)"
echo "절전 타깃:  $(systemctl is-enabled sleep.target 2>/dev/null) / suspend=$(systemctl is-enabled suspend.target 2>/dev/null)"
echo "유휴 동작:  $(grep -E "^#?IdleAction" /etc/systemd/logind.conf 2>/dev/null | tail -1)"
echo "--- 최근 절전/재개 기록:"
journalctl -b --no-pager 2>/dev/null | grep -iE "suspend|resume|sleep|hibernat" | tail -5 || echo "  (없음)"
echo "--- 최근 링크 이벤트:"
journalctl -b --no-pager -k 2>/dev/null | grep -iE "link is (up|down)|$IF" | tail -6 || echo "  (없음)"
echo "--- 이전 부팅 기록 (전원이 끊겼는지):"
last -x reboot shutdown 2>/dev/null | head -4 || echo "  (없음)"'

if [ "$DIAGNOSE_ONLY" = true ]; then
    echo
    echo "진단만 했습니다. 적용하려면 옵션 없이 다시 실행하세요."
    exit 0
fi

echo
echo "==> 적용 (노드의 sudo 비밀번호를 물어봅니다)"
"${SSH[@]}" 'set -u
IF=$(ip -o link show | awk -F": " "\$2 !~ /^(lo|docker|veth)/ {print \$2; exit}")
sudo bash -s "$IF" <<"ROOT"
set -u
IF="$1"

# 1. The card must not sleep, and must not negotiate the link away to save milliwatts.
apt-get install -y ethtool >/dev/null 2>&1 || true
cat > /usr/local/sbin/nds-nic-awake <<SCRIPT
#!/bin/bash
IF="\$1"
[ -n "\$IF" ] || exit 0
ethtool --set-eee "\$IF" eee off 2>/dev/null || true
ethtool -s "\$IF" wol d 2>/dev/null || true
# Runtime PM off: "auto" lets the kernel park the card when it looks idle, and an idle-looking
# card on an audio network is exactly the card we need awake.
echo on > "/sys/class/net/\$IF/device/power/control" 2>/dev/null || true
exit 0
SCRIPT
chmod +x /usr/local/sbin/nds-nic-awake
/usr/local/sbin/nds-nic-awake "$IF"

cat > /etc/systemd/system/nds-nic-awake.service <<UNIT
[Unit]
Description=Neuracoust NDS — keep the network card awake
After=network.target
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/nds-nic-awake $IF
[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable --now nds-nic-awake.service >/dev/null 2>&1

# 2. The machine itself must never suspend. A DSP node has no screen and nobody typing at it, so
#    every idle heuristic on the box will eventually conclude it should sleep. Masking the sleep
#    targets is the only setting that cannot be overridden by a desktop component later.
systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target >/dev/null 2>&1
mkdir -p /etc/systemd/logind.conf.d
cat > /etc/systemd/logind.conf.d/10-neuracoust.conf <<LOGIND
[Login]
IdleAction=ignore
HandleLidSwitch=ignore
HandleSuspendKey=ignore
LOGIND
systemctl restart systemd-logind >/dev/null 2>&1 || true

echo "적용 완료:"
echo "  EEE/WoL 끔, PCI 런타임 절전 끔 ($IF)"
echo "  절전 타깃 마스크, IdleAction=ignore"
echo "  부팅마다 다시 적용: nds-nic-awake.service"
ROOT'

echo
echo "==> 확인"
"${SSH[@]}" 'IF=$(ip -o link show | awk -F": " "\$2 !~ /^(lo|docker|veth)/ {print \$2; exit}")
echo "PCI 전원: $(cat /sys/class/net/$IF/device/power/control 2>/dev/null)"
ethtool --show-eee "$IF" 2>/dev/null | head -3
systemctl is-enabled sleep.target 2>/dev/null'
