param(
    [string]$InstallDir = "$env:ProgramFiles\Neuracoust\Neuracoust DAW",
    [switch]$NoShortcuts
)

$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-IsUnderPath {
    param(
        [string]$Path,
        [string]$Root
    )

    if (-not $Path -or -not $Root) {
        return $false
    }

    $fullPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    return $fullPath.Equals($fullRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
        $fullPath.StartsWith($fullRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

$requiresAdmin = (Test-IsUnderPath $InstallDir ([Environment]::GetFolderPath("ProgramFiles"))) -or (-not $NoShortcuts)
if ($requiresAdmin -and -not (Test-IsAdministrator)) {
    throw "Administrator privileges are required for Program Files installs or common Start/Desktop shortcuts. Run PowerShell as Administrator, or pass both -InstallDir to a user-writable folder and -NoShortcuts."
}

$sourceDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$required = @(
    "Neuracoust DAW.exe",
    "Neuracoust DSP Manager.exe",
    "neuracoust_remote_core_server.exe",
    "start-neuracoust-remote-core.ps1",
    "install-neuracoust-remote-core-service.ps1",
    "uninstall-neuracoust-remote-core-service.ps1",
    "bundled_plugins.json",
    "license_policy.json"
)

foreach ($file in $required) {
    $path = Join-Path $sourceDir $file
    if (-not (Test-Path $path)) {
        throw "Installer payload is missing $file."
    }
}

New-Item -ItemType Directory -Force $InstallDir | Out-Null
foreach ($file in $required) {
    Copy-Item -Force (Join-Path $sourceDir $file) (Join-Path $InstallDir $file)
}

$uninstallSource = Join-Path $sourceDir "uninstall-neuracoust-daw.ps1"
if (Test-Path $uninstallSource) {
    Copy-Item -Force $uninstallSource (Join-Path $InstallDir "uninstall-neuracoust-daw.ps1")
}

if (-not $NoShortcuts) {
    $shell = New-Object -ComObject WScript.Shell
    $exe = Join-Path $InstallDir "Neuracoust DAW.exe"
    $startMenuDir = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Neuracoust"
    New-Item -ItemType Directory -Force $startMenuDir | Out-Null

    $startShortcut = $shell.CreateShortcut((Join-Path $startMenuDir "Neuracoust DAW.lnk"))
    $startShortcut.TargetPath = $exe
    $startShortcut.WorkingDirectory = $InstallDir
    $startShortcut.Description = "Neuracoust DAW"
    $startShortcut.Save()

    $managerExe = Join-Path $InstallDir "Neuracoust DSP Manager.exe"
    if (Test-Path $managerExe) {
        $managerShortcut = $shell.CreateShortcut((Join-Path $startMenuDir "Neuracoust DSP Manager.lnk"))
        $managerShortcut.TargetPath = $managerExe
        $managerShortcut.WorkingDirectory = $InstallDir
        $managerShortcut.Description = "Neuracoust DSP Manager"
        $managerShortcut.Save()
    }

    $remoteCoreScript = Join-Path $InstallDir "start-neuracoust-remote-core.ps1"
    if (Test-Path $remoteCoreScript) {
        $remoteCoreShortcut = $shell.CreateShortcut((Join-Path $startMenuDir "Neuracoust Remote Core.lnk"))
        $remoteCoreShortcut.TargetPath = "powershell.exe"
        $remoteCoreShortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$remoteCoreScript`" -BuildDir `"$InstallDir`""
        $remoteCoreShortcut.WorkingDirectory = $InstallDir
        $remoteCoreShortcut.Description = "Neuracoust Remote Core DSP server"
        $remoteCoreShortcut.Save()
    }
    $remoteCoreServiceScript = Join-Path $InstallDir "install-neuracoust-remote-core-service.ps1"
    if (Test-Path $remoteCoreServiceScript) {
        $serviceShortcut = $shell.CreateShortcut((Join-Path $startMenuDir "Install Neuracoust Remote Core Service.lnk"))
        $serviceShortcut.TargetPath = "powershell.exe"
        $serviceShortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$remoteCoreServiceScript`" -InstallDir `"$InstallDir`""
        $serviceShortcut.WorkingDirectory = $InstallDir
        $serviceShortcut.Description = "Install Neuracoust Remote Core as a startup scheduled task"
        $serviceShortcut.Save()
    }

    $desktopDir = [Environment]::GetFolderPath("CommonDesktopDirectory")
    if ($desktopDir) {
        $desktopShortcut = $shell.CreateShortcut((Join-Path $desktopDir "Neuracoust DAW.lnk"))
        $desktopShortcut.TargetPath = $exe
        $desktopShortcut.WorkingDirectory = $InstallDir
        $desktopShortcut.Description = "Neuracoust DAW"
        $desktopShortcut.Save()
    }
}

Write-Host "Neuracoust DAW installed to $InstallDir"
