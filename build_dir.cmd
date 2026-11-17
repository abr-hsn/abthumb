@echo off
rem Usage: build_dir.cmd <build-dir-name> [target]   (separate build trees for parallel work)
setlocal
if not defined VSCMD_VER (
  for /f "usebackq tokens=*" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
)
if not defined VSCMD_VER call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo || exit /b 1
set "B=%~dp0build\%~1"
if not exist "%B%\build.ninja" (
  cmake -S "%~dp0." -B "%B%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl || exit /b 1
)
if "%~2"=="" (cmake --build "%B%") else (cmake --build "%B%" --target %~2)
