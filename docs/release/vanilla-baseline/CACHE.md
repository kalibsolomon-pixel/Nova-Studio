# Vanilla texture cache

How Nova Studio keeps the vanilla Minecraft Java textures that baseline projects show. The design
contract is [DESIGN.md](DESIGN.md); this page describes the cache as it is implemented.

## Location

One cache per Windows user, shared by every project and every Nova Studio window:

```
%LOCALAPPDATA%\Nova Studio\Nova Studio\vanilla-baseline
```

(`QStandardPaths::AppLocalDataLocation + "/vanilla-baseline"`). Edit › Preferences › Vanilla
textures and Resources › Vanilla Baseline › Manage Cached Baselines… show the exact folder. The
environment variable `NOVA_VANILLA_BASELINE_ROOT` replaces the location; the test suites use it so
that no test ever touches the real cache. Nothing is stored inside a project except
`.nova/project.json`, which names the baseline a project uses.

The cache is unrelated to the Model panel's user-configured resource folders
(`.../vanilla/<version>` made by `scripts/prepare-vanilla-cache.py`); neither reads the other.

## Structure

```
vanilla-baseline/
  objects/<aa>/<sha256>.png | .mcmeta          one file per distinct content, read-only attribute
  entries/<version>/<identity[0:32]>/manifest.json
  staging/<token>/                             temporary client download, removed afterwards
  cache.lock                                   held while the cache is changed
```

- **Objects** are content-addressed by SHA-256 and shared between versions: a texture that did not
  change between two versions is stored once.
- **Entries** describe one baseline. `manifest.json` lists every texture path
  (`assets/minecraft/textures/**.png` and `.png.mcmeta`) with its SHA-256, size and origin (client
  jar or asset index), plus where it came from (Mojang's URLs and published SHA-1 values). The
  manifest carries a SHA-256 digest of its own content; a manifest that fails to parse or whose
  digest, paths or fields are wrong is treated as absent and reported as damaged.
- Only textures are kept. The client jar is deleted after extraction; models, sounds, languages,
  code and every other file are never stored.

## Identity

A baseline is identified by

```
sha256("nova-vanilla-baseline/1|java|<version>|<client sha1>|<client size>|<asset index id>|<asset index sha1>")
```

If Mojang publishes a different client or asset index for a version, the identity changes and a new
entry is made; an existing entry is never reused for different files. A project records the
identity, so it keeps using exactly the textures it was created with.

## Verification

- **Download**: every file is checked against Mojang's published size and SHA-1 (version JSON,
  client jar, asset index, asset objects); a mismatch keeps nothing (Integrity error).
- **Opening a project**: the manifest digest is checked and every object must exist with its
  recorded size (`Check::Sizes`). This runs on a background thread; the asset browser shows project
  files until it completes.
- **Opening a vanilla texture, Reset to Vanilla and Redo Reset** read the object and verify its
  SHA-256 first. A damaged object is never shown as a document and never used to justify a reset.
- **Thumbnails** are display-only and read the object without re-hashing.
- **Verify** (Manage Cached Baselines) re-hashes every file of an entry with SHA-256 off the GUI
  thread; it can be cancelled.

## Offline use

Opening a baseline project never uses the network: the recorded identity selects the entry. A
project whose entry is missing or damaged still opens; it shows only its own files, the status bar
reads "Vanilla Java <v> baseline not available · Resources › Vanilla Baseline › Download…", Reset to
Vanilla is disabled and no project file is touched. Downloads only happen after the user agrees (new
project with a baseline, Use Vanilla Baseline for This Project…, Download / Repair Baseline…).

## Concurrency

All changes (download, repair, removal) hold `cache.lock` (a cross-process `QLockFile`). A second
window or process that wants the same baseline waits, then finds the complete entry and downloads
nothing. A process that waits longer than the timeout reports Busy and changes nothing. Readers never
lock: objects are immutable and a manifest appears with one atomic rename, so a reader sees either no
entry or a complete one.

## Recovery

Nothing is published until the final atomic manifest write. Cancelling, losing the connection,
running out of disk space or a crash before that leaves no entry; partially downloaded files stay in
`staging/` and are swept the next time the cache is changed under the lock. Objects written before
the interruption are harmless and are reused or collected later. "Download / Repair Baseline…"
re-verifies the project's entry with SHA-256 and downloads what is missing or wrong. If Mojang's
current metadata no longer produces the project's identity, Nova explains this and offers to switch
the project to the current official baseline; `.nova/project.json` changes only after confirmation
and a successful download.

## Removal

Manage Cached Baselines… lists each entry (version, identity prefix, date, files, size, bytes only
this entry uses, bytes shared with other entries, status). Remove asks for confirmation, warns when
the open project uses the entry, then under the lock deletes the entry's manifest and folder and
only the objects no remaining valid entry references. If any remaining manifest is unreadable, no
object is collected. Deletions are limited to regular files inside the cache root, never through a
link or junction. Projects are never changed by a removal.

## Privacy

- **Stored**: vanilla texture files, their manifest (paths, hashes, sizes, Mojang URLs and SHA-1
  values, the download time) and the lock file. No account, licence, launcher or personal data.
- **Hosts contacted** (HTTPS only, checked on every redirect hop): `piston-meta.mojang.com`
  (version manifest, version JSON, asset index), `piston-data.mojang.com` (client jar) and
  `resources.download.minecraft.net` (asset-index textures). The User-Agent is
  `NovaStudio-VanillaBaseline`. TLS uses the system trust store with peer verification.
- **No telemetry**: Nova Studio sends nothing about the user, the project or the cache anywhere.
- **Logs** contain status messages and file paths only, never image bytes.
- Vanilla textures are Mojang content: © Mojang Studios, downloaded from Mojang's official servers
  for local use. They are never added to a project or an exported pack unless the user edits and
  saves one. Nova Studio is not affiliated with or endorsed by Mojang or Microsoft.
