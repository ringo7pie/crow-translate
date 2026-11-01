# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Crow Translate のビルドに必要なツール（Qt 6 / Inno Setup / vcpkg）を E:\Tools に導入する。

.DESCRIPTION
    - Qt 6        : aqtinstall（公式ミラー download.qt.io から取得）で導入
    - Inno Setup  : jrsoftware.org の公式インストーラーを取得し、署名を検証してから導入
    - vcpkg       : github.com/microsoft/vcpkg を clone して bootstrap
    - Tesseract   : -InstallTesseract 指定時に vcpkg でソースからビルド（Leptonica も一緒に入る）

    aqtinstall は E:\Tools\build-env\aqt-venv に導入済みである前提（無ければ作成する）。
    各手順は、結果の存在確認まで行う。

.EXAMPLE
    powershell -File ./installer/Install-Tools.ps1
#>
[CmdletBinding()]
param(
    # 導入先のルート
    [string]$ToolsRoot = 'E:\Tools',

    # 導入する Qt のバージョンとアーキテクチャ
    [string]$QtVersion = '6.8.3',
    [string]$QtArchitecture = 'win64_msvc2022_64',

    # Crow Translate が使う Qt の追加モジュール（aqt のモジュール名。TextToSpeech は qtspeech、StateMachine は qtscxml に含まれる）
    [string[]]$QtModules = @('qtmultimedia', 'qtspeech', 'qtscxml'),

    # Inno Setup 6 のインストーラーの取得先（公式ダウンロードページ jrsoftware.org が案内する作者の GitHub リリース）
    [string]$InnoSetupUrl = 'https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe',

    [switch]$SkipQt,
    [switch]$SkipInnoSetup,
    [switch]$SkipVcpkg,

    # vcpkg で Tesseract（と Leptonica）をソースからビルドする
    [switch]$InstallTesseract,

    # vcpkg で ECM（Extra CMake Modules。Crow Translate の CMake が必須とする）を導入する
    [switch]$InstallEcm
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$buildEnvDir = Join-Path $ToolsRoot 'build-env'  # 補助ツール（venv など）の置き場
$aqtVenvDir = Join-Path $buildEnvDir 'aqt-venv'  # aqtinstall 用の仮想環境
$qtRoot = Join-Path $ToolsRoot 'Qt'  # Qt の導入先
$innoSetupDir = Join-Path $ToolsRoot 'InnoSetup6'  # Inno Setup の導入先
$vcpkgDir = Join-Path $ToolsRoot 'vcpkg'  # vcpkg の導入先

# 外部コマンドを実行し、終了コードが 0 以外なら止まる
function Invoke-Tool {
    param([string]$FilePath, [string[]]$Arguments)
    Write-Output ">> $FilePath $($Arguments -join ' ')"
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath が失敗しました（終了コード $LASTEXITCODE）"
    }
}

# ファイルまたはフォルダが存在しなければ止まる
function Assert-PathExists {
    param([string]$Path, [string]$Description)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description が見つかりません: $Path"
    }
}

New-Item -ItemType Directory -Path $ToolsRoot -Force | Out-Null

# ---- Qt 6 ----
if (-not $SkipQt) {
    $venvPython = Join-Path (Join-Path $aqtVenvDir 'Scripts') 'python.exe'  # venv の Python
    if (-not (Test-Path -LiteralPath $venvPython)) {
        New-Item -ItemType Directory -Path $buildEnvDir -Force | Out-Null
        Invoke-Tool (Join-Path (Join-Path $ToolsRoot 'Python310') 'python.exe') @('-I', '-m', 'venv', $aqtVenvDir)
        Invoke-Tool $venvPython @('-I', '-m', 'pip', 'install', '--disable-pip-version-check', 'aqtinstall')
    }
    $aqtExecutable = Join-Path (Join-Path $aqtVenvDir 'Scripts') 'aqt.exe'  # aqtinstall の実行ファイル
    Assert-PathExists $aqtExecutable 'aqt.exe'

    # 指定したアーキテクチャとモジュール名が有効か、事前に一覧で見せる（違えば install が失敗する）
    Invoke-Tool $aqtExecutable @('list-qt', 'windows', 'desktop', '--modules', $QtVersion, $QtArchitecture)

    $installArguments = @('install-qt', 'windows', 'desktop', $QtVersion, $QtArchitecture, '--outputdir', $qtRoot)  # aqt install-qt の引数
    if ($QtModules.Count -gt 0) { $installArguments += @('-m') + $QtModules }
    Invoke-Tool $aqtExecutable $installArguments

    $qtPrefix = Join-Path (Join-Path $qtRoot $QtVersion) ($QtArchitecture -replace '^win64_', '')  # Qt の prefix（例: E:\Tools\Qt\6.8.3\msvc2022_64）
    Assert-PathExists (Join-Path (Join-Path $qtPrefix 'bin') 'windeployqt6.exe') 'windeployqt6.exe'
    Assert-PathExists (Join-Path (Join-Path $qtPrefix 'bin') 'lrelease.exe') 'lrelease.exe'
    foreach ($component in 'Qt6Widgets', 'Qt6Multimedia', 'Qt6TextToSpeech', 'Qt6StateMachine', 'Qt6Concurrent', 'Qt6LinguistTools') {
        $configPath = Join-Path (Join-Path (Join-Path (Join-Path $qtPrefix 'lib') 'cmake') $component) "${component}Config.cmake"  # CMake の設定ファイル
        if (Test-Path -LiteralPath $configPath) {
            Write-Output "OK: $component"
        }
        else {
            Write-Warning "不足: $component（$configPath）。-QtModules のモジュール名を見直してください。"
        }
    }
    Write-Output "Qt の prefix: $qtPrefix"
}

# ---- Inno Setup ----
if (-not $SkipInnoSetup) {
    $downloadDir = Join-Path $buildEnvDir 'downloads'  # ダウンロード物の置き場
    New-Item -ItemType Directory -Path $downloadDir -Force | Out-Null
    $installerPath = Join-Path $downloadDir 'innosetup-setup.exe'  # 取得した Inno Setup のインストーラー
    Invoke-WebRequest -Uri $InnoSetupUrl -OutFile $installerPath -UseBasicParsing

    # 実行ファイル（先頭が MZ）でなければ中止する（HTML のエラーページなどを実行しないため）
    $headBytes = [System.IO.File]::ReadAllBytes($installerPath)[0..1]  # ファイル先頭 2 バイト
    if ($headBytes[0] -ne 0x4D -or $headBytes[1] -ne 0x5A) {
        throw "取得したファイルが実行ファイルではありません: $InnoSetupUrl"
    }

    # 署名が有効で、発行元が Inno Setup の公式の署名者であることを確認してから実行する
    $signature = Get-AuthenticodeSignature -LiteralPath $installerPath  # Authenticode 署名の検証結果
    if ($signature.Status -ne 'Valid') {
        throw "Inno Setup のインストーラーの署名が無効です（状態: $($signature.Status)）。実行を中止します。"
    }
    Write-Output "署名者: $($signature.SignerCertificate.Subject)"
    # 公式の検証ページ（https://jrsoftware.org/isdl-verify.php）によると、6.4.2 以降の発行元名は「Pyrsys B.V.」
    # （共同開発者 Martijn Laan 氏の会社）、それ以前は「Open Source Developer, Martijn Laan」
    if ($signature.SignerCertificate.Subject -notmatch 'CN=Pyrsys B\.V\.|Martijn Laan') {
        throw "署名者が公式の記載（Pyrsys B.V. / Martijn Laan）と異なります: $($signature.SignerCertificate.Subject)。実行を中止します。"
    }

    # GUI アプリは & で呼ぶと終了を待たず $LASTEXITCODE も設定されないため、Start-Process -Wait で終了コードを取る
    $installProcess = Start-Process -FilePath $installerPath -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/CURRENTUSER', "/DIR=$innoSetupDir") -Wait -PassThru  # Inno Setup インストーラーのプロセス
    if ($installProcess.ExitCode -ne 0) {
        throw "Inno Setup のインストーラーが失敗しました（終了コード $($installProcess.ExitCode)）"
    }
    Assert-PathExists (Join-Path $innoSetupDir 'ISCC.exe') 'ISCC.exe'
}

# ---- vcpkg ----
if (-not $SkipVcpkg) {
    if (-not (Test-Path -LiteralPath (Join-Path $vcpkgDir '.git'))) {
        Invoke-Tool 'git' @('clone', '--depth', '1', 'https://github.com/microsoft/vcpkg.git', $vcpkgDir)
    }
    $vcpkgExecutable = Join-Path $vcpkgDir 'vcpkg.exe'  # vcpkg の実行ファイル
    if (-not (Test-Path -LiteralPath $vcpkgExecutable)) {
        Invoke-Tool (Join-Path $vcpkgDir 'bootstrap-vcpkg.bat') @('-disableMetrics')
    }
    Assert-PathExists $vcpkgExecutable 'vcpkg.exe'

    # 各ポートのビルドで使う並列数を論理プロセッサ数に明示する（vcpkg は環境変数 VCPKG_MAX_CONCURRENCY を参照する）
    $env:VCPKG_MAX_CONCURRENCY = [string][Environment]::ProcessorCount

    if ($InstallTesseract) {
        # Tesseract と Leptonica を x64-windows でビルドして導入する（時間がかかる）
        Invoke-Tool $vcpkgExecutable @('install', 'tesseract:x64-windows', '--disable-metrics')
    }
    if ($InstallEcm) {
        # ECM は CMake モジュールの集まりでコンパイルは無い（share\ECM に入る）
        Invoke-Tool $vcpkgExecutable @('install', 'ecm:x64-windows', '--disable-metrics')
        Assert-PathExists (Join-Path (Join-Path (Join-Path $vcpkgDir 'installed') 'x64-windows\share\ECM') 'ECMConfig.cmake') 'ECMConfig.cmake'
    }
}

Write-Output '完了'
