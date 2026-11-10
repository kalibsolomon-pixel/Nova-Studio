# Vanilla textures as a baseline: user guide

A resource pack usually changes a few textures and leaves the rest to Minecraft. With a vanilla
baseline, Nova Studio shows the game's own textures for one Minecraft Java version next to your
pack's files, so you can find, open and edit any of them. Only the textures you actually change are
added to your pack.

## Creating a pack

File › New Resource Pack… has a **Textures** choice:

- **Blank pack** (default): exactly as before: `pack.mcmeta` and the folders you choose.
- **Use vanilla textures as baseline**: the selected Minecraft version's textures are shown under
  your files. Below the choice Nova says whether that version is already in your local cache (date,
  number of files, size). If it is not, Nova asks before downloading: it downloads the official
  Minecraft Java client from Mojang (about 30 MB), keeps only its textures and deletes the rest.

The download shows its stage and progress and can be cancelled. Nothing is created until it has
finished and every file has been verified; if it fails or you cancel, you can create a blank pack
instead or go back to the dialog. The version you choose is the pack's target version.

An existing pack can opt in with **Resources › Vanilla Baseline › Use Vanilla Baseline for This
Project…**. Nova preselects the version that matches `pack.mcmeta` but never assigns one without
your confirmation.

The baseline is recorded in `.nova/project.json` and shown in File › Project Properties
("Vanilla baseline: Java <version> (identity …)") and in the status bar ("Baseline: Vanilla Java
<version>"). If `pack.mcmeta` targets a different version, Nova shows a notice and changes nothing.

## What you see in Assets

| Row | Looks like | Tooltip |
|---|---|---|
| Vanilla texture not in your pack | dimmed name | "Vanilla Java <v> · not in project (read-only baseline)" |
| Your file replacing a vanilla texture (override) | accent-coloured name | "Project override of vanilla Java <v>" |
| Your own new texture | normal | its path |

The Inspector's **Origin** row says the same for the open texture: "Vanilla Java <v> (not in
project)", "Override of vanilla Java <v>" or "Project asset". Ctrl+K search, validation, batch
processing and MCP agents see only your pack's own files.

## Editing creates an override

Double-click a vanilla texture to open it. Opening writes nothing to your pack. Edit it as usual;
undo and redo work as for any texture. The first **Save** creates the file in your pack at the same
path (Nova creates missing folders such as `assets/minecraft/textures/gui/sprites/hud/` inside the
pack) and the row turns into an override. Animated textures keep their `.mcmeta` and are saved with
**Animation › Export Minecraft Animation** as a PNG + `.mcmeta` pair. If a file appears at that path
while you work, Nova never overwrites it; the usual conflict dialog appears instead. Unsaved edits
are protected by autosave and offered for recovery after a crash, still without overwriting anything.

## Reset to Vanilla and Remove from Project

- **Reset to Vanilla** (Resources › Vanilla Baseline, the Assets context menu, or the canvas context
  menu) removes your override so the vanilla texture shows again. Nova first checks that the vanilla
  file in the cache is intact; if it is missing or damaged, nothing is changed and you are told your
  override was preserved.
- **Reset All Overrides to Vanilla…** does this for every override after confirming the number of
  textures; it is one undo step.
- **Remove from Project…** is offered for your own textures that have no vanilla counterpart. It is
  never labelled "Reset to Vanilla", because there is no vanilla texture to return to.

Nothing is deleted: the PNG, its `.mcmeta` and its editable layered document are moved into
`.nova/baseline-resets/` in your project. If the texture is open with unsaved changes, Nova asks
whether to discard them first.

**Undo**: Edit › Undo (Ctrl+Z) while the Assets panel has focus, or Resources › Vanilla Baseline ›
Undo Reset. Redo with Ctrl+Y or Redo Reset. Undo restores the files only if nothing has appeared at
their place meanwhile; otherwise nothing is overwritten and Nova tells you which file is in the way.
The history lasts while the project is open. An interrupted reset (for example a power cut) is
offered for recovery when the project is opened again. Retained files are pruned after 30 days or
beyond the 64 most recent resets.

## Export stays sparse

File › Export Minecraft Pack copies only your pack's own files, exactly as before. Vanilla textures
you never saved are not included; reset textures are not included because their files were moved
into `.nova`. An override is exported even if it happens to be identical to vanilla: owning the file
decides, not its content.

## Managing the cache

**Resources › Vanilla Baseline › Manage Cached Baselines…** (also Edit › Preferences › Vanilla
textures › Manage…) lists every cached version with its identity, date, number of files, size, the
space only it uses and the space it shares with other versions. **Verify** re-checks every file;
**Remove** frees the space after confirmation. Projects keep their files; a project using a removed
version shows only its own files until you choose **Download / Repair Baseline…**.
[CACHE.md](CACHE.md) describes the location, structure and privacy in detail.

## Offline

Once a version is cached, baseline projects work without an internet connection. Nova contacts
Mojang only when you agree to a download or repair, and only Mojang's official servers.

## When something goes wrong

| Message | Meaning |
|---|---|
| Vanilla Java <v> baseline not available | The cache entry is missing or damaged. Your files are untouched; use Download / Repair Baseline…. |
| Could not download … from Mojang's servers | Network problem; nothing was kept. |
| … did not match its published checksum | A download was corrupted or unexpected; nothing was kept. |
| Not enough free disk space | Free some space and try again. |
| Another Nova Studio window is updating the cache | Wait for it to finish. |
| … is not listed in Mojang's version manifest | Mojang does not publish that version. |
| Official metadata … no longer matches | Mojang republished the version; Nova offers to switch the project to the current official baseline after confirmation. |
| Reset to Vanilla failed … preserved | The vanilla file could not be verified, or a file was in use; nothing changed. |
| Undo Reset failed … Nothing was overwritten | A file now exists where the retained one would go. |

Minecraft textures are © Mojang Studios, downloaded from Mojang's official servers for local use.
Nova Studio is not affiliated with or endorsed by Mojang or Microsoft.
