# Nova Studio 0.1.1 — release notes

Nova Studio is a Windows editor for Minecraft Java Edition resource packs. Keep backups of packs
you care about. See [known limitations](KNOWN_LIMITATIONS.md) for the supported scope and test coverage.

**NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT.**
Nova Studio is developed and published by the Nova Studio contributors. Minecraft is a trademark
of Mojang AB and Microsoft Corporation. Nova Studio contains no Minecraft game assets.

## What's new in 0.1.1

Editing tools
- Wrap-around painting (grid toggle on the paint options) continues strokes across the opposite
  edge for seamless tiling textures. A Pixel perfect option for the 1 px Pencil removes the extra
  corner pixel where a freehand line turns.
- Effects › Repeat Last Effect (Ctrl+F) applies the last effect again; Last Effect Again
  (Ctrl+Alt+F) reopens it with its settings.
- `[` and `]` change the active tool's main size: brush size in steps that grow with the brush,
  shape outline thickness, or fill and colour-selection tolerance. Keys 1–9 and 0 set the active
  tool's opacity to 10–100% while the document has focus (Edit › Tool Opacity).
- Alt-click picks a colour with Fill and Gradient too; Shift-drag snaps a gradient to 45°. Fill,
  Gradient, Eyedropper, Magic Wand and Colour Range outline the pixel under the cursor.
- The options bar shows Radius, Sides and Inner only for the shapes that use them, keeps shared
  values (such as fill and gradient opacity) in step across pages, and has tooltips for every
  control. The status bar always shows the active tool's hint, and more tool settings are kept
  between sessions.

MCP for agents
- Calls sent while another runs now wait in a short queue instead of failing as busy; request
  validation is faster.
- New abilities: selections and the active layer, 26 more image operations (adjustments, blurs,
  sharpen, noise, normal map, flip, rotate, offset, crop, trim, canvas resize and more), opening
  models, closing unmodified documents, multi-step undo and redo, progress notifications for bulk
  and workflow runs, enlarged pixel-grid previews of vanilla textures, three starter prompts and
  rule-topic completion. Every tool now has a title and destructive/idempotent hints.

Agent Session (ACP)
- Streaming is batched and process tracking throttled, so long replies no longer slow the window;
  starting an agent no longer blocks it.
- Mode and model pickers (permissive modes ask first), slash-command completion, session title,
  token and cost display, resource links, tool kinds and files, withdrawn permission requests,
  and a bounded transcript. Allow editing continues the same conversation when the agent can load
  sessions. An option mentions the open asset in a prompt.

Size and speed
- `nova-studio.exe` is about 60% smaller: the bundled icon font is subset to the icons Nova
  uses, and resources and code sections are compressed and trimmed. Editing, canvas and model
  preview paths do less work per change.
- Textures that no block or item model uses (armor and equipment, player skins, mobs, block
  entities, paintings, items, fluids and sprites) get a built-in 3D preview.

## Features

- Edit textures with layers, selections, brushes, colour tools, filters and batch operations.
- Create animated textures, edit models and UVs, and preview supported Minecraft item and block models.
- Use a verified local vanilla texture and model baseline, downloaded from Mojang on request.
- Test a pack in a managed Minecraft Java 1.21.11 Fabric runtime, reload changes and compare icons.
- Search commands and assets with Ctrl+K. Nova saves atomically and offers recovery after a crash.
- Connect an AI agent through MCP to inspect or edit a pack with your permission. Agent Session
  (ACP) starts only when you open it; automated tests use a scripted agent and real provider
  conversations remain unverified.

## Requirements and installation

Windows 11 x64 and an OpenGL 2.1 or later graphics driver are required. Testing has been on one
development PC, not a clean Windows installation or other GPUs. See [installation](INSTALLATION.md).
The installer and application are **unsigned**; Windows SmartScreen may show a warning. Check the
download against `SHA256SUMS.txt` before running it. Do not disable Windows security features.

## Source and licenses

Nova Studio is GPL-3.0-or-later. The package contains the corresponding source archive, license
and third-party notices, Qt source and patch. The matching Nova source and GCC runtime source
sidecars accompany the release downloads. See `licenses/THIRD_PARTY_NOTICES.md`, the package's
`docs/BUILDING.md`, and Help › Licenses and Source Code.
