; Inno Setup 6 script. Build with: build_installer.bat
#define AppName "BLOCKVERSE"
#define AppVer "2.0.0"
[Setup]
AppId={{B10C4E25-2D00-4A27-9C11-0B10C4E52020}
AppName={#AppName}
AppVersion={#AppVer}
AppPublisher=BLOCKVERSE
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
OutputDir=..\dist
OutputBaseFilename=BLOCKVERSE-Setup-{#AppVer}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE.txt
UninstallDisplayIcon={app}\BLOCKVERSE.exe
WizardStyle=modern
[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked
Name: "firewall"; Description: "Allow multiplayer through Windows Firewall (UDP 27125)"; Flags: unchecked
[Files]
Source: "..\build\Release\BLOCKVERSE.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
[Icons]
Name: "{group}\BLOCKVERSE"; Filename: "{app}\BLOCKVERSE.exe"
Name: "{group}\BLOCKVERSE Server"; Filename: "{app}\BLOCKVERSE.exe"; Parameters: "--server"
Name: "{group}\Uninstall BLOCKVERSE"; Filename: "{uninstallexe}"
Name: "{autodesktop}\BLOCKVERSE"; Filename: "{app}\BLOCKVERSE.exe"; Tasks: desktopicon
[Run]
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""BLOCKVERSE"" dir=in action=allow program=""{app}\BLOCKVERSE.exe"" protocol=UDP localport=27125"; Flags: runhidden; Tasks: firewall
Filename: "{app}\BLOCKVERSE.exe"; Description: "Launch BLOCKVERSE"; Flags: nowait postinstall skipifsilent
[UninstallRun]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""BLOCKVERSE"""; Flags: runhidden; RunOnceId: "DelFirewall"
