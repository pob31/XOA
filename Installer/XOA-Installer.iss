; XOA Inno Setup installer script.
; Requires Inno Setup 6.x (https://jrsoftware.org/isinfo.php).
;
; Local build, from the repo root, after a Release build:
;   ISCC.exe /DMyAppVersion=0.1.0 /Odist Installer\XOA-Installer.iss
;
; The release workflow passes /DMyAppVersion=<tag without the leading v> so the
; installer's AppVersion and filename match the GitHub Release.
;
; The installer is UNSIGNED - there is no Windows code-signing certificate for
; this project, so first run shows a SmartScreen prompt. Only the macOS DMG is
; signed and notarized.

#define MyAppName "XOA"
; Version: overridable from the command line. The default is for local manual
; builds and must track project(XOA VERSION ...) in CMakeLists.txt.
#ifndef MyAppVersion
#define MyAppVersion "0.1.0"
#endif
; Where the Release artefacts live, relative to this .iss file. CMake-native:
; cmake --build <dir> --config Release puts them in <dir>\XOA_artefacts\Release.
#ifndef MyBuildDir
#define MyBuildDir "..\build-release\XOA_artefacts\Release"
#endif
#define MyAppPublisher "Pix et Bel"
#define MyAppURL "https://wfs-diy.net"
#define MyAppExeName "XOA.exe"

[Setup]
AppId={{BC6BDDA5-15E4-4FDD-8AB5-899DAD12DA2A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputDir=Output
OutputBaseFilename=XOA-Setup-{#MyAppVersion}
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes

; The GPL requires the license to be conveyed with the binaries: show it on the
; standard Inno license page and install it alongside the app.
LicenseFile=..\LICENSE

[Languages]
; English and French only - those are the two translations the app ships
; (Resources/lang). Add a line here when a new lang file lands.
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french";  MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; The application.
Source: "{#MyBuildDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; Runtime data the app resolves relative to its own executable
; (LocalizationManager -> <exeDir>\Resources\lang, the binaural monitor ->
; <exeDir>\Resources\SOFA). CMake stages both next to the built exe; install
; them in the same relative layout or the app starts untranslated and with no
; fallback HRTF set.
Source: "..\Resources\lang\*.json"; DestDir: "{app}\Resources\lang"; Flags: ignoreversion
Source: "..\assets\SOFA\*.sofa";    DestDir: "{app}\Resources\SOFA"; Flags: ignoreversion

; Webcam head-tracker plugin for the binaural monitor, plus the OpenCV runtime
; it loads and the YuNet face model - staged next to the exe by
; tools\headtrack\build-headtrack-plugin.ps1 (the release workflow runs it).
; All optional: skipifsourcedoesntexist means a build without the plugin still
; produces a working installer, and the app logs the plugin's absence and keeps
; head orientation on manual/OSC control.
Source: "{#MyBuildDir}\wfs_headtrack.dll";                 DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#MyBuildDir}\opencv_world4100.dll";              DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#MyBuildDir}\opencv_videoio_msmf4100_64.dll";    DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#MyBuildDir}\face_detection_yunet_2023mar.onnx"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; Legal / reference documents, renamed to .txt so double-clicking opens Notepad
; instead of a "how do you want to open this file" prompt.
Source: "..\LICENSE";                DestDir: "{app}"; DestName: "LICENSE.txt";             Flags: ignoreversion
Source: "..\README.md";              DestDir: "{app}"; DestName: "README.txt";              Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; DestName: "THIRD_PARTY_NOTICES.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; .xoa project-manifest association. XoaFileManager writes <Folder>\<Folder>.xoa
; as the manifest, and AppShell::applyStartupCommandLine accepts a project path
; as the first command-line token, so "%1" opens the project it belongs to.
Root: HKA; Subkey: "Software\Classes\.xoa"; ValueType: string; ValueName: ""; ValueData: "XOA.Project"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\XOA.Project"; ValueType: string; ValueName: ""; ValueData: "XOA Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\XOA.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\XOA.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
