# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [string]$BuildName = 'studio-phase4g-Release',
    [int]$Jobs = 2,
    [string]$Log = '',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$CTestArgs = @()
)
# Runs CTest for a configured build with the Visual C++ and Qt runtime folders on PATH.
# Extra arguments are passed to ctest unchanged, for example: -R '^mcp$' -V
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot\..").Path
if ($BuildName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'BuildName must be a simple directory name.' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$env:PATH = (Split-Path $vswhere) + ';' + $env:PATH
$vs = & $vswhere -latest -products '*' -property installationPath
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$env:PATH = (Join-Path $workspace '.tools\qt\bin') + ';' + $env:PATH
# The mcp suite drives the official MCP SDK through the established Phase 4-C environment.
$sdkPython = Join-Path $workspace 'build\phase4c-venv\Scripts\python.exe'
if (-not $env:NOVA_MCP_SDK_PYTHON -and (Test-Path $sdkPython)) { $env:NOVA_MCP_SDK_PYTHON = $sdkPython }
$ctest = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
$arguments = @('--test-dir', (Join-Path $workspace "build\$BuildName"), '--output-on-failure', '-j', "$Jobs")
if ($Log) { $arguments += @('--output-log', $Log) }
if ($CTestArgs) { $arguments += $CTestArgs }
& $ctest @arguments
exit $LASTEXITCODE
