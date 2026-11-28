@echo off
setlocal
set "exe=%~dp0bin\x64\Release\a0_force_guard\ZRForceGuardPilot.exe"
if not exist "%exe%" (
    echo Build x64 Release with EntryPoint=a0_force_guard first.
    pause
    exit /b 1
)
"%exe%" --mapping
set "rc=%errorlevel%"
echo Exit code: %rc%
pause
exit /b %rc%
