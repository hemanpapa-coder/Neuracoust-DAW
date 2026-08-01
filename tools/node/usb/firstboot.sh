#!/bin/bash
# Neuracoust NDS appliance — first boot. Runs once, as root, from nds-firstboot.service.
#
# The Debian installer left a plain minimal system. This turns it into the appliance: realtime
# limits, isolated DSP cores, the engine built from the sources that rode along on the USB, and
# a systemd unit that brings the engine up on every boot. Then it deletes itself and reboots.
#
# Everything here must work with NO network: the packages it needs were installed by the
# preseed, and the sources came from the USB. A node plugged straight into the Mac with one
# cable has to come up exactly like one on the LAN.
set -uo pipefail

LOG=/var/log/nds-firstboot.log
exec > >(tee -a "$LOG") 2>&1

say() { printf '\n==> %s\n' "$*"; }
warn() { printf '!!  %s\n' "$*"; }

NODE_USER=heinhome
NODE_HOME="/home/$NODE_USER"
NODE_DIR="$NODE_HOME/neuracoust-node"
PAYLOAD=/opt/nds-install

say "Neuracoust NDS 어플라이언스 초기 설정 — $(date -Is)"

### 1. A name of its own -------------------------------------------------------------------
# Every node in the pool needs a distinct name, and it has to be stable across reboots and
# across networks. The MAC gives both. (It is also the same identity the DAW's management path
# uses: the IPv6 link-local address is derived from this MAC.)
PRIMARY_IF=$(ip -o link show | awk -F': ' '$2 !~ /^(lo|docker|veth)/ {print $2; exit}')
MAC=$(cat "/sys/class/net/$PRIMARY_IF/address" 2>/dev/null || echo "")
SUFFIX=$(echo "$MAC" | tr -d ':' | tail -c 5)
NEW_HOST="nds-${SUFFIX:-node}"
hostnamectl set-hostname "$NEW_HOST" 2>/dev/null || echo "$NEW_HOST" > /etc/hostname
sed -i "s/\<nds\>/$NEW_HOST/g" /etc/hosts
say "호스트 이름: $NEW_HOST  (MAC $MAC, 인터페이스 $PRIMARY_IF)"
say "  IPv6 링크로컬로도 언제나 도달합니다 — 맥에서 tools/node/find-node.sh 가 쓰는 주소입니다."

### 2. Realtime limits ---------------------------------------------------------------------
groupadd -f audio
usermod -aG audio "$NODE_USER" 2>/dev/null || true
cat > /etc/security/limits.d/99-neuracoust.conf << 'LIMITS'
@audio   -   rtprio      99
@audio   -   memlock     unlimited
@audio   -   nice        -20
LIMITS

### 3. DSP core isolation ------------------------------------------------------------------
# The upper half of the CPUs is handed to the DSP and taken away from the scheduler, the timer
# tick and IRQ routing. This is the part macOS cannot do, and the reason the Linux node beats
# the Intel Mac on the WORST-case round trip rather than the median.
NPROC=$(nproc)
if [ "$NPROC" -ge 4 ]; then
    DSP_FIRST=$((NPROC / 2))
    DSP_CPUS="${DSP_FIRST}-$((NPROC - 1))"
    SYS_CPUS="0-$((DSP_FIRST - 1))"
else
    DSP_CPUS=""
    SYS_CPUS="0-$((NPROC - 1))"
    warn "코어가 ${NPROC}개뿐이라 격리를 생략합니다 (전 코어 공용)."
fi

if [ -n "$DSP_CPUS" ]; then
    # No idle=poll: it pins the isolated cores at full power forever. PREEMPT_RT plus a
    # shallow C-state limit gets the latency without cooking the machine.
    CMDLINE="quiet threadirqs isolcpus=${DSP_CPUS} nohz_full=${DSP_CPUS} rcu_nocbs=${DSP_CPUS} rcu_nocb_poll irqaffinity=${SYS_CPUS} processor.max_cstate=1 intel_idle.max_cstate=1 nmi_watchdog=0 skew_tick=1 transparent_hugepage=never"
    sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT=\"${CMDLINE}\"|" /etc/default/grub
    update-grub 2>/dev/null || grub-mkconfig -o /boot/grub/grub.cfg
    say "DSP 격리 코어: ${DSP_CPUS} / 시스템 코어: ${SYS_CPUS} (재부팅 후 적용)"
fi

### 4. CPU governor ------------------------------------------------------------------------
cat > /usr/local/sbin/nds-cpu-performance << 'PERF'
#!/bin/bash
for g in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
    [ -w "$g" ] && echo performance > "$g" 2>/dev/null || true
done
[ -w /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true
exit 0
PERF
chmod +x /usr/local/sbin/nds-cpu-performance
cat > /etc/systemd/system/nds-cpu-performance.service << 'PERFSVC'
[Unit]
Description=Neuracoust NDS CPU performance governor
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/nds-cpu-performance
[Install]
WantedBy=multi-user.target
PERFSVC

### 5. Network — must survive being unplugged from the LAN ---------------------------------
# The appliance gets used three ways: on the studio LAN, on an audio-only switch with no DHCP,
# and on one cable straight into the Mac. Only the first hands out an address. link-local
# enabled means the other two still come up with a reachable IPv4 as well as the automatic
# IPv6 one, instead of sitting there with no address at all.
mkdir -p /etc/NetworkManager/conf.d
cat > /etc/NetworkManager/conf.d/10-neuracoust-linklocal.conf << 'NMCONF'
[connection]
ipv4.may-fail=true
ipv4.link-local=enabled
ipv6.may-fail=true
NMCONF
# Debian's minimal install may use ifupdown instead of NetworkManager. Cover that too.
if [ -f /etc/network/interfaces ] && ! systemctl is-enabled NetworkManager >/dev/null 2>&1; then
    grep -q "$PRIMARY_IF" /etc/network/interfaces 2>/dev/null || cat >> /etc/network/interfaces << IFUP

allow-hotplug $PRIMARY_IF
iface $PRIMARY_IF inet dhcp
# No lease (audio switch / direct cable): fall back to a link-local address so the node is
# still reachable and still answers discovery.
    post-up ip addr show dev $PRIMARY_IF | grep -q 'inet ' || ip addr add 169.254.\$((RANDOM % 250 + 2)).\$((RANDOM % 250 + 2))/16 dev $PRIMARY_IF
IFUP
fi

cat > /etc/sysctl.d/99-neuracoust-udp.conf << 'SYSCTL'
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.core.netdev_max_backlog=5000
net.ipv4.udp_rmem_min=131072
net.ipv4.udp_wmem_min=131072
SYSCTL
sysctl --system >/dev/null 2>&1 || true

### 6. The engine sources ------------------------------------------------------------------
say "엔진 소스 배치"
install -d -o "$NODE_USER" -g "$NODE_USER" "$NODE_DIR"
tar -xzf "$PAYLOAD/payload.tar.gz" -C "$NODE_DIR"
chown -R "$NODE_USER:$NODE_USER" "$NODE_DIR"
chmod +x "$NODE_DIR/run-engine.sh"

say "엔진 빌드 (노드 자신의 컴파일러로)"
sudo -u "$NODE_USER" bash -lc "cd '$NODE_DIR/rt_engine' && make -j\$(nproc) build/neuracoust-rt-engine build/na_4001e.so build/na_mirage8.so" \
    || warn "rt-engine 빌드 실패 — /var/log/nds-firstboot.log 확인"

sudo -u "$NODE_USER" bash -lc "cd '$NODE_DIR/node-module' && \
    g++ -std=c++20 -O3 -fPIC -shared -fno-math-errno -DNDEBUG -I. na_console_channel.cpp audio/ConsoleChannelProcessor.cpp -o na_console_channel.so -lm && \
    g++ -std=c++20 -O3 -fPIC -shared -fno-math-errno -DNDEBUG -I. na_api525a.cpp audio/ConsoleChannelProcessor.cpp -o na_api525a.so -lm" \
    || warn "콘솔 모듈 빌드 실패 — /var/log/nds-firstboot.log 확인"

if [ -x "$NODE_DIR/rt_engine/build/neuracoust-rt-engine" ]; then
    say "자체 검사"
    sudo -u "$NODE_USER" "$NODE_DIR/rt_engine/build/neuracoust-rt-engine" \
        --module "$NODE_DIR/node-module/na_console_channel.so" --self-test 2>&1 | tail -5 || true
fi

### 7. Autostart ---------------------------------------------------------------------------
# Type=simple + Restart=always is what makes the DAW's 업데이트 button work without root: the
# update script rebuilds the binaries and kills the process, and systemd brings the NEW one
# straight back up. Nothing on the node needs a password for that.
cat > /etc/systemd/system/neuracoust-nds.service << NDSSVC
[Unit]
Description=Neuracoust NDS realtime engine
After=network.target nds-cpu-performance.service
Wants=nds-cpu-performance.service

[Service]
Type=simple
User=$NODE_USER
Group=audio
ExecStart=$NODE_DIR/run-engine.sh --foreground
Restart=always
RestartSec=2
LimitRTPRIO=99
LimitMEMLOCK=infinity
LimitNICE=-20
$([ -n "$DSP_CPUS" ] && echo "CPUAffinity=$DSP_CPUS")
IOSchedulingClass=realtime

[Install]
WantedBy=multi-user.target
NDSSVC

systemctl daemon-reload
systemctl enable nds-cpu-performance.service
systemctl enable neuracoust-nds.service
systemctl start nds-cpu-performance.service

cat > /etc/motd << MOTD

  Neuracoust NDS — $NEW_HOST
  엔진:  systemctl status neuracoust-nds
  포트:  UDP 20002 (오디오) / 20003 (상태·검색)
  소스:  $NODE_DIR
  로그:  /var/log/nds-firstboot.log

MOTD

### 8. Step aside -------------------------------------------------------------------------
systemctl disable nds-firstboot.service 2>/dev/null || true
rm -f /etc/systemd/system/nds-firstboot.service
systemctl daemon-reload

say "설치 완료. 10초 후 재부팅합니다 (커널 파라미터 적용)."
sleep 10
systemctl reboot
