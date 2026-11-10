# Building Nova Studio from source

Nova Studio is free software under the GNU General Public License version 3 or later
(GPL-3.0-or-later). This guide builds the same programs that are in a release, from the
corresponding source archive (`source\nova-studio-<version>-source.zip` in a release download) or
from a Git checkout.

Use the release tag and manifest described in [SOURCE_CORRESPONDENCE.md](SOURCE_CORRESPONDENCE.md).
A rebuild creates new artifacts requiring their own qualification.

## GCC/MinGW candidate package

The packaged candidate uses GCC 16.2.0 (MSYS2 UCRT64), CMake 4.4.5, Ninja 1.13.2 and Qt Base
6.12.0 built from source with the included PCRE2 patch. It is the toolchain selected for the public preview. Follow `docs/phase-4i/MINGW_BUILD_GUIDE.md` and the package's `docs/BUILDING.md`.
`NOVA_MSYS2_ROOT` and `NOVA_MINGW_QT` select the toolchain and Qt prefixes; neither needs to be in
the developer's workspace. No downloads run implicitly during a build.

```powershell
./scripts/build-mingw.ps1 -BuildName gcc-release
```

Tests need the official MCP Python SDK and a short, dedicated TEMP/TMP directory on Windows.
Use serial CTest (`-j1`) for release qualification, preserve raw output and JUnit results, and
check foreground/display safety before every windowed batch. The qualification driver is kept
with the private acceptance evidence, outside the source/package. Seven optional QtTest slots
need external fixtures and may skip by design; all 67 CTest tests must pass.

## MSVC regression build: what you need

| Tool | Version used for 0.1.0-preview.1 | Notes |
| --- | --- | --- |
| Windows 11 x64 | — | |
| Visual Studio Build Tools 2026 (standalone) | 18.10, MSVC 14.51 (v145, `cl` 19.51), "Desktop development with C++" | Includes CMake 4.3.1 and Ninja 1.13.2. No Visual Studio IDE is needed. Other versions are untested. |
| Windows SDK | 10.0.26100 | Installed with the Build Tools. |
| Qt Base | **6.12.0 exactly**, `win64_msvc2022_64` | Nova uses a private Qt API (`QZipReader`), so the version must match. |
| Python | 3.12 | Only for the helper scripts. |
| Official MCP Python SDK | 1.26.0, in its own virtual environment | Only for the `mcp` test suite. |
| NSIS | 3.13 | Only to build the installer. |

Microsoft licenses Visual Studio Build Tools under its own terms (as a supplement to a Visual Studio
license, with a narrow exception for open-source dependencies). Check that those terms cover your
use before you build: https://visualstudio.microsoft.com/license-terms/. Nothing in Nova Studio's
license grants rights to Microsoft's tools.

Get Qt Base 6.12.0 either with the Qt online installer, or with the helper script, which downloads
only Qt Base from download.qt.io and checks the published checksums:

```powershell
python -m pip install -r scripts/requirements-lock.txt
python scripts/install-qt.py            # installs into .tools\qt
```

You can also build Qt 6.12.0 yourself from `qtbase-everywhere-src-6.12.0.tar.xz` (shipped in every
release's `licenses` folder). The options The Qt Company used are in
`licenses\qt-build\config_qtbase.opt`.

## MSVC build and test

From the source folder:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-studio.ps1 -Configuration Release -BuildName studio-release
```

The script selects the **standalone Build Tools** instance explicitly (`vswhere -products
Microsoft.VisualStudio.Product.BuildTools`), so a Visual Studio IDE installed on the same machine is
never used by accident. It loads `vcvars64` for MSVC 14.51 and Windows SDK 10.0.26100.0, checks
that `cl.exe` is 19.51 and that the CMake and Ninja are the ones bundled with that instance,
configures against `.tools\qt` (use `-QtRoot <folder>` for another Qt 6.12.0), builds, records
the toolchain in `build\<name>\nova-toolchain.json`, and runs the tests.
`scripts/run-ctest.ps1 -BuildName studio-release` reruns the tests.

To use a different instance or version on purpose, set `NOVA_VS_INSTALL_PATH`, `NOVA_MSVC_TOOLSET`
or `NOVA_WINDOWS_SDK` before running the scripts. To see what would be used, without running
any build tool:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/check-toolchain.ps1
```

### Test prerequisites

The `mcp` suite drives `nova-mcp.exe` through the official MCP Python SDK. The test scripts use,
in this order: `-McpSdkPython <python.exe>`, the `NOVA_MCP_SDK_PYTHON` environment variable, or a
virtual environment in the source folder (`build\phase4c-venv`, `.venv` or `build\mcp-sdk-venv`).
The interpreter is checked first (it must import the official `mcp` package). If none is usable
the test run stops with an explanation. Create the environment once with:

```powershell
python -m venv build\phase4c-venv
build\phase4c-venv\Scripts\python.exe -m pip install -r scripts/requirements-mcp-sdk.txt
```

`-SkipMcp` runs everything else and says clearly that the `mcp` suite was excluded.

`NOVA_SOURCE_URL` (default empty) is the repository address shown in **Help › Licenses and Source
Code**. Set it only to a repository that is public and holds the source (`-DNOVA_SOURCE_URL=<address>`),
or to your own address for a fork. Packaged copies always include the source archive, `LICENSE.txt`
and `licenses\THIRD_PARTY_NOTICES.md`.

The programs are `build\studio-release\src\nova-studio.exe` and `build\studio-release\src\nova-mcp.exe`.
MCP clients start `nova-mcp.exe` with their own `PATH`, so each build copies the Qt DLLs it imports (and,
for GCC builds, the compiler's runtime DLLs) next to it; the build-tree bridge then starts with only the
Windows folders on `PATH`. An MSVC build-tree bridge uses the Visual C++ runtime installed in Windows;
packages carry their own copy. `nova-studio.exe` itself still needs Qt on `PATH` in the build tree
(`scripts\run-studio.ps1` sets it).

Qt finds its plugins relative to the `Qt6Core.dll` it loads, so a folder that receives these copies
(`src\`, and `tests\`, which holds a copy of the bridge) also gets a `qt.conf`. It points `Prefix` at
the Qt installation the DLLs came from and repeats Qt's own folder layout, so every Qt program in that
folder finds its plugins exactly as before. The file names an absolute path on the build machine and
is never installed; packages write their own `qt.conf` (`Prefix=.`). Do not add a `qt6.conf` to those
folders: Qt would read it instead.
**Help › About Nova Studio** shows the source revision. A build from the source archive shows the archive's
revision followed by `+source-archive`, because the archive cannot prove that its files were not
changed. A Git checkout with uncommitted changes shows `+modified`.

## Package (portable folder, ZIP and installer)

Build with `scripts/build-studio.ps1` first: the Microsoft Visual C++ runtime folder is fixed when
it configures the build. Then run the packager from a **plain PowerShell window** (not a developer
prompt). First fetch the Qt source and notices that every package carries:

```powershell
python scripts/prepare-phase3-qt.py     # build\phase3-dependencies: Qt source archive, notices
python scripts/package-phase4g.py --build studio-release
```

`package-phase4g.py` needs a Release build of a clean Git checkout (add `--allow-modified` for
a rehearsal from changed sources), NSIS 3.13 at `.tools\nsis\nsis-3.13` (or set `NOVA_MAKENSIS`
when configuring), and writes the installer, the ZIP, the corresponding source archive and their
checksums to `build\phase4g-distribution\<label>`.

Before packaging, it checks the payload with `scripts/check-release-payload.py`: exactly the
expected 19 executables and DLLs, no Windows SDK DirectX compilers (`d3dcompiler_47.dll`,
`dxcompiler.dll`, `dxil.dll`), no software OpenGL, debug or app-local Universal CRT files, every
import resolved, the 10 Visual C++ runtime DLLs byte-identical to the Build Tools redistributable
folder, and the Qt DLLs byte-identical to the Qt kit. `--baseline <label>` also compares the new
payload with an earlier candidate and refuses added, removed or changed third-party binaries. The
checker runs no build tool, so you can also point it at any unpacked portable ZIP.

## Replacing the Qt libraries

Nova Studio links Qt dynamically. To use a modified Qt, build Qt Base 6.12.0 with your changes and
replace `Qt6*.dll` and `plugins\platforms\qwindows.dll` in the Nova Studio folder with yours. Nova
Studio does not check signatures or hashes of these files.
