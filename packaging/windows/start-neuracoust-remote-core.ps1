param(
    [string]$BuildDir = "$PSScriptRoot\..\..\build\dev",
    [string]$Bind = "0.0.0.0",
    [int]$Port = 20000,
    [int]$StatusPort = 20001,
    [string]$ModuleId = "na.neuracoust.monitor.speaker",
    [string]$ModuleName = "Neuracoust Monitor Speaker Remote Core",
    [string]$ModulePath = "",
    [string]$Vst3Path = "",
    [string]$Vst3Name = "",
    [string]$Vst3ClassId = "",
    [string]$Vst3ClassName = "",
    [double]$Gain = 1.0,
    [double]$SampleRate = 48000
)

$ErrorActionPreference = "Stop"
$RootDir = Resolve-Path "$PSScriptRoot\..\.."
$Server = Join-Path $BuildDir "neuracoust_remote_core_server.exe"

$argsList = @(
    "--bind", $Bind,
    "--port", "$Port",
    "--status-port", "$StatusPort",
    "--module-id", $ModuleId,
    "--module-name", $ModuleName,
    "--gain", "$Gain",
    "--sample-rate", "$SampleRate"
)

if ($ModulePath -ne "") {
    $argsList += @("--module-path", $ModulePath)
}
if ($Vst3Path -ne "") {
    $hostedName = if ($Vst3Name -ne "") { $Vst3Name } else { $ModuleName }
    $argsList += @("--vst3-path", $Vst3Path, "--vst3-name", $hostedName)
}
if ($Vst3ClassId -ne "") {
    $argsList += @("--vst3-class-id", $Vst3ClassId)
}
if ($Vst3ClassName -ne "") {
    $argsList += @("--vst3-class-name", $Vst3ClassName)
}

if (!(Test-Path $Server)) {
    cmake -S $RootDir -B $BuildDir
    cmake --build $BuildDir --target neuracoust_remote_core_server --config Release
}

& $Server @argsList
