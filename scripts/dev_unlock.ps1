# Dev helper: release build outputs before relinking.
# Stops the agent (and its workers), then moves any still-loaded DLL (e.g. held by Explorer or a
# file dialog) aside; Windows allows renaming a loaded DLL, and the old copy is deleted later.
param([string]$Bin = "$PSScriptRoot\..\build\me\bin")
$cli = Join-Path $Bin "abthumb.exe"
if (Test-Path $cli) { & $cli stop-agent 2>&1 | Out-Null }
Start-Sleep -Milliseconds 500
Get-Process abthumb-agent, abthumb-render, abthumb-preview -ErrorAction SilentlyContinue | Stop-Process -Force
Get-ChildItem $Bin -Filter "*.old-*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
foreach ($f in "abthumb.dll", "abthumb.exe", "abthumb-agent.exe", "abthumb-render.exe", "abthumb-preview.exe") {
  $p = Join-Path $Bin $f
  if (-not (Test-Path $p)) { continue }
  try { $s = [IO.File]::Open($p, 'Open', 'ReadWrite', 'None'); $s.Close() }
  catch { Rename-Item $p ("$f.old-" + [DateTime]::Now.Ticks) -ErrorAction SilentlyContinue }
}
