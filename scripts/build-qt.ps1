param([string]$QtRoot = "$PSScriptRoot\..\.tools\qt",[string]$BuildName = 'qt')
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot\..").Path
if ($IsWindows) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -property installationPath
    $dev = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    # Import only the toolchain environment. File operations remain in PowerShell.
    cmd /c "`"$dev`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
    }
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
} else { $cmake = 'cmake'; $ninja = 'ninja' }
& $cmake -S "$workspace\prototypes\nova-qt" -B "$workspace\build\$BuildName" -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_PREFIX_PATH=$QtRoot" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'Qt configuration failed' }
& $cmake --build "$workspace\build\$BuildName" --parallel
if ($LASTEXITCODE -ne 0) { throw 'Qt build failed' }
