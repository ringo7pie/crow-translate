# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Crow Translate が使う Mozhi 中継サーバー（インスタンス）の応答を調べる（診断用）。

.DESCRIPTION
    src/instancepinger.cpp の一覧の各インスタンスに、アプリと同じ形式の翻訳リクエストを送り、
    HTTP の状態と応答の先頭を表示する。応答の JSON に detected（自動検出）が含まれるかも表示する。

.EXAMPLE
    powershell -File ./installer/Test-MozhiInstances.ps1
#>
[CmdletBinding()]
param(
    # 試すエンジン（Mozhi の engine 引数）
    [string]$Engine = 'google',

    # 送る文字列（既定は日本語の「テスト」）
    [string]$Text = ([string][char]0x30C6 + [char]0x30B9 + [char]0x30C8),

    # タイムアウト（秒）
    [int]$TimeoutSeconds = 15
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# src/instancepinger.cpp の s_instances と同じ一覧
$instances = @(
    'https://mozhi.aryak.me',
    'https://translate.bus-hit.me',
    'https://nyc1.mz.ggtyler.dev',
    'https://translate.projectsegfau.lt',
    'https://translate.nerdvpn.de',
    'https://mozhi.ducks.party',
    'https://mozhi.frontendfriendly.xyz',
    'https://mozhi.pussthecat.org',
    'https://mo.zorby.top',
    'https://mozhi.adminforge.de',
    'https://translate.privacyredirect.com',
    'https://mozhi.canine.tools',
    'https://mozhi.gitro.xyz'
)  # 試すインスタンスの一覧

$encodedText = [System.Uri]::EscapeDataString($Text)  # URL 用にエンコードした文字列

foreach ($instance in $instances) {
    $url = "$instance/api/translate?engine=$Engine&from=auto&to=en&text=$encodedText"  # アプリと同じ形式の URL
    $statusText = ''  # 結果の状態
    $bodyText = ''  # 応答の本文（先頭だけ）
    try {
        $response = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec $TimeoutSeconds
        $statusText = [string]$response.StatusCode
        $bodyText = [System.Text.Encoding]::UTF8.GetString($response.RawContentStream.ToArray())
    }
    catch {
        $statusText = 'ERROR'
        $bodyText = $_.Exception.Message
        if ($_.PSObject.Properties.Name -contains 'ErrorDetails' -and $null -ne $_.ErrorDetails) {
            $bodyText = $_.ErrorDetails.Message
        }
    }
    if ($bodyText.Length -gt 200) { $bodyText = $bodyText.Substring(0, 200) }
    $hasDetected = $bodyText -match '"detected"'  # 自動検出の結果が含まれるか
    Write-Output "$instance  [$statusText]  detected=$hasDetected"
    Write-Output "    $($bodyText -replace '\s+', ' ')"
}
