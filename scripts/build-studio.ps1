# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [string]$QtRoot = "$PSScriptRoot\..\.tools\qt",
    [string]$BuildName = "studio-$($Configuration.ToLower())",
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot\..").Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$env:PATH = (Split-Path $vswhere) + ';' + $env:PATH
$vs = & $vswhere -latest -products '*' -property installationPath
if (-not $vs) { throw 'Visual C++ Build Tools are required.' }
$dev = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$dev`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if ($BuildName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'BuildName must be a simple directory name.' }
$buildDir = Join-Path $workspace "build\$BuildName"
& $cmake -S $workspace -B $buildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_PREFIX_PATH=$QtRoot" "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw 'Studio configuration failed.' }
& $cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw 'Studio build failed.' }
$env:PATH = (Join-Path $QtRoot 'bin') + ';' + $env:PATH
if (-not $SkipTests) {
    & $cmake --build $buildDir --target test
    if ($LASTEXITCODE -ne 0) { throw 'Studio tests failed.' }
}
