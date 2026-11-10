# SPDX-License-Identifier: GPL-3.0-or-later
# Phase 4-I (experimental): shared selection of the open-source GCC/MinGW-w64 toolchain, dot-sourced
# by check-mingw-toolchain.ps1, build-qtbase-mingw.ps1, build-mingw.ps1 and install-mingw-toolchain.ps1.
#
# The toolchain lives in its own folder, outside every repository, and is used only through
# explicit paths and a process-local PATH. Nothing here changes machine or user settings, and
# nothing here installs, updates or repairs anything.
#
#   MSYS2 root      NOVA_MSYS2_ROOT, default C:\Dev\nova-toolchains\msys64-20260927
#   GCC, binutils   <root>\ucrt64\bin (MSYS2 UCRT64: GCC 16.2.0, binutils 2.47, MinGW-w64 14.0)
#   CMake, Ninja    <root>\ucrt64\bin (CMake 4.4.5, Ninja 1.13.2)
#   Qt 6.12.0       NOVA_MINGW_QT, default C:\Dev\nova-toolchains\qt-6.12.0-ucrt64-gcc16-r2 (built by
#                   build-qtbase-mingw.ps1 from the unmodified source); -QtVariant msys2 selects
#                   MSYS2's prebuilt qt6-base 6.12.0-1 in <root>\ucrt64 instead.
#   MCP SDK Python  as for the MSVC build (NOVA_MCP_SDK_PYTHON or the workspace venv)
#
# Package versions are fixed by packaging/mingw/msys2-ucrt64-*-lock.json.

$NovaMingw = [ordered]@{
    Msys2BaseFile   = 'msys2-base-x86_64-20260927.tar.xz'
    Msys2BaseUrl    = 'https://github.com/msys2/msys2-installer/releases/download/2026-09-27/msys2-base-x86_64-20260927.tar.xz'
    Msys2BaseSha256 = 'ea2f31a0b6ade63914ce441ffb022f0f6aa96982bfefa2326460a26d5fb01322'
    DefaultRoot     = 'C:\Dev\nova-toolchains\msys64-20260927'
    DefaultQt       = 'C:\Dev\nova-toolchains\qt-6.12.0-ucrt64-gcc16-r3'
    GccVersion      = '16.2.0'
    CMakeVersion    = '4.4.5'
    NinjaVersion    = '1.13.2'
    QtVersion       = '6.12.0'
    QtSourceSha256  = 'a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307'
    RuntimeDlls     = @('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')
}

function Get-NovaMingwPaths([string]$QtVariant = 'source') {
    # Paths only; nothing is run or checked here.
    $root = if ($env:NOVA_MSYS2_ROOT) { $env:NOVA_MSYS2_ROOT } else { $NovaMingw.DefaultRoot }
    $ucrt = Join-Path $root 'ucrt64'
    $bin = Join-Path $ucrt 'bin'
    $qt = switch ($QtVariant) {
        'source' { if ($env:NOVA_MINGW_QT) { $env:NOVA_MINGW_QT } else { $NovaMingw.DefaultQt } }
        'msys2'  { $ucrt }
        default  { throw "Unknown Qt variant '$QtVariant' (source or msys2)" }
    }
    [ordered]@{
        Root = $root; Ucrt64 = $ucrt; Bin = $bin; QtVariant = $QtVariant; Qt = $qt
        Gxx = Join-Path $bin 'g++.exe'; Gcc = Join-Path $bin 'gcc.exe'; Windres = Join-Path $bin 'windres.exe'
        Objdump = Join-Path $bin 'objdump.exe'; CMake = Join-Path $bin 'cmake.exe'; CTest = Join-Path $bin 'ctest.exe'
        CPack = Join-Path $bin 'cpack.exe'; Ninja = Join-Path $bin 'ninja.exe'; Pacman = Join-Path $root 'usr\bin\pacman.exe'
        Licenses = Join-Path $ucrt 'share\licenses'
    }
}

function Get-NovaPeImports([string]$Path) {
    # Imported DLL names of a PE image, read with the payload checker's pure-Python parser.
    $checker = Join-Path $PSScriptRoot 'check-release-payload.py'
    $code = "import importlib.util,sys,json;s=importlib.util.spec_from_file_location('c',sys.argv[1]);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);print(json.dumps(m.PE(open(sys.argv[2],'rb').read()).imports()))"
    $out = & python -I -c $code $checker $Path
    if ($LASTEXITCODE -ne 0) { throw "Cannot read imports of $Path" }
    @($out | ConvertFrom-Json)
}

function ConvertFrom-NovaNativeLine($Line) {
    # A stderr line arrives as an ErrorRecord; an empty one would print as its type name.
    if ($Line -is [Management.Automation.ErrorRecord]) {
        $text = $Line.Exception.Message
        if ($text -eq 'System.Management.Automation.RemoteException') { return '' }
        return $text
    }
    "$Line"
}

function Invoke-NovaNative([string]$Exe, [string[]]$Arguments, [string]$What) {
    # Runs a native tool and judges it by its exit code only. Windows PowerShell 5.1 turns every
    # stderr line into a terminating error under ErrorActionPreference=Stop when output is
    # redirected (a log file, a pipeline), and compilers print warnings on stderr.
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { & $Exe @Arguments 2>&1 | ForEach-Object { ConvertFrom-NovaNativeLine $_ } } finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)" }
}

function Invoke-NovaVersion([string]$Exe, [string[]]$Arguments, [string]$Pattern) {
    if (-not (Test-Path -LiteralPath $Exe)) { return $null }
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $text = (& $Exe @Arguments 2>&1 | ForEach-Object { "$_" }) -join "`n" } finally { $ErrorActionPreference = $previous }
    if ($text -match $Pattern) { return $matches[1] }
    "unrecognised: $($text.Split("`n")[0])"
}

function Test-NovaMingwToolchain($Paths, [switch]$SkipQt) {
    # Returns @{ Problems; Found }. Runs only version queries of tools inside the toolchain folder.
    $problems = @(); $found = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Paths.Root)) {
        return @{ Problems = @("MSYS2 toolchain folder not found: $($Paths.Root) (not installed; see docs/phase-4i/MINGW_BUILD_GUIDE.md)"); Found = $found }
    }
    $checks = @(
        @('gcc', $Paths.Gxx, @('-dumpfullversion'), '^(\d+\.\d+\.\d+)', $NovaMingw.GccVersion),
        @('cmake', $Paths.CMake, @('--version'), 'cmake version (\S+)', $NovaMingw.CMakeVersion),
        @('ninja', $Paths.Ninja, @('--version'), '^(\S+)', $NovaMingw.NinjaVersion)
    )
    foreach ($c in $checks) {
        $v = Invoke-NovaVersion $c[1] $c[2] $c[3]
        $found[$c[0]] = $v
        if (-not $v) { $problems += "$($c[0]) missing: $($c[1])" }
        elseif ($v -ne $c[4]) { $problems += "$($c[0]) is $v, expected $($c[4])" }
    }
    $target = Invoke-NovaVersion $Paths.Gxx @('-dumpmachine') '^(\S+)'
    $found['target'] = $target
    if ($target -and $target -ne 'x86_64-w64-mingw32') { $problems += "g++ targets $target, expected x86_64-w64-mingw32" }
    foreach ($tool in 'Windres', 'Objdump') { if (-not (Test-Path -LiteralPath $Paths[$tool])) { $problems += "$tool missing: $($Paths[$tool])" } }
    foreach ($dll in $NovaMingw.RuntimeDlls) { if (-not (Test-Path -LiteralPath (Join-Path $Paths.Bin $dll))) { $problems += "GCC runtime $dll missing in $($Paths.Bin)" } }
    # UCRT, not the legacy msvcrt.dll: the C runtime the GCC runtime itself imports.
    $stdcxx = Join-Path $Paths.Bin 'libstdc++-6.dll'
    if (Test-Path -LiteralPath $stdcxx) {
        $imports = Get-NovaPeImports $stdcxx
        $found['libstdc++ C runtime'] = if ($imports -contains 'msvcrt.dll') { 'msvcrt' } elseif ($imports | Where-Object { $_ -like 'api-ms-win-crt-*' }) { 'ucrt' } else { 'unknown' }
        if ($found['libstdc++ C runtime'] -ne 'ucrt') { $problems += "libstdc++-6.dll is not a UCRT build ($($found['libstdc++ C runtime']))" }
    }
    if (-not $SkipQt) { $problems += @(Test-NovaMingwQt $Paths $found) }
    @{ Problems = $problems; Found = $found }
}

function Test-NovaMingwQt($Paths, $Found) {
    # Qt 6.12.0 built with GCC for UCRT, with the private modules Nova needs. A Qt built with MSVC
    # is refused here: its DLLs import the Visual C++ runtime.
    $problems = @()
    $config = Join-Path $Paths.Qt 'lib\cmake\Qt6\Qt6ConfigVersionImpl.cmake'
    if (-not (Test-Path -LiteralPath $config)) { $config = Join-Path $Paths.Qt 'lib\cmake\Qt6\Qt6ConfigVersion.cmake' }
    if (-not (Test-Path -LiteralPath $config)) { return @("Qt not found at $($Paths.Qt) (no lib\cmake\Qt6)") }
    $text = Get-Content -LiteralPath $config -Raw
    if ($text -notmatch [regex]::Escape("PACKAGE_VERSION `"$($NovaMingw.QtVersion)`"")) { $problems += "Qt at $($Paths.Qt) is not version $($NovaMingw.QtVersion)" }
    foreach ($module in 'Qt6Core', 'Qt6Gui', 'Qt6Widgets', 'Qt6OpenGL', 'Qt6OpenGLWidgets', 'Qt6Network', 'Qt6Concurrent', 'Qt6Test') {
        if (-not (Test-Path -LiteralPath (Join-Path $Paths.Qt "bin\$module.dll"))) { $problems += "$module.dll missing in $($Paths.Qt)\bin" }
    }
    foreach ($private in 'Qt6CorePrivate', 'Qt6GuiPrivate') {
        if (-not (Test-Path -LiteralPath (Join-Path $Paths.Qt "lib\cmake\$private"))) { $problems += "$private CMake package missing in $($Paths.Qt)\lib\cmake" }
    }
    $zip = @(Get-ChildItem -LiteralPath (Join-Path $Paths.Qt 'include') -Recurse -Filter 'qzipreader_p.h' -ErrorAction SilentlyContinue)
    if ($zip.Count -eq 0) { $problems += "private header qzipreader_p.h missing under $($Paths.Qt)\include" }
    $core = Join-Path $Paths.Qt 'bin\Qt6Core.dll'
    if (Test-Path -LiteralPath $core) {
        $imports = Get-NovaPeImports $core
        $Found['Qt6Core imports'] = ($imports -join ' ')
        if ($imports | Where-Object { $_ -match '^(msvcp|vcruntime|concrt)' }) { $problems += 'Qt6Core.dll imports the Visual C++ runtime: an MSVC Qt cannot be used with GCC' }
        if ($imports -notcontains 'libstdc++-6.dll') { $problems += 'Qt6Core.dll does not import libstdc++-6.dll: not a GCC build of Qt' }
        if ($imports -contains 'msvcrt.dll') { $problems += 'Qt6Core.dll imports msvcrt.dll: not a UCRT build of Qt' }
    }
    $Found['qt'] = $Paths.Qt
    $problems
}

function Enter-NovaMingwEnvironment($Paths) {
    # Process-local only: the toolchain and Qt first, Windows next, and no Visual Studio or Windows
    # SDK folder anywhere, so no Microsoft compiler, linker or library can be picked up.
    $system = Join-Path $env:SystemRoot 'System32'
    $keep = @($env:PATH -split ';' | Where-Object { $_ } | Where-Object {
        $_ -notmatch '(?i)Microsoft Visual Studio|\\BuildTools\\|Windows Kits|\\msys64|\\mingw|\\Strawberry|\\LLVM\\|\\Git\\(mingw64|usr)\\'
    })
    $env:PATH = (@((Join-Path $Paths.Qt 'bin'), $Paths.Bin, $system, $env:SystemRoot) + $keep | Select-Object -Unique) -join ';'
    foreach ($name in 'INCLUDE', 'LIB', 'LIBPATH', 'VCINSTALLDIR', 'VCToolsInstallDir', 'VCToolsRedistDir', 'VSINSTALLDIR',
                      'WindowsSdkDir', 'WindowsSDKVersion', 'CC', 'CXX', 'CMAKE_GENERATOR', 'CMAKE_PREFIX_PATH') {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($cl) { throw "cl.exe is still reachable on PATH ($($cl.Source)); refusing to continue" }
    $gxx = (Get-Command g++.exe -ErrorAction SilentlyContinue).Source
    if (-not $gxx -or (Resolve-Path -LiteralPath $gxx).Path -ne (Resolve-Path -LiteralPath $Paths.Gxx).Path) {
        throw "g++.exe on PATH is '$gxx', expected $($Paths.Gxx)"
    }
}
