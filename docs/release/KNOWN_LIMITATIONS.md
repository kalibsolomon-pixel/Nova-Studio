# Known limitations (0.1.2)

## Platform

- Tested only on Windows 11 x64 on one development PC with an AMD graphics driver. Not yet tested
  on a clean Windows installation, other GPUs, Windows 10 or Windows on Arm.
- The installer and programs are not code-signed. Windows SmartScreen shows a warning.
- The installer program itself is a 32-bit (x86) NSIS executable; everything it installs is 64-bit.
- The installation folder path can be at most 160 characters, because some license files have long
  names and the installer does not use Windows long paths.
- Screen readers (Narrator, NVDA) have not been tested end to end.

## Editing and Minecraft

- The 3D preview has fixed hand, head, ground and frame poses. In-game animation, stack offsets,
  lighting from the world lightmap, atlas mipmaps, fog, shader packs and enchantment glint can
  change the result. The Placed context samples a local light model for air and solid cubes.
- Vanilla parents such as `item/generated` resolve only when the target version has a baseline
  cache entry with model data (the default after a fresh download or **Download / Repair
  Baseline…**), or when a verified vanilla folder is configured in the Model panel's resource
  stack; a texture-only cache from an older Nova keeps working and shows the update hint. The
  cache never enters the project or the export.
- Generated items use per-frame alpha edges and a one-pixel thickness. Missing sprites, sprites
  above 1024 pixels per side and contours over the 4096-element budget use flat layers.
- Special item previews cover chests, shulker boxes, beds, common skulls and heads, plain shields
  and inactive conduits. Runtime banner patterns, player skins, decorated pots, dragon and piglin
  heads, and other special renderers remain incomplete. See the [model preview guide](MODEL_PREVIEW.md).
- Test in Minecraft targets Minecraft Java Edition 1.21.11 with Fabric and needs a one-time
  download. The production Minecraft launcher hand-off is not included in this release.
- Reloading from Nova uses a small Nova companion mod that the test runtime compiles when it is
  prepared, so a runtime prepared by an older Nova prepares again once. A game started before that
  shows the F3+T hint instead. Reloading and Compare Icons were checked in Minecraft 1.21.11 at
  GUI scale 3 (tests/companion_live_probe.cpp): the 16 reference icons matched within one colour step.
- Inventory icons match Minecraft 1.21.11 for 1,485 of 1,504 vanilla items at GUI scale 3 (1,493
  at scale 1); the rest are interpolated animations captured between frames and two bed pixels. See
  the [model preview guide](MODEL_PREVIEW.md).
- Minecraft warnings link to files Nova can match in the open pack. Warnings without a matching
  file are labelled **Not from this pack**. A warning with multiple matches opens the first
  matching file and lists the rest in its details.
- Live sync is off by default and needs Nova's running Fabric test client with the companion
  active. It responds to saved pack PNGs, animation metadata, model, blockstate and item
  definitions, and pack metadata. If a document has pending Minecraft output, save or update
  it before Nova can sync the pack.
- Compare Icons uses the game's current GUI scale and the packs the game has loaded, so sync and
  reload first. Icons the game draws with an enchantment glint or animation are compared as captured
  in one frame. The game's images stay in Nova's test folder (`nova-companion/icons-*`).

## AI agents (MCP)

- Automatic setup was verified with Claude Code 2.1.317 and Codex CLI 0.166.0-alpha.6 using test
  configuration folders, and with Claude Desktop's configuration format on a copy of its file.
  A live Claude Desktop session was not tested.
- One agent is connected at a time. An authorization stays on until you revoke it, switch
  projects or close Nova Studio; a disconnected agent reconnects with a freshly offered code.
- Uninstalling Nova Studio does not remove agents' `nova-studio` entries.
- **Agent Session (ACP).** Automated tests use a scripted agent. A previous
  smoke test verified OpenCode 1.19.5 startup and MCP attachment with an isolated configuration.
  Antigravity CLI 1.5.13 (`agy acp`) answered Nova's ACP handshake; a signed-in session with it
  was not tested.
  Live conversations and sign-in with real agents remain unverified. Plans, reasoning,
  tool details, and permission options depend on what the agent reports. Its built-in file,
  shell, and web tools follow that agent’s configuration. Changing from read-only to editing
  restarts the agent; an agent that supports loading sessions continues the same conversation,
  other agents start a new one. Modes, settings, commands, and usage appear only when the agent
  reports them.
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
