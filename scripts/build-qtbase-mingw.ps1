# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [switch]$OwnerApproved,
    # Also shipped in every release as licenses\qtbase-everywhere-src-6.12.0.tar.xz.
    [string]$Source = (Join-Path $PSScriptRoot '..\build\phase3-dependencies\qtbase-everywhere-src-6.12.0.tar.xz'),
    [string]$WorkDir = 'C:\Dev\nova-toolchains\qtb-6120-r3',
    [string]$Prefix = '',
    [int]$Jobs = 0,      # parallel compiler jobs; GCC needs ~1 GB of commit per job on Qt's larger files
    [switch]$Resume      # continue an interrupted build in an existing, identically configured tree
)
# Phase 4-I.2: builds Qt Base 6.12.0 with the tracked PCRE2 JIT-stack patch and pinned MSYS2
# GCC, into its own prefix (default C:\Dev\nova-toolchains\qt-6.12.0-ucrt64-gcc16-r3; the first
# Phase 4-I.1 build, without the auto-variable option below, is kept in ...-gcc16).
#
# Part of the isolated toolchain setup: it runs only with -OwnerApproved. It uses the same Qt Base
# source archive that every release already ships as corresponding source (SHA-256 checked), so no
# new Qt download is needed.
#
# The configuration follows the official Qt 6.12.0 MSVC kit that Nova ships today
# (.tools\qt\config_qtbase.opt and config_qtbase.summary), so the compiler is the only intended
# difference: bundled zlib/libpng/libjpeg/PCRE2/HarfBuzz/FreeType/double-conversion/libb2/md4c,
# no ICU, dynamic OpenGL, Schannel TLS, release build. Modules Nova does not use and that the kit
# builds as separate DLLs (SQL drivers, D-Bus) are left out; OpenSSL, Vulkan and zstd are off (the kit
# loads OpenSSL only at run time and Nova never ships it; Nova renders with OpenGL only).
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'nova-mingw-toolchain.ps1')
. (Join-Path $PSScriptRoot 'nova-qt-build-record.ps1')
$patch = Join-Path $PSScriptRoot '../packaging/mingw/qtbase-6.12.0-pcre2-jit-stack.patch'
$patchSha = (Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash.ToLowerInvariant()
$paths = Get-NovaMingwPaths 'source'
if (-not $Prefix) { $Prefix = $paths.Qt }
$check = Test-NovaMingwToolchain $paths -SkipQt
if ($check.Problems) { throw ('Toolchain not ready: ' + ($check.Problems -join '; ')) }
$options = @(
    '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_INSTALL_PREFIX=$Prefix",
    "-DCMAKE_C_COMPILER=$($paths.Gcc)", "-DCMAKE_CXX_COMPILER=$($paths.Gxx)", "-DCMAKE_MAKE_PROGRAM=$($paths.Ninja)",
    '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_EXAMPLES=OFF', '-DQT_INSTALL_CONFIG_INFO_FILES=ON',
    '-DFEATURE_pkg_config=OFF',
    '-DFEATURE_system_zlib=OFF', '-DFEATURE_system_png=OFF', '-DFEATURE_system_jpeg=OFF', '-DFEATURE_system_pcre2=OFF',
    '-DFEATURE_system_harfbuzz=OFF', '-DFEATURE_system_freetype=OFF', '-DFEATURE_system_doubleconversion=OFF',
    '-DFEATURE_system_libb2=OFF', '-DFEATURE_system_md4c=OFF',
    '-DFEATURE_icu=OFF', '-DINPUT_opengl=dynamic', '-DFEATURE_schannel=ON', '-DFEATURE_openssl=OFF',
    '-DFEATURE_dbus=OFF', '-DFEATURE_sql=OFF', '-DFEATURE_zstd=OFF', '-DFEATURE_glib=OFF', '-DFEATURE_vulkan=OFF',
    # Off in the official kit too. MSYS2's CMake package installs brotli, so without this Qt Network
    # would link the toolchain's libbrotlidec.dll.
    '-DFEATURE_brotli=OFF', '-DFEATURE_libproxy=OFF', '-DFEATURE_gssapi=OFF',
    '-DFEATURE_trivial_auto_var_init_pattern=ON'
)
Write-Host "Qt Base 6.12.0 from $Source"
Write-Host "  build in $WorkDir, install to $Prefix"
Write-Host "  options: $($options -join ' ')"
if (-not $OwnerApproved) { Write-Host 'Not approved: nothing was built. Re-run with -OwnerApproved after owner approval.'; exit 2 }
if (Test-Path -LiteralPath $Prefix) { throw "$Prefix already exists; refusing to overwrite an existing Qt." }
$sha = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash.ToLowerInvariant()
if ($sha -ne $NovaMingw.QtSourceSha256) { throw "Qt source SHA-256 is $sha, expected $($NovaMingw.QtSourceSha256)" }
$src = Join-Path $WorkDir 'qtbase-everywhere-src-6.12.0'
$bld = Join-Path $WorkDir 'b'
$started = Get-Date

Enter-NovaMingwEnvironment $paths
if ($Resume) {
    # Continue an interrupted build (for example after the compiler ran out of memory) only if the
    # existing tree was configured by this script with exactly these options.
    $cache = Join-Path $bld 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache)) { throw "-Resume: no configured build tree at $bld" }
    $text = (Get-Content -LiteralPath $cache -Raw).Replace('\', '/')   # CMake stores paths with forward slashes
    $differs = @($options | Where-Object { $_ -match '^-D([^=:]+)=(.*)$' } | Where-Object {
        $null = $_ -match '^-D([^=:]+)=(.*)$'
        $text -notmatch ('(?m)^' + [regex]::Escape($matches[1]) + ':[A-Z]+=' + [regex]::Escape($matches[2].Replace('\', '/')) + '\r?$') })
    if ($differs) { throw "-Resume: $bld was configured differently: $($differs -join ' ')" }
    if (-not (Test-Path -LiteralPath (Join-Path $WorkDir 'nova-patch.sha256')) -or
        (Get-Content -LiteralPath (Join-Path $WorkDir 'nova-patch.sha256') -Raw).Trim() -ne $patchSha) {
        throw '-Resume: Qt patch identity differs'
    }
    & git -C $src apply --reverse --check -- $patch
    if ($LASTEXITCODE -ne 0) { throw '-Resume: Qt patch is not applied exactly' }
    Write-Host "Resuming the configured build in $bld"
} else {
    if (Test-Path -LiteralPath $WorkDir) { throw "$WorkDir already exists; check its contents, then remove it or use -Resume." }
    New-Item -ItemType Directory -Path $WorkDir | Out-Null
    Invoke-NovaNative "$env:SystemRoot\System32\tar.exe" @('-xf', $Source, '-C', $WorkDir) 'Unpacking the Qt source (tar)'
    & git -C $src apply --check -- $patch
    if ($LASTEXITCODE -ne 0) { throw 'Qt patch preflight failed' }
    & git -C $src apply -- $patch
    if ($LASTEXITCODE -ne 0) { throw 'Qt patch failed' }
    [IO.File]::WriteAllText((Join-Path $WorkDir 'nova-patch.sha256'), $patchSha)
    Invoke-NovaNative $paths.CMake (@('-S', $src, '-B', $bld) + $options) 'Qt configure'
}
$buildArgs = @('--build', $bld, '--parallel'); if ($Jobs -gt 0) { $buildArgs += "$Jobs" }
Invoke-NovaNative $paths.CMake $buildArgs 'Qt build'
# --strip: release DLLs without COFF symbol tables (about a third of their size with GNU ld), like
# the MSVC kit, whose symbols live in separate PDB files.
Invoke-NovaNative $paths.CMake @('--install', $bld, '--strip') 'Qt install'

# Corresponding-source record for the payload (licenses\qt-build\nova-qtbase-build.json).
[ordered]@{
    format = 'nova-qtbase-build'; qt_version = $NovaMingw.QtVersion
    source = (Split-Path $Source -Leaf); source_sha256 = $sha
    source_url = 'https://download.qt.io/archive/qt/6.12/6.12.0/submodules/qtbase-everywhere-src-6.12.0.tar.xz'
    patches = @(@{ file = (Split-Path $patch -Leaf); sha256 = $patchSha; purpose = 'PCRE2 JIT scratch stack only; Qt-wide pattern initialization retained' })
    cmake_options = @(ConvertTo-NovaQtBuildOptions $options); install = 'cmake --install <build> --strip'
    compiler = (& $paths.Gxx --version | Select-Object -First 1); target = (& $paths.Gxx -dumpmachine)
    cmake = (& $paths.CMake --version | Select-Object -First 1); ninja = (& $paths.Ninja --version)
    # The exact MSYS2 packages (pacman -Q) that compiled this Qt, instead of a machine path.
    # Plain strings: Windows PowerShell's ConvertTo-Json would otherwise serialise each Get-Content line
    # with its provider metadata, including the user's home folder.
    msys2_packages = @(Get-NovaQtPackageLines (Join-Path $paths.Root 'nova-toolchain-manifest.txt'))
    built_at = (Get-Date).ToUniversalTime().ToString('o'); minutes = [math]::Round(((Get-Date) - $started).TotalMinutes, 1)
} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $Prefix 'nova-qtbase-build.json') -Encoding utf8
Copy-Item -LiteralPath $patch -Destination (Join-Path $Prefix (Split-Path $patch -Leaf))
$summary = Join-Path $bld 'config.summary'
if (Test-Path -LiteralPath $summary) { Copy-Item -LiteralPath $summary -Destination (Join-Path $Prefix 'config_qtbase.summary') }
Write-Host "Qt Base installed to $Prefix in $([math]::Round(((Get-Date) - $started).TotalMinutes, 1)) minutes. The build tree $WorkDir can be deleted by the owner."
$check = Test-NovaMingwToolchain $paths
if ($check.Problems) { throw ('Qt check failed: ' + ($check.Problems -join '; ')) }
