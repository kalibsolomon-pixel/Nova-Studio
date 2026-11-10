# Source corresponding to the public preview

Public repository: https://github.com/kalibsolomon-pixel/Nova-Studio .
The annotated tag **v0.1.0-preview.1** identifies the exact committed release source.
The release-manifest.json accompanying downloads records its full source_revision and source_tree;
both executables embed that committed revision. The matching Nova source ZIP is also inside both
packages and includes .nova-source-revision. Verify every artifact with SHA256SUMS.txt.

```powershell
git clone https://github.com/kalibsolomon-pixel/Nova-Studio.git
cd Nova-Studio
git checkout --detach v0.1.0-preview.1
git rev-parse HEAD 'HEAD^{tree}'
```

GitHub-generated archives have different ZIP metadata and no extra revision marker. The attached
corresponding-source ZIP is verified blob by blob against the release commit. Rebuilding it is a
new artifact; bit-identical PE output is not claimed. Earlier internal candidates and their
snapshot hashes are preserved privately and are not the downloads for this release.

## Qt source, modifications and build information

The candidate dynamically links its self-built Qt Base 6.12.0. The exact source archive is
`licenses/qtbase-everywhere-src-6.12.0.tar.xz`, SHA-256
`a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307`.
The official matching [download](https://download.qt.io/official_releases/qt/6.12/6.12.0/submodules/qtbase-everywhere-src-6.12.0.tar.xz)
and its `.sha256` endpoint were verified on 2026-11-10 UTC. This large archive is not stored in Git.

Nova's exact patch is in `packaging/mingw/qtbase-6.12.0-pcre2-jit-stack.patch`, SHA-256
`c366956a0439e1ccebf3305d1c5142aaf7dce26480805cb1512edd3063cb4417`.
The candidate carries the patch, configure summary, compiler/options/package record and
`nova-qtbase-build.json` in `licenses/qt-build/`. Its recorded modification/build date is
2026-11-05 UTC, based on `built_at` 2026-11-05T05:57:59Z. See
[the build guide](../phase-4i/MINGW_BUILD_GUIDE.md) and
[GCC component notices](../../packaging/mingw/THIRD_PARTY_NOTICES.md).
Qt replacement must use compatible MinGW-w64 and Qt 6.12.0 because Nova uses a private Qt API;
Nova adds no signature or integrity check blocking DLL replacement.

## GCC and MinGW source

The exact GCC sidecar is `mingw-w64-gcc-16.2.0-6.src.tar.zst`, SHA-256
`a901d8afb78438abcff9ed020cd9aa9431db76de1992171c2454c3c07ad289e8` (107,229,138 bytes).
The candidate's `gcc-runtime-source.json` has SHA-256
`f3fed3d9aac26b1b7ad9cbe4433581be961ac80ce5bcc412b2fffbf88c620370`.
An independently downloaded official copy is identical. Its detached MSYS2 signature, all four
runtime/CRT binary-package signatures and GNU's embedded GCC source signature verify.
The PKGBUILD matches the binary `.BUILDINFO`; all 20 declared GCC source/patch checks pass.

[RUNTIME_SOURCE_VERIFICATION.json](RUNTIME_SOURCE_VERIFICATION.json) records exact official
URLs, hashes, package names, versions, release numbers, architecture and signing fingerprints.
The shipped DLLs come from `mingw-w64-ucrt-x86_64-libgcc`, `-libstdc++` (16.2.0-6), and
`-libwinpthread` (14.0.0.r598.ge3d18511f-1). Packages are labelled `any` within MSYS2's UCRT64
repository; the DLLs are PE x86-64. `gcc-libs` is a metapackage, not the DLL's package identity.

The GCC sidecar covers GCC, its recipes and patches. MinGW CRT and winpthreads have separate,
signed official source packages documented in the JSON. Their recipes match the binary build
records, and their bundled Git source at `e3d18511f9759d455cae20c34703eea4a0723cf3` passes
Git integrity checks and makepkg's archive checksum with line-ending conversion disabled.
Do not describe the GCC sidecar as containing all MinGW sources.

Keep the exact source materials and notices available alongside binary
distributions. No large upstream archives, source sidecars or binaries were added to this Git
repository; official verified URLs and hashes are recorded instead. Verification establishes
correspondence and signature validity, not a reproducible rebuild or legal sufficiency.

## Notices and scope

Installed users can open **Help > Licenses and Source Code** for embedded notices and local-source
instructions. The candidate installs `LICENSE.txt`, `licenses/COPYRIGHT.md`,
`licenses/THIRD_PARTY_NOTICES.md`, component license texts, `docs/BUILDING.md` and `source/`.
The public preview compiles the public repository URL into Help; the local source archive remains included.

The bundled/dynamically linked inventory is in the GCC notices: Nova GPL-3.0-or-later; Qt/LGPL
and its SBOM components; GCC runtime with Runtime Library Exception 3.1; MinGW CRT/winpthreads;
Material Symbols Outlined and Gradle wrapper under Apache-2.0; NSIS installer code under zlib/libpng.
Windows UCRT is an OS dependency and is not shipped. MSVC is a separate regression-test layout;
this candidate contains no Microsoft VC runtime DLLs.

Nova packages and default exports contain no Minecraft assets. Source fixtures in the
`minecraft` namespace are original synthetic CC0 test material. Optional user-downloaded vanilla
textures stay in a local cache and enter a project/export only after an explicit edit/save.
The [review index](REVIEW_INDEX.md) and [checklist](RELEASE_CHECKLIST.md) preserve outstanding
owner decisions and source/license questions. No legal approval is asserted.
