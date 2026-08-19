; ROUNDTABLE NLE — Inno Setup Installer
; Build with: ISCC.exe installer.iss

#define MyAppName "ROUNDTABLE NLE"
#define MyAppPublisher "Roundtable"
#define MyAppURL "https://github.com/ROUNDTABLE-TALK/roundtable"
#define MyAppExeName "roundtable.exe"
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
; NOTE: Override via: ISCC.exe /DMyAppVersion=x.xx installer.iss

[Setup]
AppId={{B8A7C3D1-2E5F-4A9C-8B7D-6F1E3A2C5D8B}
AppName={#MyAppName}
AppVerName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf64}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=.\dist
OutputBaseFilename=Roundtable-NLE-v{#MyAppVersion}-Setup
Compression=lzma2/ultra64
SolidCompression=yes
SetupIconFile=icon.ico
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}
; Silent install support for auto-update
DisableProgramGroupPage=yes
CloseApplications=force
; Check for VC++ redist (requires Inno Setup 6.8+ for PrerequisitesCheck directive)
; Manual check done via [Code] section below

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"

[Dirs]
; Writable data directories — these must be user-writable so the app can
; save projects and download runtime assets (characters, backgrounds,
; fonts, etc.) even when installed under Program Files.
; Subdirectories created inside these will inherit the permissions.
Name: "{app}\assets"; Permissions: users-modify
Name: "{app}\assets\presets\shots"; Permissions: users-modify
Name: "{app}\projects"; Permissions: users-modify
; Crash logs land in {app}\logs at runtime — must be user-writable so
; non-admin users can create dumps when the app is installed under
; Program Files.  See main.cpp:71 (NEVER use %LOCALAPPDATA%).
Name: "{app}\logs"; Permissions: users-modify

[Files]
; Main executable
Source: "build\bin\Release\roundtable.exe"; DestDir: "{app}"; Flags: ignoreversion

; Runtime DLLs
Source: "build\bin\Release\*.dll"; DestDir: "{app}"; Flags: ignoreversion
; Generated CUDA closure used by launch.vbs for loader-time diagnostics.
; No skipifsourcedoesntexist: a default CUDA release must fail packaging if
; the post-link deployment/verification step did not produce this manifest.
Source: "build\bin\Release\roundtable-cuda-runtime.txt"; DestDir: "{app}"; Flags: ignoreversion

; FFmpeg CLI tools — required by ShotComposer for thumbnail extraction
; (libav* DLLs are already covered by the *.dll glob above).  Search path
; in src/ui/panels/characters/ShotComposerVideo.cpp:181-186 looks for
; ffmpeg.exe in QApplication::applicationDirPath().
Source: "third_party\ffmpeg\bin\ffmpeg.exe";  DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "third_party\ffmpeg\bin\ffprobe.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; Qt deployment configuration (overrides hardcoded plugin path)
Source: "qt.conf"; DestDir: "{app}"; Flags: ignoreversion

; Qt plugins (must preserve directory structure)
Source: "build\bin\Release\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs
Source: "build\bin\Release\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion recursesubdirs
Source: "build\bin\Release\iconengines\*"; DestDir: "{app}\iconengines"; Flags: ignoreversion recursesubdirs
Source: "build\bin\Release\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs
Source: "build\bin\Release\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs
Source: "build\bin\Release\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion recursesubdirs

; Compiled shaders (required by Vulkan compositor)
Source: "build\shaders\*"; DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs

; Runtime assets (presets, libs, and config — NOT development assets)
; Note: assets/presets/shots/ is excluded entirely — fresh install has no shots.
Source: "assets\presets\effects\*"; DestDir: "{app}\assets\presets\effects"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "assets\lib\*"; DestDir: "{app}\assets\lib"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "assets\character_metadata.json"; DestDir: "{app}\assets"; Flags: ignoreversion
Source: "assets\default_layout.bin"; DestDir: "{app}\assets"; Flags: ignoreversion skipifsourcedoesntexist
Source: "icon.png"; DestDir: "{app}"; Flags: ignoreversion

; Launcher
Source: "launch.vbs"; DestDir: "{app}"; Flags: ignoreversion

; Third-party attribution (opened by Help -> Third-Party Licenses)
Source: "docs\THIRD_PARTY_LICENSES.md"; DestDir: "{app}\docs"; Flags: ignoreversion

[Icons]
; Route installed shortcuts through the silent preflight launcher.  Keep the
; application icon and working directory identical to a direct EXE shortcut.
Name: "{group}\{#MyAppName}"; Filename: "{sys}\wscript.exe"; Parameters: """{app}\launch.vbs"""; WorkingDir: "{app}"; IconFilename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{sys}\wscript.exe"; Parameters: """{app}\launch.vbs"""; WorkingDir: "{app}"; IconFilename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Launch app after install (unless silent update)
Filename: "{sys}\wscript.exe"; Parameters: """{app}\launch.vbs"""; WorkingDir: "{app}"; Description: "Launch {#MyAppName}"; Flags: postinstall nowait skipifsilent

[UninstallRun]
; Clean up cache on uninstall (optional)
Filename: "{sys}\cmd.exe"; Parameters: "/c rmdir /s /q ""{localappdata}\ROUNDTABLE\cache"""; Flags: runhidden

[Code]
{ Check for NVIDIA GPU during install }
function IsNvidiaGpuPresent: Boolean;
begin
  Result := False;
  { Note: GPU detection disabled - NVIDIA check is advisory only }
end;

{ Check for VC++ Redistributable }
function IsVCRedistInstalled: Boolean;
var
  Installed: Boolean;
begin
  Installed := RegKeyExists(HKLM, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64');
  if not Installed then
    Installed := RegKeyExists(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64');
  Result := Installed;
end;

{ Pre-install gate: warn user about missing VC++ redist while they can
  still abort, instead of after install when the app won't launch. }
function InitializeSetup(): Boolean;
var
  Choice: Integer;
begin
  Result := True;
  if not IsVCRedistInstalled then
  begin
    Choice := MsgBox(
      'Microsoft Visual C++ 2015-2022 Redistributable (x64) is not installed.' #13#13 +
      'ROUNDTABLE will not launch without it.' #13#13 +
      'Click YES to open the download page in your browser, then install the redistributable before re-running this installer.' #13 +
      'Click NO to continue anyway (only if you plan to install the redistributable yourself).' #13 +
      'Click CANCEL to abort installation.',
      mbConfirmation, MB_YESNOCANCEL);
    if Choice = IDYES then
    begin
      ShellExec('open', 'https://aka.ms/vs/17/release/vc_redist.x64.exe',
               '', '', SW_SHOW, ewNoWait, Choice);
      Result := False;  { abort install — user is going to download first }
    end
    else if Choice = IDCANCEL then
      Result := False;
    { IDNO falls through and continues the install }
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  { Post-install confirmation that the redist is now present.  Belt and
    braces in case the user chose "continue anyway" in InitializeSetup. }
  if CurStep = ssDone then
  begin
    if not IsVCRedistInstalled then
      MsgBox('Microsoft Visual C++ Redistributable still not detected.' #13#13
             'ROUNDTABLE will not launch until you install it from:' #13
             'https://aka.ms/vs/17/release/vc_redist.x64.exe',
             mbInformation, MB_OK);
  end;
end;
