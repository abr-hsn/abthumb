@echo off
rem Usage: build.cmd [release^|debug^|fuzz] [extra cmake --build args]
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"
if defined VSCMD_VER goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found: install Visual Studio with the C++ workload & exit /b 1)
set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo || exit /b 1
:build
pushd "%~dp0"
if not exist "build\%PRESET%\build.ninja" (
  cmake --preset %PRESET% || (popd & exit /b 1)
)
cmake --build "build\%PRESET%" %2 %3 %4
set ERR=%ERRORLEVEL%
popd
exit /b %ERR%