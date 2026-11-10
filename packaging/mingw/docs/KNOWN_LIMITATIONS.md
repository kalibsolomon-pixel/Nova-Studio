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
- Each authorization serves one agent session. Reconnecting an agent needs a new Allow.
- Uninstalling Nova Studio does not remove agents' `nova-studio` entries.
- **Agent Session (ACP) is experimental and off by default.** Turn it on in **Edit › Preferences ›
  Experimental**. It was tested with a scripted test agent, and with OpenCode 1.19.5
  (`opencode acp`) for start-up and attaching Nova's tools only, with no sign-in and no message
  sent. No conversation with a real AI model has been tested, and Claude Code, Codex and other
  agents have not been tested with it. Nova controls only its own tools: an agent's built-in file,
  shell or web tools follow that agent's own configuration, and Nova can neither see nor limit them.
  The window shows the agent's replies and reasoning but does not list its tool calls. Changing
  from read-only to editing restarts the agent session.
- MCP asset creation never creates folders; an agent must ask you to create a missing folder.

## Search

- Asset results include textures and model JSON files that Nova Studio can open. Other files
  (sounds, blockstates, language files) are not listed.
- Search lists menu commands as the menus show them. A few commands are always enabled in the menus
  and do nothing when there is nothing to act on (for example Undo with no document); Search shows
  them the same way.

## Licensing

- Nova Studio is GPL-3.0-or-later. Qt, GCC runtime libraries, winpthreads and the Gradle
  wrapper retain their own licenses; see `licenses/THIRD_PARTY_NOTICES.md`.
- Clean Windows installation and owner visual review remain separate release gates.
- Nova Studio uses a private Qt API, so replacement Qt libraries must be Qt 6.12.0 builds.
