# Current Debian DSP Server Extract Summary

Collected read-only from `heinhome@192.168.0.198`.

## OS And Kernel

- OS: Debian GNU/Linux 12 bookworm
- Kernel: `Linux 6.1.0-49-rt-amd64 #1 SMP PREEMPT_RT Debian 6.1.174-1 (2026-05-26) x86_64`
- Hardware: ASUSTeK P8Z77-V LX
- Network: `enp3s0`, `192.168.0.198/24`, gateway `192.168.0.1`

## Runtime Included In The Appliance

- `/opt/neuracoust/rt_engine/neuracoust-rt-engine`
  - SHA-256: `99ceb9b7dabf967a87ffa4dd639e1ab360000903c941d7dfbe6b3394faee8010`
- `/opt/neuracoust/rt_engine/modules/na_4001e.so`
  - SHA-256: `953b48aab3478a24c041ce0db3a4a96f6ab56856e12d8d2d33fe5da2a5673568`
- `/opt/neuracoust/rt_engine/na_rt_plugin.h`
- `/opt/neuracoust/rt_engine/SDK.md`

## Services Extracted

- `neuracoust-cpu-performance.service`
- `neuracoust-net-rt.service`
- `neuracoust-rt-dsp.service`
- `neuracoust-wol.service`

`neuracoust-rt-audio.service` was observed but is not enabled in the minimal
appliance because the running server uses the UDP RT DSP engine.

## Tuning Included

- CPU governor set to `performance`
- `/dev/cpu_dma_latency` low-latency hold where available
- UDP socket buffer tuning:
  - `net.core.rmem_max=8388608`
  - `net.core.wmem_max=8388608`
  - `net.ipv4.udp_rmem_min=262144`
  - `net.ipv4.udp_wmem_min=262144`
- NIC EEE off, GRO/GSO/TSO off where supported
- NIC txqueuelen `256`
- Wake-on-LAN enabled dynamically on the default NIC
- Realtime limits:
  - `@audio - rtprio 99`
  - `@audio - memlock unlimited`
  - `@audio - nice -20`
- udev permissions for sound, `rtc0`, and `hpet`

## Packages To Include

Minimal appliance package list is intentionally smaller than the current
general Debian installation:

- `linux-image-rt-amd64`
- `systemd`, `udev`, `dbus`
- `openssh-server`, `network-manager`
- `ethtool`, `iproute2`, `procps`, `psmisc`
- `gdisk`, `parted`, `dosfstools`, `e2fsprogs`, `grub-efi-amd64`
- `cpufrequtils`, `linux-cpupower`, `rtirq-init`, `irqbalance`
- `pciutils`, `usbutils`, `dmidecode`, `lm-sensors`, `smartmontools`
- `alsa-utils`
- `curl`, `rsync`, `jq`, `python3-minimal`
- firmware packages for common NICs, especially Realtek

## Excluded

- Desktop environment packages
- NetworkManager GUI
- General Python desktop/application packages
- JACK/NetJACK service path by default
- User home directories and machine-specific state
- Existing Debian installation as a whole-disk clone

## Appliance Base Decision

Debian 13 trixie is current Debian stable, but this appliance starts on Debian
12 bookworm because the production server and RT kernel are bookworm based.
Trixie builds remain available through `NEURACOUST_DEBIAN_SUITE=trixie` for
compatibility testing after the bookworm appliance boots cleanly.
