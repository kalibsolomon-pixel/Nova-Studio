# Nova Studio 0.1.0-preview.1 — release notes

**Preview release.** This is the first public preview of Nova Studio for Windows. It is suitable
for trying the editor and giving feedback. Keep backups of resource packs you care about.

## What Nova Studio does

- Edits Minecraft Java Edition resource-pack textures with layers, selections, transforms,
  filters, palettes and pixel-accurate tools.
- Edits animated textures on a timeline and exports Minecraft `.mcmeta` animations.
- Edits block and item models and their UVs, with a live 3D preview and display transforms.
- Validates a pack and lists problems with the assets that cause them.
- Tests the pack in a Nova-managed Minecraft Java Edition 1.21.11 runtime (Fabric), downloaded on
  your request from each component's publisher.
- Saves atomically, keeps undo history, and recovers unsaved work after a crash.

## New in this release

- **Windows installer.** A per-user installer (no administrator rights) with a Start menu entry
  and an uninstaller that keeps your projects, settings and recovery data. A portable ZIP of the
  same files is also available.
- **Search (Ctrl+K).** One search box for commands, tools, settings, workspaces, panels and
  textures. It finds things by name, alias or shortcut and explains when a command is unavailable.
  The previous command palette shortcut, Ctrl+Shift+P, still works.
- **AI agent setup (MCP).** Resources › MCP Integration › Agent Setup adds Nova Studio to
  Claude Desktop, Claude Code or Codex for you, or shows the configuration to copy for other
  MCP clients. Agents still need your explicit permission for each session and project.
- **MCP diagnostics.** A local check that the bridge starts and declares its 26 tools.

## Requirements

- Windows 11, 64-bit. Other Windows versions are not tested.
- OpenGL 2.1 or later. Tested on an AMD graphics driver only.
- About 220 MB of disk space for the installation.
- Internet access only if you use Test in Minecraft (to download the runtime once).

## Installing

See [INSTALLATION.md](INSTALLATION.md). The installer is **not code-signed**, so Windows SmartScreen
may say it does not recognize the app. Choose **More info**, check that the file name and the
SHA-256 checksum match the published ones, then choose **Run anyway**. Do not turn off Windows
security features to install Nova Studio.

## Upgrading from internal builds

Earlier internal candidates were numbered 0.3.0-rc.1 and used a different installer. If one is
installed, uninstall it from Settings › Apps first. Its settings and projects are kept and are
used by this release.

## Known limitations

See [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md).

## Licensing

Nova Studio is free software under the GNU General Public License version 3 or later
(GPL-3.0-or-later). The license is `LICENSE.txt`; `licenses/COPYRIGHT.md` says what it covers.
Third-party notices are in `licenses/THIRD_PARTY_NOTICES.md`, and **Help › Licenses and Source
Code** shows all of them.

The complete corresponding source code of this build is in `source/` and is published next to
the installer as `nova-studio-0.1.0-preview.1-source.zip`. `docs/BUILDING.md` explains how to
build it.
