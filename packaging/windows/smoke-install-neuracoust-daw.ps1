param(
    [Parameter(Mandatory = $true)]
    [string]$InstallerZip,

    [string]$InstallDir = (Join-Path $env:TEMP "NeuracoustDAWInstallSmoke")
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $InstallerZip)) {
    throw "Installer ZIP does not exist: $InstallerZip"
}

$stage = Join-Path $env:TEMP ("NeuracoustDAWInstallerSmoke-" + [Guid]::NewGuid().ToString("N"))
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $InstallDir -ErrorAction SilentlyContinue

try {
    New-Item -ItemType Directory -Force $stage | Out-Null
    Expand-Archive -Force -Path $InstallerZip -DestinationPath $stage

    $installScript = Join-Path $stage "install-neuracoust-daw.ps1"
    $uninstallScript = Join-Path $stage "uninstall-neuracoust-daw.ps1"
    if (-not (Test-Path $installScript)) {
        throw "Install script is missing from installer ZIP."
    }
    if (-not (Test-Path $uninstallScript)) {
        throw "Uninstall script is missing from installer ZIP."
    }

    & $installScript -InstallDir $InstallDir -NoShortcuts

    $requiredInstalled = @(
        "Neuracoust DAW.exe",
        "Neuracoust DSP Manager.exe",
        "bundled_plugins.json",
        "license_policy.json",
        "uninstall-neuracoust-daw.ps1"
    )
    foreach ($file in $requiredInstalled) {
        $path = Join-Path $InstallDir $file
        if (-not (Test-Path $path)) {
            throw "Installed payload is missing $file."
        }
    }

    & (Join-Path $InstallDir "uninstall-neuracoust-daw.ps1") -InstallDir $InstallDir
    if (Test-Path $InstallDir) {
        throw "Uninstall left the smoke install directory behind: $InstallDir"
    }

    Write-Host "Windows installer smoke passed for $InstallerZip"
} finally {
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $InstallDir -ErrorAction SilentlyContinue
}
