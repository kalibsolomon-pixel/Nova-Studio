; Nova Studio — per-user Windows installer (Phase 3-E).
; Compiled by scripts/package-phase3e.py, which supplies every {#...} define.
; The payload is the accepted portable candidate produced by scripts/package-phase3.py,
; so the installed file set always equals the reviewed package manifest.
;
; Scope decisions (see docs/phase-3e/PHASE_3E_PACKAGING.md):
;   * Per-user install into %LOCALAPPDATA%\Programs\Nova Studio; never elevated.
;   * The application writes mutable state only under %LOCALAPPDATA%
;     (settings, diagnostics, recovery, fabric-runtime), never into the app folder.
;   * Uninstall removes installed files only. Projects, settings, recovery
;     snapshots and diagnostics are never touched; the Fabric runtime cache
;     (including the Nova Test World) is offered as a separate opt-in cleanup
;     that defaults to "keep".
;   * No file associations, services, drivers or scheduled tasks are registered.

#define AppName "Nova Studio"
#define AppExeName "nova-studio.exe"

[Setup]
AppId={{4160E321-01F3-4BA8-B8DF-65FEE14D6D65}
AppName={#AppName}
AppVersion={#AppDisplayVersion}
AppPublisher=Nova Studio
VersionInfoVersion={#AppVersionNumber}
VersionInfoProductVersion={#AppVersionNumber}
UninstallDisplayName={#AppName}
DefaultDirName={localappdata}\Programs\{#AppName}
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
OutputDir={#OutputDir}
OutputBaseFilename={#SetupBaseName}
SetupIconFile={#SetupIcon}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
SetupLogging=yes
; AppComments records the exact source revision for support and traceability.
AppComments=Source revision {#Revision}

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Files]
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"
Name: "{autoprograms}\{#AppName}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Code]
function SumDirBytes(path: string): Int64;
var
  find: TFindRec;
begin
  Result := 0;
  if not FindFirst(path + '\*', find) then
    Exit;
  try
    repeat
      if (find.Name <> '.') and (find.Name <> '..') then
      begin
        if find.Attributes and FILE_ATTRIBUTE_DIRECTORY <> 0 then
          Result := Result + SumDirBytes(path + '\' + find.Name)
        else
          Result := Result + (Int64(find.SizeHigh) * 4294967296) + find.SizeLow;
      end;
    until not FindNext(find);
  finally
    FindClose(find);
  end;
end;

// Separate, explicit opt-in cleanup for the Fabric runtime cache. The default
// answer is "No"; projects, settings, recovery files and diagnostics are never
// candidates for removal here — only the Nova-managed runtime directory is.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  runtime: string;
  megaBytes: Int64;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;
  runtime := ExpandConstant('{localappdata}') + '\NovaStudio';
  if not DirExists(runtime) then
    Exit;
  megaBytes := SumDirBytes(runtime) div (1024 * 1024);
  if MsgBox(
      '{#AppName} has been uninstalled.' + #13#10 + #13#10 +
      'Optional cleanup: the Nova Fabric runtime cache still occupies about ' +
      IntToStr(megaBytes) + ' MB at' + #13#10 + runtime + #13#10 + #13#10 +
      'It contains the downloaded Minecraft and Fabric components and the ' +
      'Nova Test World (including its saves). Keep it to reuse the runtime ' +
      'after reinstalling {#AppName}; removing it now also removes that world.' + #13#10 + #13#10 +
      'Resource-pack projects, personal settings, recovery snapshots and ' +
      'diagnostics are kept either way.' + #13#10 + #13#10 +
      'Remove the runtime cache now as well?',
      mbConfirmation, MB_DEFBUTTON2) = IDYES then
    DelTree(runtime, True, True, True);
end;
