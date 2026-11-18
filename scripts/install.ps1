# abthumb installer / uninstaller (D-015, D-023).
#   install.cmd   -> powershell -File install.ps1
#   uninstall.cmd -> powershell -File install.ps1 -Uninstall
# Machine-wide steps run in an elevated copy of this script (one UAC prompt). COM registration is
# done by regsvr32 calling abthumb.dll's own DllRegisterServer, which backs up the handlers it
# replaces (FastPictureViewer / Photo Thumbnail Provider, Illustrator and Acrobat preview handlers)
# and restores them on uninstall.
param(
    [switch]$Uninstall,
    [switch]$ResetCache,      # clear Explorer's thumbnail cache without asking
    [switch]$NoPrompt,        # never ask questions
    [string]$Dir = (Join-Path $env:ProgramFiles "abthumb"),
    [string]$Log,             # internal: log file of the elevated step
    [switch]$ElevatedStep     # internal
)
$ErrorActionPreference = "Stop"
$Files = "abthumb.dll", "abthumb.exe", "abthumb-agent.exe", "abthumb-render.exe", "abthumb-preview.exe",
         "pdfium.dll", "LICENSE", "README.md", "THIRD_PARTY_NOTICES.md", "install.ps1", "install.cmd", "uninstall.cmd"
$UninstallKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\abthumb"
$RunKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run"

Add-Type -Namespace Abt -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool MoveFileEx(string from, string to, int flags);
'@

function Say($m) { if ($Log) { Add-Content -Path $Log -Value $m } else { Write-Host $m } }

# A loaded DLL cannot be overwritten but can be renamed; the old copy goes at the next reboot.
function Replace-File($src, $dst) {
    try { Copy-Item $src $dst -Force; return } catch { }
    $old = "$dst.old-$([DateTime]::Now.Ticks)"
    Rename-Item $dst (Split-Path $old -Leaf) -Force
    [Abt.Native]::MoveFileEx($old, $null, 4) | Out-Null   # MOVEFILE_DELAY_UNTIL_REBOOT
    Copy-Item $src $dst -Force
}

function Remove-OrSchedule($path) {
    try { Remove-Item $path -Force -ErrorAction Stop } catch { [Abt.Native]::MoveFileEx($path, $null, 4) | Out-Null }
}

function Stop-Ours($dir) {
    Get-Process abthumb-agent, abthumb-render, abthumb-preview -ErrorAction SilentlyContinue |
        Where-Object { -not $dir -or ($_.Path -and $_.Path.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase)) } |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

function Do-Machine-Install {
    $src = $PSScriptRoot
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    Stop-Ours $Dir
    if ((Resolve-Path $src).Path.TrimEnd('\') -ne (Resolve-Path $Dir).Path.TrimEnd('\')) {
        foreach ($f in $Files) { if (Test-Path "$src\$f") { Replace-File "$src\$f" "$Dir\$f" } }
        if (Test-Path "$src\licenses") { Copy-Item "$src\licenses" "$Dir" -Recurse -Force }
    }
    $p = Start-Process regsvr32.exe -ArgumentList "/s", "`"$Dir\abthumb.dll`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($p.ExitCode)" }
    Set-ItemProperty $RunKey -Name "abthumb" -Value "`"$Dir\abthumb-agent.exe`""
    New-Item $UninstallKey -Force | Out-Null
    $ver = (Get-Item "$Dir\abthumb.dll").VersionInfo.ProductVersion
    $props = @{
        DisplayName = "abthumb (thumbnails for Illustrator, EPS, Photoshop, PDF)"; DisplayVersion = $ver
        Publisher = "abthumb contributors"; InstallLocation = $Dir; DisplayIcon = "$Dir\abthumb.exe"
        UninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$Dir\install.ps1`" -Uninstall"
    }
    foreach ($k in $props.Keys) { Set-ItemProperty $UninstallKey -Name $k -Value $props[$k] }
    foreach ($k in "NoModify", "NoRepair") { Set-ItemProperty $UninstallKey -Name $k -Value 1 -Type DWord }
    Set-ItemProperty $UninstallKey -Name "EstimatedSize" -Value 9300 -Type DWord
    Say "installed to $Dir and registered machine-wide"
}

function Do-Machine-Uninstall {
    Stop-Ours $Dir
    if (Test-Path "$Dir\abthumb.dll") {
        Start-Process regsvr32.exe -ArgumentList "/u", "/s", "`"$Dir\abthumb.dll`"" -Wait | Out-Null
    }
    Remove-ItemProperty $RunKey -Name "abthumb" -ErrorAction SilentlyContinue
    Remove-Item $UninstallKey -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $Dir) {
        Get-ChildItem $Dir -Recurse -File | ForEach-Object { Remove-OrSchedule $_.FullName }
        Get-ChildItem $Dir -Recurse -Directory | Sort-Object FullName -Descending | ForEach-Object { Remove-OrSchedule $_.FullName }
        Remove-OrSchedule $Dir
    }
    Say "abthumb removed; the previous thumbnail and preview handlers were restored"
}

# ---------------------------------------------------------------- elevated half
if ($ElevatedStep) {
    try {
        if ($Uninstall) { Do-Machine-Uninstall } else { Do-Machine-Install }
        exit 0
    } catch {
        Say "ERROR: $($_.Exception.Message)"
        exit 1
    }
}

# ---------------------------------------------------------------- user half
Write-Host ($(if ($Uninstall) { "Removing abthumb..." } else { "Installing abthumb (Windows will ask for administrator permission once)..." }))
Stop-Ours $null   # this user's agent and workers hold files open
$log = Join-Path $env:TEMP "abthumb-setup-$PID.log"
$elevArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-ElevatedStep", "-Dir", "`"$Dir`"", "-Log", "`"$log`"")
if ($Uninstall) { $elevArgs += "-Uninstall" }
try {
    $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $elevArgs -WindowStyle Hidden -PassThru
    $null = $p.Handle   # keep the handle so ExitCode is readable for an elevated process
    $p.WaitForExit()
} catch {
    Write-Host "Administrator permission was declined; nothing was changed."
    exit 1
}
$failed = $false
if (Test-Path $log) {
    Get-Content $log | ForEach-Object { Write-Host $_; if ($_ -like "ERROR:*") { $failed = $true } }
    Remove-Item $log -Force
}
if ($failed -or ($null -ne $p.ExitCode -and $p.ExitCode -ne 0)) { Write-Host "Setup failed (exit $($p.ExitCode))."; exit 1 }

# Per-user developer registrations (HKCU) would shadow the machine-wide ones for in-process callers.
foreach ($clsid in "{0A1153BB-F895-4458-8DE8-D08EDC536A91}", "{A4E31D04-2524-467B-9B51-520F3D114E32}") {
    Remove-Item "HKCU:\Software\Classes\CLSID\$clsid" -Recurse -Force -ErrorAction SilentlyContinue
}
foreach ($ext in ".ai", ".ait", ".eps", ".epsf", ".epsi", ".psd", ".psdt", ".pdd", ".psb", ".pdf") {
    foreach ($base in "HKCU:\Software\Classes\$ext", "HKCU:\Software\Classes\SystemFileAssociations\$ext") {
        foreach ($iid in "{e357fccd-a995-4576-b01f-234630154e96}", "{8895b1c6-b41f-4c1c-a562-0d564250836f}") {
            $k = "$base\ShellEx\$iid"
            $v = (Get-ItemProperty $k -ErrorAction SilentlyContinue).'(default)'
            if ($v -in "{0A1153BB-F895-4458-8DE8-D08EDC536A91}", "{A4E31D04-2524-467B-9B51-520F3D114E32}") { Remove-Item $k -Force }
        }
    }
}
Remove-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\PreviewHandlers" -Name "{A4E31D04-2524-467B-9B51-520F3D114E32}" -ErrorAction SilentlyContinue

if ($Uninstall) {
    Remove-Item (Join-Path $env:LOCALAPPDATA "abthumb") -Recurse -Force -ErrorAction SilentlyContinue  # cache + logs
} else {
    # Thumbnails made by the previous handler stay in Explorer's cache until a file changes.
    $reset = $ResetCache
    if (-not $reset -and -not $NoPrompt) {
        Write-Host ""
        Write-Host "Explorer may keep showing old thumbnails it made with the previous handler."
        $a = Read-Host "Clear Explorer's thumbnail cache now? Explorer restarts briefly. [Y/n]"
        $reset = -not ($a -match '^[nN]')
    }
    if ($reset) {
        Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 800
        Get-ChildItem (Join-Path $env:LOCALAPPDATA "Microsoft\Windows\Explorer") -Filter "thumbcache_*.db" -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
        Start-Process explorer.exe
        Write-Host "Explorer's thumbnail cache was cleared."
    }
    Start-Process (Join-Path $Dir "abthumb-agent.exe") -WorkingDirectory $Dir
    Write-Host "Done. Thumbnails and the preview pane for .ai .ait .eps .epsf .epsi .psd .psdt .pdd .psb .pdf now come from abthumb."
}
