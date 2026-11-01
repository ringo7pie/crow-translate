# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Crow Translate の Windows インストーラー（Inno Setup）を作る。

.DESCRIPTION
    1. cmake --install でステージング領域へ配置
    2. windeployqt6 で Qt の DLL / プラグインを配置
    3. Tesseract / Leptonica などの依存 DLL を配置
    4. OCR 言語データ（tessdata）を配置
    5. 翻訳（.qm）を <exe の場所>/data/locale/<言語>/LC_MESSAGES/ に整える
    6. ISCC.exe（Inno Setup）で installer/crow-translate.iss をコンパイル

    ビルド環境のパスはすべて引数で受け取る（既定値は持たない）。

.EXAMPLE
    .\installer\Build-Installer.ps1 `
        -BuildDir D:\build\crow `
        -QtBinDir D:\Qt\6.8.0\msvc2022_64\bin `
        -DependencyBinDir D:\vcpkg\installed\x64-windows\bin `
        -TessdataDir D:\tessdata_fast `
        -LicenseBundleDir D:\release\license-bundle `
        -InnoSetupDir "C:\Program Files (x86)\Inno Setup 6"
#>
[CmdletBinding()]
param(
    # CMake のビルドディレクトリ（cmake --install の対象。Release 構成でビルド済みであること）
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,

    # windeployqt6.exe があるフォルダ（Qt の bin）
    [Parameter(Mandatory = $true)]
    [string]$QtBinDir,

    # Tesseract / Leptonica とその依存 DLL があるフォルダ（例: vcpkg の installed\x64-windows\bin）
    [Parameter(Mandatory = $true)]
    [string]$DependencyBinDir,

    # *.traineddata があるフォルダ（tessdata / tessdata_fast / tessdata_best のいずれか）
    [Parameter(Mandatory = $true)]
    [string]$TessdataDir,

    # Inno Setup のインストール先（ISCC.exe があるフォルダ）
    [Parameter(Mandatory = $true)]
    [string]$InnoSetupDir,

    # Prepare-ReleaseLicenses.py が生成した license-bundle フォルダー（追加のランタイムの通知も含める）
    [Parameter(Mandatory = $true)]
    [string]$LicenseBundleDir,

    # tools/setup_local_models.py --step runtime で作成したランタイム
    [string]$OptionalRuntimeDir = '',
    [string]$PythonExecutable = 'python',

    # cmake.exe のパス（PATH に無い環境向け。既定は Visual Studio 同梱の CMake）
    [string]$CMakeExecutable = 'E:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',

    # 同梱する OCR 言語（tessdata のファイル名の拡張子を除いた部分）
    [string[]]$TessdataLanguages = @('eng', 'jpn', 'osd'),

    # 作業用フォルダ（ステージングと出力先の親）。省略時はリポジトリ直下の installer\out
    [string]$OutputDir = '',

    # 同梱する VC++ 再頒布可能パッケージ（vc_redist.x64.exe）のパス。省略時は同梱しない
    [string]$VcRedistPath = '',

    # ビルド構成（マルチ構成ジェネレーターのとき cmake --install --config に渡す）
    [string]$Configuration = 'Release',

    # インストーラーに付けるバージョン。省略時は CMakeLists.txt の project(VERSION ...) から取る
    [string]$Version = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path  # このスクリプトのあるフォルダ（param の既定値では $PSScriptRoot が空になる環境があるため本体で求める）
$repositoryRoot = Split-Path -Parent $scriptDirectory  # リポジトリのルート
if ($OptionalRuntimeDir -eq '') { $OptionalRuntimeDir = Join-Path $repositoryRoot 'third_party/local-translation/runtime' }
if ($OutputDir -eq '') { $OutputDir = Join-Path $scriptDirectory 'out' }
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
$stagingDir = Join-Path $OutputDir 'staging'  # インストーラーに詰めるファイルの配置先
$installPrefix = Join-Path $OutputDir 'install-prefix'  # cmake --install の一時配置先

# 必須パスの存在確認。無ければ理由を示して止まる
function Assert-PathExists {
    param([string]$Path, [string]$Description)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description が見つかりません: $Path"
    }
}

# 外部コマンドを実行し、終了コードが 0 以外なら止まる
function Invoke-Tool {
    param([string]$FilePath, [string[]]$Arguments)
    Write-Output ">> $FilePath $($Arguments -join ' ')"
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath が失敗しました（終了コード $LASTEXITCODE）"
    }
}

Assert-PathExists $BuildDir 'ビルドディレクトリ'
Assert-PathExists $CMakeExecutable 'cmake.exe'
Assert-PathExists (Join-Path $QtBinDir 'windeployqt6.exe') 'windeployqt6.exe'
Assert-PathExists $DependencyBinDir '依存 DLL のフォルダ'
Assert-PathExists $TessdataDir 'tessdata のフォルダ'
Assert-PathExists (Join-Path $InnoSetupDir 'ISCC.exe') 'ISCC.exe'
Assert-PathExists (Join-Path $LicenseBundleDir 'THIRD-PARTY-NOTICES.txt') '第三者ライセンスの通知'
Assert-PathExists (Join-Path $LicenseBundleDir 'licenses/dependency-manifest.json') '依存ライブラリのマニフェスト'
Assert-PathExists (Join-Path $OptionalRuntimeDir 'llama-server.exe') '追加翻訳用ランタイム（-OptionalRuntimeDir）'
if ($VcRedistPath -ne '') { Assert-PathExists $VcRedistPath 'VC++ 再頒布可能パッケージ' }

# バージョンを CMakeLists.txt から取得する
if ($Version -eq '') {
    $cmakeText = Get-Content -LiteralPath (Join-Path $repositoryRoot 'CMakeLists.txt') -Raw  # CMakeLists.txt の全文
    if ($cmakeText -match 'project\(\s*crow-translate\s+VERSION\s+([0-9]+(\.[0-9]+)*)') {
        $Version = $Matches[1]
    }
    else {
        throw 'CMakeLists.txt からバージョンを取得できません。-Version で指定してください。'
    }
}
Write-Output "バージョン: $Version"
$numericVersion = [regex]::Match($Version, '^\d+(?:\.\d+){1,3}').Value
if ($Version -match '^(\d+\.\d+\.\d+)-local\.(\d+)$') {
    $numericVersion = "$($Matches[1]).$($Matches[2])"
}
if ($numericVersion -eq '') { throw "数値バージョンを取得できません: $Version" }

# 1. ステージングを作り直す
foreach ($directory in @($stagingDir, $installPrefix)) {
    $resolvedDirectory = [System.IO.Path]::GetFullPath($directory)
    if (-not $resolvedDirectory.StartsWith($OutputDir.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "作業フォルダーが出力先の外にあります: $resolvedDirectory"
    }
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    New-Item -ItemType Directory -Path $directory | Out-Null
}

# 2. cmake --install で一時配置先へ入れる
Invoke-Tool $CMakeExecutable @('--install', $BuildDir, '--prefix', $installPrefix, '--config', $Configuration)

# 3. 実行ファイルを探す（KDE の既定では bin\ に入る。無ければ全体から探す）
$executable = Get-ChildItem -LiteralPath $installPrefix -Recurse -Filter 'crow.exe' | Select-Object -First 1  # 配置された crow.exe
if ($null -eq $executable) { throw "crow.exe が $installPrefix に見つかりません。ビルドと install の結果を確認してください。" }
$executableDirectory = $executable.DirectoryName  # crow.exe のあるフォルダ（espeak-ng-data や ONNX の DLL も同居）

# 実行ファイルのあるフォルダの中身（exe / DLL / espeak-ng-data など）をステージング直下へ移す
Copy-Item -Path (Join-Path $executableDirectory '*') -Destination $stagingDir -Recurse -Force
$stagedExecutable = Join-Path $stagingDir 'crow.exe'  # ステージング上の crow.exe

# 4. Qt の DLL とプラグインを配置する
Invoke-Tool (Join-Path $QtBinDir 'windeployqt6.exe') @(
    '--release', '--no-translations', '--no-compiler-runtime', '--no-opengl-sw',
    '--no-system-d3d-compiler', '--no-system-dxc-compiler', '--no-ffmpeg',
    '--exclude-plugins', 'ffmpegmediaplugin', $stagedExecutable
)

# 5. Tesseract / Leptonica と依存 DLL を配置する（Qt 本体の DLL は windeployqt が配置済みなので除く）
$copiedDependencies = @()  # 追加で配置した DLL の名前一覧
foreach ($dll in Get-ChildItem -LiteralPath $DependencyBinDir -Filter '*.dll') {
    if ($dll.Name -like 'Qt6*') { continue }
    $destination = Join-Path $stagingDir $dll.Name  # 配置先
    if (-not (Test-Path -LiteralPath $destination)) {
        Copy-Item -LiteralPath $dll.FullName -Destination $destination
        $copiedDependencies += $dll.Name
    }
}
Write-Output "依存 DLL を配置: $($copiedDependencies -join ', ')"
$tesseractDll = Get-ChildItem -LiteralPath $stagingDir -Filter 'tesseract*.dll' | Select-Object -First 1  # Tesseract 本体の DLL
if ($null -eq $tesseractDll) { throw "Tesseract の DLL が $DependencyBinDir にありません。OCR が動かないため中止します。" }
if (-not (Get-ChildItem -LiteralPath $stagingDir -Filter 'leptonica*.dll')) {
    Write-Warning 'Leptonica の DLL が見つかりません（Tesseract が静的に取り込んでいれば問題ありません）。'
}

# 6. OCR 言語データ（tessdata）を配置する。アプリは <exe の場所>\tessdata を既定で参照する
$tessdataStaging = Join-Path $stagingDir 'tessdata'  # 同梱する言語データの配置先
New-Item -ItemType Directory -Path $tessdataStaging | Out-Null
foreach ($language in $TessdataLanguages) {
    $source = Join-Path $TessdataDir "$language.traineddata"  # 言語データの元ファイル
    Assert-PathExists $source "言語データ $language.traineddata"
    Copy-Item -LiteralPath $source -Destination $tessdataStaging
}

# 7. 翻訳（.qm）を <exe の場所>\data\locale\<言語>\LC_MESSAGES\ に整える
#    （Windows の GenericDataLocation には <APPDIR>\data が含まれるため、ここに置くとアプリが見つけられる）
$localeStaging = Join-Path (Join-Path $stagingDir 'data') 'locale'  # 翻訳の配置先
$translationFiles = @(Get-ChildItem -LiteralPath $installPrefix -Recurse -Filter 'crow-translate_qt.qm')  # install された .qm
if ($translationFiles.Count -eq 0) {
    Write-Warning '翻訳ファイル（.qm）が見つかりません。ビルド時に ECM の .qm 生成が行われたか確認してください。'
}
foreach ($translationFile in $translationFiles) {
    $messagesDirectory = $translationFile.Directory  # LC_MESSAGES フォルダ
    $languageName = $messagesDirectory.Parent.Name  # 言語コード（例: ja）
    $destinationDirectory = Join-Path (Join-Path $localeStaging $languageName) 'LC_MESSAGES'  # 言語ごとの配置先
    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    Copy-Item -LiteralPath $translationFile.FullName -Destination $destinationDirectory -Force
}
Write-Output "翻訳を配置: $($translationFiles.Count) 言語"

# 8. ライセンスと VC++ 再頒布可能パッケージ
Copy-Item -LiteralPath (Join-Path (Join-Path $repositoryRoot 'LICENSES') 'GPL-3.0-or-later.txt') -Destination (Join-Path $stagingDir 'LICENSE.txt')
Copy-Item -Path (Join-Path $LicenseBundleDir '*') -Destination $stagingDir -Recurse -Force
$readmeText = Get-Content -LiteralPath (Join-Path $scriptDirectory 'README-Windows.txt') -Raw -Encoding utf8
[System.IO.File]::WriteAllText((Join-Path $stagingDir 'README-Windows.txt'), $readmeText, [System.Text.UTF8Encoding]::new($true))
if ($VcRedistPath -ne '') {
    Copy-Item -LiteralPath $VcRedistPath -Destination (Join-Path $stagingDir 'vc_redist.x64.exe')
}
Invoke-Tool $PythonExecutable @((Join-Path $scriptDirectory 'Prepare-OptionalComponents.py'),
    '--runtime-dir', $OptionalRuntimeDir, '--output-dir', (Join-Path $stagingDir 'optional'))

# 9. Inno Setup でコンパイルする
$scriptPath = Join-Path $scriptDirectory 'crow-translate.iss'  # Inno Setup スクリプト
$isccArguments = @(
    "/DAppVersion=$Version",
    "/DAppNumericVersion=$numericVersion",
    "/DStagingDir=$stagingDir",
    "/DOutputDir=$OutputDir"
)  # ISCC へ渡す定義
if ($VcRedistPath -ne '') { $isccArguments += '/DIncludeVcRedist' }
$isccArguments += $scriptPath
Invoke-Tool (Join-Path $InnoSetupDir 'ISCC.exe') $isccArguments

Write-Output "完了: $(Join-Path $OutputDir "crow-translate-$Version-setup.exe")"
