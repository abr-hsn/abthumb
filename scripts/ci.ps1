# Runs the same checks as .github/workflows/ci.yml on this PC, plus the slower quality gates (D-035):
#   always : release build (/W4 /WX), unit tests, clang-format on changed files, cppcheck
#   full   : clang-tidy, MSVC /analyze build, scripts\regress.ps1, libFuzzer + ASan smoke run
# It builds in its own trees (build\ci-release, build\ci-tidy, build\ci-analyze, build\ci-fuzz), so it never
# disturbs build\release or another build that is running in the same folder.
#
#   powershell -File scripts\ci.ps1                 full run (60 s per fuzzer, like GitHub)
#   powershell -File scripts\ci.ps1 -Quick          build + unit tests + format + cppcheck (the pre-push hook)
#   powershell -File scripts\ci.ps1 -FuzzSeconds 10 shorter fuzz run (0 skips fuzzing)
#   -NoTidy / -NoAnalyze / -NoRegress               skip one slow step of the full run
#   -StrictFormat                                   clang-format findings fail the run (advisory otherwise)
#   -Package                                        also build the installer (scripts\package.ps1)
param([switch]$Quick, [int]$FuzzSeconds = 60, [switch]$Package, [switch]$StrictFormat,
      [switch]$NoTidy, [switch]$NoAnalyze, [switch]$NoRegress)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$t0 = Get-Date
$timings = [ordered]@{}
$warnings = New-Object System.Collections.ArrayList

function Step([string]$name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan; $script:stepName = $name; $script:stepT = Get-Date }
function EndStep { $timings[$script:stepName] = [int]((Get-Date) - $script:stepT).TotalSeconds }
function Advise([string]$msg) { Write-Host "WARNING: $msg" -ForegroundColor Yellow; [void]$warnings.Add($msg) }

# Load the MSVC x64 environment once (same toolset lookup as build.cmd).
$vs = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $env:VSCMD_VER) {
  if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio with the C++ workload" }
  if (-not $vs) { throw "Visual Studio with C++ tools not found" }
  $envDump = cmd /c "`"$vs\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 -no_logo >nul 2>nul && set"
  if ($LASTEXITCODE) { throw "VsDevCmd.bat failed" }
  foreach ($line in $envDump) { if ($line -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] } }
}

# LLVM tools: PATH first, then the copy that ships with Visual Studio (VC\Tools\Llvm\x64\bin).
function Find-Tool([string]$exe) {
  $c = Get-Command $exe -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  if ($vs) { $p = "$vs\VC\Tools\Llvm\x64\bin\$exe"; if (Test-Path $p) { return $p } }
  $p = "$env:ProgramFiles\cppcheck\$exe"
  if (Test-Path $p) { return $p }
  return $null
}

function Build([string]$dir, [string[]]$defs) {
  $b = "$root\build\$dir"
  # Always re-run configure: it is cheap, and it picks up option changes (ABT_WERROR, ABT_ANALYZE).
  cmake -S $root -B $b -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=RelWithDebInfo @defs
  if ($LASTEXITCODE) { throw "cmake configure failed ($dir)" }
  cmake --build $b
  if ($LASTEXITCODE) { throw "build failed ($dir)" }
}

# Our own sources (never third_party), as repo-relative paths.
function Source-Files([string[]]$ext) {
  Push-Location $root
  try {
    git ls-files -- src tests/unit tests/fuzz | Where-Object { $e = [IO.Path]::GetExtension($_); $ext -contains $e }
  } finally { Pop-Location }
}

Step "Build (release, /W4 /WX)"
Build "ci-release" @("-DABT_WERROR=ON")
EndStep

Step "Unit tests"
# Tests write their output images to the current directory, so run them from a temp dir.
$tmp = Join-Path $env:TEMP "abthumb-ci"
New-Item -ItemType Directory -Force $tmp | Out-Null
Push-Location $tmp
try { & "$root\build\ci-release\bin\abt_tests.exe"; $code = $LASTEXITCODE } finally { Pop-Location }
if ($code) { throw "unit tests failed (exit $code)" }
EndStep

Step "clang-format (files changed vs origin/main)"
$cf = Find-Tool "clang-format.exe"
if (-not $cf) {
  Advise "clang-format.exe not found (install the VS 'C++ Clang tools' component); skipped"
} else {
  Push-Location $root
  $ErrorActionPreference = "Continue"  # native stderr must not become a terminating error in PS 5.1
  try {
    $base = "origin/main"
    git rev-parse --verify --quiet $base *> $null
    if ($LASTEXITCODE) { $base = "main" }
    $changed = @(git diff --name-only --diff-filter=d "$base...HEAD") + @(git diff --name-only --diff-filter=d HEAD) +
               @(git ls-files --others --exclude-standard)
    $files = $changed | Sort-Object -Unique | Where-Object {
      $_ -match '\.(c|cpp|h)$' -and $_ -notmatch '^third_party/' -and (Test-Path $_) }
    if (-not $files) { Write-Host "no changed C/C++ files" }
    else {
      $bad = 0
      foreach ($f in $files) {
        $out = & $cf --dry-run -Werror --style=file $f 2>&1 | Out-String
        if ($LASTEXITCODE) { $bad++; Write-Host "format: $f" -ForegroundColor Yellow }
      }
      if ($bad -eq 0) { Write-Host "$(@($files).Count) changed file(s) formatted" }
      elseif ($StrictFormat) { throw "$bad file(s) need clang-format (run: clang-format -i --style=file <file>)" }
      else { Advise "$bad changed file(s) differ from .clang-format (advisory; pass -StrictFormat to fail)" }
    }
  } finally { Pop-Location; $ErrorActionPreference = "Stop" }
}
EndStep

Step "cppcheck"
$cpp = Find-Tool "cppcheck.exe"
if (-not $cpp) {
  Advise "cppcheck.exe not found (choco install cppcheck); skipped"
} else {
  $cppOut = Join-Path $root "build\cppcheck"
  New-Item -ItemType Directory -Force $cppOut | Out-Null
  $ErrorActionPreference = "Continue"
  & $cpp --enable=warning,performance,portability --inline-suppr --error-exitcode=1 --quiet `
    --suppressions-list="$root\scripts\cppcheck-suppressions.txt" --cppcheck-build-dir="$cppOut" `
    -j 4 --std=c++20 --platform=win64 -DWIN32 -D_WIN32 -DUNICODE -D_UNICODE -D_M_X64 --include="$root\scripts\cppcheck-defs.h" -DABT_VERSION_MAJOR=0 -DABT_VERSION_MINOR=0 -DABT_VERSION_PATCH=0 "-DIFACEMETHODIMP=HRESULT" "-DIFACEMETHODIMP_(t)=t" `
    --template="{file}:{line}: {severity}: {message} [{id}]" -I "$root\src\core" -I "$root\src\win" `
    -I "$root\src\render" -I "$root\src\agent" -I "$root\src\shellext" "$root\src" 2>&1 | ForEach-Object { "$_" } | Out-Host
  $code = $LASTEXITCODE
  $ErrorActionPreference = "Stop"
  if ($code) { throw "cppcheck reported problems (fix, or suppress narrowly with a comment)" }
}
EndStep

if (-not $Quick) {
  if (-not $NoTidy) {
    Step "clang-tidy (src/)"
    $tidy = Find-Tool "clang-tidy.exe"
    if (-not $tidy) {
      Advise "clang-tidy.exe not found (install the VS 'C++ Clang tools' component); skipped"
    } else {
      # Configure only: clang-tidy needs the compile_commands.json, not the objects.
      $b = "$root\build\ci-tidy"
      cmake -S $root -B $b -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
      if ($LASTEXITCODE) { throw "cmake configure failed (ci-tidy)" }
      $srcs = Source-Files @(".c", ".cpp") | Where-Object { $_ -like 'src/*' } | ForEach-Object { "$root\$($_ -replace '/', '\')" }
      $ErrorActionPreference = "Continue"
      $fail = 0
      # A few parallel processes: clang-tidy is single-threaded per file. The VS-bundled clang can lag the
      # MSVC STL (STL1000 "expected Clang N or newer"); the STL check is irrelevant to static analysis.
      $procs = @()
      $logDir = "$b\tidy-logs"; New-Item -ItemType Directory -Force $logDir | Out-Null
      Remove-Item "$logDir\*" -Force -ErrorAction SilentlyContinue
      $i = 0
      $queue = New-Object System.Collections.Queue
      foreach ($s in $srcs) { $queue.Enqueue($s) }
      $running = @()
      while ($queue.Count -or $running.Count) {
        while ($queue.Count -and $running.Count -lt 6) {
          $s = $queue.Dequeue(); $i++
          $log = "$logDir\$i.txt"
          $p = Start-Process -FilePath $tidy -ArgumentList @("-p", "`"$b`"", "--quiet", "--extra-arg=-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH", "`"$s`"") -NoNewWindow -PassThru `
                 -RedirectStandardOutput $log -RedirectStandardError "$log.err"
          $null = $p.Handle  # keeps the exit code readable after exit (PS 5.1 quirk)
          $running += [pscustomobject]@{ P = $p; Log = $log; Src = $s }
        }
        Start-Sleep -Milliseconds 200
        $still = @()
        foreach ($r in $running) {
          if ($r.P.HasExited) {
            $txt = (Get-Content $r.Log -Raw) + (Get-Content "$($r.Log).err" -Raw)
            if ($r.P.ExitCode -ne 0 -or ($txt -match 'warning:|error:')) {
              $fail++; Write-Host $txt
            }
          } else { $still += $r }
        }
        $running = $still
      }
      $ErrorActionPreference = "Stop"
      if ($fail) { throw "clang-tidy reported problems in $fail file(s)" }
    }
    EndStep
  }

  if (-not $NoAnalyze) {
    Step "Build with MSVC /analyze"
    Build "ci-analyze" @("-DABT_WERROR=ON", "-DABT_ANALYZE=ON")
    EndStep
  }

  if (-not $NoRegress) {
    Step "Regression: blank thumbnails (D:\04-OUTPUT, skipped when absent)"
    & "$PSScriptRoot\regress.ps1"
    if ($LASTEXITCODE) { throw "regress.ps1 failed" }
    EndStep
  }

  if ($FuzzSeconds -gt 0) {
    Step "Build fuzzers (libFuzzer + ASan)"
    Build "ci-fuzz" @("-DABT_FUZZ=ON")
    EndStep

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
    EndStep
  }
}

if ($Package) {
  Step "Installer"
  & "$PSScriptRoot\package.ps1"
  EndStep
}

Write-Host "`nStep timings:" -ForegroundColor Cyan
foreach ($k in $timings.Keys) { Write-Host ("  {0,5} s  {1}" -f $timings[$k], $k) }
foreach ($w in $warnings) { Write-Host "  warning: $w" -ForegroundColor Yellow }
Write-Host ("`nCI passed in {0:n0} s" -f ((Get-Date) - $t0).TotalSeconds) -ForegroundColor Green
