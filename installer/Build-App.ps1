# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Crow Translate 本体を MSVC + Ninja で Release ビルドする（Qt 6 と vcpkg の Tesseract を使う）。

.DESCRIPTION
    1. Visual Studio の vcvars64.bat の環境変数を取り込む
    2. cmake で構成（Ninja、Release）
    3. cmake --build を並列実行

    FindTesseract.cmake / FindLeptonica.cmake は pkg-config 前提のため、vcpkg のライブラリ名
    （tesseract55.lib / leptonica-1.87.0.lib）を探せない。キャッシュ変数で場所を直接渡す。
    Piper TTS（ONNX Runtime が必要）、KIconThemes（KDE Frameworks が必要）、KWayland（Linux 用）は無効にする。

.EXAMPLE
    powershell -File ./installer/Build-App.ps1
#>
[CmdletBinding()]
param(
    # ビルドディレクトリ（省略時はリポジトリ直下の build-release。.gitignore が build-*/ を除外している）
    [string]$BuildDir = '',

    # Qt の prefix
    [string]$QtPrefix = 'E:\Tools\Qt\6.8.3\msvc2022_64',

    # vcpkg の installed\x64-windows
    [string]$VcpkgInstalledDir = 'E:\Tools\vcpkg\installed\x64-windows',

    # Visual Studio のフォルダ（省略時は VS 18 → VS 2022 の順に E:\Program Files から探す）
    [string]$VisualStudioDir = '',

    # Visual Studio の vcvars64.bat（省略時は $VisualStudioDir から求める）
    [string]$VcVarsPath = '',

    # Visual Studio 同梱の CMake と Ninja のフォルダ（省略時は $VisualStudioDir から求める）
    [string]$CMakeBinDir = '',
    [string]$NinjaDir = '',

    # 並列数（既定は論理プロセッサ数）
    [int]$Parallel = [Environment]::ProcessorCount,

    # 単体試験（tests/）もビルドする（CMake の BUILD_TESTING=ON。本体のライブラリにも BUILD_TESTING が定義されるので、配布用とは別の -BuildDir を使う）
    [switch]$BuildTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path  # このスクリプトのあるフォルダ（param の既定値では $PSScriptRoot が空になる環境があるため本体で求める）
$repositoryRoot = Split-Path -Parent $scriptDirectory  # リポジトリのルート
if ($BuildDir -eq '') { $BuildDir = Join-Path $repositoryRoot 'build-release' }
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $repositoryRoot $BuildDir }  # 相対指定はリポジトリのルートから

# Visual Studio のフォルダを決める（指定が無ければ、vcvars64.bat のある最初の候補）
if ($VisualStudioDir -eq '') {
    $visualStudioCandidates = @(
        'E:\Program Files\Microsoft Visual Studio\18\Professional',
        'E:\Program Files\Microsoft Visual Studio\2022\Professional'
    )  # 探す順の候補
    foreach ($candidate in $visualStudioCandidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'VC\Auxiliary\Build\vcvars64.bat')) { $VisualStudioDir = $candidate; break }
    }
    if ($VisualStudioDir -eq '') { $VisualStudioDir = $visualStudioCandidates[0] }  # 見つからなければ下の存在確認で理由を出す
}
$cmakeExtensionDir = Join-Path $VisualStudioDir 'Common7\IDE\CommonExtensions\Microsoft\CMake'  # VS 同梱の CMake / Ninja の親フォルダ
if ($VcVarsPath -eq '') { $VcVarsPath = Join-Path $VisualStudioDir 'VC\Auxiliary\Build\vcvars64.bat' }
if ($CMakeBinDir -eq '') { $CMakeBinDir = Join-Path $cmakeExtensionDir 'CMake\bin' }
if ($NinjaDir -eq '') { $NinjaDir = Join-Path $cmakeExtensionDir 'Ninja' }

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

Assert-PathExists $VcVarsPath 'vcvars64.bat'
Assert-PathExists (Join-Path $CMakeBinDir 'cmake.exe') 'cmake.exe'
Assert-PathExists (Join-Path $NinjaDir 'ninja.exe') 'ninja.exe'
Assert-PathExists (Join-Path (Join-Path $QtPrefix 'lib') 'cmake') 'Qt の CMake 設定'
Assert-PathExists (Join-Path $VcpkgInstalledDir 'share\ECM\ECMConfig.cmake') 'ECMConfig.cmake（Install-Tools.ps1 -SkipQt -SkipInnoSetup -InstallEcm で導入）'

# 1. vcvars64.bat を cmd で実行し、その後の環境変数（set の出力）を現在のプロセスへ取り込む
$environmentLines = & cmd.exe /d /c "`"$VcVarsPath`" >nul && set"  # NAME=VALUE の行
if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat が失敗しました（終了コード $LASTEXITCODE）" }
foreach ($line in $environmentLines) {
    $separatorIndex = $line.IndexOf('=')  # 最初の = の位置（先頭が = の隠し変数は除く）
    if ($separatorIndex -gt 0) {
        Set-Item -LiteralPath ("Env:" + $line.Substring(0, $separatorIndex)) -Value $line.Substring($separatorIndex + 1)
    }
}
$env:PATH = "$CMakeBinDir;$NinjaDir;$env:PATH"

# 実行ファイルの実行時の依存 DLL（Qt / vcpkg）は windeployqt と Build-Installer.ps1 が配置するため、ここでは PATH に足さない

# 2. 構成
$libraryDir = Join-Path $VcpkgInstalledDir 'lib'  # vcpkg の .lib の場所
$includeDir = Join-Path $VcpkgInstalledDir 'include'  # vcpkg のヘッダーの場所
$configureArguments = @(
    '-S', $repositoryRoot,
    '-B', $BuildDir,
    '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_PREFIX_PATH=$QtPrefix;$VcpkgInstalledDir",
    '-DWITH_PIPER_TTS=OFF',
    '-DWITH_KICONTHEMES=OFF',
    '-DWITH_KWAYLAND=OFF',
    "-DTesseract_INCLUDE_DIR=$includeDir",
    "-DTesseract_LIBRARY_RELEASE=$(Join-Path $libraryDir 'tesseract55.lib')",
    "-DLeptonica_INCLUDE_DIR=$includeDir",
    "-DLeptonica_LIBRARY_RELEASE=$(Join-Path $libraryDir 'leptonica-1.87.0.lib')"
)  # cmake 構成の引数
if ($BuildTests) { $configureArguments += '-DBUILD_TESTING=ON' }
Invoke-Tool 'cmake' $configureArguments

# 3. ビルド（並列）
Invoke-Tool 'cmake' @('--build', $BuildDir, '--parallel', [string]$Parallel)

Write-Output "完了: $BuildDir"
