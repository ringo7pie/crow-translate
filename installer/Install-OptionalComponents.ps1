# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [switch]$Worker,
    [string]$DataRoot = '',
    [string]$Receipt = '',
    [string]$ProgressFile = '',
    [switch]$NoConfigure
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# A caller such as PowerShell 7 can pass its incompatible PSModulePath through
# Inno Setup/Start-Process. Prefer the Windows host's own built-in modules.
if ($PSVersionTable.PSEdition -eq 'Desktop') {
    $env:PSModulePath = (Join-Path $PSHOME 'Modules') + [IO.Path]::PathSeparator + $env:PSModulePath
}
$catalogPath = Join-Path $PSScriptRoot 'optional-components.json'
$catalog = Get-Content -LiteralPath $catalogPath -Raw -Encoding utf8 | ConvertFrom-Json
$catalogHash = (Get-FileHash -LiteralPath $catalogPath -Algorithm SHA256).Hash.ToLowerInvariant()
$defaultRoot = Join-Path $env:LOCALAPPDATA 'Crow Translate/Components'
$descriptor = Join-Path $env:LOCALAPPDATA 'Crow Translate/components.json'
if ($DataRoot -eq '') {
    $DataRoot = $defaultRoot
    if (Test-Path -LiteralPath $descriptor) {
        try { $DataRoot = (Get-Content -LiteralPath $descriptor -Raw -Encoding utf8 | ConvertFrom-Json).root } catch { }
    }
}

function Write-SetupStatus([string]$Message, [int]$Percent = -1, [bool]$Done = $false) {
    $state = @{ message=$Message; percent=$Percent; done=$Done }
    $temporary = $ProgressFile + '.tmp'
    [IO.File]::WriteAllText($temporary, ($state | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $ProgressFile -Force
}

function Get-VerifiedDownload($Record, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Destination)) {
        Write-SetupStatus ('共通環境をダウンロードしています: ' + $Record.file)
        $temporary = $Destination + '.partial'
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $Record.url -OutFile $temporary -UseBasicParsing
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Record.sha256) {
            Remove-Item -LiteralPath $temporary
            throw ('ダウンロードのチェックサムが一致しません: ' + $Record.file)
        }
        Move-Item -LiteralPath $temporary -Destination $Destination
    }
    if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Record.sha256) {
        throw ('共通環境のチェックサムが一致しません: ' + $Record.file)
    }
}

function Expand-VerifiedArchive([string]$Archive, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $targetRoot = [IO.Path]::GetFullPath($Destination).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $archiveFile = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        foreach($entry in $archiveFile.Entries){
            if($entry.Name -eq ''){continue}
            $target = [IO.Path]::GetFullPath((Join-Path $Destination $entry.FullName))
            if(-not $target.StartsWith($targetRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'アーカイブの保存先が不正です'}
            New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
            $inputStream=$entry.Open(); $outputStream=[IO.File]::Open($target,[IO.FileMode]::Create,[IO.FileAccess]::Write)
            try{$inputStream.CopyTo($outputStream)}finally{$outputStream.Dispose();$inputStream.Dispose()}
        }
    } finally{$archiveFile.Dispose()}
}

if ($Worker) {
    $installationLock = $null
    try {
        # Validate consent before even bootstrapping Python or creating downloads.
        $acceptance = Get-Content -LiteralPath $Receipt -Raw -Encoding utf8 | ConvertFrom-Json
        $knownIds = @($catalog.components | ForEach-Object { $_.id })
        if ($acceptance.accepted -isnot [bool] -or -not $acceptance.accepted -or $acceptance.catalog_sha256 -ne $catalogHash -or
            @($acceptance.components).Count -eq 0 -or
            @($acceptance.components | Select-Object -Unique).Count -ne @($acceptance.components).Count -or
            @($acceptance.components | Where-Object { $_ -notin $knownIds }).Count -gt 0) {
            throw '選択内容とライセンスへの明示的な同意を確認できません'
        }
        $DataRoot = [IO.Path]::GetFullPath($DataRoot)
        if ($DataRoot.TrimEnd('\','/') -eq [IO.Path]::GetPathRoot($DataRoot).TrimEnd('\','/')) { throw 'ドライブ直下は保存先に指定できません' }
        New-Item -ItemType Directory -Path $DataRoot -Force | Out-Null
        $installationLock = [IO.File]::Open((Join-Path $DataRoot '.optional-setup.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $pythonRoot = Join-Path $DataRoot 'local-translation/venv'
        $downloads = Join-Path $DataRoot 'downloads'
        New-Item -ItemType Directory -Path $downloads -Force | Out-Null
        $python = Join-Path $pythonRoot 'python.exe'
        if (-not (Test-Path -LiteralPath (Join-Path $pythonRoot 'crow-python.json'))) {
            if (Test-Path -LiteralPath $pythonRoot) { throw '保存先には既存のPython環境があります。別の追加機能保存先を選んでください' }
            $package = Join-Path $downloads $catalog.python.file
            Get-VerifiedDownload $catalog.python $package
            # A private embeddable CPython, without PATH/registry/system changes.
            $pythonStaging = $pythonRoot + '.' + [guid]::NewGuid().ToString('N') + '.staging'
            Expand-VerifiedArchive $package $pythonStaging
            New-Item -ItemType Directory -Path (Join-Path $pythonStaging 'Lib/site-packages') -Force | Out-Null
            [IO.File]::WriteAllText((Join-Path $pythonStaging 'python314._pth'), "python314.zip`n.`nLib/site-packages`nimport site`n", [Text.UTF8Encoding]::new($false))
            [IO.File]::WriteAllText((Join-Path $pythonStaging 'crow-python.json'), ($catalog.python | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
            Move-Item -LiteralPath $pythonStaging -Destination $pythonRoot
        }
        # Supply native dependencies before Python starts and locks its own
        # vcruntime DLLs. ONNX Runtime needs these even for Indra-only installs.
        $runtimeManifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'runtime-manifest.json') -Raw -Encoding utf8 | ConvertFrom-Json
        foreach ($record in $runtimeManifest.files) {
            if ($record.file -notmatch '^(msvcp|vcruntime|concrt|vccorlib|vcomp)[A-Za-z0-9_.-]*\.dll$') { continue }
            $runtimeSource = Join-Path (Join-Path $PSScriptRoot 'runtime') $record.file
            if ((Get-FileHash -LiteralPath $runtimeSource -Algorithm SHA256).Hash.ToLowerInvariant() -ne $record.sha256) { throw '同梱ランタイムのチェックサムが一致しません' }
            $runtimeTarget = Join-Path $pythonRoot $record.file
            if ((Test-Path -LiteralPath $runtimeTarget) -and
                (Get-FileHash -LiteralPath $runtimeTarget -Algorithm SHA256).Hash.ToLowerInvariant() -eq $record.sha256) { continue }
            $runtimeTemporary = $runtimeTarget + '.partial'
            Copy-Item -LiteralPath $runtimeSource -Destination $runtimeTemporary -Force
            Move-Item -LiteralPath $runtimeTemporary -Destination $runtimeTarget -Force
        }
        $pipPackage = Join-Path $downloads $catalog.pip.file
        Get-VerifiedDownload $catalog.pip $pipPackage
        $pipModule = Join-Path $pythonRoot 'Lib/site-packages/pip/__main__.py'
        if (-not (Test-Path -LiteralPath (Join-Path $pythonRoot 'crow-pip.json'))) {
            Expand-VerifiedArchive $pipPackage (Join-Path $pythonRoot 'Lib/site-packages')
            [IO.File]::WriteAllText((Join-Path $pythonRoot 'crow-pip.json'), ($catalog.pip | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
        }
        Write-SetupStatus '選択した追加機能を準備しています'
        $workerArguments = @((Join-Path $PSScriptRoot 'install_optional_components.py'), '--catalog', $catalogPath,
                             '--root', $DataRoot, '--receipt', $Receipt, '--progress', $ProgressFile)
        if ($NoConfigure) { $workerArguments += '--no-configure' }
        $env:PYTHONUTF8 = '1'
        $env:PYTHONIOENCODING = 'utf-8'
        & $python @workerArguments >> (Join-Path $DataRoot 'optional-bootstrap.log') 2>&1
        $workerExit = $LASTEXITCODE
        exit $workerExit
    }
    catch {
        if ($ProgressFile -ne '') { Write-SetupStatus $_.Exception.Message 0 $true }
        exit 1
    }
    finally { if ($null -ne $installationLock) { $installationLock.Dispose() } }
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot 'OptionalSetupJob.cs')
[Windows.Forms.Application]::EnableVisualStyles()
$form = New-Object Windows.Forms.Form
$form.Add_Shown({ [CrowSetupJob]::ShowSetupWindow($form.Handle) })
$form.Text = 'Crow Translate - 追加機能のセットアップ'
$form.Size = New-Object Drawing.Size(820, 760)
$form.StartPosition = 'CenterScreen'
$form.MinimumSize = $form.Size
$form.MaximizeBox = $false
$label = New-Object Windows.Forms.Label
$label.Text = "導入する項目を選択してください。初期状態では何も選択していません。`n選択したモデルを配布元から取得します。共通環境は別途最大約2 GBです。"
$label.SetBounds(18, 14, 770, 42)
$form.Controls.Add($label)
$choices = New-Object Windows.Forms.CheckedListBox
$choices.CheckOnClick = $true
$choices.SetBounds(18, 62, 768, 112)
foreach ($component in $catalog.components) {
    $extra = if ($component.id -eq 'plamo-2-translate') { ' / 変換時の空き容量80 GB以上' } else { '' }
    [void]$choices.Items.Add(('{0} - モデル {1:N2} GB{2}' -f $component.name, ($component.download_bytes/1e9), $extra), $false)
}
$form.Controls.Add($choices)
$licenseLabel = New-Object Windows.Forms.Label
$licenseLabel.Text = '項目をクリックすると利用条件とライセンス全文を表示します。'
$licenseLabel.SetBounds(18, 182, 768, 36)
$form.Controls.Add($licenseLabel)
$license = New-Object Windows.Forms.TextBox
$license.Multiline = $true; $license.ReadOnly = $true; $license.ScrollBars = 'Vertical'
$license.SetBounds(18, 222, 768, 230)
$form.Controls.Add($license)
$choices.Add_SelectedIndexChanged({
    if ($choices.SelectedIndex -lt 0) { return }
    $component = $catalog.components[$choices.SelectedIndex]
    $licenseLabel.Text = $component.name + ': ' + $component.license
    $parts = @($component.license_summary, '', ('配布元: ' + $component.license_url), '')
    foreach ($record in $component.licenses) {
        $path = Join-Path (Join-Path $PSScriptRoot 'notices') $record.file
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $record.sha256) { throw 'ライセンス文書の検証に失敗しました' }
        $parts += Get-Content -LiteralPath $path -Raw -Encoding utf8
    }
    $license.Text = $parts -join "`r`n"
})
$accept = New-Object Windows.Forms.CheckBox
$accept.Text = '選択したすべての項目のライセンスを読み、同意します。'
$accept.SetBounds(18, 462, 768, 24)
$form.Controls.Add($accept)
$eligibility = New-Object Windows.Forms.CheckBox
$eligibility.Text = "Indra / PLaMoを選ぶ場合、表示された用途・商用条件を満たすか、`n必要な登録・商用許諾を取得済みであることを確認しました。"
$eligibility.SetBounds(18, 490, 768, 44)
$form.Controls.Add($eligibility)
$choices.Add_ItemCheck({ $accept.Checked=$false; $eligibility.Checked=$false })
$destinationLabel = New-Object Windows.Forms.Label
$destinationLabel.Text = '追加機能の保存先（アプリ本体とは別に保存します）'
$destinationLabel.SetBounds(18, 542, 760, 22)
$form.Controls.Add($destinationLabel)
$destination = New-Object Windows.Forms.TextBox
$destination.Text = $DataRoot; $destination.SetBounds(18, 568, 660, 24)
$form.Controls.Add($destination)
$browse = New-Object Windows.Forms.Button
$browse.Text = '参照...'; $browse.SetBounds(690, 566, 96, 28)
$browse.Add_Click({ $dialog = New-Object Windows.Forms.FolderBrowserDialog; $dialog.SelectedPath=$destination.Text; if($dialog.ShowDialog() -eq 'OK'){$destination.Text=$dialog.SelectedPath}; $dialog.Dispose() })
$form.Controls.Add($browse)
$status = New-Object Windows.Forms.Label
$status.Text = 'いいえの場合は「閉じる」を押してください。通信は行いません。'
$status.SetBounds(18, 606, 768, 36); $form.Controls.Add($status)
$bar = New-Object Windows.Forms.ProgressBar
$bar.SetBounds(18, 648, 768, 18); $form.Controls.Add($bar)
$install = New-Object Windows.Forms.Button
$install.Text = '選択した項目をインストール'; $install.SetBounds(440, 680, 234, 30)
$form.Controls.Add($install)
$close = New-Object Windows.Forms.Button
$close.Text = '閉じる'; $close.SetBounds(690, 680, 96, 30); $form.Controls.Add($close)
$script:setupJob = $null
$script:progressPath = ''
$script:receiptPath = ''
$script:finished = $false
$timer = New-Object Windows.Forms.Timer
$timer.Interval = 300
$install.Add_Click({
    $selected = @(); for($i=0; $i -lt $choices.Items.Count; $i++){ if($choices.GetItemChecked($i)){ $selected += $catalog.components[$i].id } }
    if ($selected.Count -eq 0 -or -not $accept.Checked -or
        ((('indra' -in $selected) -or ('plamo-2-translate' -in $selected)) -and -not $eligibility.Checked)) {
        [void][Windows.Forms.MessageBox]::Show('導入する項目を選択し、ライセンスと必要な利用条件への同意を確認してください。', $form.Text)
        return
    }
    try {
        $chosenRoot = [IO.Path]::GetFullPath($destination.Text)
        if ($chosenRoot.TrimEnd('\','/') -eq [IO.Path]::GetPathRoot($chosenRoot).TrimEnd('\','/')) { throw 'ドライブ直下は保存先に指定できません' }
        New-Item -ItemType Directory -Path $chosenRoot -Force | Out-Null
        $runId = [guid]::NewGuid().ToString('N')
        $script:receiptPath = Join-Path $chosenRoot ('consent-' + $runId + '.json')
        $script:progressPath = Join-Path $chosenRoot ('progress-' + $runId + '.json')
        $receiptData = @{ accepted=$true; catalog_sha256=$catalogHash; components=$selected; accepted_at=[DateTimeOffset]::UtcNow.ToString('o') }
        [IO.File]::WriteAllText($script:receiptPath, ($receiptData | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
        $arguments = @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File', $PSCommandPath,
                       '-Worker','-DataRoot',$chosenRoot,'-Receipt',$script:receiptPath,'-ProgressFile',$script:progressPath)
        if($NoConfigure){$arguments += '-NoConfigure'}
        $shell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
        $script:setupJob = [CrowSetupJob]::new($shell, [string[]]$arguments, $PSScriptRoot)
        $script:finished = $false
        foreach($control in @($choices,$accept,$eligibility,$destination,$browse,$install)){ $control.Enabled=$false }
        $close.Text='中止'; $status.Text='共通環境を準備しています...'; $bar.Style='Marquee'; $timer.Start()
    } catch { [void][Windows.Forms.MessageBox]::Show($_.Exception.Message,$form.Text) }
})
$timer.Add_Tick({
    if ($null -eq $script:setupJob) { return }
    if(Test-Path -LiteralPath $script:progressPath){
        try {
            $state = Get-Content -LiteralPath $script:progressPath -Raw -Encoding utf8 | ConvertFrom-Json
            $status.Text=$state.message
            if($state.percent -lt 0){$bar.Style='Marquee'}else{$bar.Style='Continuous';$bar.Value=[Math]::Max(0,[Math]::Min(100,$state.percent))}
        } catch { }
    }
    if (-not $script:setupJob.IsRunning) {
        $timer.Stop(); $exitCode=$script:setupJob.ExitCode; $script:setupJob.Dispose(); $script:setupJob=$null
        if($exitCode -eq 0){$status.Text='導入が完了しました。Crow TranslateでOCRエンジン・翻訳モデルを選択してください。';$bar.Style='Continuous';$bar.Value=100}
        else{
            $details=$status.Text
            try{$failed=Get-Content -LiteralPath $script:progressPath -Raw -Encoding utf8|ConvertFrom-Json; if($failed.failures){$details=($failed.failures.PSObject.Properties|ForEach-Object{$_.Name+': '+$_.Value}) -join "`r`n"}}catch{}
            $status.Text='追加導入を完了できませんでした。導入済みの項目を残して再試行できます。'
            [void][Windows.Forms.MessageBox]::Show(($details+"`r`n保存先のoptional-install.log / optional-bootstrap.logも参照できます。"),$form.Text)
            foreach($control in @($choices,$accept,$eligibility,$destination,$browse,$install)){ $control.Enabled=$true }
            $install.Text='再試行'
        }
        $close.Text='閉じる'; $script:finished=$true
    }
})
$close.Add_Click({$form.Close()})
$form.Add_FormClosing({
    if($null -ne $script:setupJob){$timer.Stop();$script:setupJob.Dispose();$script:setupJob=$null}
})
try { [void]$form.ShowDialog() } finally { $timer.Dispose(); $form.Dispose(); if($null -ne $script:setupJob){$script:setupJob.Dispose()} }
