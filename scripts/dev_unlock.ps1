# Dev helper: release build outputs before relinking.
# Stops only agent/worker processes running from this build folder (never the installed copy),
# then moves any still-loaded DLL (e.g. held by Explorer or a file dialog) aside; Windows allows
# renaming a loaded DLL, and the old copy is deleted on the next run.
param([string]$Bin = "$PSScriptRoot\..\build\me\bin")
$Bin = (Resolve-Path $Bin).Path
Get-Process abthumb-agent, abthumb-render, abthumb-preview -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($Bin, [StringComparison]::OrdinalIgnoreCase) } |
    Stop-Process -Force
Get-ChildItem $Bin -Filter "*.old-*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
foreach ($f in "abthumb.dll", "abthumb.exe", "abthumb-agent.exe", "abthumb-render.exe", "abthumb-preview.exe") {
  $p = Join-Path $Bin $f
  if (-not (Test-Path $p)) { continue }
  try { $s = [IO.File]::Open($p, 'Open', 'ReadWrite', 'None'); $s.Close() }
  catch { Rename-Item $p ("$f.old-" + [DateTime]::Now.Ticks) -ErrorAction SilentlyContinue }
}
