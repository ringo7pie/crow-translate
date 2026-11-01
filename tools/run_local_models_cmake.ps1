# SPDX-FileCopyrightText: 2026 Crow Translate contributors
# SPDX-License-Identifier: GPL-3.0-or-later
param([Parameter(ValueFromRemainingArguments=$true)][string[]]$CmakeArguments)
$ErrorActionPreference = 'Stop'
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $locator)) { throw 'Visual Studio C++ Build Tools and vswhere are required.' }
$installation = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'No Visual Studio installation with C++ tools was found.' }
$environmentLines = & cmd.exe /d /c "`"$installation/VC/Auxiliary/Build/vcvars64.bat`" >nul && set"
foreach ($line in $environmentLines) {
    $index = $line.IndexOf('=')
    if ($index -gt 0) { Set-Item -LiteralPath ('Env:' + $line.Substring(0,$index)) -Value $line.Substring($index+1) }
}
$env:PATH = "$installation/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja;$env:PATH"
& "$installation/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" @CmakeArguments
exit $LASTEXITCODE
