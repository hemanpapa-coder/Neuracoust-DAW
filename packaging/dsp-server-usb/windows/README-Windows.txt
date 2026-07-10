Neuracoust DSP Server USB Maker for Windows
Copyright (C) 2026 Neuracoust. All rights reserved.

This package is an administrator-only utility for creating a Neuracoust DSP
Server boot USB on Windows.

What it does
------------
1. Shows only safe USB candidates from 4 GB to 32 GB.
2. Refuses boot/system disks and non-USB disks.
3. Formats the selected USB as a single MBR/FAT32 UEFI boot disk.
4. Copies the Debian minimal Neuracoust DSP Server appliance files.
5. Verifies EFI boot files, GRUB config, Linux kernel, initrd, and squashfs.
6. Writes a detailed log under:

   %LOCALAPPDATA%\Neuracoust\DSPUSBMaker\Logs

Quick start
-----------
1. Right-click Start-NeuracoustDspUsbMaker.cmd.
2. Choose "Run as administrator".
3. First list the detected safe USB disks:

   powershell -ExecutionPolicy Bypass -File .\Write-NeuracoustDspUsb.ps1 -List

4. Create the USB:

   powershell -ExecutionPolicy Bypass -File .\Write-NeuracoustDspUsb.ps1 `
     -DiskNumber 3 `
     -ImagePath .\appliance-images\Neuracoust_DSP_Server_Debian_bookworm_Appliance_260704.1340.iso `
     -ConfirmErase ERASE

Replace DiskNumber 3 with the safe USB disk number shown by -List.

Target PC boot
--------------
1. Insert the USB into the DSP server PC.
2. Power on the PC and open the boot menu or firmware setup.
3. For many Samsung desktop PCs, try F10 for the boot menu and F2 for BIOS/UEFI
   setup. Some models use Esc, F8, or Del.
4. Select the entry that starts with UEFI and the USB name.
5. If Secure Boot blocks the USB, disable Secure Boot and keep boot mode as UEFI.
6. Do not choose a legacy/non-UEFI USB entry.

Expected result
---------------
The PC should not show a Debian installer. It should boot directly into the
Neuracoust DSP Server live appliance, start the DSP service, probe hardware,
and, when a safe internal SSD is found, install itself to that SSD for the next
boot.

Safety note
-----------
The booted appliance is designed to install only to an internal non-USB SSD or
NVMe disk of at least 32 GB. External USB disks and large development drives are
excluded and logged.
