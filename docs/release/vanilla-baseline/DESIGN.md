# Vanilla texture baseline and sparse export: design

Status: design agreed before implementation (this document is the contract the code follows).
Scope: Minecraft Java Edition **textures only**. This is not a release document.

## 1. Findings that shaped the design

- A Nova "project" is a folder. Nothing records the target version except `pack.mcmeta`; there is
  no project manifest and no migration code. `gameVersions()` (`src/app/project.cpp`) is the single
  version / pack-format table (26.3, 26.1, 1.21.11, 1.21.9, 1.21.4) used by creation and Properties.
- `Project::exportMinecraft` copies only files that physically exist in the project and skips every
  `.nova` folder. A baseline that is never written into the project is therefore excluded from
  export by construction. Export is a folder (no zip writer exists); tests inspect the exported
  folder's complete file list and bytes.
- `.nova/` at the project root is ignored by the index, watcher, export, validation, batch and MCP.
- Documents can already be built for a path that does not exist yet (`Document(path, image)`); their
  first save is *create-only* (`atomicWrite` with `Revision{}`), so a concurrent external file is
  never overwritten.
- No project-level undo or undoable file delete exists. The safest precedent is
  `animation_export.cpp`: never delete, rename into a journal folder on the same volume.
- A user-supplied vanilla folder mechanism exists for the Model panel only
  (`mc::ResourceSource`, `scripts/prepare-vanilla-cache.py`, `.../vanilla/<v>`). It is left untouched;
  the new cache lives in a different folder and never reads or modifies those folders.
- Nova downloads nothing from Mojang's metadata service today. `testing::Fetch` + `DownloadPolicy`
  give bounded HTTPS downloads with per-hop host checks and default (validated) TLS.

## 2. Official sources (verified 2026-11-07 against the live endpoints)

| Step | Endpoint | Verified field |
|---|---|---|
| Version manifest | `https://piston-meta.mojang.com/mc/game/version_manifest_v2.json` | `versions[].{id,type,url,sha1}` |
| Version JSON | `versions[].url` (piston-meta) | SHA-1 of body == manifest `sha1`; `id` matches |
| Client jar | `downloads.client.{url,sha1,size}` (piston-data) | size and SHA-1 |
| Asset index | `assetIndex.{id,url,sha1,size}` (piston-meta) | size and SHA-1 |
| Asset objects | `https://resources.download.minecraft.net/<sha1[0:2]>/<sha1>` | size and SHA-1 per object |

Block/item/entity textures are in the client jar (`assets/minecraft/textures/**`). The asset index
contains a few overridable textures (1.21.11: the 7 title-screen panorama files under
`minecraft/textures/`). The baseline is:

- jar entries `assets/minecraft/textures/**.png` and `**.png.mcmeta`, plus
- asset-index objects `minecraft/textures/**.png|.png.mcmeta` mapped to `assets/minecraft/textures/...`,
  used only for paths the jar does not contain (inference: the jar is the primary vanilla pack).

Only these hosts are allowed, on every redirect hop: `piston-meta.mojang.com`,
`piston-data.mojang.com`, `resources.download.minecraft.net`. TLS uses Qt defaults (system trust,
peer verification); nothing calls `ignoreSslErrors`. The client jar is deleted after extraction;
only textures are kept. Non-texture assets are never stored.

## 3. Shared per-user cache

Root: `QStandardPaths::AppLocalDataLocation + "/vanilla-baseline"`
(`%LOCALAPPDATA%\Nova Studio\Nova Studio\vanilla-baseline`). Tests and isolated runs set
`NOVA_VANILLA_BASELINE_ROOT`. Layout:

```
vanilla-baseline/
  objects/<aa>/<sha256>.png | .mcmeta       content-addressed, read-only attribute, shared by all versions
  entries/<version>/<identity32>/manifest.json   the only "complete" marker (published last, atomically)
  staging/<token>/                          temporary downloads (client.jar), swept under the lock
  cache.lock                                QLockFile held for every mutation (populate, repair, remove)
```

**Identity.** `identity = sha256("nova-vanilla-baseline/1|java|<id>|<client.sha1>|<client.size>|<assetIndex.id>|<assetIndex.sha1>")`.
The version-JSON SHA-1 is *not* part of it (Mojang republishes version JSONs without changing the
client, e.g. 1.21.11's JSON changed from `96403ac4…` to `35f8a9bc…` while the client stayed
`ba2df812…`). A new client or asset index produces a new identity and therefore a new entry; a stale
entry is never reused for different metadata.

**manifest.json** (schema 1): `version, identity, created, source{manifest_url, version_url,
version_sha1, client{url,sha1,size}, asset_index{id,url,sha1,size}}, files{path:{sha256,size,origin}},
digest` where `digest` is SHA-256 of the compact JSON of all other keys. A manifest that fails
parsing, digest, path or field validation is treated as not present (reported as corrupt).

**Population** (transactional): take `cache.lock` → re-check for a complete entry (another process
may have finished) → fetch and verify metadata → check free disk space → download jar to
`staging/<token>/client.jar` → verify size+SHA-1 → extract matching entries on a worker thread
(path rules below, per-entry and total limits) → for each file write `objects/..../<sha256>.<ext>` via
temp+rename unless an object with that name already exists *and* re-hashes correctly (dedupe; a bad
existing object is replaced) → download asset-index textures (size+SHA-1) → write `manifest.json`
atomically → delete staging → release lock. Cancellation or any error before the manifest rename
leaves no entry; orphan objects are harmless and collected by the next removal.

**Path rules** for every baseline path: `assets/minecraft/textures/` prefix, segments match
`[a-z0-9_.-]+`, no empty/`.`/`..` segment, no Windows device names, length ≤ 255 per segment and ≤ 33
segments, suffix `.png` or `.png.mcmeta`. Entries outside the prefix are ignored; entries inside the
prefix that break the rules fail the population (unexpected data).

**Use.** A project records its identity, so opening it uses `entries/<version>/<identity>` with no
network access (offline). Loading checks the manifest digest and that every object exists with the
recorded size; objects are SHA-256-verified when read for editing, reset, or the explicit Verify
command. Thumbnails are display-only.

**Concurrency.** One cross-process `QLockFile` (`cache.lock`) serialises mutations; a waiting process
re-checks for a complete entry after acquiring it, so two projects/processes requesting the same
baseline download it once. Readers do not lock: objects are immutable and manifests appear atomically.

**Removal.** Manage Cached Baselines lists version, identity prefix, date, file count, size (unique and
shared). Remove (confirmed) takes the lock, deletes the selected entry's manifest and folder, then
deletes only objects not referenced by any remaining *valid* manifest; if any remaining manifest is
unreadable, object collection is skipped. All deletions are restricted to regular files inside the
cache root with no reparse points on the path; nothing outside the cache root is ever deleted.

## 4. Project data model

New file `<project>/.nova/project.json` (never exported, indexed, or visible to MCP):

```json
{"schema":1,"baseline":{"edition":"java","version":"1.21.11","identity":"<64 hex>",
 "client_sha1":"<40 hex>","asset_index":{"id":"29","sha1":"<40 hex>"}}}
```

- Missing file, or no `baseline` key: **legacy/blank project** — behaviour is exactly as today; no
  version is assigned and no download is offered unless the user chooses
  *Resources › Vanilla Baseline › Use Vanilla Baseline for This Project…*.
- Unknown `schema` or invalid content: baseline off, a diagnostic is shown, the file is never rewritten.
- Unknown keys are preserved on save. Writes use `atomicWrite` with the read `Revision`.
- Baseline version must be a `gameVersions()` id. If `pack.mcmeta`'s format does not match the
  baseline version, a diagnostic is shown; nothing is changed silently.

Layers (by logical resource path, project wins):

| Origin | Where | Shown as |
|---|---|---|
| Baseline | cache object, read-only | dimmed name, tooltip "Vanilla Java <v> · not in project" |
| Override | project file at the same path as a baseline file | accent name, tooltip "Project override of vanilla" |
| New asset | project file with no baseline counterpart | normal |

## 5. Editing, reset, history

- Opening a baseline-only texture builds a document bound to the *project* path with verified cache
  pixels (animated textures keep their `.mcmeta`). Nothing is written. The first save creates the
  override create-only (parent folders are created inside the project after containment checks;
  animated textures use the journaled PNG+`.mcmeta` pair export). Autosave/recovery snapshots work as
  for any unsaved document; restore keeps the "missing on disk" original, so saves stay create-only.
- **Reset to Vanilla** (override with baseline counterpart) and **Remove from Project…** (project
  texture without one, baseline projects only) share one mechanism: verify the baseline object first
  (reset only), resolve the open document (unsaved changes require explicit discard), then *rename*
  the PNG, its `.png.mcmeta` and its `.nova/documents/*.nvdoc` into
  `.nova/baseline-resets/<uuid>/` with `journal.json` (`prepared` → `applied` ↔ `undone`). Nothing is
  deleted. Empty folders left behind are removed with non-recursive `rmdir`.
- Undo restores the retained files only if every destination is still vacant (an external or newer
  file is never overwritten; the user is told). Redo re-applies only if the files still match the
  journal hashes. The reset history is a project-level stack reached through Edit › Undo/Redo when
  the asset browser has focus and through explicit menu commands. An interrupted journal is
  offered for recovery on project open. Journals are pruned after 30 days or beyond 64.
- If the baseline is missing or corrupt, reset is refused and the override is untouched.
- **Reset All Overrides to Vanilla…** always asks for confirmation and is one undo step.

## 6. Export

Export is unchanged: it copies project files only (`.nova` excluded). Baseline-only textures are never
enumerated; reset textures are absent because their files were moved into `.nova`. Ownership, not
byte comparison, decides inclusion: an override identical to vanilla is still exported.

## 7. Out of scope (documented, not implemented)

MCP sees project files only (baseline rows are not in MCP indexes). Ctrl+K search lists project
files only. The Model panel's user-configured resource stack is unchanged and does not use the new
cache. Asset-index sounds/languages and jar models/blockstates are never stored.
