@echo off
setlocal
cd /d "%~dp0"
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
if /i not "%ARCH%"=="x64" if /i not "%ARCH%"=="x86" exit /b 2
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio C++ tools not found.
  exit /b 2
)
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT exit /b 2
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH%
if errorlevel 1 exit /b 1
if not exist "bin\%ARCH%" mkdir "bin\%ARCH%"
cl /nologo /W4 /EHsc /utf-8 /std:c++14 /MD /I sdk main.cpp /Fo"bin\%ARCH%\main.obj" /Fe"bin\%ARCH%\OpenLoopApiReadTest.exe" /link "sdk\%ARCH%\gts.lib"
if errorlevel 1 exit /b 1
copy /y "sdk\%ARCH%\gts.dll" "bin\%ARCH%\gts.dll" >nul
if errorlevel 1 exit /b 1
copy /y "Googol.xml" "bin\%ARCH%\Googol.xml" >nul
if errorlevel 1 exit /b 1
copy /y "Gecat.xml" "bin\%ARCH%\Gecat.xml" >nul
if errorlevel 1 exit /b 1
echo Build complete. Hardware test was NOT executed.
