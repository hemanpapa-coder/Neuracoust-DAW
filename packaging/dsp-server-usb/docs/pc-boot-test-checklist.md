# PC Boot Test Checklist

Copyright (C) 2026 Neuracoust. All rights reserved.

This checklist is for administrator-only validation of the Neuracoust DSP
Server Debian minimal live appliance.

## USB Boot

1. Insert the Neuracoust DSP Server USB.
2. Enter firmware boot menu and choose the USB entry with `UEFI` in the name.
   - Samsung desktop common keys: `F10` for boot menu, `F2` for BIOS/UEFI setup.
   - Other common keys: `Esc`, `F8`, `F11`, `F12`, `Del`.
   - If the USB is blocked, disable Secure Boot and keep boot mode as UEFI.
   - Do not choose a legacy/non-UEFI USB entry.
3. Confirm no Debian installer screen appears.
4. Confirm the system reaches a console or SSH state as `neuracoust-dsp`.
5. Run:

```bash
uname -a
systemctl is-active neuracoust-rt-dsp.service
systemctl status neuracoust-rt-dsp.service --no-pager
ss -lunp | grep -E ':20000|:20001'
```

Expected:

- Kernel contains `rt-amd64` or `PREEMPT_RT`.
- `neuracoust-rt-dsp.service` is `active`.
- UDP ports `20000` and `20001` are listening.

## Hardware Probe

Check:

```bash
cat /var/log/neuracoust/dsp-server/hardware.json
cat /var/log/neuracoust/dsp-server/install-candidate-disks.log
```

Expected:

- CPU, memory, PCI, network, block devices are recorded.
- USB/external disks are logged but not selected.

## SSD Self Install

Check:

```bash
cat /var/log/neuracoust/dsp-server/install-report.json
lsblk -o NAME,TYPE,TRAN,RM,HOTPLUG,SIZE,MODEL,MOUNTPOINTS
```

Expected when a safe internal SSD exists:

- Target disk is internal, non-removable, non-hotplug, non-USB, and at least 32 GB.
- UEFI partition and root partition are created.
- Bootloader is installed as `NeuracoustDSP`.
- External USB drives and large development drives are never selected.
- The report clearly says skipped if no safe internal SSD exists.

## Boot Without USB

1. Shut down.
2. Remove USB.
3. Boot the PC from internal SSD.
4. Re-run the service and UDP checks.

Expected:

- No reinstall loop.
- `neuracoust-rt-dsp.service` starts automatically.
- DAW/server discovery can see the DSP server.

## Failure Notes To Collect

If the PC says a file cannot be found, photograph the exact UEFI/GRUB message
and keep the USB Maker log. The expected USB root must contain:

```text
/EFI/BOOT/BOOTX64.EFI
/boot/grub/grub.cfg
/live/filesystem.squashfs
/live/vmlinuz
/live/initrd.img
```

If GRUB references versioned files, those files must also exist, for example:

```text
/live/vmlinuz-6.1.0-50-rt-amd64
/live/initrd.img-6.1.0-50-rt-amd64
```
