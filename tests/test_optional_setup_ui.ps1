# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later
# Run with Windows PowerShell 5.1. Exercises the real WinForms events without
# starting the installer worker, downloading files, or changing user settings.
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')
$testRoot = [IO.Path]::GetFullPath((Join-Path $temporaryParent ('crow-setup-ui-test-' + [guid]::NewGuid().ToString('N'))))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    foreach($name in @('optional-components.json','OptionalSetupJob.cs')){
        Copy-Item -LiteralPath (Join-Path $repository ('installer/' + $name)) -Destination (Join-Path $testRoot $name)
    }
    $checks = @'
function Assert-InstallState([bool]$Expected, [string]$Scenario) {
    if($install.Enabled -ne $Expected){throw ('Unexpected install button state: ' + $Scenario)}
    Write-Output ('PASS: ' + $Scenario)
}
Assert-InstallState $false 'initial unchecked consent'
$accept.Checked=$true
Assert-InstallState $false 'Indra/PLaMo need eligibility consent too'
$eligibility.Checked=$true
Assert-InstallState $true 'all required consent enables install'
$accept.Checked=$false
Assert-InstallState $false 'revoking license consent disables install'
$accept.Checked=$true
$eligibility.Checked=$false
Assert-InstallState $false 'revoking eligibility consent disables install'
$eligibility.Checked=$true
$defaultModel.SelectedIndex=0
for($i=0;$i -lt $choices.Items.Count;$i++){$choices.SetItemChecked($i,$false)}
$accept.Checked=$true; $eligibility.Checked=$true
Assert-InstallState $false 'no selected component stays disabled'
$ids=@($catalog.components|ForEach-Object{$_.id})
$indexPosition=[Array]::IndexOf($ids,'index-translate-2b')
$indraPosition=[Array]::IndexOf($ids,'indra')
$choices.SetItemChecked($indexPosition,$true)
Assert-InstallState $false 'changing components clears consent'
$accept.Checked=$true
Assert-InstallState $true 'Index needs license consent without Indra/PLaMo eligibility'
$choices.SetItemChecked($indexPosition,$false)
Assert-InstallState $false 'unchecking the last component disables immediately'
$choices.SetItemChecked($indexPosition,$true)
$defaultModel.SelectedIndex=-1
$accept.Checked=$true
Assert-InstallState $false 'missing default selection stays disabled'
$defaultModel.SelectedIndex=0
Assert-InstallState $true 'keep existing default is valid'
$choices.SetItemChecked($indraPosition,$true)
$accept.Checked=$true
Assert-InstallState $false 'adding Indra requires renewed eligibility'
$eligibility.Checked=$true
Assert-InstallState $true 'renewed consent enables install'
$script:setupJob=[object]::new()
try {
    $accept.Checked=$false; $accept.Checked=$true
    Assert-InstallState $false 'consent events cannot enable during installation'
} finally {$script:setupJob=$null}
# The retry branch restores controls, then reevaluates the same requirements.
$choices.Enabled=$false
$eligibility.Checked=$false
$choices.Enabled=$true
Update-InstallButton
Assert-InstallState $false 'retry cannot bypass missing consent'
$eligibility.Checked=$true
Assert-InstallState $true 'retry enables once requirements are met'
'@
    $source = Get-Content -LiteralPath (Join-Path $repository 'installer/Install-OptionalComponents.ps1') -Raw -Encoding utf8
    $needle = 'try { [void]$form.ShowDialog() }'
    if(-not $source.Contains($needle)){throw 'Setup dialog entry point was not found'}
    $source = $source.Replace($needle, ('try {' + [Environment]::NewLine + $checks + [Environment]::NewLine + '}'))
    $scriptPath = Join-Path $testRoot 'Test-OptionalSetup.ps1'
    [IO.File]::WriteAllText($scriptPath, $source, [Text.UTF8Encoding]::new($true))
    & $scriptPath -DataRoot (Join-Path $testRoot 'components') -NoConfigure
} finally {
    # Delete only the unique directory created by this test under local Temp.
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $entry = Get-Item -LiteralPath $resolved -ErrorAction SilentlyContinue
    if($null -ne $entry -and
       [IO.Path]::GetDirectoryName($resolved).Equals($temporaryParent,[StringComparison]::OrdinalIgnoreCase) -and
       [IO.Path]::GetFileName($resolved).StartsWith('crow-setup-ui-test-') -and
       ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0){
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
