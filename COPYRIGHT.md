# Nova Studio copyright and license

Nova Studio — a native Windows editor for Minecraft Java Edition resource packs.

Copyright (C) 2026 Nova Studio contributors

This program is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License along with this program
(`LICENSE` in the source code, `LICENSE.txt` in an installed or portable copy). If not, see
<https://www.gnu.org/licenses/>.

SPDX-License-Identifier: GPL-3.0-or-later

## What this license covers

The GPL-3.0-or-later grant above covers the original Nova Studio work in this repository: the
application and MCP bridge source (`src/`), tests (`tests/`), build and packaging files
(`CMakeLists.txt`, `cmake/`, `packaging/`, `scripts/`), the Phase 1 prototypes (`prototypes/`),
the Nova Studio icon artwork (`assets/branding/`) and the documentation (`README.md`, `docs/`),
**except** for the material listed below.

## What it does not cover

These keep their own licenses. Nova Studio's license does not change them.

| Material | Where | License / status |
| --- | --- | --- |
| Gradle wrapper 9.8.2 (unmodified jar, embedded in `nova-studio.exe`) | `assets/fabric-runtime/gradle-wrapper-9.8.2.jar` | Apache-2.0 (vendor: Gradle, Inc., per the jar manifest; license text inside the jar at `META-INF/LICENSE`) |
| Test fixture artwork and model JSON | `fixtures/phase-2c5-models/`, `fixtures/phase-2c6-models/`, `fixtures/phase-2c7-authoring/` | CC0-1.0, as stated in each folder's `LICENSE.txt` |
| Qt, the Microsoft Visual C++ runtime, and their third-party code | Not in this repository; shipped with the binaries | Their own licenses: see `packaging/THIRD_PARTY_NOTICES.md` |
| Screenshots of the Minecraft Java Edition client taken during development testing | Not in this repository or in any package. Kept only in the private development history | Contain Mojang's game artwork and interface. Not licensed by Nova Studio and not covered by the GPL |

## Minecraft

NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT.

Nova Studio is an independent tool for making Minecraft Java Edition resource packs. It is developed
and published by the Nova Studio contributors, who are solely responsible for it. "Minecraft" is a
trademark of Mojang AB and Microsoft Corporation. Nova Studio contains no Minecraft game assets.

## Source code for binary releases

Every Nova Studio binary release is accompanied by the complete corresponding source code for that
exact build: inside the package (`source/`) and as a separate download next to the installer.
See `docs/release/BUILDING.md`.
