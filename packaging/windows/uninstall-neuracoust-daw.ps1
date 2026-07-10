param(
    [string]$InstallDir = "$env:ProgramFiles\Neuracoust\Neuracoust DAW"
)

$ErrorActionPreference = "Stop"

$startMenuShortcut = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Neuracoust\Neuracoust DAW.lnk"
$managerShortcut = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Neuracoust\Neuracoust DSP Manager.lnk"
$remoteCoreShortcut = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Neuracoust\Neuracoust Remote Core.lnk"
$remoteCoreServiceShortcut = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Neuracoust\Install Neuracoust Remote Core Service.lnk"
$desktopShortcut = Join-Path ([Environment]::GetFolderPath("CommonDesktopDirectory")) "Neuracoust DAW.lnk"

Remove-Item -Force $startMenuShortcut -ErrorAction SilentlyContinue
Remove-Item -Force $managerShortcut -ErrorAction SilentlyContinue
Remove-Item -Force $remoteCoreShortcut -ErrorAction SilentlyContinue
Remove-Item -Force $remoteCoreServiceShortcut -ErrorAction SilentlyContinue
Remove-Item -Force $desktopShortcut -ErrorAction SilentlyContinue

if (Test-Path $InstallDir) {
    Remove-Item -Recurse -Force $InstallDir
}

$startMenuDir = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Neuracoust"
if ((Test-Path $startMenuDir) -and -not (Get-ChildItem $startMenuDir -ErrorAction SilentlyContinue)) {
    Remove-Item -Force $startMenuDir -ErrorAction SilentlyContinue
}

Write-Host "Neuracoust DAW removed from $InstallDir"
