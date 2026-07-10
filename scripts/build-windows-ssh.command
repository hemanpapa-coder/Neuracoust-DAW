#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
neuracoust_date_version() {
  printf '%s%02d\n' "$(date '+%y%m%d.%H')" "$((10#$(date '+%M') / 10 * 10))"
}
VERSION="${1:-$(neuracoust_date_version)}"
SSH_HOST="${NEURACOUST_WINDOWS_SSH:-windows11-server}"
ARCH="${NEURACOUST_WINDOWS_ARCH:-x64}"
REMOTE_ROOT="${NEURACOUST_WINDOWS_REMOTE_ROOT:-C:/NeuracoustBuild/DAW}"
REMOTE_BUILD="$REMOTE_ROOT/build/windows-$ARCH"
REMOTE_CMAKE="${NEURACOUST_WINDOWS_CMAKE:-}"
DIST_DIR="$ROOT_DIR/dist"
REMOTE_ZIP="$REMOTE_ROOT/dist/Neuracoust_DAW_Windows_${ARCH}_${VERSION}_app.zip"
LOCAL_ZIP="$DIST_DIR/Neuracoust_DAW_Windows_${ARCH}_${VERSION}_app.zip"
REMOTE_INSTALLER_ZIP="$REMOTE_ROOT/dist/Neuracoust_DAW_Windows_${ARCH}_${VERSION}_installer.zip"
LOCAL_INSTALLER_ZIP="$DIST_DIR/Neuracoust_DAW_Windows_${ARCH}_${VERSION}_installer.zip"

case "$ARCH" in
  x64)
    CMAKE_ARCH="x64"
    ;;
  x86|Win32)
    CMAKE_ARCH="Win32"
    ;;
  arm64|ARM64)
    CMAKE_ARCH="ARM64"
    ;;
  *)
    echo "Unsupported NEURACOUST_WINDOWS_ARCH '$ARCH'. Use x64, x86, or arm64." >&2
    exit 2
    ;;
esac

mkdir -p "$DIST_DIR"

echo "==> Preparing Windows build folder on $SSH_HOST"
ssh "$SSH_HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"if (Test-Path '$REMOTE_ROOT') { cmd /c attrib -R '$REMOTE_ROOT\\*' /S /D | Out-Null; Remove-Item -Recurse -Force '$REMOTE_ROOT' -ErrorAction SilentlyContinue }; New-Item -ItemType Directory -Force '$REMOTE_ROOT' | Out-Null; New-Item -ItemType Directory -Force '$REMOTE_ROOT/dist' | Out-Null\""

echo "==> Copying source tree"
(
  cd "$ROOT_DIR"
  COPYFILE_DISABLE=1 tar \
    --no-xattrs \
    --exclude './build' \
    --exclude './dist' \
    --exclude './.git' \
    --exclude './.codex_tmp' \
    --exclude './packaging/dsp-server-usb/payload' \
    --exclude './packaging/dsp-server-usb/server-extract' \
    --exclude './third_party/depot_tools' \
    --exclude './third_party/libwebrtc-src' \
    --exclude './third_party/vst3sdk/.git' \
    -czf - .
) | ssh "$SSH_HOST" "tar -xzf - -C \"$REMOTE_ROOT\""

echo "==> Configuring Windows app/core build ($ARCH, version $VERSION)"
ssh "$SSH_HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"\$cmakeCandidates = @('$REMOTE_CMAKE', 'cmake', 'C:/Program Files/CMake/bin/cmake.exe', 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe', 'C:/Program Files/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe', 'C:/Program Files/Microsoft Visual Studio/18/Insiders/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'); \$cmake = \$cmakeCandidates | Where-Object { \$_ -and (Get-Command \$_ -ErrorAction SilentlyContinue) } | Select-Object -First 1; if (-not \$cmake) { throw 'CMake not found. Install CMake or set NEURACOUST_WINDOWS_CMAKE.' }; & \$cmake -S '$REMOTE_ROOT' -B '$REMOTE_BUILD' -A '$CMAKE_ARCH' -DNEURACOUST_DAW_VERSION='$VERSION'\""

echo "==> Building and testing Windows app/core"
ssh "$SSH_HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"\$cmakeCandidates = @('$REMOTE_CMAKE', 'cmake', 'C:/Program Files/CMake/bin/cmake.exe', 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe', 'C:/Program Files/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe', 'C:/Program Files/Microsoft Visual Studio/18/Insiders/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'); \$cmake = \$cmakeCandidates | Where-Object { \$_ -and (Get-Command \$_ -ErrorAction SilentlyContinue) } | Select-Object -First 1; if (-not \$cmake) { throw 'CMake not found. Install CMake or set NEURACOUST_WINDOWS_CMAKE.' }; \$ctest = Join-Path (Split-Path \$cmake) 'ctest.exe'; if (!(Test-Path \$ctest)) { \$ctest = 'ctest' }; & \$cmake --build '$REMOTE_BUILD' --config Release --target NeuracoustDAW NeuracoustDspManager neuracoust_remote_core_server neuracoust_video_render_ffmpeg neuracoust_daw_core_tests neuracoust_daw_remote_dsp_tests neuracoust_daw_audio_tests neuracoust_daw_desktop_wav_workflow_tests neuracoust_dsp_manager_tests neuracoust_native_webrtc_smoke neuracoust_vst3_host_audit neuracoust_vst3_process_worker neuracoust_vst3_isolated_process_smoke; if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }; & '$REMOTE_BUILD/Release/neuracoust_remote_core_server.exe' --self-test; if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }; & \$ctest --test-dir '$REMOTE_BUILD' -C Release --output-on-failure\""

echo "==> Packaging Windows app validation artifact"
ssh "$SSH_HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Remove-Item -Force '$REMOTE_ZIP' -ErrorAction SilentlyContinue; Compress-Archive -Force -Path '$REMOTE_BUILD/Release/Neuracoust DAW.exe','$REMOTE_BUILD/Release/Neuracoust DSP Manager.exe','$REMOTE_BUILD/Release/neuracoust_remote_core_server.exe','$REMOTE_BUILD/Release/neuracoust_video_render_ffmpeg.exe','$REMOTE_BUILD/Release/bundled_plugins.json','$REMOTE_BUILD/Release/license_policy.json','$REMOTE_BUILD/Release/neuracoust_daw_core_tests.exe','$REMOTE_BUILD/Release/neuracoust_daw_audio_tests.exe','$REMOTE_BUILD/Release/neuracoust_daw_desktop_wav_workflow_tests.exe','$REMOTE_ROOT/packaging/windows/start-neuracoust-remote-core.ps1','$REMOTE_ROOT/packaging/windows/install-neuracoust-remote-core-service.ps1','$REMOTE_ROOT/packaging/windows/uninstall-neuracoust-remote-core-service.ps1','$REMOTE_ROOT/docs/PRODUCT_BRIEF.md','$REMOTE_ROOT/docs/ARCHITECTURE.md','$REMOTE_ROOT/docs/REMOTE_CORE_ROADMAP.md' -DestinationPath '$REMOTE_ZIP'\""

echo "==> Packaging Windows install ZIP"
ssh "$SSH_HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Remove-Item -Force '$REMOTE_INSTALLER_ZIP' -ErrorAction SilentlyContinue; \$stage = '$REMOTE_ROOT/dist/installer-stage-$VERSION-$ARCH'; Remove-Item -Recurse -Force \$stage -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force \$stage | Out-Null; Copy-Item -Force '$REMOTE_BUILD/Release/Neuracoust DAW.exe' \$stage; Copy-Item -Force '$REMOTE_BUILD/Release/Neuracoust DSP Manager.exe' \$stage; Copy-Item -Force '$REMOTE_BUILD/Release/neuracoust_remote_core_server.exe' \$stage; Copy-Item -Force '$REMOTE_BUILD/Release/neuracoust_video_render_ffmpeg.exe' \$stage; Copy-Item -Force '$REMOTE_BUILD/Release/bundled_plugins.json' \$stage; Copy-Item -Force '$REMOTE_BUILD/Release/license_policy.json' \$stage; Copy-Item -Force '$REMOTE_ROOT/packaging/windows/start-neuracoust-remote-core.ps1' \$stage; Copy-Item -Force '$REMOTE_ROOT/packaging/windows/install-neuracoust-remote-core-service.ps1' \$stage; Copy-Item -Force '$REMOTE_ROOT/packaging/windows/uninstall-neuracoust-remote-core-service.ps1' \$stage; Copy-Item -Force '$REMOTE_ROOT/packaging/windows/install-neuracoust-daw.ps1' \$stage; Copy-Item -Force '$REMOTE_ROOT/packaging/windows/uninstall-neuracoust-daw.ps1' \$stage; Copy-Item -Force '$REMOTE_ROOT/packaging/windows/README-Windows-Install.txt' \$stage; Compress-Archive -Force -Path (Join-Path \$stage '*') -DestinationPath '$REMOTE_INSTALLER_ZIP'; Remove-Item -Recurse -Force \$stage -ErrorAction SilentlyContinue\""

echo "==> Smoke-testing Windows installer ZIP"
ssh "$SSH_HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"& '$REMOTE_ROOT/packaging/windows/smoke-install-neuracoust-daw.ps1' -InstallerZip '$REMOTE_INSTALLER_ZIP' -InstallDir \\\"\$env:TEMP\\NeuracoustDAWInstallSmoke-$VERSION\\\"; if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }\""

echo "==> Copying artifact back to Mac"
scp "$SSH_HOST:$REMOTE_ZIP" "$LOCAL_ZIP"
scp "$SSH_HOST:$REMOTE_INSTALLER_ZIP" "$LOCAL_INSTALLER_ZIP"
echo "$LOCAL_ZIP" > "$DIST_DIR/latest-windows-app-zip.txt"
echo "$LOCAL_ZIP" > "$DIST_DIR/latest-windows-core-zip.txt"
echo "$LOCAL_INSTALLER_ZIP" > "$DIST_DIR/latest-windows-installer-zip.txt"

cat <<MSG
Windows app build complete.
artifact: $LOCAL_ZIP
installer: $LOCAL_INSTALLER_ZIP

The installer ZIP contains install/uninstall PowerShell scripts and the app
payload. The installer ZIP was smoke-tested on the Windows build machine. A
signed EXE/MSI installer remains future release work.
MSG
