; Inno Setup script for Smart Knob v2.8.3

[Setup]
AppName=Smart Knob
AppVersion=2.8.3
DefaultDirName={commonpf}\SmartKnob
DefaultGroupName=Smart Knob
UninstallDisplayIcon={app}\smartknob.exe
OutputDir=output
OutputBaseFilename=SmartKnobSetup
Compression=lzma
SolidCompression=yes

[Files]
Source: "dist\smartknob.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "main_window_state.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\radio_profiles.ini"; DestDir: "{app}"; Flags: ignoreversion
; Add any other required files/folders below
Source: "dist\*.*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Smart Knob"; Filename: "{app}\smartknob.exe"
Name: "{commondesktop}\Smart Knob"; Filename: "{app}\smartknob.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"

[Run]
Filename: "{app}\smartknob.exe"; Description: "Launch Smart Knob"; Flags: nowait postinstall skipifsilent
