; SPDX-FileCopyrightText: 2026 ringo7pie
; SPDX-License-Identifier: GPL-3.0-or-later

; Crow Translate インストーラー（Inno Setup 6）
; Build-Installer.ps1 から ISCC に /DAppVersion= /DAppNumericVersion= /DStagingDir= /DOutputDir= [/DIncludeVcRedist] を渡して使う。

#ifndef AppVersion
  #error AppVersion が未定義です。Build-Installer.ps1 経由でコンパイルしてください。
#endif
#ifndef AppNumericVersion
  #error AppNumericVersion が未定義です。Build-Installer.ps1 経由でコンパイルしてください。
#endif
#ifndef StagingDir
  #error StagingDir が未定義です。Build-Installer.ps1 経由でコンパイルしてください。
#endif
#ifndef OutputDir
  #define OutputDir "out"
#endif

#define AppName "Crow Translate"
#define AppExeName "crow.exe"
#define AppPublisher "ringo7pie (unofficial fork)"
#define AppUrl "https://github.com/ringo7pie/crow-translate"

[Setup]
; アプリを一意に識別する GUID（変更するとアップグレードできなくなる）
AppId={{6F3C2A64-9B0E-4C57-8D1B-5A7E2C9F4D10}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion} (Unofficial)
VersionInfoVersion={#AppNumericVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
DefaultDirName={autopf}\Crow Translate Unofficial
DefaultGroupName={#AppName} (Unofficial)
DisableProgramGroupPage=yes
LicenseFile={#StagingDir}\LICENSE.txt
InfoAfterFile={#StagingDir}\README-Windows.txt
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
Name: "{group}\Readme"; Filename: "{app}\README-Windows.txt"
Name: "{group}\追加機能のセットアップ"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File ""{app}\optional\Install-OptionalComponents.ps1"""
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
#ifdef IncludeVcRedist
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Microsoft Visual C++ ランタイムをインストールしています..."; Check: NeedsVcRedist; Flags: waituntilterminated
#endif
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\optional\Install-OptionalComponents.ps1"""; StatusMsg: "追加機能のセットアップを開いています..."; Check: ShouldOpenOptionalSetup; Flags: runasoriginaluser runhidden waituntilterminated skipifsilent
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[Code]
var
  OptionalSetupPage: TInputOptionWizardPage;

procedure InitializeWizard;
begin
  OptionalSetupPage := CreateInputOptionPage(wpSelectTasks,
    'Indra OCR・翻訳モデルの追加導入',
    '追加機能をインストールしますか？',
    '「はい」を選ぶと、本体の配置後に追加セットアップを開きます。' + #13#10 +
    'Indra OCR、PLaMo、Index、Hy-MT2、Seed-X、CATを個別に選択でき、容量と利用条件を確認してから取得します。' + #13#10 +
    '「いいえ」の場合、モデルやPythonのダウンロードは行いません。後からアプリ内の「ローカルAIの管理」やスタートメニューで追加できます。',
    True, False);
  OptionalSetupPage.Add('はい：追加セットアップを開く');
  OptionalSetupPage.Add('いいえ：本体だけをインストールする');
  OptionalSetupPage.SelectedValueIndex := 1;
end;

function ShouldOpenOptionalSetup: Boolean;
begin
  Result := (not WizardSilent) and OptionalSetupPage.Values[0];
end;

#ifdef IncludeVcRedist
// VC++ 2015-2022 (x64) ランタイムが未導入なら True を返す（導入済みの Installed が 1 かどうかで判定）
function NeedsVcRedist: Boolean;
var
  installedFlag: Cardinal; // レジストリから読んだ Installed の値
begin
  Result := not (RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', installedFlag) and (installedFlag = 1));
end;
#endif
