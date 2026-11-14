[Setup]
AppId=Pakr.{#PakrAppId}
AppName={#PakrAppName}
AppVersion={#PakrVersion}
DefaultDirName={localappdata}\Programs\Pakr\{#PakrAppId}
DefaultGroupName={#PakrAppName}
UninstallDisplayIcon={app}\Pakr.Windows.exe
#ifdef PakrIconFile
SetupIconFile={#PakrIconFile}
#endif
OutputDir=..\windows-artifact
OutputBaseFilename=Pakr-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

[Files]
Source: "..\windows-publish\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#PakrAppName}"; Filename: "{app}\Pakr.Windows.exe"

[Run]
Filename: "{app}\Pakr.Windows.exe"; Description: "Open {#PakrAppName}"; Flags: nowait postinstall skipifsilent
