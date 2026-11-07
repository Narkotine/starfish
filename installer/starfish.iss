; StarFish -- per-user installer (Inno Setup 6). Built by tools/build_installer.ps1,
; which passes /DAppVersion (from CMake) and /DStageDir (a windeployqt staging dir).
; No admin: everything lives under %LOCALAPPDATA%\Programs\StarFish. User data
; (%APPDATA%\StarFish, %LOCALAPPDATA%\StarFish, HKCU\Software\StarFish) is never
; touched by install or upgrade; uninstall keeps it unless the user ticks the box.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #error StageDir must point at the deployed files
#endif
#ifndef OutDir
  #define OutDir "."
#endif

[Setup]
AppId={{6F1B6C1E-5A7D-4E1C-9B8E-57A2F15F1A51}
AppName=StarFish
AppVersion={#AppVersion}
AppVerName=StarFish {#AppVersion}
VersionInfoVersion={#AppVersion}
AppPublisher=StarFish
DefaultDirName={localappdata}\Programs\StarFish
DisableDirPage=yes
DefaultGroupName=StarFish
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=StarFish-{#AppVersion}-setup
UninstallDisplayIcon={app}\starfish.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; We stop StarFish ourselves (--quit restores the desktop icons cleanly).
CloseApplications=no
RestartApplications=no

[Languages]
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
fr.RemoveData=Supprimer aussi mes enclos, réglages et sauvegardes (%APPDATA%\StarFish)
en.RemoveData=Also delete my fences, settings and backups (%APPDATA%\StarFish)
fr.LaunchNow=Lancer StarFish
en.LaunchNow=Launch StarFish

[Files]
; Replace the program files only. ignoreversion: Qt DLLs carry their own versions.
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Drop stale plugin folders from an older Qt so an upgrade never mixes runtimes.
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\tls"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\translations"

[Icons]
Name: "{userprograms}\StarFish"; Filename: "{app}\starfish.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\starfish.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchNow}"; Flags: nowait postinstall skipifsilent
; Silent installs (the updater) relaunch too, unless /NORELAUNCH was passed.
Filename: "{app}\starfish.exe"; WorkingDir: "{app}"; Flags: nowait; Check: RelaunchAfterSilent

[Code]
procedure QuitStarFish(const Dir: String);
var
  Code: Integer;
begin
  { --quit signals the running instance (any copy) and waits for it to restore
    the desktop icons and exit. Harmless when nothing runs. }
  if FileExists(Dir + '\starfish.exe') then
    Exec(Dir + '\starfish.exe', '--quit', Dir, SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  QuitStarFish(ExpandConstant('{app}'));
  Result := '';
end;

function RelaunchAfterSilent: Boolean;
begin
  Result := WizardSilent and (Pos('/NORELAUNCH', Uppercase(GetCmdTail)) = 0);
end;

function InitializeUninstall: Boolean;
begin
  QuitStarFish(ExpandConstant('{app}'));
  Result := True;
end;

function UninstallNeedsDataChoice: Boolean;
begin
  Result := not UninstallSilent;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  RunValue: String;
  RemoveData: Boolean;
begin
  if CurUninstallStep = usUninstall then begin
    { Start-with-Windows: only drop the entry if it points into this install. }
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'StarFish', RunValue) then
      if Pos(Lowercase(ExpandConstant('{app}')), Lowercase(RunValue)) > 0 then
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'StarFish');
  end;
  if CurUninstallStep = usPostUninstall then begin
    { Data is kept by default. /REMOVEDATA (silent) or an explicit "Yes" removes it. }
    RemoveData := Pos('/REMOVEDATA', Uppercase(GetCmdTail)) > 0;
    if (not RemoveData) and UninstallNeedsDataChoice then
      RemoveData := MsgBox(CustomMessage('RemoveData') + '?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
    if RemoveData then begin
      DelTree(ExpandConstant('{userappdata}\StarFish'), True, True, True);
      DelTree(ExpandConstant('{localappdata}\StarFish'), True, True, True);
      RegDeleteKeyIncludingSubkeys(HKCU, 'Software\StarFish');
    end;
  end;
end;
