@echo off
setlocal
set "exe=%~dp0bin\x64\Release\a0_open_loop_handover\ZRForceOpenLoopHandover.exe"
if not exist "%exe%" (
    echo Build x64 Release with EntryPoint=a0_open_loop_handover first.
    pause
    exit /b 1
)
"%exe%" --commission
set "rc=%errorlevel%"
echo Exit code: %rc%
pause
exit /b %rc%
