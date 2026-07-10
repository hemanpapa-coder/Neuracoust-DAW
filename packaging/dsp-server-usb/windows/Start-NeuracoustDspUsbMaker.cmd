@echo off
setlocal
cd /d "%~dp0"
echo Neuracoust DSP Server USB Maker for Windows
echo Copyright (C) 2026 Neuracoust. All rights reserved.
echo.
echo This tool must run as Administrator.
echo.
echo Safe USB candidates:
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Write-NeuracoustDspUsb.ps1" -List
echo.
echo To create a USB, run:
echo powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Write-NeuracoustDspUsb.ps1" -DiskNumber N -ImagePath "%~dp0appliance-images\IMAGE.iso" -ConfirmErase ERASE
echo.
pause
