@echo off
call "E:\VS Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
cl /nologo /W4 /EHsc /utf-8 /std:c++14 /I sdk tests\offline_checks.cpp /Fotests\offline_checks.obj /Fetests\offline_checks.exe
if errorlevel 1 exit /b 1
tests\offline_checks.exe
