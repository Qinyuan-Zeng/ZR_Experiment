@echo off
setlocal
title ZRA Watch - Passive Motion
set "probe=%~dp0bin\x64\Release\probe_hs_capture\ZRInertiaCurrent.exe"
if not exist "%probe%" (
    echo Build ZRInertiaCurrent in Release x64 first.
    pause
    exit /b 1
)
"%probe%" --passive-motion
set "probe_status=%errorlevel%"
echo Watch closed with exit code %probe_status%.
pause
exit /b %probe_status%
