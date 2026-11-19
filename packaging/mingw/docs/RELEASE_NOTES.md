# Nova Studio 0.1.0 — release notes

Nova Studio is a Windows editor for Minecraft Java Edition resource packs. Keep backups of packs
you care about. See [known limitations](KNOWN_LIMITATIONS.md) for the supported scope and test coverage.

**NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT.**
Nova Studio is developed and published by the Nova Studio contributors. Minecraft is a trademark
of Mojang AB and Microsoft Corporation. Nova Studio contains no Minecraft game assets.

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
