# Building Nova Studio from source

Nova Studio is free software under the GNU General Public License version 3 or later
(GPL-3.0-or-later). This guide builds the same programs that are in a release, from the
corresponding source archive (`source\nova-studio-<version>-source.zip` in a release download) or
from a Git checkout.

## What you need

| Tool | Version used for 0.1.0-preview.1 | Notes |
| --- | --- | --- |
| Windows 11 x64 | — | |
| Visual Studio Build Tools 2026 | 18.10, MSVC 14.51 (v145), "Desktop development with C++" | Includes CMake 4.3.1 and Ninja 1.13.2. Other versions are untested. |
| Qt Base | **6.12.0 exactly**, `win64_msvc2022_64` | Nova uses a private Qt API (`QZipReader`), so the version must match. |
| Python | 3.12 | Only for the helper scripts. |
| NSIS | 3.13 | Only to build the installer. |

Get Qt Base 6.12.0 either with the Qt online installer, or with the helper script, which downloads
only Qt Base from download.qt.io and checks the published checksums:

```powershell
python -m pip install -r scripts/requirements-lock.txt
python scripts/install-qt.py            # installs into .tools\qt
```

You can also build Qt 6.12.0 yourself from `qtbase-everywhere-src-6.12.0.tar.xz` (shipped in every
release's `licenses` folder). The options The Qt Company used are in
`licenses\qt-build\config_qtbase.opt`.

## Build and test

From the source folder:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-studio.ps1 -Configuration Release -BuildName studio-release
```

The script finds Visual Studio with `vswhere`, configures with CMake and Ninja against
`.tools\qt` (use `-QtRoot <folder>` for another Qt 6.12.0), builds, and runs the tests.
`scripts/run-ctest.ps1 -BuildName studio-release` reruns the tests.

`NOVA_SOURCE_URL` (default `https://github.com/kalibsolomon-pixel/Nova-Studio`) is the repository
address shown in **Help › Licenses and Source Code**. Pass `-DNOVA_SOURCE_URL=` to clear it, or your
own address for a fork.

The programs are `build\studio-release\src\nova-studio.exe` and `build\studio-release\src\nova-mcp.exe`.
**Help › About Nova Studio** shows the source revision. A build from the source archive shows the archive's
revision followed by `+source-archive`, because the archive cannot prove that its files were not
changed. A Git checkout with uncommitted changes shows `+modified`.

## Package (portable folder, ZIP and installer)

Run from a Visual Studio x64 developer prompt, so that the Microsoft Visual C++ runtime can be
found. First fetch the Qt source and notices that every package carries:

```powershell
python scripts/prepare-phase3-qt.py     # build\phase3-dependencies: Qt source archive, notices
python scripts/package-phase4g.py --build studio-release
```

`package-phase4g.py` needs a Release build of a clean Git checkout (add `--allow-modified` for
a rehearsal from changed sources), NSIS 3.13 at `.tools\nsis\nsis-3.13` (or set `NOVA_MAKENSIS`
when configuring), and writes the installer, the ZIP, the corresponding source archive and their
checksums to `build\phase4g-distribution\<label>`.

## Replacing the Qt libraries

Nova Studio links Qt dynamically. To use a modified Qt, build Qt Base 6.12.0 with your changes and
replace `Qt6*.dll` and `plugins\platforms\qwindows.dll` in the Nova Studio folder with yours. Nova
Studio does not check signatures or hashes of these files.
