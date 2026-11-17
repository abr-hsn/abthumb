<#
.SYNOPSIS
  Regenerates the abthumb test corpus with Adobe Illustrator and Photoshop (Windows PowerShell 5.1).

.DESCRIPTION
  Runs gen_ai.jsx (Illustrator, via COM) and gen_ps.jsx (Photoshop) and merges their per-app manifests
  into generated\manifest.csv. Idempotent: every run overwrites the generated files.
  Writes only below tests\corpus\generated.

  Safety: if Illustrator/Photoshop is already running it is reused and NOT quit; the scripts only touch
  documents they created. An app this script started is quit when finished.

.PARAMETER Only
  'ai' or 'ps' to run just one generator (the other app's previous manifest rows are kept).

.PARAMETER Filter
  Optional comma separated list of base names (e.g. "tiny,basic_rgb") for quick partial runs.

.PARAMETER TimeoutMinutes
  Per-application watchdog; a hung run is aborted (and the app killed if this script started it).
#>
[CmdletBinding()]
param(
    [ValidateSet('ai', 'ps')][string]$Only,
    [string]$Filter = '',
    [int]$TimeoutMinutes = 60,
    [switch]$SkipPsCom   # skip the (slow to fail) Photoshop COM attempt and launch Photoshop.exe with a driver script directly
)
$ErrorActionPreference = 'Stop'
$scripts = $PSScriptRoot
$root = (Resolve-Path (Join-Path $scripts '..')).Path
$gen = Join-Path $root 'generated'
foreach ($d in 'ai', 'eps', 'pdf', 'psd', 'psb', 'ref') { New-Item -ItemType Directory -Force (Join-Path $gen $d) | Out-Null }
$rootJs = $root.Replace('\', '/')

function Wait-AppGone([string]$name, [int]$graceSec = 60) {
    $end = (Get-Date).AddSeconds($graceSec)
    while ((Get-Process -Name $name -ErrorAction SilentlyContinue) -and (Get-Date) -lt $end) { Start-Sleep 2 }
    $p = Get-Process -Name $name -ErrorAction SilentlyContinue
    if ($p) { Write-Warning "$name did not exit, killing"; $p | Stop-Process -Force }
}

function Find-Exe([string]$pattern) {
    Get-ChildItem 'C:\Program Files\Adobe' -Directory -Filter $pattern -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | ForEach-Object {
            Get-ChildItem $_.FullName -Recurse -Include 'Illustrator.exe', 'Photoshop.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
        } | Select-Object -First 1
}

# Runs ExtendScript through COM inside a job so a hang can be timed out. Returns the script result string.
function Invoke-ComScript([string]$progId, [string]$code, [int]$timeoutMin) {
    $job = Start-Job -ArgumentList $progId, $code -ScriptBlock {
        param($p, $c)
        $app = New-Object -ComObject $p
        $app.DoJavaScript($c)
    }
    if (-not (Wait-Job $job -Timeout ($timeoutMin * 60))) {
        Stop-Job $job; Remove-Job $job -Force
        throw "timeout after $timeoutMin min"
    }
    $res = Receive-Job $job -ErrorAction Stop
    Remove-Job $job -Force
    return $res
}

function Get-DriverCode([string]$genFile, [string]$statusFile, [bool]$quit) {
    $f = $Filter
    @"
var CORPUS_ROOT='$rootJs'; var CORPUS_FILTER='$f';
var __st=new File('$($statusFile.Replace('\','/'))');
try { `$.evalFile(File('$rootJs/scripts/$genFile')); __st.open('w'); __st.write('done'); __st.close(); }
catch(e) { __st.open('w'); __st.write('EXC ' + e + ' line ' + e.line); __st.close(); }
$(if ($quit) { 'app.quit();' })
"@
}

function Run-Illustrator {
    $wasRunning = [bool](Get-Process -Name Illustrator -ErrorAction SilentlyContinue)
    Write-Host "Illustrator: already running = $wasRunning"
    $code = "var CORPUS_ROOT='$rootJs'; var CORPUS_FILTER='$Filter'; `$.evalFile(File('$rootJs/scripts/gen_ai.jsx'));"
    try {
        $res = Invoke-ComScript 'Illustrator.Application' $code $TimeoutMinutes
        Write-Host "Illustrator result: $res"
    } catch {
        Write-Warning "Illustrator COM run failed: $_"
        if (-not $wasRunning) { Get-Process -Name Illustrator -ErrorAction SilentlyContinue | Stop-Process -Force }
        throw
    } finally {
        if (-not $wasRunning -and (Get-Process -Name Illustrator -ErrorAction SilentlyContinue)) {
            try { (New-Object -ComObject Illustrator.Application).Quit() } catch { }
            Wait-AppGone 'Illustrator'
        }
    }
}

function Run-Photoshop {
    $wasRunning = [bool](Get-Process -Name Photoshop -ErrorAction SilentlyContinue)
    Write-Host "Photoshop: already running = $wasRunning"
    $comOk = $false
    $code = "var CORPUS_ROOT='$rootJs'; var CORPUS_FILTER='$Filter'; `$.evalFile(File('$rootJs/scripts/gen_ps.jsx'));"
    try {
        if ($SkipPsCom) { throw 'COM skipped by -SkipPsCom' }
        $res = Invoke-ComScript 'Photoshop.Application' $code ([Math]::Min($TimeoutMinutes, 90))
        Write-Host "Photoshop result (COM): $res"; $comOk = $true
    } catch {
        Write-Warning "Photoshop COM unavailable ($($_.Exception.Message)); falling back to launching Photoshop.exe with a driver script"
    }
    if ($comOk) {
        if (-not $wasRunning -and (Get-Process -Name Photoshop -ErrorAction SilentlyContinue)) {
            try { (New-Object -ComObject Photoshop.Application).Quit() } catch { }
            Wait-AppGone 'Photoshop'
        }
        return
    }
    # Fallback: a *fresh* Photoshop launched with a .jsx argument runs it (arguments are NOT honoured by an already-running instance).
    if ($wasRunning) { throw "Photoshop is already running and COM automation failed; close it manually (not touched) and rerun with -Only ps" }
    $exe = Find-Exe 'Adobe Photoshop 2*'
    if (-not $exe) { throw 'Photoshop.exe not found' }
    $status = Join-Path $env:TEMP 'abthumb_ps_status.txt'
    Remove-Item $status -ErrorAction SilentlyContinue
    $drv = Join-Path $env:TEMP 'abthumb_ps_driver.jsx'
    Set-Content $drv (Get-DriverCode 'gen_ps.jsx' $status $true) -Encoding UTF8
    $proc = Start-Process $exe.FullName -ArgumentList "`"$drv`"" -PassThru
    $end = (Get-Date).AddMinutes($TimeoutMinutes)
    try {
        while (-not (Test-Path $status) -and (Get-Date) -lt $end) { Start-Sleep 3 }
        if (-not (Test-Path $status)) { throw "Photoshop driver timeout after $TimeoutMinutes min" }
        Write-Host ("Photoshop driver status: " + (Get-Content $status -Raw))
    } finally {
        Wait-AppGone 'Photoshop' 45   # the driver calls app.quit(); force-kill only the instance we started
    }
}

$errors = @()
if (-not $Only -or $Only -eq 'ai') { try { Run-Illustrator } catch { $errors += "ai: $_"; Write-Warning "ai failed: $_" } }
if (-not $Only -or $Only -eq 'ps') { try { Run-Photoshop } catch { $errors += "ps: $_"; Write-Warning "ps failed: $_" } }

# merge manifests
$rows = @()
foreach ($m in 'manifest_ai.csv', 'manifest_ps.csv') {
    $p = Join-Path $gen $m
    if (Test-Path $p) { $l = Get-Content $p -Encoding UTF8; if ($rows.Count -eq 0) { $rows += $l } else { $rows += $l | Select-Object -Skip 1 } }
}
Set-Content (Join-Path $gen 'manifest.csv') $rows -Encoding UTF8
Write-Host "manifest.csv: $($rows.Count - 1) rows"

# sanity-check reference PNG dimensions against the manifest
Add-Type -AssemblyName System.Drawing
foreach ($r in (Import-Csv (Join-Path $gen 'manifest.csv') | Where-Object { $_.file -like 'ref/*.png' -and $_.ok -eq 'yes' })) {
    $f = Join-Path $gen $r.file
    if (Test-Path $f) {
        $img = [System.Drawing.Image]::FromFile($f)
        if ($img.Width -ne [int]$r.width_pt_or_px -or $img.Height -ne [int]$r.height) {
            Write-Warning "$($r.file): actual $($img.Width)x$($img.Height) vs expected $($r.width_pt_or_px)x$($r.height)"
        }
        $img.Dispose()
    }
}
if ($errors) { Write-Warning ("Errors:`n" + ($errors -join "`n")); exit 1 }
