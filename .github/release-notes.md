Thumbnails and a preview pane for Illustrator (.ai, .eps), Photoshop (.psd, .psb) and PDF files in Windows Explorer.

## Install
1. Download `abthumb-setup-{VERSION}-x64.exe` below and run it. It installs machine-wide, so Windows asks for administrator approval once.
2. Optional: on the "Select Additional Tasks" page, tick "Clear Explorer's thumbnail cache" so thumbnails made by the previous handler are redrawn (Explorer restarts for a moment).

To remove it, use Settings > Apps > Installed apps > abthumb. Uninstalling restores the handlers abthumb replaced.

## This build is not code-signed
Windows SmartScreen will say "Windows protected your PC". Choose **More info**, then **Run anyway**. Some antivirus products flag unsigned installers that register shell extensions. Before running it you can check the download against the SHA-256 below or `SHA256SUMS.txt`:

```
{SETUP_SHA256}  abthumb-setup-{VERSION}-x64.exe
```

PowerShell: `Get-FileHash .\abthumb-setup-{VERSION}-x64.exe`

## Files
* `abthumb-setup-{VERSION}-x64.exe`: the installer (Windows 10 1809 or newer, x64).
* `abthumb-{VERSION}-x64.zip`: the same files without an installer, for inspection or manual registration (see CONTRIBUTING.md).
* `SHA256SUMS.txt`: checksums of every shipped file.

Problems and security reports: see [SECURITY.md](https://github.com/abr-hsn/abthumb/blob/main/SECURITY.md) for security issues, and the issue tracker for everything else.
