# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [string]$QtRoot = "$PSScriptRoot\..\.tools\qt",
    [string]$McpSdkPython = '',
    [string]$Json = ''
)
# Read-only preflight for a build: reports which Build Tools instance, MSVC toolset, Windows SDK,
# CMake, Ninja, Qt kit and MCP SDK interpreter build-studio.ps1 and run-ctest.ps1 would use.
# It reads files and version resources only. It does not run vcvars, the compiler, the linker or
# CMake, and installs nothing. Exit code 0 when everything is present, 1 otherwise.
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot\..").Path
. (Join-Path $PSScriptRoot 'nova-toolchain.ps1')
$report = [ordered]@{ pinned = $NovaToolchain; problems = @() }
try {
    $paths = Get-NovaToolchainPaths (Find-NovaBuildTools)
    $report.build_tools = $paths
    $report.build_tools_version = Get-NovaVsVersion $paths.VsPath
    if ($paths.Cl -and (Test-Path -LiteralPath $paths.Cl)) { $report.cl_file_version = (Get-Item -LiteralPath $paths.Cl).VersionInfo.FileVersion }
    $report.problems += @(Test-NovaToolchainFiles $paths)
    if ($paths.Crt -and (Test-Path -LiteralPath $paths.Crt)) {
        $report.msvc_runtime = @(Get-ChildItem -LiteralPath $paths.Crt -Filter *.dll | ForEach-Object {
            [ordered]@{ name = $_.Name; version = $_.VersionInfo.FileVersion } })
    }
} catch { $report.problems += $_.Exception.Message }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
    $report.all_instances = @(& $vswhere -all -prerelease -products * -format json | ConvertFrom-Json | ForEach-Object {
        [ordered]@{ product = $_.productId; version = $_.installationVersion; path = $_.installationPath } })
}
$report.qt_root = $QtRoot
$report.problems += @(Test-NovaQt $QtRoot)
try {
    $sdk = Resolve-NovaMcpSdkPython $workspace $McpSdkPython
    if ($sdk) { $report.mcp_sdk = $sdk } else { $report.problems += (Get-NovaMcpSdkHelp) }
} catch { $report.problems += $_.Exception.Message }
$report.problems = @($report.problems | Where-Object { $_ })
$text = $report | ConvertTo-Json -Depth 5
if ($Json) { $text | Set-Content -LiteralPath $Json -Encoding utf8 }
$text
if ($report.problems.Count) { Write-Warning ("Toolchain preflight: " + ($report.problems -join ' | ')); exit 1 }
Write-Host 'Toolchain preflight: OK'
exit 0
