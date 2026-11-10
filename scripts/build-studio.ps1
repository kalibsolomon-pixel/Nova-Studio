# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [string]$QtRoot = "$PSScriptRoot\..\.tools\qt",
    [string]$BuildName = "studio-$($Configuration.ToLower())",
    [string]$McpSdkPython = '',
    [string]$SourceSnapshot = '',
    [switch]$SkipMcp,
    [switch]$SkipTests
)
# Configures, builds and tests Nova Studio with the pinned standalone Build Tools toolchain
# (scripts/nova-toolchain.ps1). The tests run through run-ctest.ps1, so the mcp suite gets the
# same validated MCP SDK interpreter. The selected toolchain is recorded in
# build\<BuildName>\nova-toolchain.json for the packager.
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot\..").Path
. (Join-Path $PSScriptRoot 'nova-toolchain.ps1')
if ($BuildName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'BuildName must be a simple directory name.' }
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$qtProblems = @(Test-NovaQt $QtRoot)
if ($qtProblems) { throw ('Qt check failed: ' + ($qtProblems -join '; ')) }
# Fail before a long build if the tests could not run completely.
if (-not $SkipTests -and -not $SkipMcp -and -not (Resolve-NovaMcpSdkPython $workspace $McpSdkPython)) {
    throw (Get-NovaMcpSdkHelp)
}
$paths = Get-NovaToolchainPaths (Find-NovaBuildTools)
Enter-NovaMsvcEnvironment $paths
$buildDir = Join-Path $workspace "build\$BuildName"
& $paths.CMake -S $workspace -B $buildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$($paths.Ninja)" `
    "-DCMAKE_CXX_COMPILER=$($paths.Cl)" "-DCMAKE_PREFIX_PATH=$QtRoot" "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DNOVA_SOURCE_SNAPSHOT=$SourceSnapshot"
if ($LASTEXITCODE -ne 0) { throw 'Studio configuration failed.' }
$cache = Get-Content -LiteralPath (Join-Path $buildDir 'CMakeCache.txt')
$cachedCl = (($cache | Where-Object { $_ -match '^CMAKE_CXX_COMPILER:[A-Z]+=' }) -replace '^[^=]+=', '')
if ((Resolve-Path -LiteralPath $cachedCl).Path -ne (Resolve-Path -LiteralPath $paths.Cl).Path) {
    throw "The build folder is configured for $cachedCl, not $($paths.Cl). Use a new -BuildName."
}
$identity = [ordered]@{
    format          = 'nova-toolchain'
    product         = $NovaToolchain.Product
    vs_install_path = $paths.VsPath
    vs_version      = Get-NovaVsVersion $paths.VsPath
    msvc_toolset    = $env:VCToolsVersion
    cl              = $paths.Cl
    cl_version      = (Get-Item -LiteralPath $paths.Cl).VersionInfo.FileVersion
    windows_sdk     = "$env:WindowsSDKVersion".TrimEnd('\')
    msvc_runtime    = (Join-Path "$env:VCToolsRedistDir" 'x64\Microsoft.VC145.CRT')
    redist_txt      = $paths.RedistTxt
    cmake           = $paths.CMake
    ninja           = $paths.Ninja
    qt_root         = $QtRoot
    qt_version      = $NovaToolchain.QtVersion
    source_snapshot = $SourceSnapshot
}
$identity | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $buildDir 'nova-toolchain.json') -Encoding utf8
& $paths.CMake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw 'Studio build failed.' }
if (-not $SkipTests) {
    $testArgs = @{ BuildName = $BuildName; QtRoot = $QtRoot }
    if ($McpSdkPython) { $testArgs.McpSdkPython = $McpSdkPython }
    if ($SkipMcp) { $testArgs.SkipMcp = $true }
    & (Join-Path $PSScriptRoot 'run-ctest.ps1') @testArgs
    if ($LASTEXITCODE -ne 0) { throw 'Studio tests failed.' }
}
