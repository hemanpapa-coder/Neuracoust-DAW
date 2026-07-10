param(
    [int]$DiskNumber = -1,
    [string]$ImagePath = "",
    [string]$ConfirmErase = $env:NEURACOUST_CONFIRM_ERASE,
    [switch]$List,
    [switch]$ValidateOnly
)

$ErrorActionPreference = "Stop"
$MinimumBytes = [int64]4GB
$MaximumBytes = [int64]32GB
$LogRoot = Join-Path $env:LOCALAPPDATA "Neuracoust\DSPUSBMaker\Logs"
New-Item -ItemType Directory -Force -Path $LogRoot | Out-Null
$LogPath = Join-Path $LogRoot ("write-{0}.log" -f (Get-Date -Format "yyyyMMdd-HHmmss"))

function Write-Log {
    param([string]$Message)
    $line = "{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    $line | Tee-Object -FilePath $LogPath -Append
}

function Get-NeuracoustUsbCandidate {
    Get-Disk | Where-Object {
        $_.BusType -eq "USB" -and
        -not $_.IsBoot -and
        -not $_.IsSystem -and
        $_.Size -ge $MinimumBytes -and
        $_.Size -le $MaximumBytes
    } | Sort-Object Number
}

function Show-Candidates {
    $candidates = @(Get-NeuracoustUsbCandidate)
    if ($candidates.Count -eq 0) {
        Write-Log "No safe USB candidates found. Only USB disks from 4 GB to 32 GB are shown."
        return
    }
    Write-Log "Safe USB candidates:"
    $candidates | ForEach-Object {
        Write-Log ("  Disk {0}: {1}, {2:N2} GB, BusType={3}, PartitionStyle={4}" -f $_.Number, $_.FriendlyName, ($_.Size / 1GB), $_.BusType, $_.PartitionStyle)
    }
}

function Test-Admin {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-IsoMountRoot {
    param([string]$Path)
    $image = Mount-DiskImage -ImagePath $Path -PassThru
    Start-Sleep -Seconds 2
    $volume = $image | Get-Volume | Where-Object DriveLetter | Select-Object -First 1
    if (-not $volume) {
        Dismount-DiskImage -ImagePath $Path -ErrorAction SilentlyContinue
        throw "Mounted image has no drive letter: $Path"
    }
    return ("{0}:\" -f $volume.DriveLetter)
}

function Find-Fat32Volume {
    param([int]$Number)
    for ($i = 0; $i -lt 30; $i++) {
        $vol = Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue |
            Get-Volume -ErrorAction SilentlyContinue |
            Where-Object { $_.FileSystemLabel -eq "NEURACOUST" -and $_.DriveLetter } |
            Select-Object -First 1
        if ($vol) {
            return ("{0}:\" -f $vol.DriveLetter)
        }
        Start-Sleep -Seconds 1
    }
    throw "Could not find mounted NEURACOUST FAT32 volume on disk $Number."
}

function Copy-TreeWithProgress {
    param(
        [string]$SourceRoot,
        [string]$TargetRoot,
        [int64]$TotalBytes
    )
    $files = @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -Force -File)
    $copied = [int64]0
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($SourceRoot.Length).TrimStart("\")
        $dest = Join-Path $TargetRoot $relative
        $destDir = Split-Path -Parent $dest
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $dest -Force
        $copied += $file.Length
        if ($TotalBytes -gt 0) {
            $percent = [Math]::Min(95, [Math]::Floor(($copied * 100.0) / $TotalBytes))
            Write-Progress -Activity "Creating Neuracoust DSP Server USB" -Status "$percent%" -PercentComplete $percent
            Write-Log ("COPY_PROGRESS_BYTES={0} TOTAL_BYTES={1}" -f $copied, $TotalBytes)
        }
    }
}

function Repair-LiveSymlinkCopies {
    param([string]$UsbRoot)
    $grubPath = Join-Path $UsbRoot "boot\grub\grub.cfg"
    if (-not (Test-Path -LiteralPath $grubPath)) {
        return
    }
    $grub = Get-Content -LiteralPath $grubPath -Raw
    $kernelMatches = [regex]::Matches($grub, "/live/(vmlinuz-[^\s`"']+)")
    $initrdMatches = [regex]::Matches($grub, "/live/(initrd\.img-[^\s`"']+)")
    foreach ($match in $kernelMatches) {
        $target = Join-Path $UsbRoot ("live\" + $match.Groups[1].Value)
        $generic = Join-Path $UsbRoot "live\vmlinuz"
        if ((-not (Test-Path -LiteralPath $target)) -and (Test-Path -LiteralPath $generic)) {
            Write-Log ("Creating FAT32 kernel copy: /live/{0}" -f $match.Groups[1].Value)
            Copy-Item -LiteralPath $generic -Destination $target -Force
        }
    }
    foreach ($match in $initrdMatches) {
        $target = Join-Path $UsbRoot ("live\" + $match.Groups[1].Value)
        $generic = Join-Path $UsbRoot "live\initrd.img"
        if ((-not (Test-Path -LiteralPath $target)) -and (Test-Path -LiteralPath $generic)) {
            Write-Log ("Creating FAT32 initrd copy: /live/{0}" -f $match.Groups[1].Value)
            Copy-Item -LiteralPath $generic -Destination $target -Force
        }
    }
}

function Test-BootFiles {
    param([string]$UsbRoot)
    $required = @(
        "EFI\BOOT\BOOTX64.EFI",
        "boot\grub\grub.cfg",
        "live\filesystem.squashfs",
        "live\vmlinuz",
        "live\initrd.img"
    )
    foreach ($item in $required) {
        $path = Join-Path $UsbRoot $item
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Copied USB is missing required boot file: /$($item -replace '\\','/')"
        }
    }
    $grubPath = Join-Path $UsbRoot "boot\grub\grub.cfg"
    $grub = Get-Content -LiteralPath $grubPath -Raw
    foreach ($match in [regex]::Matches($grub, "/live/(vmlinuz-[^\s`"']+|initrd\.img-[^\s`"']+)")) {
        $path = Join-Path $UsbRoot ("live\" + $match.Groups[1].Value)
        if (-not (Test-Path -LiteralPath $path)) {
            throw "GRUB references missing live file: /live/$($match.Groups[1].Value)"
        }
    }
    Write-Log "UEFI boot file verification passed."
}

Write-Log "===== Neuracoust DSP Server USB Maker for Windows ====="
Write-Log ("Log file: {0}" -f $LogPath)
Write-Log ("User: {0}" -f [Security.Principal.WindowsIdentity]::GetCurrent().Name)
Write-Log ("Windows: {0}" -f (Get-CimInstance Win32_OperatingSystem).Caption)

if ($List) {
    Show-Candidates
    exit 0
}

if ($DiskNumber -lt 0 -or [string]::IsNullOrWhiteSpace($ImagePath)) {
    Show-Candidates
    throw "Usage: .\Write-NeuracoustDspUsb.ps1 -List OR .\Write-NeuracoustDspUsb.ps1 -DiskNumber N -ImagePath C:\path\image.iso -ConfirmErase ERASE"
}

if (-not (Test-Admin)) {
    throw "Run this PowerShell script as Administrator."
}

if (-not (Test-Path -LiteralPath $ImagePath)) {
    throw "Image not found: $ImagePath"
}

$imageItem = Get-Item -LiteralPath $ImagePath
$disk = Get-Disk -Number $DiskNumber
Write-Log ("Selected disk: {0}, {1}, {2:N2} GB, BusType={3}, IsBoot={4}, IsSystem={5}" -f $disk.Number, $disk.FriendlyName, ($disk.Size / 1GB), $disk.BusType, $disk.IsBoot, $disk.IsSystem)
Write-Log ("Image: {0}" -f $imageItem.FullName)
Write-Log ("Image bytes: {0}" -f $imageItem.Length)

$safeNumbers = @(Get-NeuracoustUsbCandidate | ForEach-Object Number)
if ($safeNumbers -notcontains $DiskNumber) {
    Show-Candidates
    throw "Refusing Disk $DiskNumber. Only non-boot USB disks from 4 GB to 32 GB are allowed."
}

if ($ValidateOnly) {
    Write-Log "Validation only; no write was performed."
    exit 0
}

if ($ConfirmErase -ne "ERASE") {
    throw "Refusing to erase a disk because NEURACOUST_CONFIRM_ERASE/ConfirmErase is not ERASE."
}

$isoRoot = $null
try {
    Write-Log "Formatting USB as single MBR/FAT32 partition."
    Set-Disk -Number $DiskNumber -IsReadOnly $false
    Set-Disk -Number $DiskNumber -IsOffline $false
    Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Partition -DiskNumber $DiskNumber -PartitionNumber $_.PartitionNumber -Confirm:$false
    }
    Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false
    Initialize-Disk -Number $DiskNumber -PartitionStyle MBR
    $partition = New-Partition -DiskNumber $DiskNumber -UseMaximumSize -AssignDriveLetter -IsActive
    Format-Volume -Partition $partition -FileSystem FAT32 -NewFileSystemLabel "NEURACOUST" -Confirm:$false -Force | Out-Null
    $usbRoot = Find-Fat32Volume -Number $DiskNumber
    Write-Log ("USB mounted at: {0}" -f $usbRoot)

    Write-Log "Mounting appliance ISO."
    $isoRoot = Get-IsoMountRoot -Path $imageItem.FullName
    Write-Log ("ISO mounted at: {0}" -f $isoRoot)

    Write-Log "Copying ISO contents to FAT32 UEFI USB."
    Copy-TreeWithProgress -SourceRoot $isoRoot -TargetRoot $usbRoot -TotalBytes $imageItem.Length
    Repair-LiveSymlinkCopies -UsbRoot $usbRoot
    Test-BootFiles -UsbRoot $usbRoot
    Write-Progress -Activity "Creating Neuracoust DSP Server USB" -Completed
    Write-Log "Neuracoust DSP Server USB file-copy complete."
    Write-Log "Safe to remove the USB after Windows finishes any pending device activity."
}
finally {
    if ($isoRoot) {
        Dismount-DiskImage -ImagePath $imageItem.FullName -ErrorAction SilentlyContinue
    }
}
