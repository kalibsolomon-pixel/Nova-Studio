param([string]$Workspace = (Resolve-Path "$PSScriptRoot\..\..").Path)
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -property installationPath
$dev = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$dev`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$build = Join-Path $Workspace 'build\studio-c4-compatibility-final\probe'
$out = Join-Path $Workspace 'docs\phase-2c4\compatibility-final'
& $cmake -S $PSScriptRoot -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_PREFIX_PATH=$Workspace\.tools\qt" '-DCMAKE_BUILD_TYPE=Release' *> "$out\probe-build.txt"
if ($LASTEXITCODE -ne 0) { throw 'Probe configuration failed' }
& $cmake --build $build --parallel *>> "$out\probe-build.txt"
if ($LASTEXITCODE -ne 0) { throw 'Probe build failed' }
$env:PATH = "$Workspace\.tools\qt\bin;$env:PATH"
& "$build\c4-compatibility-probe.exe" > "$out\backend-probe.json" 2> "$out\backend-probe-stderr.txt"
if ($LASTEXITCODE -ne 0) { throw 'Offscreen probe failed' }
