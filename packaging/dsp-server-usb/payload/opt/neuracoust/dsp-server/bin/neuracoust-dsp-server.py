#!/usr/bin/env python3
import os
import socket
import struct
import threading
import time

MAGIC = 0x4E415254
VERSION = 1
HEADER_SIZE = 20
AUDIO_PORT = int(os.environ.get("NEURACOUST_DSP_AUDIO_PORT", "20000"))
STATUS_PORT = int(os.environ.get("NEURACOUST_DSP_STATUS_PORT", "20001"))
SERVER_VERSION = "260703.2050"

packets_in = 0
packets_out = 0
bad_packets = 0


def read_text(path, fallback=""):
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as handle:
            return handle.read().strip()
    except OSError:
        return fallback


def first_line(command, fallback=""):
    try:
        import subprocess

        out = subprocess.check_output(command, stderr=subprocess.DEVNULL, text=True, timeout=1.5)
        return out.splitlines()[0].strip() if out.splitlines() else fallback
    except Exception:
        return fallback


def memory_mb():
    for line in read_text("/proc/meminfo").splitlines():
        if line.startswith("MemTotal:"):
            parts = line.split()
            if len(parts) >= 2:
                return str(int(int(parts[1]) / 1024))
    return "0"


def cpu_model():
    for line in read_text("/proc/cpuinfo").splitlines():
        if "model name" in line or "Hardware" in line:
            return line.split(":", 1)[-1].strip()
    return "Neuracoust DSP CPU"


def cpu_mhz():
    for line in read_text("/proc/cpuinfo").splitlines():
        if "cpu MHz" in line:
            return line.split(":", 1)[-1].strip()
    return "0"


def primary_mac():
    for name in os.listdir("/sys/class/net"):
        if name == "lo":
            continue
        mac = read_text(f"/sys/class/net/{name}/address")
        if mac:
            return name, mac
    return "", ""


def handle_audio():
    global packets_in, packets_out, bad_packets
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", AUDIO_PORT))
    while True:
        data, peer = sock.recvfrom(262144)
        packets_in += 1
        if len(data) < HEADER_SIZE:
            bad_packets += 1
            continue
        try:
            magic, version, header_size, sequence, channels, frames, _flags = struct.unpack("!IHHIHHI", data[:HEADER_SIZE])
        except struct.error:
            bad_packets += 1
            continue
        payload_size = channels * frames * 4
        if magic != MAGIC or version != VERSION or header_size != HEADER_SIZE or len(data) != HEADER_SIZE + payload_size:
            bad_packets += 1
            continue
        # v0 appliance runtime is transparent pass-through. Product DSP packages
        # can replace this service while keeping the same UDP protocol.
        sock.sendto(data, peer)
        packets_out += 1


def status_text():
    nic, mac = primary_mac()
    cores = os.cpu_count() or 0
    return "\n".join(
        [
            "vendor=Neuracoust",
            "model=Neuracoust DSP Server Appliance",
            f"version={SERVER_VERSION}",
            f"hostname={socket.gethostname()}",
            f"mac={mac}",
            f"cpu_model={cpu_model()}",
            f"cpu_mhz={cpu_mhz()}",
            f"memory_mb={memory_mb()}",
            "temperature_c=0",
            "temperature_f=0",
            "cpu_core_loads=" + ",".join(["0"] * max(1, cores)),
            f"nic={nic}",
            f"audio_port={AUDIO_PORT}",
            f"monitor_port={STATUS_PORT}",
            "channels=2",
            f"core_count={cores}",
            "buffer_profiles=128,256,512",
            "performance_modes=transparent,low-latency",
            "lpfc=auto",
            "lpee=auto",
            "plugin_id=neuracoust-dsp-appliance",
            "plugin_name=Neuracoust DSP Server",
            f"packets_in={packets_in}",
            f"packets_out={packets_out}",
            f"bad_packets={bad_packets}",
            "",
        ]
    ).encode("utf-8")


def handle_status():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", STATUS_PORT))
    while True:
        _data, peer = sock.recvfrom(4096)
        sock.sendto(status_text(), peer)


def main():
    threading.Thread(target=handle_audio, daemon=True).start()
    threading.Thread(target=handle_status, daemon=True).start()
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()

