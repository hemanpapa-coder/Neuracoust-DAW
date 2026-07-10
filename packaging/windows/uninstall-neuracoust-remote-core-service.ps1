param()

$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdministrator)) {
    throw "Administrator privileges are required to remove the Remote Core scheduled task and firewall rule."
}

$taskName = "Neuracoust Remote Core"
if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}
Get-NetFirewallRule -DisplayName "Neuracoust Remote Core UDP Ports" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Get-Process neuracoust_remote_core_server -ErrorAction SilentlyContinue | Stop-Process -Force

Write-Host "Removed $taskName"
