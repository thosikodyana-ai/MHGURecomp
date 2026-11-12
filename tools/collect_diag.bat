@echo off
setlocal
echo [MHGURecomp] Collecting logs, minidumps, config, and system info (excluding game ROM and saves)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0collect_diag.ps1"
if exist "%~dp0..\collect_diag.ps1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\collect_diag.ps1"
)
echo Done. Send the generated .zip file inside the diag\ folder to the developers.
if "%MHGU_SELFTEST%"=="" pause
