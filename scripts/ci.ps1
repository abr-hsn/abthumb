# Runs the same checks as .github/workflows/ci.yml on this PC:
#   1. release build      2. unit tests (from a temp dir)      3. libFuzzer + ASan smoke run
# It builds in its own trees (build\ci-release, build\ci-fuzz), so it never disturbs build\release
# or another build that is running in the same folder.
#
#   powershell -File scripts\ci.ps1                 full run (60 s per fuzzer, like GitHub)
#   powershell -File scripts\ci.ps1 -Quick          build + unit tests only (the pre-push hook)
#   powershell -File scripts\ci.ps1 -FuzzSeconds 10 shorter fuzz run
#   powershell -File scripts\ci.ps1 -Package        also build the installer (scripts\package.ps1)
param([switch]$Quick, [int]$FuzzSeconds = 60, [switch]$Package)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$t0 = Get-Date

function Step([string]$name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }

# Load the MSVC x64 environment once (same toolset lookup as build.cmd).
if (-not $env:VSCMD_VER) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio with the C++ workload" }
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $vs) { throw "Visual Studio with C++ tools not found" }
  $envDump = cmd /c "`"$vs\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 -no_logo >nul 2>nul && set"
  if ($LASTEXITCODE) { throw "VsDevCmd.bat failed" }
  foreach ($line in $envDump) { if ($line -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] } }
}

function Build([string]$dir, [string[]]$defs) {
  $b = "$root\build\$dir"
  if (-not (Test-Path "$b\build.ninja")) {
    cmake -S $root -B $b -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=RelWithDebInfo @defs
    if ($LASTEXITCODE) { throw "cmake configure failed ($dir)" }
  }
  cmake --build $b
  if ($LASTEXITCODE) { throw "build failed ($dir)" }
}

Step "Build (release)"
Build "ci-release" @()

Step "Unit tests"
# Tests write their output images to the current directory, so run them from a temp dir.
$tmp = Join-Path $env:TEMP "abthumb-ci"
New-Item -ItemType Directory -Force $tmp | Out-Null
Push-Location $tmp
try { & "$root\build\ci-release\bin\abt_tests.exe"; $code = $LASTEXITCODE } finally { Pop-Location }
if ($code) { throw "unit tests failed (exit $code)" }

if (-not $Quick -and $FuzzSeconds -gt 0) {
  Step "Build fuzzers (libFuzzer + ASan)"
  Build "ci-fuzz" @("-DABT_FUZZ=ON")

  Step "Fuzz smoke run ($FuzzSeconds s per fuzzer)"
  # The ASan runtime must come from the same MSVC toolset that built the fuzzers.
  $asan = Join-Path $env:VCToolsInstallDir "bin\Hostx64\x64"
  if (-not (Test-Path "$asan\clang_rt.asan_dynamic-x86_64.dll")) { throw "ASan runtime not found in $asan" }
  $savedPath = $env:PATH
  $env:PATH = "$asan;$env:PATH"
  # libFuzzer logs to stderr; in Windows PowerShell 5.1 a redirected stderr line would be a
  # terminating error under "Stop", so rely on the exit code here instead.
  $ErrorActionPreference = "Continue"
  try {
    $art = "$root\build\ci-fuzz\art"
    New-Item -ItemType Directory -Force $art | Out-Null
    foreach ($t in "probe", "psd", "qoi") {
      $corp = "$root\build\ci-fuzz\corp\$t"
      New-Item -ItemType Directory -Force $corp | Out-Null
      & "$root\build\ci-fuzz\bin\fuzz_$t.exe" "-max_total_time=$FuzzSeconds" -max_len=262144 -timeout=10 `
        -rss_limit_mb=2048 -print_final_stats=1 "-artifact_prefix=$art\${t}_" $corp 2>&1 | ForEach-Object { "$_" } | Out-Host
      if ($LASTEXITCODE) { throw "fuzz_$t failed (exit $LASTEXITCODE), crash input in $art" }
    }
  } finally { $env:PATH = $savedPath; $ErrorActionPreference = "Stop" }
}

if ($Package) {
  Step "Installer"
  & "$PSScriptRoot\package.ps1"
}

Write-Host ("`nCI passed in {0:n0} s" -f ((Get-Date) - $t0).TotalSeconds) -ForegroundColor Green
