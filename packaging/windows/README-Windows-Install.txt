Neuracoust DAW Windows installer package

Install:
1. Extract this ZIP.
2. Open PowerShell as Administrator.
3. Run:
   powershell -ExecutionPolicy Bypass -File .\install-neuracoust-daw.ps1

Default install path:
  C:\Program Files\Neuracoust\Neuracoust DAW

DSP Manager:
  After install, launch "Neuracoust DSP Manager" from the Start Menu to inspect
  Windows/Mac Remote Core discovery, RTT, buffer, jitter, and DSP engine state.

Remote Core:
  The installer also copies neuracoust_remote_core_server.exe and
  start-neuracoust-remote-core.ps1. After install, launch
  "Neuracoust Remote Core" from the Start Menu to use this Windows computer as
  a LAN DSP core for another Neuracoust DAW computer.

Remote Core startup service:
  After installation, run "Install Neuracoust Remote Core Service" from the
  Start Menu as Administrator, or run:
    powershell -ExecutionPolicy Bypass -File .\install-neuracoust-remote-core-service.ps1

  This creates a startup scheduled task under SYSTEM and opens inbound UDP
  ports 20000 and 20001 in Windows Firewall. To remove it:
    powershell -ExecutionPolicy Bypass -File .\uninstall-neuracoust-remote-core-service.ps1

Hosted Neuracoust VST3 Remote Core:
  powershell -ExecutionPolicy Bypass -File .\start-neuracoust-remote-core.ps1 `
    -ModuleId "na.neuracoust.4001e" `
    -ModuleName "Neuracoust 4001E Remote Core" `
    -Vst3Path "C:\Program Files\Common Files\VST3\Newacoust4001E.vst3" `
    -Vst3Name "Newacoust4001E"

Uninstall:
  powershell -ExecutionPolicy Bypass -File .\uninstall-neuracoust-daw.ps1

For development or non-admin testing, pass a user-writable install path:
  powershell -ExecutionPolicy Bypass -File .\install-neuracoust-daw.ps1 -InstallDir "$env:LOCALAPPDATA\Neuracoust\Neuracoust DAW" -NoShortcuts
