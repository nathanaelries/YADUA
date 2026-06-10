; ============================================================================
; YADUA installer (Inno Setup 6).
;
; Installs the GUI and CLI together, with optional desktop shortcut and
; optional "add CLI to PATH". Users who prefer a portable app should grab the
; YADUA-*-portable.zip from the Releases page instead - the executables are
; fully self-contained and write nothing outside their folder.
;
; Compile (CI does this per architecture):
;   ISCC.exe /DAppVersion=0.1.1 /DArch=amd64 /DSrcDir=<repo root> yadua.iss
; ============================================================================

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Arch
  #define Arch "amd64"
#endif
#ifndef SrcDir
  #define SrcDir ".."
#endif

[Setup]
; Stable GUID identifies the app across upgrades - never change it.
AppId={{6E3B58A1-42D7-4F0B-9C2E-7B1A93F0D514}
AppName=YADUA
AppVersion={#AppVersion}
AppVerName=YADUA {#AppVersion}
AppPublisher=nathanaelries
AppPublisherURL=https://github.com/nathanaelries/YADUA
AppSupportURL=https://github.com/nathanaelries/YADUA/issues
DefaultDirName={autopf}\YADUA
DisableProgramGroupPage=yes
LicenseFile={#SrcDir}\LICENSE
OutputDir={#SrcDir}\dist
OutputBaseFilename=YADUA-setup-v{#AppVersion}-windows-{#Arch}
SetupIconFile={#SrcDir}\assets\yadua.ico
UninstallDisplayIcon={app}\yadua-gui.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Default to an all-users install (Program Files); the dialog lets the user
; pick a per-user install instead, which needs no elevation.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
; The PATH task edits the user environment; tell Windows to broadcast it.
ChangesEnvironment=yes
#if Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif

[Tasks]
Name: desktopicon; Description: "Create a &desktop shortcut"; Flags: unchecked
Name: addtopath;   Description: "Add the &CLI (yadua.exe) to your user PATH"; Flags: unchecked

[Files]
Source: "{#SrcDir}\yadua-gui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\yadua.exe";     DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\LICENSE";       DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\README.md";     DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\YADUA"; Filename: "{app}\yadua-gui.exe"
Name: "{autodesktop}\YADUA";  Filename: "{app}\yadua-gui.exe"; Tasks: desktopicon

[Registry]
; Append the install dir to the *user* PATH (works for both install modes).
; NeedsAddPath prevents duplicate entries on reinstall/upgrade.
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; \
  ValueData: "{olddata};{app}"; Tasks: addtopath; \
  Check: NeedsAddPath(ExpandConstant('{app}'))

[Run]
; The GUI elevates itself via its manifest, so launching it post-install
; shows the expected UAC prompt.
Filename: "{app}\yadua-gui.exe"; Description: "Launch YADUA"; \
  Flags: nowait postinstall skipifsilent shellexec

[Code]
function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';',
                ';' + Uppercase(OrigPath) + ';') = 0;
end;
