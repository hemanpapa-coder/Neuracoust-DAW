# Neuracoust DSP Server USB Maker

Copyright (C) 2026 Neuracoust. All rights reserved.

This package defines the Neuracoust DSP Server Debian minimal live appliance
USB flow.

This is an administrator-only Neuracoust utility. It is not intended for public
end-user distribution until the DSP server appliance boot/install flow has been
validated across the target PC hardware set.

## Current Base

The production reference server is currently Debian 12 bookworm with the Debian
PREEMPT_RT kernel:

```text
Linux 6.1.0-49-rt-amd64 PREEMPT_RT
```

Debian 13 trixie is the current Debian stable release, but the first appliance
track intentionally starts from bookworm for runtime compatibility with the
currently running DSP server. Build trixie candidates with:

```zsh
NEURACOUST_DEBIAN_SUITE=trixie scripts/build-dsp-server-appliance-iso.command
```

The intended user flow is:

1. Build or download a Neuracoust DSP Server Debian appliance image.
2. Write that image to a USB drive from the Mac mini, or from a Windows
   machine on the local network that has the USB drive attached.
3. Boot the target DSP server computer from the USB drive.
4. The booted live appliance immediately starts the Neuracoust RT DSP Server.
5. It probes hardware, safely copies itself to an eligible internal SSD/NVMe
   drive, installs a UEFI bootloader, enables the DSP server service, runs a
   health check, and stores an install report.

## Administrator Release Package

The release package contains:

- macOS USB Maker app with the Debian appliance image built in.
- Windows USB Maker PowerShell package with the same Debian appliance image
  built in.
- Debian DSP Server appliance ISO.
- Current server extraction summary.
- PC boot test checklist.
- Copyright and administrator-only release notes.

The product should be registered in the Neuracoust License Agent product table
as:

```text
appId: neuracoust-dsp-usb-maker
productName: Neuracoust DSP Server USB Maker
status: active
adminOnly: true
allowedTiers: Admin
platforms: Windows,macOS
formats: Standalone
category: utility
```

The same version number must be used for the macOS and Windows packages in a
single release batch.

## Safety Model

USB creation is intentionally split from internal-SSD installation.

- Host-side USB maker scripts only write to removable or external USB media.
- Boot-side install scripts only run inside Linux and select non-removable,
  non-hotplug, non-USB NVMe/SATA/SCSI disks of at least 32 GB, excluding the disk
  that currently hosts the live USB.
- Destructive actions require `NEURACOUST_CONFIRM_ERASE=ERASE`; the Debian live
  appliance sets this only inside its firstboot unit.
- Candidate disks are logged before any erase at
  `/var/log/neuracoust/dsp-server/install-candidate-disks.log`.

## Files

```text
packaging/dsp-server-usb/
├─ manifest.json
├─ payload/
│  ├─ etc/neuracoust-dsp-server/install.conf
│  ├─ opt/neuracoust/dsp-server/bin/neuracoust-dsp-firstboot.sh
│  ├─ opt/neuracoust/dsp-server/bin/neuracoust-dsp-hardware-probe.sh
│  ├─ opt/neuracoust/dsp-server/bin/neuracoust-dsp-install-to-ssd.sh
│  └─ etc/systemd/system/neuracoust-dsp-firstboot.service
└─ windows/
   └─ Write-NeuracoustDspUsb.ps1
```

Host-side helpers live in `scripts/`:

```text
scripts/make-dsp-server-usb-macos.command
scripts/make-dsp-server-usb-windows-remote.command
```

## Mac USB Write

List candidate USB disks:

```zsh
scripts/make-dsp-server-usb-macos.command --list
```

Write an appliance image:

```zsh
NEURACOUST_CONFIRM_ERASE=ERASE \
scripts/make-dsp-server-usb-macos.command \
  --disk /dev/diskN \
  --image /path/to/Neuracoust-DSP-Server-Appliance.img
```

The script refuses non-external/non-removable disks.

The macOS GUI app uses the same safety policy:

- shows only USB disks from 4 GB to 32 GB,
- hides large external drives,
- writes detailed logs to `~/Library/Logs/Neuracoust/DSPUSBMaker`,
- keeps log open/copy buttons,
- uses a FAT32 file-copy UEFI fallback when macOS blocks raw `dd` writes.

After completion, the status should say the USB is complete and safe to remove.

## Windows USB Write Over Local Network

Use this when the USB drive is plugged into a Windows computer on the local
network and the Mac can reach it by SSH.

```zsh
NEURACOUST_CONFIRM_ERASE=ERASE \
scripts/make-dsp-server-usb-windows-remote.command \
  --host windows11-server \
  --disk-number 3 \
  --image /path/to/Neuracoust-DSP-Server-Appliance.img
```

The Windows writer refuses disks whose `BusType` is not USB.

## Windows Local USB Write

For a Windows computer with the USB plugged in locally, open PowerShell as
Administrator from the Windows package folder.

List safe candidates:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Write-NeuracoustDspUsb.ps1 -List
```

Create the USB:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Write-NeuracoustDspUsb.ps1 `
  -DiskNumber 3 `
  -ImagePath .\appliance-images\Neuracoust_DSP_Server_Debian_bookworm_Appliance_260704.1340.iso `
  -ConfirmErase ERASE
```

The Windows writer:

- shows only non-boot USB disks from 4 GB to 32 GB,
- refuses system, boot, non-USB, too-small, and too-large disks,
- formats as single MBR/FAT32,
- copies the ISO contents instead of raw-writing the disk,
- repairs FAT32-incompatible live kernel/initrd symlink copies,
- verifies UEFI boot files before reporting success,
- writes logs under `%LOCALAPPDATA%\Neuracoust\DSPUSBMaker\Logs`.

## PC Firmware / CMOS Boot Guide

1. Insert the USB into the target DSP server PC.
2. Power on the PC.
3. Open the boot menu or firmware setup.
   - Samsung desktops commonly use `F10` for boot menu and `F2` for BIOS/UEFI
     setup.
   - Some boards use `Esc`, `F8`, `F11`, `F12`, or `Del`.
4. Choose the USB entry that explicitly starts with `UEFI`.
5. Keep boot mode as UEFI.
6. Disable Secure Boot if the firmware refuses to boot the USB.
7. Do not choose a legacy/non-UEFI USB entry.
8. Confirm no Debian installer screen appears. The Neuracoust DSP Server should
   boot directly.

## Boot-Side SSD Install

On first boot, `neuracoust-dsp-firstboot.service` runs:

1. `neuracoust-dsp-hardware-probe.sh`
2. `neuracoust-dsp-install-to-ssd.sh`

The installer writes reports under:

```text
/var/log/neuracoust/dsp-server/
```

and copies the final report to the installed SSD root.

## Firmware And Runtime Updates

The installed DSP server is designed to keep receiving updates after the USB
install. The appliance includes:

```text
/etc/neuracoust-dsp-server/update.conf
/opt/neuracoust/dsp-server/bin/neuracoust-dsp-update.sh
/etc/systemd/system/neuracoust-dsp-update.service
/etc/systemd/system/neuracoust-dsp-update.timer
```

Manual check:

```bash
sudo /opt/neuracoust/dsp-server/bin/neuracoust-dsp-update.sh --check
```

Manual install from a package already copied to the server:

```bash
sudo /opt/neuracoust/dsp-server/bin/neuracoust-dsp-update.sh \
  --apply /path/to/neuracoust-dsp-firmware-260703.2020.tar.gz \
  --sha256 EXPECTED_SHA256
```

Scheduled updates use a manifest URL from `update.conf`. The first package
format is intentionally simple:

```text
firmware.tar.gz
├─ manifest.json
└─ install.sh
```

`install.sh` owns the version-specific update logic, while the updater owns
download, checksum verification, staging, logging, and service restart.

The control surface is intentionally flexible. The same server-side updater can
be triggered by:

- Neuracoust License Agent, after it discovers a DSP server on the local network.
- Neuracoust DAW, from a Monitor DSP / server management panel.
- A future standalone Neuracoust DSP Server Updater app.
- The DSP server itself through `neuracoust-dsp-update.timer`.
- A local USB/offline package copied onto the server.

Those controllers should all use the same package contract instead of each
inventing a different update format. The controller only decides when to update
and which manifest/package URL to use; the DSP server validates and applies the
firmware package locally.

Examples:

```bash
# License Agent / DAW / updater app can point at its chosen manifest.
sudo /opt/neuracoust/dsp-server/bin/neuracoust-dsp-update.sh \
  --check \
  --manifest-url https://neuracoust.tplinkdns.com/api/dsp-server/firmware/stable/latest.json

# A controller can apply a known package URL directly.
sudo /opt/neuracoust/dsp-server/bin/neuracoust-dsp-update.sh \
  --package-url https://neuracoust.tplinkdns.com/downloads/dsp-server/firmware-260703.2030.tar.gz \
  --sha256 EXPECTED_SHA256

# Offline service install from USB or a local maintenance folder.
sudo /opt/neuracoust/dsp-server/bin/neuracoust-dsp-update.sh \
  --apply /media/NEURACOUST/firmware/firmware-260703.2030.tar.gz \
  --sha256 EXPECTED_SHA256
```

## Debian Image Builder

Build the default Debian 12/bookworm compatibility appliance:

```zsh
scripts/build-dsp-server-appliance-iso.command
```

The builder uses Debian `live-build` inside Docker and includes:

- `linux-image-rt-amd64`
- systemd, NetworkManager, OpenSSH
- CPU/network realtime tuning services
- Wake-on-LAN service
- hardware probe and SSD self-install scripts
- the extracted production RT engine:
  `/opt/neuracoust/rt_engine/neuracoust-rt-engine`
- the extracted production module:
  `/opt/neuracoust/rt_engine/modules/na_4001e.so`

The output path is written to:

```text
dist/latest-dsp-server-appliance-image.txt
```
