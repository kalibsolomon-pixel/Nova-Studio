# Building Nova Studio with GCC/MinGW-w64 (Phase 4-I, experimental)

Status: **executed in Phase 4-I.1** (owner approval of §1 given; historical raw results remain
private; see the [public qualification summary](../release/PUBLIC_SOURCE_PREPARATION.md)). The toolchain is installed in
`C:\Dev\nova-toolchains`; §7 is the complete, repeatable procedure that produced the experimental
candidate. §1 is kept as the approved request. The MSVC build (`docs/release/BUILDING.md`,
`scripts/build-studio.ps1`) is unchanged and unaffected.

## 1. Installation request (for owner approval)

Nothing below has been installed. Approving this request authorizes exactly these steps:

| # | What | Where | Size | Verification |
| --- | --- | --- | --- | --- |
| 1 | MSYS2 base `msys2-base-x86_64-20260927.tar.xz` from https://github.com/msys2/msys2-installer/releases/tag/2026-09-27 | unpacked to `C:\Dev\nova-toolchains\msys64-20260927` | 42.9 MB download | SHA-256 `ea2f31a0b6ade63914ce441ffb022f0f6aa96982bfefa2326460a26d5fb01322` (release asset digest) |
| 2 | 51 MSYS2 UCRT64 packages: GCC 16.2.0-6, binutils 2.47-4, MinGW-w64 14.0.0.r598, CMake 4.4.5-1, Ninja 1.13.2-1 and their dependencies (`packaging/mingw/msys2-ucrt64-toolchain-lock.json`) | into that folder | 102.1 MiB download, 664.7 MiB installed | SHA-256 from the lock **and** MSYS2 GPG signature per file; `pacman -U` of exactly those files |
| 3 | Qt Base 6.12.0 compiled from the retained, unmodified source archive | `C:\Dev\nova-toolchains\qt-6.12.0-ucrt64-gcc16` (build tree `C:\Dev\nova-toolchains\qtb-6120`) | ≈0.3 GB installed, 3–4 GB temporary (deletable), 30–90 min (estimate) | SHA-256 `a951bd16…1307` of the source archive |

Properties: no installer program runs; no administrator rights; no change to PATH or any machine or
user setting; no service, scheduled task, Start-menu or uninstall entry; nothing inside either
repository or `.tools`; the existing MSVC Build Tools, Qt kit, NSIS and Python are not touched. The
keyring's `gpg-agent` is stopped at the end. Removal = delete `C:\Dev\nova-toolchains`.

Optional, only if a faster first compile is wanted while Qt builds: the prebuilt MSYS2 Qt tier
(`packaging/mingw/msys2-ucrt64-qt-prebuilt-lock.json`, 73 packages, 1.24 GiB installed) instead of
step 2. Not needed for the recommended route and not recommended for packaging
(`QT_COMPATIBILITY.md` §3).

Commands once approved (normal, non-elevated PowerShell):

```powershell
cd <your Nova Studio checkout>
.\scripts\install-mingw-toolchain.ps1 -OwnerApproved          # steps 1-2
.\scripts\build-qtbase-mingw.ps1 -OwnerApproved               # step 3
.\scripts\check-mingw-toolchain.ps1                           # read-only preflight, must print OK
```

Without `-OwnerApproved` both scripts print their plan and exit with code 2 without downloading or
installing anything (verified).

## 2. Toolchain selection

`scripts/nova-mingw-toolchain.ps1` pins and checks everything; overrides are explicit:

| Item | Default | Override |
| --- | --- | --- |
| MSYS2 root | `C:\Dev\nova-toolchains\msys64-20260927` | `NOVA_MSYS2_ROOT` |
| Qt prefix (source build) | `C:\Dev\nova-toolchains\qt-6.12.0-ucrt64-gcc16` | `NOVA_MINGW_QT` |
| Qt variant | `source` | `-QtVariant msys2` (MSYS2 prebuilt, experiments only) |
| MCP SDK interpreter | `NOVA_MCP_SDK_PYTHON` or `build\phase4c-venv` | `-McpSdkPython` |

Checks before any build: GCC 16.2.0 targeting `x86_64-w64-mingw32`, CMake 4.4.5, Ninja 1.13.2,
`windres`/`objdump`, the three GCC runtime DLLs present and **UCRT** (`libstdc++-6.dll` imports
`api-ms-win-crt-*`, not `msvcrt.dll`); Qt 6.12.0 with the eight module DLLs, `Qt6CorePrivate`,
`Qt6GuiPrivate`, `qzipreader_p.h`, and `Qt6Core.dll` importing `libstdc++-6.dll` but neither the
Visual C++ runtime nor `msvcrt.dll` (an MSVC or msvcrt Qt is refused).

The build process PATH is reduced to Qt `bin`, `ucrt64\bin`, System32 and Windows; every Visual
Studio, Build Tools, Windows Kits, other MSYS2/MinGW, LLVM, Strawberry and Git-for-Windows MinGW
folder is removed, the MSVC environment variables are cleared, and the script stops if `cl.exe` is
still reachable. Machine and user environment are never changed.

## 3. Build and test

```powershell
.\scripts\build-mingw.ps1                  # configure + build, build\mingw-ucrt64-source-Release
.\scripts\build-mingw.ps1 -Test            # + all 43 CTest suites (MCP SDK required)
.\scripts\build-mingw.ps1 -Test -SkipMcp   # explicitly partial run (warned)
```

Equivalent with presets (after setting `NOVA_MSYS2_ROOT` and `NOVA_MINGW_QT` in the shell):
`cmake --preset mingw-ucrt64-release`, `cmake --build --preset mingw-ucrt64-release`,
`ctest --preset mingw-ucrt64-release`.

The build records its toolchain in `build\<dir>\nova-mingw-toolchain.json`.

## 4. Experimental payload and packages

```powershell
$b = 'build\mingw-ucrt64-source-Release'
cmake --install $b --prefix build\mingw-payload\Nova Studio
python -I scripts\check-mingw-payload.py --payload "build\mingw-payload\Nova Studio" `
       --qt $env:NOVA_MINGW_QT --toolchain-bin C:\Dev\nova-toolchains\msys64-20260927\ucrt64\bin --report build\mingw-payload-check.json
cpack --config $b\CPackConfig.cmake -G ZIP  -D NOVA_PAYLOAD="build\mingw-payload\Nova Studio"
cpack --config $b\CPackConfig.cmake -G NSIS -D NOVA_PAYLOAD="build\mingw-payload\Nova Studio"
```

Package names end in `-windows-x64-mingw-experimental` so they cannot be confused with an MSVC
candidate. They are not for publication. Phase 4-I.1 added `scripts/package-mingw-experimental.py`,
which does all of the above plus the corresponding-source archive (checked blob by blob against
Git), notice and manifest checks, the bridge self-test with only Windows on PATH, and SHA256SUMS
(§7, step 5). Use it instead of the manual commands.

## 5. What changed in the repository (experimental branch only)

| File | Change |
| --- | --- |
| `CMakeLists.txt` | `MINGW`: `_WIN32_WINNT=WINVER=0x0A00`; `MINGW` includes `cmake/NovaPackagingMinGW.cmake`, other Windows compilers keep `cmake/NovaPackaging.cmake` |
| `src/CMakeLists.txt` | `Psapi` for `nova-workspace` on `WIN32`; GCC warning/charset flags under `MINGW`; MinGW builds embed `packaging/mingw/THIRD_PARTY_NOTICES.md` |
| `tests/CMakeLists.txt` | `Psapi`/`Dbghelp` for 7 benches and probes on `WIN32` instead of `MSVC` |
| `tests/phase4g1_licensing_test.cpp` | GCC builds assert the GCC/MinGW notices; MSVC assertions unchanged |
| `cmake/NovaPackagingMinGW.cmake`, `cmake/NovaMingwHelpers.cmake` | experimental payload and CPack configuration |
| `packaging/mingw/*` | notices, README, installer licence page, package locks |
| `scripts/*mingw*`, `scripts/msys2-resolve-lock.py`, `scripts/check-mingw-payload.py` | toolchain, build, install, Qt build, lock and payload tools |
| `CMakePresets.json` | one MinGW preset (MSVC scripts do not use presets) |

For the MSVC compiler every changed CMake condition evaluates exactly as before (`MSVC` implies
`WIN32`, never `MINGW`), and the licensing test keeps its MSVC assertions verbatim.

## 6. Refreshing the lock

```powershell
curl -o ucrt64.db https://repo.msys2.org/mingw/ucrt64/ucrt64.db
python scripts\msys2-resolve-lock.py ucrt64.db --out packaging\mingw\msys2-ucrt64-toolchain-lock.json gcc cmake ninja
```

Reading a zstd database needs Python 3.14, `backports.zstd` or `zstandard`. Review the diff of the
lock before installing anything from it.

## 7. Clean, repeatable build (Phase 4-I.1)

Everything below runs from a normal (non-elevated) Windows PowerShell 5.1 in the experimental
checkout. No step changes PATH, the registry, services or any setting outside the folders named;
every tool is called by its full path with a process-local PATH, and Visual Studio folders are
removed from that PATH (the scripts stop if `cl.exe` is reachable).

| Step | Command | Result / check | Time here |
| --- | --- | --- | --- |
| 1. Toolchain | `.\scripts\install-mingw-toolchain.ps1 -OwnerApproved` | MSYS2 base (SHA-256 pinned) + exactly the 51 locked packages: SHA-256 per file, MSYS2 GPG signature per file (`gpg --status-fd`, VALIDSIG + full trust), `pacman -U --print` dry run must equal the lock, `pacman -Q` compared with the lock afterwards, `gpg-agent` stopped | ≈5 min |
| 2. Qt Base | `.\scripts\build-qtbase-mingw.ps1 -OwnerApproved -Jobs 16` | Qt 6.12.0 from the shipped, unmodified tarball (SHA-256 checked) into `C:\Dev\nova-toolchains\qt-6.12.0-ucrt64-gcc16-r2`, installed with `--strip`; writes `nova-qtbase-build.json` (options, compiler, the 136 MSYS2 packages as plain strings) and Qt's SPDX SBOM | 8.9 min |
| 3. Preflight | `.\scripts\check-mingw-toolchain.ps1` (set `NOVA_MCP_SDK_PYTHON` first) | GCC 16.2.0, CMake 4.4.5, Ninja 1.13.2, `x86_64-w64-mingw32`, UCRT runtime, Qt 6.12.0 with private modules, `Qt6Core.dll` imports `libstdc++-6.dll` and no `msvcrt.dll`/Visual C++ DLL | seconds |
| 4. Build + 43 suites | `.\scripts\build-mingw.ps1 -Test -Makensis <makensis.exe> -ReleaseDependencies <dir with qtbase tarball, qt-source.json, qt-notices>` | `build\mingw-ucrt64-source-Release`; compiler jobs default to min(logical CPUs, RAM GB / 2) (`-BuildJobs`) | ≈6 min |
| 5. Packages | commit first, rebuild (step 4 without `-Test`), then `python -I scripts\package-mingw-experimental.py` | `build\phase4i-distribution\…-mingw-experimental-<rev>\`: payload, ZIP (= payload byte for byte), NSIS installer, source ZIP, `payload-check.json` (13 checks), `release-manifest.json`, `SHA256SUMS.txt` | ≈2 min |
| 6. Installer acceptance | `python -I scripts\verify-phase4g-installer.py --distribution phase4i-distribution --candidate <label> --build mingw-ucrt64-source-Release` (Qt and toolchain `bin` on PATH for the UI-automation client only) | 47 checks, including the owner's existing installation and data unchanged | ≈8 min |

Notes from the first execution:

- Pass paths to CMake with forward slashes: CMake writes `CMAKE_RC_COMPILER` into
  `CMakeRCCompiler.cmake` verbatim, and a backslash path fails to parse (`Invalid character escape`).
- An unlimited `cmake --build --parallel` (logical CPUs + 2 = 30 jobs) ran `cc1plus` out of memory on
  this 32 GB machine, for Qt and for Nova; both scripts now bound the job count.
- Windows PowerShell 5.1 turns native stderr lines into errors under `ErrorActionPreference=Stop`
  and serialises `Get-Content` lines with provider metadata (including the user's home folder) in
  `ConvertTo-Json`; the scripts judge native tools by exit code and convert lines to plain strings.
- Packaging inputs for a worktree without `.tools`: `-Makensis` / `NOVA_MAKENSIS` and
  `-ReleaseDependencies` / `NOVA_RELEASE_DEPENDENCIES` (read only).
- Disk: MSYS2 + packages ≈0.9 GB, Qt prefix 104 MB, Qt build tree 3.5 GB (deletable by the owner),
  Nova build tree ≈1.5 GB.
