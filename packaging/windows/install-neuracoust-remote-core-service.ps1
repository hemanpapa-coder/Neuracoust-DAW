param(
    [string]$InstallDir = "$env:ProgramFiles\Neuracoust\Neuracoust DAW",
    [string]$ModuleId = "na.neuracoust.4001e",
    [string]$ModuleName = "Neuracoust4001ERemoteCore",
    [string]$Vst3Path = "C:\Program Files\Common Files\VST3\Newacoust4001E.vst3",
    [int]$Port = 20000,
    [int]$StatusPort = 20001
)

$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdministrator)) {
    throw "Administrator privileges are required to install the Remote Core scheduled task and firewall rule."
}

$script = Join-Path $InstallDir "start-neuracoust-remote-core.ps1"
$server = Join-Path $InstallDir "neuracoust_remote_core_server.exe"
if (-not (Test-Path $script)) {
    throw "Remote Core start script not found: $script"
}
if (-not (Test-Path $server)) {
    throw "Remote Core server not found: $server"
}

$taskName = "Neuracoust Remote Core"
$argumentParts = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$script`"",
    "-BuildDir", "`"$InstallDir`"",
    "-ModuleId", "`"$ModuleId`"",
    "-ModuleName", "`"$ModuleName`"",
    "-Port", "$Port",
    "-StatusPort", "$StatusPort"
)
if ($Vst3Path -ne "" -and (Test-Path $Vst3Path)) {
    $argumentParts += @("-Vst3Path", "`"$Vst3Path`"", "-Vst3Name", "Newacoust4001E")
}

$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument ($argumentParts -join " ") -WorkingDirectory $InstallDir
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null

New-NetFirewallRule -DisplayName "Neuracoust Remote Core UDP Ports" -Direction Inbound -Action Allow -Protocol UDP -LocalPort $Port,$StatusPort -Profile Any -ErrorAction SilentlyContinue | Out-Null
Start-ScheduledTask -TaskName $taskName

Write-Host "Installed and started $taskName on UDP $Port/$StatusPort"
