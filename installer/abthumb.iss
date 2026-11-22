; abthumb installer (Inno Setup 6). Build: scripts\package.ps1 (or ISCC installer\abthumb.iss)
;
; Safety properties:
; - Machine-wide (Explorer only loads machine-wide handlers, D-023), one UAC prompt.
; - COM registration only through abthumb.dll's own DllRegisterServer/DllUnregisterServer
;   (regserver flag). Registration backs up the handlers it replaces (Photo Thumbnail Provider,
;   Illustrator and Acrobat preview handlers) and uninstall restores them.
; - Never closes or restarts Explorer unless the user ticks "clear thumbnail cache". A DLL that
;   Explorer still has loaded is renamed aside and deleted at the next reboot, so no reboot is needed.
; - Upgrades in place, stops our own agent/workers first, repairs registration when re-run.

#ifndef AppVer
  #define AppVer "0.1.4"
#endif
#define Dist "..\dist\abthumb-" + AppVer + "-x64"

[Setup]
AppId={{AC44C4EF-AB7E-43A2-BE66-A6CD6458E4EA}
AppName=abthumb
AppVersion={#AppVer}
AppVerName=abthumb {#AppVer}
AppPublisher=abthumb contributors
AppComments=Thumbnails and preview pane for Illustrator, EPS, Photoshop and PDF files
DefaultDirName={autopf}\abthumb
DisableDirPage=auto
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir=..\dist
OutputBaseFilename=abthumb-setup-{#AppVer}-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile={#Dist}\LICENSE
UninstallDisplayName=abthumb (thumbnails for Illustrator, EPS, Photoshop, PDF)
UninstallDisplayIcon={app}\abthumb.exe
CloseApplications=no
RestartIfNeededByRun=no
ChangesAssociations=yes
SetupLogging=yes
VersionInfoVersion={#AppVer}.0
VersionInfoDescription=abthumb setup

[Tasks]
Name: resetcache; Description: "Clear Explorer's thumbnail cache so thumbnails made by the previous handler are regenerated (Explorer restarts for a moment)"; GroupDescription: "After installing:"

[InstallDelete]
; files of the earlier script-based install
Type: files; Name: "{app}\install.ps1"
Type: files; Name: "{app}\install.cmd"
Type: files; Name: "{app}\uninstall.cmd"

[Files]
Source: "{#Dist}\abthumb.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit uninsrestartdelete
Source: "{#Dist}\pdfium.dll"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "{#Dist}\abthumb-agent.exe"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "{#Dist}\abthumb-render.exe"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "{#Dist}\abthumb-preview.exe"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "{#Dist}\abthumb.exe"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "{#Dist}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "reset-thumbcache.ps1"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Start the agent in every user session (it also starts on demand from the shell extension).
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "abthumb"; ValueData: """{app}\abthumb-agent.exe"""; Flags: uninsdeletevalue

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\reset-thumbcache.ps1"""; Flags: runasoriginaluser runhidden waituntilterminated; Tasks: resetcache; StatusMsg: "Clearing Explorer's thumbnail cache..."
Filename: "{app}\abthumb-agent.exe"; Flags: runasoriginaluser nowait; StatusMsg: "Starting abthumb..."

[UninstallRun]
; Ask the agent to flush its cache snapshot and exit cleanly first, then force-stop any stragglers.
Filename: "{app}\abthumb.exe"; Parameters: "stop-agent"; Flags: runhidden; RunOnceId: "GracefulStopAgent"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /T /IM abthumb-agent.exe"; Flags: runhidden; RunOnceId: "StopAgent"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM abthumb-render.exe"; Flags: runhidden; RunOnceId: "StopRender"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM abthumb-preview.exe"; Flags: runhidden; RunOnceId: "StopPreview"

[UninstallDelete]
Type: files; Name: "{app}\*.old-*"
Type: filesandordirs; Name: "{app}\licenses"
Type: dirifempty; Name: "{app}"

[Code]
const
  MOVEFILE_DELAY_UNTIL_REBOOT = 4;

function MoveFileEx(lpExistingFileName, lpNewFileName: string; dwFlags: Cardinal): Boolean;
  external 'MoveFileExW@kernel32.dll stdcall';

function GetFileAttributesW(lpFileName: string): Cardinal;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function IsReparsePoint(const Path: string): Boolean;
var
  Attr: Cardinal;
begin
  Attr := GetFileAttributesW(Path);
  Result := (Attr <> $FFFFFFFF) and ((Attr and FILE_ATTRIBUTE_REPARSE_POINT) <> 0);
end;

procedure StopOurProcesses();
var
  Cli: string;
  Code: Integer;
begin
  { Try a graceful stop first so the agent flushes its cache snapshot cleanly. }
  Cli := ExpandConstant('{app}\abthumb.exe');
  if FileExists(Cli) then begin
    Exec(Cli, 'stop-agent', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Sleep(500);
  end;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM abthumb-agent.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM abthumb-render.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM abthumb-preview.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

{ Explorer (and its thumbnail host) keep abthumb.dll loaded, and a still-loaded DLL can respawn
  abthumb-agent.exe during upgrade. Move locked binaries aside and delete them at next reboot. }
procedure MoveAsideIfLocked(const Name: string);
var
  Path, Base, Old: string;
  N: Integer;
begin
  Path := ExpandConstant('{app}\') + Name;
  if not FileExists(Path) then Exit;
  Base := Path + '.old-' + GetDateTimeString('yyyymmddhhnnss', #0, #0);
  Old := Base;
  N := 1;
  while FileExists(Old) and (N < 100) do begin
    Old := Base + '-' + IntToStr(N);
    N := N + 1;
  end;
  if RenameFile(Path, Old) then begin
    if not DeleteFile(Old) then
      MoveFileEx(Old, '', MOVEFILE_DELAY_UNTIL_REBOOT);
    Log('Moved aside ' + Path);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopOurProcesses();
  MoveAsideIfLocked('abthumb.dll');
  MoveAsideIfLocked('pdfium.dll');
  MoveAsideIfLocked('abthumb-agent.exe');
  MoveAsideIfLocked('abthumb-render.exe');
  { Uninstall entry of the earlier script-based installer, replaced by Inno's own. }
  RegDeleteKeyIncludingSubkeys(HKLM, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\abthumb');
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir, CacheDir: string;
begin
  if CurUninstallStep = usPostUninstall then begin
    { Thumbnail cache and logs of the user running the uninstaller. Refuse to recurse into
      reparse points/junctions since uninstall runs elevated. }
    DataDir := ExpandConstant('{localappdata}\abthumb');
    CacheDir := DataDir + '\cache';
    if DirExists(DataDir) and (not IsReparsePoint(DataDir)) and (not IsReparsePoint(CacheDir)) then
      DelTree(DataDir, True, True, True);
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\abthumb');
  end;
end;
