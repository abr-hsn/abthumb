# Builds a release and assembles dist\abthumb-<version>-x64 (+ .zip).
param([string]$Preset = "release")
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
cmd /c "`"$root\build.cmd`" $Preset"
if ($LASTEXITCODE) { throw "build failed" }
$bin = "$root\build\$Preset\bin"
$ver = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(abthumb VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$out = "$root\dist\abthumb-$ver-x64"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$out\licenses" | Out-Null
foreach ($f in "abthumb.dll", "abthumb.exe", "abthumb-agent.exe", "abthumb-render.exe", "abthumb-preview.exe", "pdfium.dll") {
  Copy-Item "$bin\$f" $out
}
Copy-Item "$root\LICENSE", "$root\README.md", "$root\THIRD_PARTY_NOTICES.md", "$root\scripts\install.cmd", "$root\scripts\uninstall.cmd", "$root\scripts\install.ps1" $out
Copy-Item "$root\third_party\pdfium\LICENSE" "$out\licenses\pdfium-LICENSE.txt"
Copy-Item "$root\third_party\pdfium\licenses\*" "$out\licenses\"
Copy-Item "$root\third_party\miniz\LICENSE" "$out\licenses\miniz-LICENSE.txt"
$zip = "$out.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$out\*" -DestinationPath $zip
Write-Host "package: $out"
Write-Host "zip:     $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
