#ifndef PayloadDir
  #error PayloadDir must point to the deployed application folder
#endif
#ifndef PackageDir
  #error PackageDir must point to the output folder
#endif
#ifndef ReleaseVersion
  #error ReleaseVersion must match the application's release tag
#endif
[Setup]
AppId={{EA0BD783-856D-479D-89AA-26B443231FEB}
AppName=LAMBDA Player
AppVersion={#ReleaseVersion}
AppPublisher=LAMBDA
DefaultDirName={localappdata}\Programs\LAMBDA Player
DefaultGroupName=LAMBDA Player
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#PackageDir}
OutputBaseFilename=LAMBDA-Player-Setup-{#ReleaseVersion}-x64
SetupIconFile=..\resources\branding\lambda.ico
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\LambdaPlayer.exe
ChangesAssociations=yes
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
Name: "associations"; Description: "Offer LAMBDA Player in Windows Open with and Default Apps"

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "frc_bench.exe,vc_redist.x64.exe"

[Icons]
Name: "{group}\LAMBDA Player"; Filename: "{app}\LambdaPlayer.exe"
Name: "{group}\Choose default video player"; Filename: "ms-settings:defaultapps"
Name: "{group}\Uninstall LAMBDA Player"; Filename: "{uninstallexe}"
Name: "{autodesktop}\LAMBDA Player"; Filename: "{app}\LambdaPlayer.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\LAMBDA.Video"; ValueType: string; ValueData: "LAMBDA video"; Flags: uninsdeletekey; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\LAMBDA.Video\DefaultIcon"; ValueType: string; ValueData: "{app}\LambdaPlayer.exe,0"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\LAMBDA.Video\shell\open\command"; ValueType: string; ValueData: """{app}\LambdaPlayer.exe"" ""%1"""; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities"; ValueName: "ApplicationName"; ValueType: string; ValueData: "LAMBDA Player"; Flags: uninsdeletekey; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities"; ValueName: "ApplicationDescription"; ValueType: string; ValueData: "Play local movies and series with LAMBDA Player"; Tasks: associations
Root: HKCU; Subkey: "Software\RegisteredApplications"; ValueName: "LAMBDA Player"; ValueType: string; ValueData: "Software\LAMBDA Player\Capabilities"; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.mkv\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".mkv"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.mp4\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".mp4"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.m4v\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".m4v"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.avi\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".avi"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.mov\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".mov"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.webm\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".webm"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.wmv\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".wmv"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.flv\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".flv"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.ts\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".ts"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.mts\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".mts"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.m2ts\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".m2ts"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.mpg\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".mpg"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.mpeg\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".mpeg"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.ogv\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".ogv"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.3gp\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".3gp"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations
Root: HKCU; Subkey: "Software\Classes\.vob\OpenWithProgids"; ValueName: "LAMBDA.Video"; ValueType: none; Flags: uninsdeletevalue; Tasks: associations
Root: HKCU; Subkey: "Software\LAMBDA Player\Capabilities\FileAssociations"; ValueName: ".vob"; ValueType: string; ValueData: "LAMBDA.Video"; Tasks: associations

[Run]
Filename: "{app}\LambdaPlayer.exe"; Description: "Launch LAMBDA Player"; Flags: nowait postinstall skipifsilent
Filename: "ms-settings:defaultapps"; Description: "Choose LAMBDA Player as your default video player"; Flags: shellexec postinstall unchecked skipifsilent
