@echo off
call "E:\VS Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
cl /nologo /W4 /EHsc /utf-8 /std:c++14 /I sdk tests\zero_mode_offline.cpp /Fotests\zero_mode_offline.obj /Fetests\zero_mode_offline.exe
if errorlevel 1 exit /b 1
tests\zero_mode_offline.exe
