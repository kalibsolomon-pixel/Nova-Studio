# Nova Studio — a native resource-pack editor for Minecraft Java Edition

<img src="assets/branding/nova-studio-mark.png" alt="Nova Studio geometric starburst logo" width="112" height="112">

> **NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT.**
>
> Nova Studio is an independent project. It is developed and published by the Nova Studio
> contributors, who are solely responsible for it. "Minecraft" is a trademark of Mojang AB and
> Microsoft Corporation. Nova Studio contains no Minecraft game assets.

Nova Studio is a Windows 11 desktop editor for Minecraft Java Edition resource packs. It is written
in C++20 with Qt Widgets and is free software under the GNU GPL, version 3 or later.

**Status: preview.** Version `0.1.0-preview.1` is the first preview. Keep backups of resource packs
you care about. See [known limitations](docs/release/KNOWN_LIMITATIONS.md).

## Features

- **Textures:** layers, selections, brushes, shapes, transforms, colour and palette tools,
  adjustments, filters, directional shading, procedural generators and batch processing. Layered
  work is kept in an editable Nova document. **Update Minecraft PNG** writes the PNG the game reads.
- **Animated textures:** a timeline for source frames and playback order, with `.png.mcmeta` import
  and export.
- **Models:** block and item models, blockstates and item definitions, with a 3D preview, UV
  editing, item display transforms and resource validation.
- **Test in Minecraft:** prepares a separate, Nova-managed Minecraft Java Edition 1.21.11 runtime
  with Fabric when you ask for it. It does not touch your normal Minecraft installation and never
  asks for account credentials.
- **Search (Ctrl+K):** commands, tools, settings, workspaces, panels and textures.
- **AI agents (MCP):** `nova-mcp.exe` is a local Model Context Protocol server. Agents get no
  access until you allow it for one session and one project.
- **File safety:** atomic saves, undo history, external-change review and crash recovery.

## Documentation

| Guide | Contents |
| --- | --- |
| [Installation](docs/release/INSTALLATION.md) | Installer, portable ZIP, upgrade and uninstall |
| [First run](docs/release/FIRST_RUN.md) | Opening a pack and the basic workflow |
| [Search](docs/release/SEARCH.md) | The Ctrl+K search box |
| [MCP setup](docs/release/MCP_SETUP.md) | Connecting Claude Desktop, Claude Code, Codex or other MCP clients |
| [Known limitations](docs/release/KNOWN_LIMITATIONS.md) | What is not tested or not supported yet |
| [Release notes](docs/release/RELEASE_NOTES.md) | 0.1.0-preview.1 |
| [Building from source](docs/release/BUILDING.md) | Toolchain, build, tests and packaging |

## Building

You need Windows 11 x64, Visual Studio 2026 (or its Build Tools) with the C++ workload, Qt Base
6.12.0 for MSVC x64 and Python 3.12. [BUILDING.md](docs/release/BUILDING.md) has the exact
versions.

```powershell
python -m pip install -r scripts/requirements-lock.txt
python scripts/install-qt.py                      # Qt Base 6.12.0 into .tools\qt
./scripts/build-studio.ps1 -Configuration Release # build and run the tests
./scripts/run-studio.ps1                          # start the build
```

The programs are `build\studio-release\src\nova-studio.exe` and `nova-mcp.exe` next to it.
`scripts/run-ctest.ps1 -BuildName studio-release` reruns the tests.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | Application (`nova-studio.exe`) and MCP bridge (`nova-mcp.exe`) |
| `tests/` | Qt Test suites, run by CTest |
| `cmake/`, `packaging/` | Install layout, NSIS installer, ZIP and package text |
| `docs/release/` | User documentation shipped in every package |
| `assets/` | Nova Studio icon artwork and the Gradle wrapper used by the test runtime |
| `fixtures/` | Original CC0 test packs |
| `scripts/` | Build, packaging, licensing checks and development helpers |
| `prototypes/` | The original Rust/egui and Qt prototypes. Not part of the application |

## License

Copyright (C) 2026 Nova Studio contributors.

Nova Studio is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version (`GPL-3.0-or-later`). It is distributed WITHOUT ANY
WARRANTY. The license text is [LICENSE](LICENSE).

Some material keeps its own license: the embedded Gradle wrapper (Apache-2.0), the CC0 test
fixtures, and Qt and the Microsoft Visual C++ runtime, which ship with the binaries. See
[COPYRIGHT.md](COPYRIGHT.md) and [THIRD_PARTY_NOTICES.md](packaging/THIRD_PARTY_NOTICES.md).

## Source code for binary releases

Every binary release includes the complete corresponding source code of that exact build, in the
package's `source` folder and as a separate `nova-studio-<version>-source.zip` download. In the
application, **Help › Licenses and Source Code** shows where it is. The source revision of a build is
shown in **Help › About Nova Studio**.
