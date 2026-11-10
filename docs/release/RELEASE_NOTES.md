# Nova Studio 0.1.0-preview.1

The first public **preview**, distributed as GitHub prerelease `v0.1.0-preview.1`.
Keep backups of resource packs you care about.

The owner authorized source, tag and binary publication on 2026-11-10 while deferring personal
visual and physical-presentation review. This authorization is not personal testing or legal sign-off.

**NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT.**
Nova Studio is developed and published by the Nova Studio contributors, who are solely responsible
for it. Minecraft is a trademark of Mojang AB and Microsoft Corporation. No Minecraft game assets
are bundled in the source or packages; included test fixtures are original synthetic material.

## Downloads and platform

- Windows 11 x64; tested on one development PC with an AMD graphics driver. OpenGL 2.1 or later.
- Installer: `nova-studio-0.1.0-preview.1-windows-x64-mingw.exe` (per-user, no administrator rights).
- Portable: `nova-studio-0.1.0-preview.1-windows-x64-mingw.zip`.
- Corresponding Nova source: `nova-studio-0.1.0-preview.1-source.zip`, also inside both packages.
- Exact GCC runtime source sidecar: `mingw-w64-gcc-16.2.0-6.src.tar.zst`, with
  `gcc-runtime-source.json`. Separate MinGW CRT/winpthreads source packages and signatures accompany
  the release. Matching Qt source, patch, configuration and notices are inside both packages.
- `SHA256SUMS.txt` and `release-manifest.json` identify the selected artifacts and committed source.

**Unsigned:** no authorized signing certificate was available. The installer and programs have no
code signature; no SmartScreen trust or reputation is claimed. Verify downloads against the checksums.
See [installation instructions](INSTALLATION.md).

The distribution toolchain is MSYS2 UCRT64 GCC 16.2.0-6, CMake 4.4.5, Ninja 1.13.2 and self-built
Qt Base 6.12.0 with the supplied PCRE2 patch; NSIS 3.13 builds the installer. The separate regression
toolchain is MSVC 19.51 (Build Tools 18.10, toolset 14.51, Windows SDK 10.0.26100.0), Qt 6.12.0,
CMake 4.3.1 and Ninja 1.13.2. MSVC binaries and runtimes are not distributed in this release.

## Included features

- Layered pixel editing, selections, transforms, filters, palette tools and animated texture timelines.
- Model JSON and UV editing, resource validation and a live approximate 3D preview.
- Atomic saves, undo, external-change handling, and recovery that distinguishes earlier sessions
  from the current session's checkpoints. Ctrl+S saves editable Nova work; **Update Minecraft PNG**
  writes the game-facing texture. Review this distinction before using an existing pack.
- Optional vanilla texture baseline, project overrides, reset-to-vanilla and cached-texture tools.
- Search with Ctrl+K, refined editor panels and tool controls, and generated/handheld item preview.
- A managed Minecraft Java Edition 1.21.11 Fabric test runtime, downloaded only on request.
- MCP setup, read-only queries, consent-controlled asset/edit/save operations, compact texture tools,
  diagnostics and a 39-tool bridge self-test. Each agent session/project needs explicit authorization.
- Agent Session (ACP), **experimental and off by default**, enabled through Preferences > Experimental.

## Qualification criteria and accepted limitations

The published candidate must pass both complete 67-test GCC and MSVC CTest suites on its exact
committed source, plus package, installer and portable checks. Actual batch results, optional skips,
failures and retries are recorded in the release qualification summary accompanying these notes.

- **Windows acceptance uses the owner-approved composite dependency-isolation test on this host.**
  Installer install/launch/repair/uninstall, portable extraction/launch, Windows-only PATH, dependency
  closure, actual loaded Qt/GCC modules and packaged plugins form that criterion. **No pristine
  Windows guest was tested.** Other Windows versions, GPUs and Windows on Arm remain unqualified.
- The owner accepts the earlier approximately **150-second 50k workload** as a temporary exception
  to the previous 90-second threshold. The original threshold did not pass. Optional large-fixture
  slots do not remeasure that workload; no new performance result is inferred from their skips.
- Owner visual and physical-presentation review, including Phase 1 visuals and personal review of
  the Ctrl+S workflow, is deferred. Automated UI checks do not complete those reviews.
- Historical intermittent incidents include editor-gui, tool-placement, texture-pixels, save-drafts,
  MCP, ACP and Fabric tests. Fabric supervision and fixture synchronization received reviewed fixes;
  passing final suites do not prove all historical failures fixed. The owner accepts proceeding
  with this disclosed history. Each failed or interrupted final attempt is retained separately.
- Seven intentional optional QtTest slots require external fixtures: two large-fixture slots,
  two vanilla-cache slots, real-log replay, the MCP benchmark and search screenshots. Optional
  slots may skip; all 67 required CTest entries on each compiler must pass.

## Known issues and scope

- The model preview approximates Minecraft. Generated/handheld items use thin alpha-contoured layers
  and resolved transforms. Missing sprites, sprites over 1024 pixels per side or contours beyond
  4096 spans use an explicitly named flat approximation. Missing model data retains a fallback.
  Full Minecraft rendering parity is not claimed; verify results in-game.
- The vanilla cache contains textures only, not vanilla model JSON, blockstates, item definitions
  or a complete resource stack. Project/configured stack resources are required for those previews.
  User-downloaded vanilla assets stay in a per-user cache and enter a pack only after edit/save.
- Native Nova documents retain editable layers; game PNG/MCMeta exports have their own format limits.
  Sounds, language files and other project formats are not general-purpose editors in this preview;
  asset search lists supported textures and model JSON.
- Test in Minecraft targets Java 1.21.11 with Fabric. Production launcher hand-off is not included.
- ACP was tested with a scripted agent. OpenCode 1.19.5 startup and bridge attachment were exercised
  without sign-in or a message. **Real provider conversations and Claude Code/Codex ACP adapters
  remain unqualified.** MCP setup-format/CLI checks do not establish universal agent compatibility;
  a live Claude Desktop conversation was not tested. Agent-built-in shell/file/web tools are outside
  Nova's permission controls. ACP shows replies/reasoning, not a tool-call transcript.
- Screen readers have not been tested end to end. Installation paths are limited to 160 characters;
  the NSIS installer stub is x86 while its installed payload is x64. Uninstall preserves user data
  and does not remove agent client configuration entries.

See [known limitations](KNOWN_LIMITATIONS.md), [MCP setup](MCP_SETUP.md),
[texture tools](MCP_TEXTURE_TOOLS.md), [asset tools](MCP_ASSET_TOOLS.md) and
[source correspondence](SOURCE_CORRESPONDENCE.md).

## License and corresponding source

Nova Studio is GPL-3.0-or-later. Help > Licenses and Source Code shows the notices and the public
repository. Both packages contain LICENSE.txt, licenses/COPYRIGHT.md, third-party license texts,
the corresponding Nova source archive and build instructions. Qt Base 6.12.0 is dynamically linked;
its exact upstream archive, Nova patch, configuration and replacement instructions are supplied.
GCC Runtime Library Exception, MinGW, Apache and other component notices retain their own scope.
Keep exact source materials with redistributed binaries. These engineering checks do not decide
legal sufficiency, contributor scope, trademark questions or Microsoft Build Tools terms.
