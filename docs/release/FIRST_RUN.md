# First run

## Open a resource pack

- **File › Open Resource Pack** (Ctrl+Shift+O) and choose the folder that contains `pack.mcmeta`,
  or **File › New Resource Pack** to create one for a chosen Minecraft Java version.
- The **Assets** panel lists the pack's files. Double-click a texture, or select it and press
  Enter, to open it. Model JSON opens in the **Models / UV** panel.

## Find anything: Ctrl+K

Press **Ctrl+K** (or **Help › Search**) and type what you want: `undo`, `layers`, `uv`,
`export animation`, `test in minecraft`, a setting such as `theme`, or a texture name. Use the arrow
keys and Enter. See [SEARCH.md](SEARCH.md).

## Workspaces

The workspace switcher in the application bar arranges the panels for **Essentials**, **Texture
Editing**, **Animation**, **Model Editing** and **UV Editing**. Switching workspaces never changes
your documents.

## Edit and save

- **B** pencil, **E** eraser, **Ctrl+Z / Ctrl+Y** undo and redo.
- **Ctrl+S** saves. Layered textures and animations keep an editable Nova document beside the
  pack; **Ctrl+Alt+E** (Update Minecraft PNG) writes the PNG that Minecraft reads.
- If Nova Studio closes unexpectedly, it offers to recover unsaved work the next time you open
  the same pack.

## The resource-pack workflow

1. Open or create the pack.
2. Edit textures, animations and models. Nova Studio saves the Minecraft files in place.
3. **Resources › Validate Resources** (F8) lists problems such as missing textures or unsupported
   fields.
4. **Resources › Test in Minecraft…** (Ctrl+Shift+M) prepares a managed Minecraft Java Edition
   1.21.11 runtime the first time (a download you confirm), copies the pack into it and starts the
   game. **Sync Test Pack** (Ctrl+Alt+M) updates the running game after more edits. Nova Studio
   never asks for or stores your Microsoft or Minecraft account credentials.
5. **File › Export Minecraft Pack…** writes a clean copy of the pack to a new folder, ready to zip
   and share.

## AI agents

To let an AI agent work with the open pack, see [MCP_SETUP.md](MCP_SETUP.md).
