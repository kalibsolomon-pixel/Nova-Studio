# Known limitations (0.1.0-preview.1)

## Platform

- Tested only on Windows 11 x64 on one development PC with an AMD graphics driver. Not yet tested
  on a clean Windows installation, other GPUs, Windows 10 or Windows on Arm.
- The installer and programs are not code-signed. Windows SmartScreen shows a warning.
- The installer program itself is a 32-bit (x86) NSIS executable; everything it installs is 64-bit.
- The installation folder path can be at most 160 characters, because some license files have long
  names and the installer does not use Windows long paths.
- Screen readers (Narrator, NVDA) have not been tested end to end.

## Editing and Minecraft

- The 3D preview approximates Minecraft's rendering. Check final results in the game.
- Vanilla parents such as `item/generated` resolve only when the target version has a baseline
  cache entry with model data (the default after a fresh download or **Download / Repair
  Baseline…**), or when a verified vanilla folder is configured in the Model panel's resource
  stack; a texture-only cache from an older Nova keeps working and shows the update hint. The
  cache never enters the project or the export.
- Common generated and handheld item models use thin, alpha-contoured texture layers and
  resolved item display transforms from project models and explicitly configured resource
  stacks. Missing sprites, sprites above 1024 pixels per side, or contours beyond the
  4096-span budget use a named flat approximation. Missing model data retains the explicit
  preview fallback. This does not expand the vanilla cache or provide full rendering parity.
- Test in Minecraft targets Minecraft Java Edition 1.21.11 with Fabric and needs a one-time
  download. The production Minecraft launcher hand-off is not part of this preview.

## AI agents (MCP)

- Automatic setup was verified with Claude Code 2.1.317 and Codex CLI 0.166.0-alpha.6 using test
  configuration folders, and with Claude Desktop's configuration format on a copy of its file.
  A live Claude Desktop session was not tested.
- One agent is connected at a time. An authorization stays on until you revoke it, switch
  projects or close Nova Studio; a disconnected agent reconnects with a freshly offered code.
- Uninstalling Nova Studio does not remove agents' `nova-studio` entries.
- **Agent Session (ACP) is experimental.** Automated tests use a scripted agent. A previous
  smoke test verified OpenCode 1.19.5 startup and MCP attachment with an isolated configuration.
  Live conversations and sign-in with real agents remain unverified. Plans, reasoning,
  tool details, and permission options depend on what the agent reports. Its built-in file,
  shell, and web tools follow that agent’s configuration. Changing from read-only to editing
  restarts the agent session.
- With editing permission, `nova.pack.bootstrap` can initialize a folder with Java 1.21.11 metadata and standard
  Minecraft texture folders. Other asset creation requires the destination folder to exist.

## Search

- Asset results include textures and model JSON files that Nova Studio can open. Other files
  (sounds, blockstates, language files) are not listed.
- Search lists menu commands as the menus show them. A few commands are always enabled in the menus
  and do nothing when there is nothing to act on (for example Undo with no document); Search shows
  them the same way.

## Licensing

- Nova Studio is licensed under GPL-3.0-or-later (`LICENSE.txt`). Qt, the selected compiler runtime
  and the Gradle wrapper keep their own licenses (`licenses\THIRD_PARTY_NOTICES.md`). The GCC
  candidate ships GCC/MinGW notices and a separate GCC source sidecar; MSVC packages instead carry
  Microsoft runtime terms in `licenses\MICROSOFT-VC-RUNTIME-TERMS.txt`.
- Nova Studio uses a private Qt API, so replacement Qt libraries must be Qt 6.12.0 builds.
