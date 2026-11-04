# Nova Studio — native Minecraft resource-pack editor

<img src="assets/branding/nova-studio-mark.png" alt="Nova Studio geometric starburst logo" width="112" height="112">

## License

Copyright (C) 2026 Nova Studio contributors.

Nova Studio is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version (`GPL-3.0-or-later`). It is distributed WITHOUT ANY
WARRANTY. The license text is [LICENSE](LICENSE).

Not everything in this repository is Nova Studio's work: the embedded Gradle wrapper (Apache-2.0),
the CC0 test fixtures, and Minecraft client screenshots in historical evidence under `docs/` keep
their own terms, and the Qt and Microsoft libraries shipped with the binaries keep theirs. See
[COPYRIGHT.md](COPYRIGHT.md) and [THIRD_PARTY_NOTICES.md](packaging/THIRD_PARTY_NOTICES.md).
Binary releases include their complete corresponding source code; see
[BUILDING.md](docs/release/BUILDING.md). Phase 4-G.1 compliance evidence is in
[docs/phase-4g1](docs/phase-4g1/GPL_COMPLIANCE_REPORT.md).

## Phase 3-E — Windows release finalization

Nova Studio now has a per-user Windows installer (Inno Setup) built from the same
reviewed payload as the portable candidate: Start Menu entry, optional desktop
shortcut, uninstaller that preserves projects, settings, recovery files and the
Fabric runtime (offered as a separate, declined-by-default cleanup). Installing
never requires elevation and the application writes mutable state only under
`%LOCALAPPDATA%`/`%APPDATA%`. Opening an untouched animated texture no longer
asks to save; genuine edits still do. The Fabric runtime location resolution
keeps user-scoped storage even with an empty `LOCALAPPDATA`, and workspace
integrity verification now refuses unlisted build scripts anywhere under the
runtime workspace. Phase 4's AI/MCP integration design is documented in
[the architecture handoff](docs/phase-4/MCP_ARCHITECTURE_HANDOFF.md); see
[release audit](docs/phase-3e/PHASE_3E_RELEASE_AUDIT.md),
[packaging](docs/phase-3e/PHASE_3E_PACKAGING.md),
[security](docs/phase-3e/PHASE_3E_SECURITY.md),
[installed acceptance](docs/phase-3e/PHASE_3E_NATIVE_ACCEPTANCE.md),
[performance](docs/phase-3e/PHASE_3E_PERFORMANCE.md) and the
[final report](docs/phase-3e/PHASE_3E_FINAL_REPORT.md).

## Phase 3-D.1 — built-in Fabric test client

**Resources › Test in Minecraft…** (Ctrl+Shift+M) now runs Nova's own Minecraft
development client — Minecraft 1.21.11 with Fabric Loader 0.20.1 and Remove Reloading
Screen — prepared on first use (about 1 GB, pinned and hash-verified) in
`%LOCALAPPDATA%\NovaStudio\fabric-runtime`, isolated from every other Minecraft
installation. Later launches reuse it offline; **Sync Test Pack** (Ctrl+Alt+M) and F3+T
reload edits. It is a local development client: no sign-in, no ownership check, no
online play. The Phase 3-D launcher hand-off is dormant (`NOVA_ENABLE_LAUNCHER_HANDOFF=1`).
See [architecture](docs/phase-3d1/PHASE_3D1_ARCHITECTURE.md),
[dependencies](docs/phase-3d1/PHASE_3D1_DEPENDENCIES.md),
[runtime](docs/phase-3d1/PHASE_3D1_RUNTIME.md),
[pack testing](docs/phase-3d1/PHASE_3D1_RESOURCE_PACK_TESTING.md) and
[acceptance](docs/phase-3d1/PHASE_3D1_NATIVE_ACCEPTANCE.md).
## Phase 3-D — test packs in real Minecraft

**Resources › Test in Minecraft…** (Ctrl+Shift+M) hands the saved pack to a launcher
that signs you in: the official **Minecraft Launcher** (Nova adds an optional
*Nova Studio Test* installation with its own game folder) or an **Aurora Launcher**
instance you pick. The launcher performs Microsoft sign-in, the Java Edition ownership
check and the launch; Nova never sees account credentials and never starts a demo or
offline session. The pack is exported atomically as `Nova Test - <pack>`, so Minecraft
never loads a half-written copy; **Sync Test Pack** (Ctrl+Alt+M) then F3+T reloads it.
Nova follows the game's own log to show whether the pack is active and to list
model/texture warnings. If no launcher is installed, Nova can download one from its
official source after you confirm, and verifies it before running the installer.
Editing never requires an account. See the
[Phase 3-D report](docs/phase-3d/PHASE_3D_REPORT.md),
[Aurora comparison](docs/phase-3d/AURORA_ARCHITECTURE_COMPARISON.md) and
[live test steps](docs/phase-3d/LIVE_TEST_STEPS.md).

## Phase 3-B — Windows-native UI/UX refinement

Phase 3-B reorganises the native Qt Widgets interface around a canvas-first
workspace without changing any editing engine or file format:

* one application bar (logo, Save/Undo/Redo, project location, workspace
  switcher, command palette) and a tool-options row sized to its controls;
* standard menus — File, Edit, Image, Layer, Select, Effects, Animation,
  Resources, View, Help — with unique mnemonics, a new Layer menu and every
  tool searchable in the command palette;
* five task workspaces (Essentials, Texture Editing, Animation, Model Editing,
  UV Editing) that never touch documents, plus saved layouts and Reset;
* a structured Inspector, compact Layers and Assets panels, grouped Models / UV
  sections, keyboard zoom (Ctrl+= / Ctrl+- / Ctrl+0 / Ctrl+1), Preferences
  (Ctrl+,), a generated Keyboard Shortcuts reference (Ctrl+/) and an Activity log;
* one token-based design system for light and dark themes (WCAG AA text).

See the [UI audit](docs/phase-3b/PHASE_3B_UI_AUDIT.md),
[design system](docs/phase-3b/PHASE_3B_DESIGN_SYSTEM.md),
[feature matrix](docs/phase-3b/PHASE_3B_FEATURE_MATRIX.md) and
[native acceptance](docs/phase-3b/PHASE_3B_NATIVE_ACCEPTANCE.md).

## Phase 3 — Windows stabilization candidate

Phase 2-C8 is the final feature phase. Phase 3 preserves the native editor and file
formats while correcting pending-edit loss, model close/project-switch lifetimes,
recovery wiring, compact-window layouts and keyboard/accessibility feedback. It
adds local bounded diagnostics, explicit build identity and an unsigned portable
Windows candidate. There is no Phase 2-C9; optional AI and Phase 4 are not started.

See the [Phase 3 engineering report](docs/phase-3/FINAL_REPORT.md),
[acceptance matrix](docs/phase-3/FULL_ACCEPTANCE_MATRIX.md),
[build/package instructions](docs/phase-3/BUILDING.md), and
[release limits](docs/phase-3/LIMITATIONS.md). Public distribution remains subject
to owner authorization, licensing and the documented acceptance gaps. Local
diagnostics never upload automatically; use Help to export and review them.

```powershell
./scripts/build-studio.ps1 -BuildName studio-phase3-release
./scripts/build-studio.ps1 -Configuration Debug -BuildName studio-phase3-debug
./scripts/run-studio.ps1 -BuildName studio-phase3-release
```

Earlier phase instructions and evidence below are retained for history.

Nova Studio is a native Minecraft resource-pack texture IDE. The permanent
Windows 11 application uses C++20, Qt Widgets, a pixel-exact QPainter canvas and
an OpenGL cube preview. Phase 2-B adds Java resource-pack projects, guarded saves,
bounded undo, recovery snapshots and incremental asset navigation. Phase 2-C1/C2
adds permanent raster layers, editable sidecar documents, guarded composite export,
layered recovery and a dockable Layers panel. Phase 2-C3 adds a full texture editor:
selections, brushes, shapes, transforms, colour/palette tools, adjustments, filters,
directional shading, procedural generators and safe batch pipelines, all built on one
deterministic image-operation engine.

Phase 2-C5 adds versioned Minecraft cuboid models, parent/texture dependency
resolution, actual block/item previews, UV overlays and face picking, live animated
textures, bounded native-payload reuse, RGBA stamps, numeric shape presets and
coherent multi-layer operations. See the [C5 engineering report](docs/phase-2c5/FINAL_REPORT.md)
and [documented compatibility limits](docs/phase-2c5/LIMITATIONS.md).

Phase 2-C6 adds explicit resource-pack/verified vanilla fallback, item subtree
transforms and tints, generated alpha contours, guarded interactive UV editing,
independent animated materials, floating selected-layer transforms, project preview
preferences and bounded unique-animation serialization. See the
[C6 engineering report](docs/phase-2c6/FINAL_REPORT.md) and
[compatibility boundaries](docs/phase-2c6/MODEL_COMPATIBILITY.md).

## Item display, bulk UV and native batches (Phase 2-C8)

**Item display transforms** (Models / UV panel, collapsed by default): pick a display
context (GUI, ground, frame, head, hands, shelf), then edit rotation / translation /
scale numerically or drag in the preview (Shift rotates, Ctrl translates, Ctrl+Shift
scales). The source line shows whether a value is local, mirrored from the right hand
or inherited, exactly as Minecraft resolves it. **Enter** applies one model undo step,
**Escape** cancels; **Inherit**, **Left mirrors right**, Copy/Paste and **Block preset**
are one click. **Context framing** keeps the camera fixed with a GUI-slot or one-block
reference so size and offset are visible. New cuboid templates include display
transforms, so they are no longer oversized in hand.

**Edit model…** now opens a non-modal window; the texture editor and main preview stay
usable. Its Faces / UV tab has **Bulk numeric UV**: select several elements and faces,
then set absolute U/V or size (mixed values are left untouched), move, mirror, rotate,
distribute, snap per texture, make inferred UVs explicit or copy/paste UVs.

**Batch Processor** gains source modes: exported PNG, **Native document** (processes
layers and source frames of `.nvdoc` documents without flattening) and native plus a
separate PNG/animation export. Choose layer scope (active, named, all, visible) and frame
scope (current, selected, all, playback entries processed once). Dry runs list target
layers/frames and conflicts; every replacement is verified and journaled in a
transaction folder outside the pack, and **Recover…** rolls back or confirms an
interrupted run. See the [C8 report](docs/phase-2c8/FINAL_REPORT.md) and
[limitations](docs/phase-2c8/LIMITATIONS.md).

```powershell
./scripts/build-studio.ps1 -BuildName studio-c8-release
./scripts/build-studio.ps1 -Configuration Debug -BuildName studio-c8-debug
.tools/python/Scripts/python scripts/validate-phase2c8.py studio-c8-release docs/phase-2c8/my-release-check
```

## Native visual-asset authoring (Phase 2-C7)

**Resources > Create Minecraft asset** previews and creates block/item models,
basic blockstates, modern item definitions, original texture/animation templates
and model/texture pairs. Existing destinations are preserved; related writes retain
a recovery journal. **Recover partial creation** checks all file hashes before rollback.

Open a model from Assets or Models / UV and choose **Edit model**. Add/duplicate
cuboids, edit precise coordinates/rotations, use snapped XY/XZ/YZ handles, assign
face textures and author multiple face UV mappings. Texture choices create/reuse
Minecraft aliases automatically. **Apply preview** creates one model undo step;
**Cancel preview** restores the document; **Save JSON** checks external revisions.
Save As creates a copy; Rename retains a forwarding model to preserve references.
**Restore model snapshot** recovers checked model edits into memory.

**Resources > Validate resources** opens the contextual Problems dock, with asset,
folder, namespace and pack scopes. It validates in the background, supports cancel,
shows locations/dependencies/suggestions, and exports text/JSON reports. Deterministic
repairs preview exact changes and preserve backups. Validation covers supported
constructs and cannot prove identical Minecraft rendering.

Pick a model face to select its texture pixels for normal image operations.
**Select pixels used by model faces** unions associated regions; **Process model
textures** uses an explicit dependency subset with shared batch previews, dry run,
revision checks and output manifests. Native editable sources reject in-place batch
replacement. See the [C7 delivery](docs/phase-2c7/FINAL_REPORT.md) and
[explicit limitations](docs/phase-2c7/LIMITATIONS.md).

C7 builds preserve earlier outputs:

```powershell
./scripts/build-studio.ps1 -BuildName studio-c7-release
./scripts/build-studio.ps1 -Configuration Debug -BuildName studio-c7-debug
.tools/python/Scripts/python scripts/validate-phase2c7.py studio-c7-release docs/phase-2c7/my-release-check
```

## Model-aware editing (Phase 2-C6)

Open a pack, then a PNG. **View > Models / UV** opens the contextual inspector.
Choose an associated model, or double-click JSON under `models`, `blockstates` or
modern `items` in Assets. **File > Inspect Minecraft model / definition** provides
the same workflow. Choose the explicit Java profile (1.21.11, 26.1 or 26.3).
**Resource stack** configures additional folder packs and an explicitly selected,
verified target-version vanilla cache. Editable assets win; Dependencies identifies
fallback sources. Fallbacks are read-only until **Import override** is chosen.
Double-click a dependency to inspect winner/shadowed contents without changing
priority; **Open editable** returns to the local asset when available.
The [resource-stack guide](docs/phase-2c6/RESOURCE_STACK.md) explains local cache preparation.

Click a model face to open its PNG and select its UV region. Enable **UV overlay**;
Ctrl+Alt-click a region to cycle overlapping faces. Paint normally: layers,
undo/redo and timeline playback update the same model texture. Orbit with left-drag,
pan with middle/right-drag and zoom with the wheel. The inspector provides camera,
projection, wire/outline, lighting and item display-context controls. **Generic cube**
remains available. **Inspect source JSON** is read-only. Definition inputs are manual
JSON, e.g. `{"facing":"north","_choice":1}` or `{"minecraft:using_item":true}`.

Select a face and choose **Edit UV** for drag/resize handles, pixel snapping,
arrow nudging, numeric UVs, rotation and aspect locking. Enter commits; Escape
cancels. **Undo model / Redo model** use a separate model history. **Save JSON**
checks the current disk revision before atomic replacement. Inherited geometry
requires an explicit local override; generated faces do not expose JSON UV edits.
See the [UV editor guide](docs/phase-2c6/UV_EDITOR.md).

**Animate materials** appears for animated models. Each material uses its own timing;
Stop and millisecond seek are available. Hidden previews stop scheduling animation.
**View controls** expands camera/lighting options. Model, camera, context and guide
preferences persist per project outside Minecraft assets; **Reset Preview** restores defaults.

**Edit > Original Stamp Library** supplies original geometric RGBA stamps; **Stamp
colours** preserves their colors, while disabling it recolors with the foreground.
Palette lock also maps source colors. **Numeric Shape / Presets** provides exact
geometry and persistent named presets. Ctrl/Shift-select several layers, then use
**Transform selected layers** with explicit Selected/All scope; the operation is one
undo step. Canvas resizing requires All layers; Scale Content resizes selected pixels.

The model inspector approximates isolated rendering. Constants are exact RGB tints;
runtime colors require explicit simulated `_tints` RGB inputs. Special/entity
renderers, world AO, custom atlases and intersecting transparency retain limitations.
Generated alpha contours and 26.1+ subtree transforms are supported within documented
budgets. No complete Minecraft runtime certification is claimed.
The accepted C4 rectangular-animation/mipmap limitation remains preserved.

After building both configurations, reproduce C5 regression evidence with:

```powershell
.tools/python/Scripts/python scripts/validate-phase2c5.py studio-release docs/phase-2c5/my-release-check --preservation
.tools/python/Scripts/python scripts/validate-phase2c5.py studio-debug docs/phase-2c5/my-debug-check
```

Each output directory must be new. Tests include original C4 suites, model/UV
rendering and editor/storage refinements, with the optional existing 50,000-asset
fixture enabled by the driver. Native Windows input evidence is recorded separately.

C6 builds/evidence use `studio-c6-release` and `studio-c6-debug` to preserve prior
builds. Run `scripts/validate-phase2c6.py <build> <new-output-directory>` for all
28 suites and the 50k fixture. Set `NOVA_VANILLA_CACHE` to an authorized verified
26.1 cache to include its real-parent regression; absence is reported as a skip.
Ctrl/Shift-select layers and use Move/Transform for a common floating preview.
Include hidden is explicit, layers stay separate, and Apply is one undo transaction.
Over-budget transforms are blocked before mutation. See
[editor workflows](docs/phase-2c6/EDITOR_WORKFLOWS.md) and
[storage measurements](docs/phase-2c6/STORAGE_OPTIMIZATION.md).

## Build and launch the permanent application

Use PowerShell 7 with Visual C++ Build Tools (MSVC x64), Windows SDK, CMake/Ninja,
and the Qt Base 6.12.0 shared MSVC 2022 x64 SDK. Tested locally: MSVC
19.51.36257.0, SDK 10.0.26100.0, CMake 4.3.1-msvc1, Ninja 1.13.2. Existing Qt is
under ignored `.tools/qt`. The script imports the installed Visual C++ environment
and finds its bundled CMake/Ninja through vswhere.

```powershell
./scripts/build-studio.ps1
./scripts/run-studio.ps1 -Project fixtures/generated/pack-1500
# Or launch without a folder, then use File > Open texture / Open folder:
./scripts/run-studio.ps1
./scripts/build-studio.ps1 -Configuration Debug
./scripts/run-studio.ps1 -Configuration Debug
```

Build outputs are `build/studio-release/` and `build/studio-debug/`. The executable
is `src/nova-studio.exe` inside each build folder. Close that executable before
rebuilding it; Windows holds running executable files open. A different build
configuration can remain open. Qt DLLs must be on PATH; run-studio.ps1 sets it.
These are development executables, with no installer or release packaging.

If provisioning a fresh SDK, use the preserved Python environment instructions
below. Set `-QtRoot C:/path/to/qt` on build-studio.ps1 for another matching SDK;
set its `bin` on PATH for direct launch/tests. The optional Vulkan-header CMake
warning does not prevent this OpenGL build. No Rust toolchain is needed to build
the permanent application.

## Permanent application tests and profiling

The build script runs CTest after each build. Run focused tests directly:

```powershell
$env:PATH = "$PWD/.tools/qt/bin;$env:PATH"
./build/studio-release/tests/nova-core-tests.exe
./build/studio-release/tests/nova-session-tests.exe
$env:NOVA_LARGE_FIXTURE = "$PWD/fixtures/generated/pack-50000"
./build/studio-release/tests/nova-workspace-tests.exe
.tools/python/Scripts/python scripts/validate-phase2a.py
```

The optional 50k test requires the existing generated pack. The comparison script
also requires the preserved `build/qt-review/nova-qt.exe` and small/large fixtures;
it creates unique evidence directories and takes about seven minutes. It measures
warm startup, core operations, a large editing exercise and five-minute idle CPU/
working set. `--idle-only` repeats just the soak. Raw timing scopes and limitations
are documented in [Phase 2-A performance](docs/phase-2a/PERFORMANCE.md).

Ctrl+O opens a PNG; Ctrl+Shift+O opens a folder; Ctrl+S saves; Ctrl+Shift+S saves
as; Ctrl+F4 closes a tab with Save/Discard/Cancel; Ctrl+Z/Y undo/redo. Tool letters
(B pencil, E eraser, M select, L lasso, W wand, U shape, G fill, I eyedropper,
V move, T transform) work while no text field has focus. Wheel zooms; middle/right
drag or Space+drag pans.
Double-click textures to open. Drag dock titles or double-click to float them.
View reopens panels, switches theme/accent and resets the workspace.
Ctrl+Alt+S saves the workspace explicitly; normal exit also saves it.

Preferences, layout, recent folders and open paths live in the user's Qt
application-config directory (`Nova Studio/Nova Studio/studio.ini`), never inside
the pack. No `.nova` folder is created by production browsing. Unsaved textures
are snapshotted in the application-local `recovery/` folder. `--settings <file>`
isolates preferences and recovery; `--fresh-layout`
skips workspace restoration/persistence during QA. Source PNGs use atomic saves.

## Resource-pack projects and file safety

Use **New Resource Pack** on the welcome surface or Ctrl+N. Choose a project name,
a new destination folder, a verified Java version, description and initial
namespace. Optional PNG icon and common asset folders are supported. Existing
destination folders are preserved and rejected; choose a new folder name.

**Open Resource Pack**, **Open Folder** and **Recent Projects** use real folders.
Opening never repairs or rewrites pack files. **File > Project Properties** shows
namespaces, metadata diagnostics, description, target format, icon and editable
JSON. Use the explicitly labeled Apply transformation to apply description/target
fields and reformat JSON, then Save. Unknown fields are retained. Cancelling an
already confirmed immediate icon replacement does not undo that icon write.

The asset selector switches between physical folders and virtual Minecraft
categories across namespaces. Tree and Grid share the same indexed files. Empty
folders, models, sounds, fonts and metadata stay visible; the pixel editor opens
PNG files. External additions/removals update through a recursive Windows monitor.

External changes never automatically discard unsaved pixels. **File > Review
External Change** offers Keep, Reload, Save local as and explicitly confirmed
Overwrite. Save verifies content revisions before replacing the file. The final
check/rename race with other editors is documented, not claimed to be eliminated.

**File > Undo history limits** configures the per-document byte budget (16 MiB /
2,048 operations by default). Very long strokes can be split to bound temporary
history memory. Startup recovery offers Restore, Inspect, Discard and Later;
restoring never writes the original PNG. Work since the latest completed snapshot
can be lost in a crash. Sudden power-loss durability is not certified.

Both Release and Debug include nine CTest suites. Additional executables are
`nova-project-tests`, `nova-integrity-tests`, `nova-indexing-tests` and
`nova-acceptance-tests` in the build's tests folder. Set `NOVA_EVIDENCE_DIR` to
retain acceptance screenshots and `NOVA_LARGE_FIXTURE` for the 50k GUI checks.
`nova-layered-tests` covers layer/compositor/history/storage/recovery behavior;
`nova-layer-gui-tests` exercises the native layer workflow and forced termination.

## Editable layers and Minecraft export

An ordinary PNG opens as one layer. Pencil and eraser edit the selected layer;
the canvas and cube show the same composite. The **Layers** dock provides Add,
Duplicate, Delete, inline/F2 Rename, visibility checks, exact 0–255 Opacity and
Up/Down ordering. Hidden layers reject drawing. Ctrl+Z/Y traverses both strokes
and layer operations under the existing 16 MiB / 2,048-operation history limits.
The transparency checker aligns to editable pixels, including 16×16 textures.

For an ordinary PNG, **Ctrl+S** still saves the PNG. Adding layers or changing
layer properties converts the document in memory. **Ctrl+S** then saves its editable
source into `.nova/documents/` inside the project. Merely opening, browsing or
drawing never creates metadata. **File > Save Editable Document** also explicitly
converts a single-layer file. The native source preserves hidden layers and exact
source pixels across restarts; reopen the same PNG in its project to restore it.

**File > Update Minecraft PNG** (**Ctrl+Alt+E**) flattens the current stack through
the guarded atomic save pipeline. **Export PNG As…** (**Ctrl+Shift+S**) writes a
composite elsewhere and keeps the native source attached to its original asset.
Save again after updating the original PNG to persist that successful export
baseline. The tab shows **●** for unsaved editable changes and **↑PNG** for a
composite awaiting export. Inspector lists editable, PNG and external-change states.
An external PNG edit cannot silently overwrite your layers or be overwritten by
normal export; use the existing explicit conflict review choices.

**File > Export Minecraft Pack…** copies current Minecraft files to a new folder
without `.nova` metadata. Update unsynchronized PNGs first; saved closed layered
documents are checked too. Existing destinations and linked entries are rejected.
This produces a folder, not a ZIP archive. No publishing or release action occurs.

Recovery snapshots now preserve the full layer stack, properties, active layer and
export revisions. Prior flat recovery snapshots remain readable. Restore never
overwrites source PNGs. At most one background batch runs; recent work since the
last completed snapshot can still be lost. See the [Phase 2-C report](docs/phase-2c/FINAL_REPORT.md),
[native format](docs/phase-2c/NATIVE_FORMAT.md), [data guarantees](docs/phase-2c/DATA_INTEGRITY.md),
[C1 measurements](docs/phase-2c/PERFORMANCE_C1.md), [layer performance](docs/phase-2c/PERFORMANCE_C2.md),
and [validation](docs/phase-2c/VALIDATION.md).

For a complete reproducible check after building on Windows:

```powershell
.tools/python/Scripts/python scripts/validate-phase2c.py --preservation
.tools/python/Scripts/python scripts/validate-phase2c.py --build studio-debug --configuration debug
```

These write new evidence under `docs/phase-2c/`, leaving historical reports intact.
See [limitations](docs/phase-2c/LIMITATIONS.md) for size, save, driver and recovery
constraints. The original [Phase 2-C3 proposal](docs/phase-2c/PHASE_2C3_PLAN.md)
is preserved; implemented workflows are described below.

Read the [Phase 2-B report](docs/phase-2b/FINAL_REPORT.md),
[file guarantees](docs/phase-2b/DATA_INTEGRITY.md),
[Minecraft mappings and sources](docs/phase-2b/MINECRAFT_COMPATIBILITY.md),
[performance evidence](docs/phase-2b/PERFORMANCE.md), and
[validation](docs/phase-2b/VALIDATION.md).

Read the [engineering report](docs/phase-2a/FINAL_REPORT.md),
[architecture](docs/phase-2a/ARCHITECTURE.md),
[migration](docs/phase-2a/MIGRATION.md),
[validation and screenshots](docs/phase-2a/VALIDATION.md),
[limitations](docs/phase-2a/LIMITATIONS.md), and
[proposed Phase 2-B](docs/phase-2a/PHASE_2B_PLAN.md).
At the time, no license/distribution decision had been made (Phase 4-G.1 later selected
GPL-3.0-or-later; see [License](#license)). No release or remote push was
performed. Phase 2-C1/C2 and the Phase 2-C3 workflows below are implemented locally.

## Texture editing workflows (Phase 2-C3)

The left **tool box** groups 17 tools into Draw, Select, Transform, Shapes, Fill,
Colour and fx; the **context bar** above the canvas shows only the active tool's
options. Every action targets the active layer, respects the active selection and is
one undo step (see History).

* **Select:** rectangle, ellipse, lasso, polygon lasso, magic wand and colour range with
  Shift add / Alt subtract / Shift+Alt intersect; Ctrl+A, Ctrl+D, Ctrl+Shift+I.
  Selections are transient (never saved into `.nvdoc`, recovery or PNGs).
* **Draw:** pencil (exact pixels, unchanged), brush (square/circle/diamond/line/custom
  stamp, width/height, opacity, hardness, spacing, smoothing), eraser, colour replace,
  symmetry (mirror/radial), palette lock; line and shape tools (rectangles, rounded
  rectangles, ellipses, polygons, regular polygons, stars); fill bucket; gradient;
  eyedropper.
  Shapes offer opt-in **Smooth edges**; pixel-exact drawing stays the default.
  Brush presets retain custom stamp pixels across restarts. A selection with zero
  pixels keeps drawing/effects blocked until Deselect.
* **Clipboard:** Ctrl+C/X/V paste as a movable floating placement (Enter applies, Esc
  cancels); Ctrl+Shift+V paste as layer; Ctrl+J duplicate selection to a layer.
* **Transform:** Move (V) and Transform (T, scale/rotate with handles) never touch
  pixels until applied. Image menu: flip, rotate 90/180/270, resize image (nearest by
  default), resize canvas (anchors, transparent/wrap/extend padding), crop, trim.
* **Colour:** the Colours dock (HSV/HSL/RGB/hex/alpha, recent colours, persistent
  palettes with GIMP .gpl, JASC .pal, Paint.NET .txt and .hex import/export).
* **Adjust / Filters / Shading / Generate:** one operation dialog with live background
  preview, presets and before/after view — levels, curves, quantization with
  dithering, blur/sharpen/median/edge/emboss/outline, normal/height estimates,
  directional shading with material presets (non-destructive shading layer by
  default), and deterministic seeded generators (stone, bricks, tiles, wood, ore,
  metal, GUI panel, items, seamless noise). Image › Tile Preview checks seams.
* **Batch:** Tools › Batch Processor (Ctrl+Alt+B) runs a saved operation stack over
  selected files, folders, categories, namespaces, search results or the whole pack,
  with sample previews, a mandatory dry run, conflict detection, cancellation, an
  output folder outside the pack by default (or backed-up in-place updates) and a JSON
  manifest of every file.
* **Command palette:** Ctrl+Shift+P searches every menu command.
  **View › Workspace Layouts** saves and restores named dock layouts.

Details: [tools](docs/phase-2c3/TOOLS.md), [architecture](docs/phase-2c3/EDITOR_ARCHITECTURE.md),
[shading and generators](docs/phase-2c3/SHADING_AND_GENERATORS.md),
[batch](docs/phase-2c3/BATCH_PROCESSING.md), [MCreator parity](docs/phase-2c3/MCREATOR_PARITY.md),
[limitations](docs/phase-2c3/LIMITATIONS.md). Editor benchmarks:
`build/<build>/tests/nova-editor-bench.exe out.json` (add `--quick` for a short run).

Latest local C3 continuation: [delivery report](docs/phase-2c3/CONTINUATION_REPORT.md),
[Release/Debug validation](docs/phase-2c3/VALIDATION.md), and
[current performance](docs/phase-2c3/PERFORMANCE.md). Full MCreator parity remains
incomplete; the matrix identifies the remaining template/stamp and layer-model gaps.
The validated Release executable is `build/studio-c3-closeout-release/src/nova-studio.exe`
(put `.tools/qt/bin` on PATH before launching it).

## Animated textures (Phase 2-C4)

Use **Animation > New Animated Texture** or **Convert Current Texture to Animation**.
Opening a PNG with a matching `.png.mcmeta` imports Java animation cells and timing;
an existing `.nvdoc` remains the authoritative editable source. The dockable timeline
separates editable source frames from playback references, including repeated and
skipped sources. Add/duplicate/delete sources, drag playback entries, edit tick
durations, and use Properties for interpolation, format profile and advanced JSON.

Every existing drawing tool and layer command targets the active source. **Process
Frames** offers active/selected/all scopes, a cancellable preview and one undo action.
Resizing requires explicit all-frame scope. Onion ghosts are view overlays. Canvas
and cube follow the same playback image; stop returns to the selected editing source.
**Ctrl+Enter** plays/pauses, **Alt+Left/Right** steps, **Alt+Home/End** jumps,
**Ctrl+Alt+Insert** adds a source, **Ctrl+Alt+D** duplicates and **Alt+O** toggles onions.

**Ctrl+S** saves layered animation to native storage. **Ctrl+Alt+E** reviews and exports
the PNG + metadata pair, preserving unrelated metadata. Each file is individually
atomic; the pair uses retained backups and a recovery journal. Interrupted exports
are detected on reopen and offer retry/rollback. External edits block ordinary
export. Save native again after successful export to preserve its new baselines.
The supported format profiles are Java 1.21.11, 26.1 and 26.3; animation availability
still depends on the game's atlas and asset type. In-game runtime certification,
GIF/APNG, sparse frames and disk payload deduplication are not included.

Build and validate on Windows with the existing Qt/MSVC environment:

```powershell
./scripts/build-studio.ps1 -Configuration Release -BuildName studio-c4-release -SkipTests
./scripts/build-studio.ps1 -Configuration Debug -BuildName studio-c4-debug -SkipTests
.tools/python/Scripts/python scripts/validate-phase2c4.py studio-c4-release docs/phase-2c4/my-release-check --preservation
.tools/python/Scripts/python scripts/validate-phase2c4.py studio-c4-debug docs/phase-2c4/my-debug-check
```

Validation output directories must be new. Without `-SkipTests`, the build script
also runs CTest. Run `build/studio-c4-release/tests/nova-animation-bench.exe out.json`
with `.tools/qt/bin` on PATH for animation measurements. The Release executable is
`build/studio-c4-release/src/nova-studio.exe`.

See the [C4 delivery report](docs/phase-2c4/FINAL_REPORT.md),
[architecture](docs/phase-2c4/ANIMATION_ARCHITECTURE.md),
[timeline guide](docs/phase-2c4/TIMELINE_UI.md),
[native schema](docs/phase-2c4/NATIVE_FORMAT.md),
[Minecraft compatibility](docs/phase-2c4/MINECRAFT_ANIMATION.md),
[paired-file guarantees](docs/phase-2c4/EXPORT_INTEGRITY.md),
[performance](docs/phase-2c4/PERFORMANCE.md),
[validation](docs/phase-2c4/VALIDATION.md), and
[remaining limits](docs/phase-2c4/LIMITATIONS.md).

## Preserved Phase 1 experiments

Native resource-pack editor experiments: Rust + egui/wgpu and C++ + Qt Widgets.
Independent of Aurora. Synthetic fixtures only; no Minecraft assets are included.

Both historical prototypes and all Phase 1 evidence remain intact. Their commands,
project-local layouts and limitations below describe the experiments only.

### Historical Windows build and run

Requirements: Rust 1.98.1, Visual C++ Build Tools, CMake/Ninja, Python 3.12, Qt Base
6.12.0 for MSVC x64. This workspace's SDK lives in ignored `.tools/qt`.

```powershell
python -m venv .tools/python
.tools/python/Scripts/python -m pip install -r scripts/requirements.txt
.tools/python/Scripts/python scripts/install-qt.py
.tools/python/Scripts/python fixtures/generator/generate.py fixtures/generated/pack-1500 --count 1500
./scripts/build-rust.ps1
./scripts/build-qt.ps1 -BuildName qt-review
./build/nova-egui.exe --project fixtures/generated/pack-1500
./scripts/run-qt.ps1 -Project fixtures/generated/pack-1500
```

The installer refuses to replace an existing archive. Skip SDK/fixture setup if
already present. The Rust build script puts the review executable in `build/`
so an older test window can remain open during development. Cargo's explicit
output-name warnings are expected. Qt's Vulkan-header CMake warning does not
prevent its OpenGL build. Qt DLLs must be on PATH; run-qt.ps1 sets this per process.

### Historical tests and evidence

```powershell
cargo test --workspace --locked
cargo clippy --workspace --all-targets -- -D warnings
$env:PATH = "$PWD/.tools/qt/bin;$env:PATH"
./build/qt-review/nova-qt-tests.exe
.tools/python/Scripts/python -m unittest discover -s fixtures/generator -v
.tools/python/Scripts/python scripts/native-smoke.py
.tools/python/Scripts/python scripts/validate.py qa
.tools/python/Scripts/python scripts/validate.py core
.tools/python/Scripts/python scripts/validate.py exercise
.tools/python/Scripts/python scripts/process-benchmark.py
```

Generate the 10k/50k packs before running the full benchmark. Process benchmark
telemetry names must be unused, to preserve earlier measurements. QA and exercise
modes change in-memory state, capture the real native rendering, and exit without
saving fixture textures or changing saved layouts. `--fresh-layout` skips layout
restoration and persistence. Native smoke uses a new isolated fixture copy.

Ctrl+S saves, Ctrl+Z/Y undo/redo, B/E pencil/eraser. Wheel zooms; middle/right drag
pans. Double-click PNGs to open. Drag dock titles/tabs to rearrange; View reopens
panels. Saved layouts live in `.nova/` inside each project.

Current evidence favors the Qt implementation. The Rust wgpu path shows high
driver-thread idle CPU on this AMD machine, plus a recorded shutdown timeout;
both prototypes miss the provisional 150 MB working-set target. See the reports
before treating either as a production baseline. Close the review executable
before rebuilding that same output file; older executables under target/ can stay
open while build/ is rebuilt.

Phase 3-C native performance and lifecycle results are in the
[Phase 3-C report](docs/phase-3c/PHASE_3C_REPORT.md), with
[matched measurements](docs/phase-3c/PERFORMANCE.md) and reproducible probe commands.

Read [implementation](docs/phase-1/IMPLEMENTATION.md),
[benchmarks](docs/phase-1/BENCHMARKS.md),
[comparison](docs/phase-1/FRAMEWORK_COMPARISON.md),
[limitations](docs/phase-1/LIMITATIONS.md), and
[next-phase proposal](docs/phase-1/NEXT_PHASE.md).
