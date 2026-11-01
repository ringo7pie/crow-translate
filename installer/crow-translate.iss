; SPDX-FileCopyrightText: 2026 ringo7pie
; SPDX-License-Identifier: GPL-3.0-or-later

; Crow Translate インストーラー（Inno Setup 6）
; Build-Installer.ps1 から ISCC に /DAppVersion= /DStagingDir= /DOutputDir= [/DIncludeVcRedist] を渡して使う。

#ifndef AppVersion
  #error AppVersion が未定義です。Build-Installer.ps1 経由でコンパイルしてください。
#endif
#ifndef StagingDir
  #error StagingDir が未定義です。Build-Installer.ps1 経由でコンパイルしてください。
#endif
#ifndef OutputDir
  #define OutputDir "out"
#endif

#define AppName "Crow Translate"
#define AppExeName "crow.exe"
#define AppPublisher "Crow Translate"
#define AppUrl "https://apps.kde.org/crow-translate"

[Setup]
; アプリを一意に識別する GUID（変更するとアップグレードできなくなる）
AppId={{6F3C2A64-9B0E-4C57-8D1B-5A7E2C9F4D10}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile={#StagingDir}\LICENSE.txt
OutputDir={#OutputDir}
OutputBaseFilename=crow-translate-{#AppVersion}-setup
UninstallDisplayIcon={app}\{#AppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; 64bit 専用（Qt / Tesseract を 64bit でビルドする前提）
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; ステージング内のすべて（exe / Qt / Tesseract / tessdata / 翻訳 / espeak-ng-data など）
Source: "{#StagingDir}\*"; DestDir: "{app}"; Excludes: "vc_redist.x64.exe"; Flags: ignoreversion recursesubdirs createallsubdirs
#ifdef IncludeVcRedist
Source: "{#StagingDir}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
#endif

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
#ifdef IncludeVcRedist
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Microsoft Visual C++ ランタイムをインストールしています..."; Check: NeedsVcRedist; Flags: waituntilterminated
#endif
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

#ifdef IncludeVcRedist
[Code]
// VC++ 2015-2022 (x64) ランタイムが未導入なら True を返す（導入済みの Installed が 1 かどうかで判定）
function NeedsVcRedist: Boolean;
var
  installedFlag: Cardinal; // レジストリから読んだ Installed の値
begin
  Result := not (RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', installedFlag) and (installedFlag = 1));
end;
#endif
