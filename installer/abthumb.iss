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
  #define AppVer "0.1.1"
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
; Stop agents and workers (all sessions) before DllUnregisterServer restores the previous handlers.
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

procedure StopOurProcesses();
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM abthumb-agent.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM abthumb-render.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM abthumb-preview.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

{ Explorer (and its thumbnail host) keep abthumb.dll loaded. A loaded DLL cannot be overwritten
  but can be renamed: move it aside and delete it at the next reboot, so the new one can be
  copied without closing Explorer and without asking for a restart. }
procedure MoveAsideIfLocked(const Name: string);
var
  Path, Old: string;
begin
  Path := ExpandConstant('{app}\') + Name;
  if not FileExists(Path) then Exit;
  Old := Path + '.old-' + GetDateTimeString('yyyymmddhhnnss', #0, #0);
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
  { Uninstall entry of the earlier script-based installer, replaced by Inno's own. }
  RegDeleteKeyIncludingSubkeys(HKLM, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\abthumb');
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then begin
    { Thumbnail cache and logs of the user who runs the uninstaller (other users' caches are
      plain thumbnail files under their own %LOCALAPPDATA%\abthumb and are harmless). }
    DelTree(ExpandConstant('{localappdata}\abthumb'), True, True, True);
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\abthumb');
  end;
end;
