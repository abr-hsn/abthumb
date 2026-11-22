# Builds a release, assembles dist\abthumb-<version>-x64 and compiles the installer
# dist\abthumb-setup-<version>-x64.exe (Inno Setup 6), plus SHA256SUMS.txt.
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
if (Test-Path "$bin\gswin64c.exe") {
  Copy-Item "$bin\gswin64c.exe", "$bin\gsdll64.dll" $out
} elseif (Test-Path "$root\third_party\ghostscript\bin\gswin64c.exe") {
  Copy-Item "$root\third_party\ghostscript\bin\gswin64c.exe", "$root\third_party\ghostscript\bin\gsdll64.dll" $out
}
Copy-Item "$root\LICENSE", "$root\README.md", "$root\THIRD_PARTY_NOTICES.md" $out
Copy-Item "$root\third_party\pdfium\LICENSE" "$out\licenses\pdfium-LICENSE.txt"
Copy-Item "$root\third_party\pdfium\licenses\*" "$out\licenses\"
Copy-Item "$root\third_party\miniz\LICENSE" "$out\licenses\miniz-LICENSE.txt"
if (Test-Path "$root\third_party\ghostscript\COPYING") {
  Copy-Item "$root\third_party\ghostscript\COPYING" "$out\licenses\ghostscript-LICENSE.txt"
}

# Installer
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 not found (winget install JRSoftware.InnoSetup --scope user)" }
& $iscc /Q "/DAppVer=$ver" "$root\installer\abthumb.iss"
if ($LASTEXITCODE) { throw "installer build failed" }
$setup = "$root\dist\abthumb-setup-$ver-x64.exe"

# Checksums for everything we ship
$sums = Get-ChildItem $out -Recurse -File | ForEach-Object {
  "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.FullName.Substring($out.Length + 1)
}
$sums += "{0}  {1}" -f (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower(), (Split-Path $setup -Leaf)
$sums | Set-Content "$root\dist\SHA256SUMS.txt" -Encoding ascii
Write-Host "files:     $out"
Write-Host "installer: $setup ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB)"
Write-Host "checksums: $root\dist\SHA256SUMS.txt"
