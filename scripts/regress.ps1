# Blank-thumbnail regression check (D-032, D-035) against real design files.
# Renders up to 200 .ai files from D:\04-OUTPUT the way Explorer's thumbnails do (--canvas) and fails when a
# result is a blank image: a PNG under 900 bytes while the CLI line does not say "no visible art" (that
# wording is the deliberate "nothing to draw" outcome, nor does "saved without PDF content": an .ai with no
# PDF stream and no preview cannot be drawn at all, which is a property of the file). Skips quietly when the folder does not exist
# (GitHub runners, other PCs).
#
#   powershell -File scripts\regress.ps1 [-Folder D:\04-OUTPUT] [-Max 200] [-Exe path\to\abthumb.exe]
param([string]$Folder = "D:\04-OUTPUT", [int]$Max = 200, [string]$Exe = "")
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
if (-not $Exe) { $Exe = "$root\build\ci-release\bin\abthumb.exe" }

if (-not (Test-Path $Folder)) { Write-Host "regress: $Folder not found, skipped"; exit 0 }
if (-not (Test-Path $Exe)) { Write-Host "regress: $Exe not built, run scripts\ci.ps1 first" -ForegroundColor Red; exit 1 }

$all = @(Get-ChildItem -LiteralPath $Folder -Recurse -File -Filter *.ai -ErrorAction SilentlyContinue | Sort-Object FullName)
if (-not $all.Count) { Write-Host "regress: no .ai files in $Folder, skipped"; exit 0 }
# Evenly spaced sample, so the same files are checked on every run.
if ($all.Count -gt $Max) {
  $step = $all.Count / $Max
  $files = for ($i = 0; $i -lt $Max; $i++) { $all[[int][math]::Floor($i * $step)] }
} else { $files = $all }

$tmp = Join-Path $env:TEMP "abthumb-regress"
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
New-Item -ItemType Directory -Force $tmp | Out-Null

$bad = @(); $blank = 0; $n = 0
$saved = $ErrorActionPreference
$ErrorActionPreference = "Continue"
foreach ($f in $files) {
  $n++
  $png = Join-Path $tmp "$($f.Name)_256.png"
  Remove-Item -LiteralPath $png -ErrorAction SilentlyContinue
  $out = & $Exe render $f.FullName --canvas --size 256 --out $tmp 2>&1 | ForEach-Object { "$_" } | Out-String
  $size = if (Test-Path -LiteralPath $png) { (Get-Item -LiteralPath $png).Length } else { 0 }
  $declared = $out -match "no visible art|saved without PDF content"
  if ($size -lt 900 -and -not $declared) { $bad += "$($f.FullName) ($size bytes)" }
  elseif ($declared) { $blank++ }
}
$ErrorActionPreference = $saved

Write-Host ("regress: {0} files rendered, {1} declared blank or unrenderable, {2} blank/missing" -f $n, $blank, $bad.Count)
if ($bad.Count) {
  Write-Host "Blank thumbnail regression in:" -ForegroundColor Red
  $bad | ForEach-Object { Write-Host "  $_" }
  exit 1
}
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
exit 0
