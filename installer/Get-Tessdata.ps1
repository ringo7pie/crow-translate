# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    OCR 言語データ（tessdata_fast の .traineddata）を取得する。

.DESCRIPTION
    公式リポジトリ tesseract-ocr/tessdata_fast のタグ 4.1.0 から、指定した言語のファイルを取得する。
    取得物は実行ファイルではなくデータだが、内容を検証できるよう、サイズと SHA-256 を表示する。

.EXAMPLE
    powershell -File ./installer/Get-Tessdata.ps1
#>
[CmdletBinding()]
param(
    # 保存先
    [string]$DestinationDir = 'E:\Tools\tessdata_fast',

    # 取得する言語
    [string[]]$Languages = @('eng', 'jpn', 'osd'),

    # 取得元のタグ（固定して、取得物が変わらないようにする）
    [string]$Tag = '4.1.0'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Path $DestinationDir -Force | Out-Null

foreach ($language in $Languages) {
    $url = "https://github.com/tesseract-ocr/tessdata_fast/raw/$Tag/$language.traineddata"  # 取得元
    $destination = Join-Path $DestinationDir "$language.traineddata"  # 保存先
    Invoke-WebRequest -Uri $url -OutFile $destination -UseBasicParsing

    $file = Get-Item -LiteralPath $destination  # 取得したファイル
    # HTML のエラーページなどを掴んでいないかの目安（言語データは 1 MB 前後以上ある）
    if ($file.Length -lt 500KB) {
        throw "$language.traineddata が小さすぎます（$($file.Length) バイト）。取得元を確認してください: $url"
    }
    $hash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash  # SHA-256
    Write-Output "$language.traineddata  $($file.Length) バイト  SHA256=$hash"
}

Write-Output "完了: $DestinationDir"
