# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [ValidateSet('source', 'msys2')][string]$QtVariant = 'source',
    [ValidateSet('Release', 'RelWithDebInfo')][string]$Config = 'Release',
    [switch]$Test,
    [switch]$SkipMcp,
    [string]$McpSdkPython = '',
    [int]$Jobs = 2,          # CTest parallelism (as in the MSVC scripts)
    [int]$BuildJobs = 0,     # compiler jobs; 0 = the smaller of logical CPUs and installed GB / 2
    [string]$SourceSnapshot = '',
    [string]$BuildName = '',
    # Packaging inputs, for checkouts without their own .tools\nsis or build\phase3-dependencies
    # (for example a Git worktree): makensis.exe and the folder with the Qt source archive and notices.
    [string]$Makensis = $env:NOVA_MAKENSIS,
    [string]$ReleaseDependencies = $env:NOVA_RELEASE_DEPENDENCIES,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$CTestArgs = @()
)
# Phase 4-I (experimental): configure and build Nova Studio with the pinned GCC/MinGW-w64 toolchain
# in build\mingw-ucrt64-<variant>-<config>, then optionally run all CTest suites.
# Uses no Visual Studio, Build Tools, Windows SDK or MSVC component: the process PATH is reduced
# to the toolchain, Qt and Windows, and the script stops if cl.exe is still reachable.
# The MSVC build (scripts/build-studio.ps1) is not affected.
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot\..").Path
. (Join-Path $PSScriptRoot 'nova-toolchain.ps1')        # MCP SDK detection only
. (Join-Path $PSScriptRoot 'nova-mingw-toolchain.ps1')
$paths = Get-NovaMingwPaths $QtVariant
$check = Test-NovaMingwToolchain $paths
if ($check.Problems) { throw ("Toolchain not ready (scripts/check-mingw-toolchain.ps1): " + ($check.Problems -join '; ')) }

# MCP SDK first, so a missing prerequisite stops the run before anything is built.
$sdk = $null
if ($Test -and -not $SkipMcp) {
    $sdk = Resolve-NovaMcpSdkPython $workspace $McpSdkPython
    if (-not $sdk) { throw (Get-NovaMcpSdkHelp) }
}
if (-not $BuildName) { $BuildName = "mingw-ucrt64-$QtVariant-$Config" }
if ($BuildName -notmatch '^[A-Za-z0-9_-]+$') { throw 'BuildName must be a simple directory name' }
$buildDir = Join-Path $workspace "build\$BuildName"
Enter-NovaMingwEnvironment $paths
# CMake keeps some cache paths verbatim (CMAKE_RC_COMPILER is written into CMakeRCCompiler.cmake as
# is), so a backslash would be read as an escape: pass every path with forward slashes.
function ConvertTo-CMakePath([string]$Path) { $Path.Replace('\', '/') }
$packagingArgs = @()
if ($Makensis) {
    if (-not (Test-Path -LiteralPath $Makensis -PathType Leaf)) { throw "makensis not found: $Makensis" }
    $packagingArgs += "-DNOVA_MAKENSIS=$(ConvertTo-CMakePath $Makensis)"
}
if ($ReleaseDependencies) {
    if (-not (Test-Path -LiteralPath (Join-Path $ReleaseDependencies 'qt-source.json'))) { throw "No qt-source.json in $ReleaseDependencies" }
    $packagingArgs += "-DNOVA_RELEASE_DEPENDENCIES=$(ConvertTo-CMakePath $ReleaseDependencies)"
}
Invoke-NovaNative $paths.CMake (@('-S', $workspace, '-B', $buildDir, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_CXX_COMPILER=$(ConvertTo-CMakePath $paths.Gxx)", "-DCMAKE_RC_COMPILER=$(ConvertTo-CMakePath $paths.Windres)",
    "-DCMAKE_MAKE_PROGRAM=$(ConvertTo-CMakePath $paths.Ninja)", "-DCMAKE_PREFIX_PATH=$(ConvertTo-CMakePath $paths.Qt)",
    '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', "-DNOVA_SOURCE_SNAPSHOT=$SourceSnapshot") + $packagingArgs) 'CMake configure'
[ordered]@{ format = 'nova-mingw-toolchain'; qt_variant = $QtVariant; config = $Config; found = $check.Found
            msys2_root = $paths.Root; qt = $paths.Qt; gcc_runtime = $paths.Bin
            msys2_manifest = (Join-Path $paths.Root 'nova-toolchain-manifest.txt') } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $buildDir 'nova-mingw-toolchain.json') -Encoding utf8
# GCC needs up to ~1.5 GB per job on Nova's larger translation units; an unlimited --parallel
# (logical CPUs + 2) ran cc1plus out of memory on a 28-thread, 32 GB machine.
if ($BuildJobs -le 0) {
    $gb = [math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    $BuildJobs = [math]::Max(1, [math]::Min([Environment]::ProcessorCount, [math]::Floor($gb / 2)))
}
Invoke-NovaNative $paths.CMake @('--build', $buildDir, '--parallel', "$BuildJobs") "Build ($BuildJobs jobs)"
if (-not $Test) { exit 0 }

$arguments = @('--test-dir', $buildDir, '--output-on-failure', '-j', "$Jobs")
if ($SkipMcp) { $arguments += @('-E', '^mcp$'); Write-Warning 'The mcp suite is EXCLUDED (-SkipMcp). This is not a full regression run.' }
else { $env:NOVA_MCP_SDK_PYTHON = $sdk.Path; Write-Host "MCP SDK $($sdk.Version): $($sdk.Path)" }
if ($CTestArgs) { $arguments += $CTestArgs }
$ErrorActionPreference = 'Continue'   # test output on stderr must not end the run; the exit code decides
& $paths.CTest @arguments 2>&1 | ForEach-Object { ConvertFrom-NovaNativeLine $_ }
exit $LASTEXITCODE
