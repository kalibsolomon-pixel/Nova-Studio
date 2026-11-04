param(
    [string]$NovaBuild = "$PSScriptRoot\..\..\build\studio-c3-closeout-release",
    [string]$ProbeBuild = "$PSScriptRoot\..\..\build\studio-c3-shutdown-probe"
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot\..\..").Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -property installationPath
$dev = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$dev`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
& $cmake -S $PSScriptRoot -B $ProbeBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_PREFIX_PATH=$workspace/.tools/qt" -DCMAKE_BUILD_TYPE=Release "-DNOVA_BUILD=$((Resolve-Path $NovaBuild).Path)"
if ($LASTEXITCODE -ne 0) { throw 'Probe configuration failed.' }
& $cmake --build $ProbeBuild
if ($LASTEXITCODE -ne 0) { throw 'Probe build failed.' }
