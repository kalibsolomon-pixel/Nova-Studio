$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot\..\..").Path
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products '*' -property installationPath
$dev=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$dev`" >nul && set" | ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}
}
$cmake=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$build=Join-Path $workspace 'build\studio-c4-autonomous-final\artifact-verification-build'
$out=Join-Path $workspace 'docs\phase-2c4\compatibility-final\autonomous'
& $cmake -S $PSScriptRoot -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_PREFIX_PATH=$workspace\.tools\qt" '-DCMAKE_BUILD_TYPE=Release' *> "$out\artifact-build.txt"
if($LASTEXITCODE -ne 0){throw 'Artifact verifier configuration failed'}
& $cmake --build $build --parallel *>> "$out\artifact-build.txt"
if($LASTEXITCODE -ne 0){throw 'Artifact verifier build failed'}
$env:PATH="$workspace\.tools\qt\bin;$env:PATH"
& "$build\c4-artifact-verification.exe" "$out\..\fixture-manifest.json" "$out\..\fixtures" "$out\nova-roundtrip" > "$out\artifact-run.txt" 2> "$out\artifact-stderr.txt"
if($LASTEXITCODE -ne 0){throw 'Artifact import/export/reimport verification failed'}
