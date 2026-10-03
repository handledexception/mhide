@echo off
setlocal enabledelayedexpansion
rem Builds build\mhide.exe with MSVC. Requires Visual Studio 2022 (or Build Tools) with the C++ workload.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo vswhere.exe not found. Install Visual Studio 2022 Build Tools with the C++ workload.
    exit /b 1
)
for /f "tokens=*" %%i in ('"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VSPATH=%%i"
if not defined VSPATH (
    echo No Visual Studio installation with the C++ toolset was found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 exit /b 1

if not exist build mkdir build

rc /nologo /fo build\mhide.res src\mhide.rc
if errorlevel 1 exit /b 1

cl /nologo /O1 /W4 /WX /MT /GS /guard:cf /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /Fo:build\ /Fe:build\mhide.exe src\main.c build\mhide.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /DYNAMICBASE /NXCOMPAT /guard:cf ^
   user32.lib shell32.lib advapi32.lib gdi32.lib comctl32.lib
if errorlevel 1 exit /b 1

echo Built build\mhide.exe
