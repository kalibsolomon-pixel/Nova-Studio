# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [string]$BuildName = 'studio-phase4g-Release',
    [int]$Jobs = 2,
    [string]$Log = '',
    [string]$McpSdkPython = '',
    [switch]$SkipMcp,
    [string]$QtRoot = '',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$CTestArgs = @()
)
# Runs CTest for a configured build with the Visual C++ and Qt runtime folders on PATH.
# Extra arguments are passed to ctest unchanged, for example: -R '^mcp$' -V
# The mcp suite drives the official MCP Python SDK. Its interpreter is taken from -McpSdkPython,
# NOVA_MCP_SDK_PYTHON or the workspace's virtual environment (scripts/nova-toolchain.ps1) and
# validated before anything else runs. If none is usable the run stops, unless -SkipMcp excludes
# that suite openly.
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot\..").Path
. (Join-Path $PSScriptRoot 'nova-toolchain.ps1')
if ($BuildName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'BuildName must be a simple directory name.' }
$buildDir = Join-Path $workspace "build\$BuildName"
if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'CTestTestfile.cmake'))) { throw "No configured build at $buildDir" }

# 1. Test prerequisites first, so a missing MCP SDK is reported before any tool runs.
$arguments = @('--test-dir', $buildDir, '--output-on-failure', '-j', "$Jobs")
if ($SkipMcp) {
    $arguments += @('-E', '^mcp$')
    Write-Warning 'The mcp suite is EXCLUDED (-SkipMcp). This is not a full regression run.'
} else {
    $sdk = Resolve-NovaMcpSdkPython $workspace $McpSdkPython
    $filtered = @($CTestArgs | Where-Object { $_ -in '-R', '--tests-regex', '-E', '--exclude-regex', '-L', '--label-regex', '-I' }).Count -gt 0
    if ($sdk) {
        $env:NOVA_MCP_SDK_PYTHON = $sdk.Path
        Write-Host "MCP SDK $($sdk.Version): $($sdk.Path) ($($sdk.Source))"
    } elseif ($filtered) {
        Write-Warning ((Get-NovaMcpSdkHelp) + ' Continuing because a test filter was given; the mcp suite fails if it is selected.')
    } else {
        throw (Get-NovaMcpSdkHelp)
    }
}
if ($Log) { $arguments += @('--output-log', $Log) }
if ($CTestArgs) { $arguments += $CTestArgs }

# 2. The pinned toolchain environment (runtime folders on PATH), then CTest.
$paths = Get-NovaToolchainPaths (Find-NovaBuildTools)
Enter-NovaMsvcEnvironment $paths
if (-not $QtRoot) { $QtRoot = Join-Path $workspace '.tools\qt' }
$env:PATH = (Join-Path $QtRoot 'bin') + ';' + $env:PATH
& $paths.CTest @arguments
exit $LASTEXITCODE
