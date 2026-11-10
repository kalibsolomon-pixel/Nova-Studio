# Building the GCC public preview

The public preview is unsigned. Nova Studio is
GPL-3.0-or-later, native Windows C++20 and Qt 6.12.0. No Microsoft compiler or separately
installed compiler redistributable is used. The legacy MSVC build path remains supported
by `scripts/build-studio.ps1` for existing licensed development environments.

## Pinned inputs

- Windows 11 x64; MSYS2 UCRT64 GCC 16.2.0-6, POSIX threads, UCRT.
- MinGW-w64 CRT/headers/winpthreads 14.0.0.r598.ge3d18511f-1 (headers report 15.0.0 snapshot).
- CMake 4.4.5, Ninja 1.13.2, Qt Base 6.12.0, NSIS 3.13.
- Python 3.12 and official MCP SDK 1.26.0 only for qualification scripts and MCP tests.
- Exact authenticated binary packages are locked in `packaging/mingw/msys2-ucrt64-*-lock.json`.

## Build Qt from corresponding source

Every binary package contains `licenses/qtbase-everywhere-src-6.12.0.tar.xz`,
`licenses/qt-build/nova-qtbase-build.json`, the configuration summary and the PCRE2 patch.
The source SHA-256 is a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307.
Use the existing pinned toolchain; for a new machine follow the guarded installation procedure
in `docs/phase-4i/MINGW_BUILD_GUIDE.md`. No tool downloads run implicitly while building.

```powershell
$env:NOVA_MSYS2_ROOT = 'C:\BuildTools\msys64'
$env:NOVA_MINGW_QT = 'C:\BuildTools\qt-6.12.0-gcc'
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-qtbase-mingw.ps1 -OwnerApproved -Source 'C:\Sources\qtbase-everywhere-src-6.12.0.tar.xz' -WorkDir 'C:\QtBuild' -Prefix $env:NOVA_MINGW_QT -Jobs 12
```

The script verifies the tarball, applies the tracked patch, enables Qt pattern initialization,
builds shared release libraries, strips the installed copies, and records sanitized build inputs.
It refuses existing prefixes. Resume verifies configuration and patch identity.
The patch exempts only PCRE2's 32 KiB JIT scratch stack via its existing uninitialized attribute
hook; stack probing, stack canaries, CET and all other Qt auto-variable initialization remain.

## Build and test Nova

```powershell
$env:NOVA_MAKENSIS = 'C:\BuildTools\nsis\makensis.exe'
$env:NOVA_RELEASE_DEPENDENCIES = 'C:\Sources\nova-release-dependencies'
$env:NOVA_MCP_SDK_PYTHON = 'C:\BuildTools\mcp-venv\Scripts\python.exe'
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-mingw.ps1 -Test -BuildJobs 12
```

The dependency directory contains the Qt tarball, `qt-source.json` and extracted `qt-notices/`.
The scripts select explicit compiler, resource compiler, Ninja, Qt and CMake paths and refuse a
reachable `cl.exe`. Environment changes are process-local. The configured tests require their
documented fixtures; see the source tree and legacy test-prerequisite documentation.

## Qualification snapshot identity

When a local commit is not authorized, `scripts/phase4i2-source-snapshot.py` records an immutable
Git tree without changing the real index or making a commit. Build with `-SourceSnapshot <tree>`;
package using `python scripts/package-mingw-experimental.py --source-tree <tree> --label <label>`.
The executable identity contains both HEAD and the complete snapshot tree. The packager verifies
working source bytes against the tree and verifies each corresponding-source ZIP member against
the archived blobs. `clean_source=false` truthfully distinguishes this from a committed build.
An extracted source ZIP is marked `+source-archive`; bit-identical PE reproduction is not claimed.

The exact GCC runtime source package and its metadata accompany the candidate separately to keep
the installed editor small. The release download location must provide that source
alongside installers and portable ZIPs. Qt DLLs may be replaced by compatible GCC Qt 6.12.0 builds;
no signature, hash or licensing mechanism prevents replacement or debugging modified libraries.

No command here authorizes publishing, pushing, merging or tagging.

## Public preview build

For v0.1.0-preview.1, use a clean checkout of that tag and build without -SourceSnapshot.
Run serial CTest with the documented short TEMP/TMP driver on both compiler layouts.
Package the committed GCC build with scripts/package-mingw-experimental.py --build <build-name>;
the historical script name is retained, while download names identify windows-x64-mingw and
the preview version. The manifest records the exact commit and tree. Source-archive builds
remain marked +source-archive; reproducible PE binaries are not claimed.
