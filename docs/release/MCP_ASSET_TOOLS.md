# Nova Studio MCP: vanilla textures, asset relationships and validation

Nova Studio's MCP Phase 3 tools help an agent understand a resource pack before changing it. All of them are
**read-only**: they never change the pack, Nova's caches or any other file, never start a download, and never
accept a filesystem path. They work in read-only and editing connections alike and follow the same rules as every
other Nova tool: the user authorizes one project at a time, revocation and project switches end access, and each
call needs the `generation` and `projectId` from `nova.project.info`.

| Tool | What it answers |
| --- | --- |
| `nova.vanilla.list` | Which vanilla textures does Nova's verified local cache hold for a Minecraft version? |
| `nova.vanilla.texture` | Give me one cached vanilla texture (PNG), with its hash, size and provenance. |
| `nova.asset.relationships` | How is this pack asset selected and used: definitions, models, variables, faces, companions? |
| `nova.pack.validate` | What is wrong in the saved files *and* the open unsaved drafts, and how do I know? |

## Where the data comes from

Nova uses these sources, in this order of precedence. The first source that has a resource path provides it.

1. **Open drafts**: unsaved documents open in Nova (models are read from their live source).
2. **The project**: the authorized pack's own files.
3. **The resource stack**: folders the user added in Nova under **Models › Resource stack** for this project and
   Minecraft version, in their configured order. A folder made with `prepare-vanilla-cache.py` counts as a
   *verified vanilla resource source* only if its `nova-vanilla.json` matches the official client Nova pins for that
   version; otherwise Nova refuses it and reports it as unavailable.
4. **Nova's vanilla texture cache**: the per-user cache behind *Use Vanilla Baseline* (Resources › Vanilla
   Baseline). It holds **textures and their `.png.mcmeta` only**. It contains no models, item definitions,
   blockstates or equipment definitions and is never used as a model source.

Consequences an agent must respect:

- Vanilla *models* (for example `minecraft:item/generated` or `minecraft:block/cube_all`) are available only when
  the user configured a verified vanilla resource source. Without one, Nova reports links to them as unresolved or
  unchecked. It never guesses a model, never treats a texture as a cube, and never reads a Minecraft JAR, the
  `.minecraft` folder or any other location.
- If the cache has no entry for a version, Nova says so (`vanilla_not_cached`). Only the user can add a version, in
  Nova, which asks before downloading. An MCP request never downloads anything.
- Responses name resource paths (`assets/<namespace>/...`), stack positions and folder names. They never contain
  the location of the project, the cache or a resource-stack folder.

## Vanilla texture cache

### Verification and selection

A cache entry belongs to one Minecraft Java version and one *identity* (a hash of the official client and asset
index it came from). Nova uses an entry only if its manifest passes digest and field checks **and** every file it
lists exists with the recorded size. Entries that fail are never used, repaired or removed by these tools.

Which entry is used for a version:

1. the `identity` argument, if given;
2. otherwise the project's own baseline entry, when the project is bound to that version;
3. otherwise the newest complete verified entry for the version.

`selection` in each result says which rule applied. The version is always explicit: Nova never substitutes another
version, and `matchesProjectVersion` says whether it is the project's target.

### `nova.vanilla.list`

Arguments: `generation`, `projectId`, `version` (required); `identity`, `prefix` (under
`assets/minecraft/textures/`, such as `block/` or `item/`), `search` (substring), `animated` (only textures with
`.png.mcmeta`), `offset` (0–100000), `limit` (1–200, default 50).

Each texture row has `path`, `sha256`, `bytes`, `origin` (`jar` or `asset-index`), `width`, `height` (read from the
PNG header and not hash-checked; `null` if unreadable), `animationMetadata` and `projectOverride` (the project has
its own file at that path). `matched` counts all matching textures; `nextOffset` is `null` on the last page.
`verification` states exactly what was checked: `manifest: digest_verified`, `objects: present_with_recorded_sizes`.
Content hashes are checked when a texture is fetched.

Example (trimmed):

```json
{"name": "nova.vanilla.list", "arguments": {"generation": "…", "projectId": "…", "version": "1.21.11",
 "prefix": "block/", "limit": 2}}
```

```json
{
  "version": "1.21.11",
  "identity": "7251f4f6…5ea37b",
  "selection": "latest_verified",
  "matchesProjectVersion": true,
  "provenance": {"edition": "java", "publisher": "Mojang official metadata", "clientSha1": "…", "clientSize": 4096,
                 "assetIndexId": "29", "assetIndexSha1": "…", "versionJsonSha1": "…"},
  "textures": [{"path": "assets/minecraft/textures/block/dirt.png", "sha256": "50b59cf3…", "bytes": 107,
                "origin": "jar", "width": 16, "height": 16, "animationMetadata": false, "projectOverride": true}],
  "matched": 3,
  "nextOffset": 2,
  "verification": {"manifest": "digest_verified", "objects": "present_with_recorded_sizes",
                   "dimensions": "png_header_unverified", "content": "sha256_verified_by_nova.vanilla.texture"}
}
```

### `nova.vanilla.texture`

Arguments: `generation`, `projectId`, `version`, `path` (a cached `.png` path) required; `identity`; `include`:
`image` (default) or `metadata`.

Nova reads the file and checks its SHA-256 against the manifest before returning anything. With `include: image`
the PNG is returned as an MCP **image content block** (`type: image`, `mimeType: image/png`, base64 data) after the
JSON text block; the structured content holds the metadata only (`imageIncluded: true`). Animation metadata, when
present, is returned as parsed JSON in `animationMetadata.json` (at most 16,384 bytes; larger metadata is reported
as `omitted_too_large`, never cut).

**Response limit:** at most **262,144 bytes** of PNG inline. A larger texture is not truncated: the call fails with
`texture_too_large`, whose details give `bytes`, `width`, `height`, `maxImageBytes` and `alternative: "metadata"`.
Call again with `include: "metadata"` to get everything except the image.

Example result (structured content, trimmed; an image block follows the text block):

```json
{
  "path": "assets/minecraft/textures/block/stone.png",
  "sha256": "deab5053…dd8a294d", "bytes": 107, "width": 16, "height": 16, "origin": "jar",
  "mimeType": "image/png", "imageIncluded": true, "projectOverride": true, "animationMetadata": null,
  "verification": {"manifest": "digest_verified", "content": "sha256_verified"},
  "limits": {"maxImageBytes": 262144, "maxMetadataBytes": 16384}
}
```

## `nova.asset.relationships`

Arguments: `generation`, `projectId`, `path` (a resource-pack path) required; `version` (optional; must be the
project's target version, which is also the default, because the resource stack is loaded per version).

The result describes the asset (`kind`: `texture`, `animation_metadata`, `model`, `item_definition`, `blockstate`,
`equipment`, or another kind that is not analysed), every source that provides it (`providers`, first = selected),
the state of each source (`sources`), `relationships`, kind-specific `detail`, `limitations` and `truncated`.

### Relationship rows

Each row has `type`, `status`, usually `from` and `to` (resource paths), the `source` of the related asset, and
`evidence` (JSON pointers, contexts, variables, faces, parent chains). `reason` explains every status other than a
plain confirmation.

| `type` | Meaning | Evidence |
| --- | --- | --- |
| `selects_model`, `selected_by_item_definition`, `selected_by_blockstate` | A definition references a model | `pointer` to the `model` field, `context` (variant, select case, condition), parent chain, resolved textures |
| `parent_chain` | A model's parents, one step per model | each step's `id`, `path`, `source`, `status` |
| `model_uses_texture` | A resolved model samples the texture | variables with the model that `definedBy` them, faces (`element`, `face`, `uv`, `rotation`, `elementsDefinedBy`), or generated layers; `inherited` when a child model inherits it |
| `model_texture_binding`, `model_resolution` | A model binds the texture but cannot be resolved | the binding pointer; the failing chain |
| `variable_override` | A model inherits a parent that uses the texture but replaces the variable | `variable`, `inheritedFrom`, `replacedWith` |
| `texture_reference`, `child_model` | A model's resolved textures; models that use it as parent | |
| `animation_metadata`, `animation_texture` | `<texture>.png.mcmeta` beside its texture | frames, frame size and timing (Nova's parser for the version) in `detail` |
| `equipment_layer_texture` | An equipment layer's worn texture | `pointer`, `layer`, `texture`, path convention |
| `companion_asset` | A naming-convention suggestion (armor icon ↔ worn texture ↔ equipment) | why it is only a suggestion |

| `status` | Meaning |
| --- | --- |
| `confirmed` | Established from resolved data in the listed source. |
| `conditional` | Selected only in some game states (select, condition, range dispatch, blockstate variants, weighted alternatives, multipart conditions). Nova cannot know which applies, so all candidates are listed. This is how runtime ambiguity is reported. |
| `possible` | A suggestion from naming, or a path convention Nova has not verified for this version. Never treat it as fact. |
| `unresolved` | A referenced asset is not provided by any available source; `reason` says which and why. |
| `invalid` | The file cannot be read as the expected JSON or has an invalid reference. |
| `cyclic` | The parent chain returns to a model already in it. |
| `unsupported` | Nova does not resolve it (built-in or special renderers, chains longer than 64 models, unknown item model types). |

**Limits:** 200 relationship rows and about 160 KiB of rows per result (each result travels twice in one
512 KiB response line), 32 faces per relationship row and 64 in a model's `detail`, parent chains of 64 models, and
a 30-second deadline. `truncated: true` means rows were omitted; rows are never cut part-way. Relationships follow
Nova's model resolver; they are evidence about the data, not a guarantee of how Minecraft renders it.

**Drafts.** Open model drafts are read from their live source (`source: open_draft`). An open texture or animation
draft is noted on its provider (`unsavedDraft: true` with its `revision`), but its relationship data comes from the
saved file. Animation metadata from a different source than its texture is reported as `possible`.

**Catalog budget.** Nova reads at most 8,192 model, item-definition and blockstate files (32 MiB) across the project
and the resource stack. If a large stack exceeds this, `sources.modelCatalogComplete` is `false`, a limitation says
so, and models that were not read are reported as unknown (`unresolved` with that reason), never as missing.

### Example: an item texture

`assets/nova/textures/item/ruby.png` (trimmed):

```json
{
  "kind": "texture", "found": true, "providers": [{"source": "project", "selected": true}],
  "relationships": [
    {"type": "model_uses_texture", "status": "confirmed", "from": "assets/nova/models/item/ruby.json",
     "to": "assets/nova/textures/item/ruby.png", "source": {"source": "project"},
     "evidence": {"model": "nova:item/ruby", "variables": [{"variable": "layer0", "definedBy": "nova:item/ruby"}],
                  "faces": [{"generatedLayer": 0, "variable": "layer0", "uv": "whole texture (item/generated)"}],
                  "parentChain": ["minecraft:builtin/generated", "nova:item/base"], "inherited": false}},
    {"type": "selected_by_item_definition", "status": "confirmed", "from": "assets/nova/items/ruby.json",
     "to": "assets/nova/models/item/ruby.json", "evidence": {"pointer": "/model/model", "context": "item definition"}},
    {"type": "selected_by_item_definition", "status": "conditional", "from": "assets/nova/items/gem.json",
     "evidence": {"pointer": "/model/cases/0/model/model",
                  "context": "item definition / minecraft:display_context = \"gui\""},
     "reason": "Selected only in some game states; Nova cannot evaluate which one applies."}
  ]
}
```

### Example: unresolved and suggested links

A model whose parent is vanilla, with no verified vanilla resource source configured:

```json
{"type": "parent_chain", "status": "unresolved", "from": "assets/nova/models/item/vanilla_parent.json",
 "evidence": {"steps": [
   {"id": "nova:item/vanilla_parent", "path": "assets/nova/models/item/vanilla_parent.json",
    "source": {"source": "project"}, "status": "confirmed"},
   {"id": "minecraft:item/generated", "path": "assets/minecraft/models/item/generated.json", "status": "unresolved"}]},
 "reason": "The model minecraft:item/generated is not in the project or the configured resource stack. It may be a vanilla model, but no verified vanilla resource source is configured for Java 1.21.11, and Nova's vanilla texture cache contains textures only, so Nova cannot resolve it."}
```

A vanilla texture that only the cache provides:

```json
{"providers": [{"source": "vanilla_texture_cache", "verified": true, "selected": true}],
 "relationships": [{"type": "model_uses_texture", "status": "unresolved",
   "to": "assets/minecraft/textures/item/apple.png",
   "reason": "No model in the project or the configured resource stack references this texture. No verified vanilla resource source is configured, so vanilla models that may use it are unavailable; Nova's vanilla texture cache contains textures only. Nova does not guess a model."}]}
```

How to read these: `unresolved` means *Nova cannot establish the link from the available sources*, not that the
link is broken in game. Ask the user to add a verified vanilla resource source if the answer matters.
`conditional` rows are all real candidates; `possible` rows are hints to check, for example "this pack restyles
`item/ruby_chestplate.png`; `entity/equipment/humanoid/ruby.png` is usually its worn texture". Which equipment an
item uses is game data outside the resource pack, so that link is never confirmed.

## `nova.pack.validate`

Arguments: `generation` (required), `assetId` (validate one asset), `limit` (1–200 findings, default 100). The input
schema is unchanged from MCP Phase 1.

Validation covers the saved project files, every open unsaved draft (models from their live source, textures and
animations with Nova's native checks), the resource stack the user configured (to resolve references, reported per
source), and the verified texture cache for the project's version. It reads only; it never modifies a file and
never downloads. An open draft that cannot be read, or a stack source that cannot be loaded, is reported as a
finding, never skipped.

Each finding has:

| Field | Meaning |
| --- | --- |
| `severity` | `Error`, `Warning`, `Information` or `Unsupported` (Nova could not check it). |
| `rule` | Stable rule identifier (below). `code` keeps the Phase 1 identifier for compatibility. |
| `path`, `assetId` | Exact resource path (and its identifier when it is a project asset), or `null` for a stack source. |
| `source` | `disk`, `open_draft` or `resource_stack`. |
| `revision` | The open draft's revision for this connection, when the finding is about a draft. |
| `field`, `line` | JSON pointer and line inside the file, when known. |
| `evidence` | Data supporting the finding: references and what was searched, affected dependents, UVs, registry formats. |
| `message`, `nextStep` | A concise explanation and an actionable next step. |

The result also has `minecraftVersion`, `sources` (precedence, each stack source with status, whether a verified
vanilla model source is loaded, whether the model catalog is complete, the texture cache's status and identity),
`drafts` (included, unavailable, complete) and `limitations` (what could not be checked, and why). `counts` covers
every finding; `truncated: true` means some were not listed because of `limit` or the response budget (about 160 KiB
of findings).

| Rule | Severity | When |
| --- | --- | --- |
| `resource_path`, `unsafe_path`, `duplicate_asset` | Error | Invalid namespace or path; missing, linked or escaped file; case conflict |
| `pack_format`, `missing_pack_metadata` | Error | Invalid or missing `pack.mcmeta` |
| `pack_target` | Warning | The declared format range excludes the selected version (evidence: Nova's version registry) |
| `malformed_json`, `json_budget` | Error / Unsupported | Unreadable or too large JSON |
| `unresolved_parent`, `unresolved_model`, `missing_texture` | Error | A reference in the pack's own namespaces that no source provides |
| `parent_source_unavailable`, `model_source_unavailable` | Unsupported | A `minecraft:` model reference while no verified vanilla resource source is configured: unchecked, not an error |
| `model_catalog_incomplete` | Unsupported | A model reference while the model catalog budget was exceeded: unknown, not an error |
| `texture_in_vanilla_cache` | Information | A `minecraft:` texture reference provided by the verified texture cache |
| `texture_source_unavailable` | Unsupported | A `minecraft:` texture reference with no verified cache entry for the version |
| `parent_cycle_or_depth`, `unresolved_texture_variable`, `texture_variable_cycle`, `unresolved_face_texture`, `invalid_uv`, `invalid_texture_reference`, `face_texture_reference` | Error | Model resolution failures |
| `uv_outside_texture`, `uv_zero_area` | Warning | A face UV outside 0–16 or with zero width/height |
| `item_definition`, `blockstate`, `blockstate_rotation`, `blockstate_uvlock`, `invalid_reference`, `definition` | Error / Unsupported | Definition structure and model references |
| `animation`, `missing_spritesheet`, `animation_budget`, `texture_header`, `texture_size` | Error / Unsupported / Warning | Animation metadata and frame dimensions for the version; PNG headers |
| `live_texture_checked`, `live_animation_checked`, `live_texture_invalid`, `live_animation_invalid`, `unsaved_model` | Information / Error | Open drafts checked live |
| `draft_unavailable` | Warning | An open draft could not be read; only its saved file was checked |
| `draft_disk_conflict` | Warning | The file on disk changed after the draft was opened or saved |
| `stale_index_entry` | Warning | The file changed or disappeared after Nova indexed it |
| `resource_stack_unavailable` | Warning | A configured stack source could not be loaded or was refused |
| `unreferenced_texture` | Warning | A `block/` or `item/` texture in the pack's own namespace that no model references (it may still be used by game code) |
| `companion_suggestion` | Information | Java 1.21.11 only: an armor icon changed without its usual worn texture. Labelled a suggestion. |
| `preview_limitation` | Information | A limit of Nova's preview (flat generated items), not a pack problem |
| `catalog` | Unsupported | Model catalog or stack budgets exceeded |

`minecraft:` textures in `block/` and `item/` that no available model references are not reported unless a verified
vanilla resource source is configured; `limitations` gives their count instead.

### Example: validating with open unsaved drafts

```json
{"name": "nova.pack.validate", "arguments": {"generation": "…", "limit": 50}}
```

```json
{
  "minecraftVersion": "1.21.11",
  "drafts": {"complete": false,
             "included": [{"path": "assets/nova/models/block/flat.json", "type": "model", "revision": "…"}],
             "unavailable": [{"path": "assets/nova/textures/item/ruby.png", "reason": "a canvas interaction is in progress"}]},
  "diagnostics": [
    {"severity": "Warning", "rule": "draft_disk_conflict", "path": "assets/nova/textures/block/crystal.png",
     "source": "open_draft", "revision": "…",
     "message": "The file on disk changed after this texture was opened or last saved; the open draft and the saved file now differ independently.",
     "nextStep": "Ask the user to review both versions in Nova before saving; nova.drafts.save refuses to overwrite an external change.",
     "evidence": [{"draftRevision": "…", "diskChanged": true, "diskMissing": false}]},
    {"severity": "Warning", "rule": "uv_outside_texture", "path": "assets/nova/models/block/flat.json",
     "source": "open_draft", "revision": "…", "field": "/elements/0/faces/east/uv",
     "evidence": [{"pointer": "/elements/0/faces/east/uv", "uv": [-2, 0, 16, 16], "texture": "#all"}]},
    {"severity": "Unsupported", "rule": "parent_source_unavailable", "code": "model",
     "path": "assets/nova/models/item/vanilla_parent.json", "source": "disk",
     "evidence": [{"reference": "minecraft:item/generated", "searched": ["open_draft", "project", "resource_stack"],
                   "vanillaModelSource": false}]},
    {"severity": "Warning", "rule": "resource_stack_unavailable", "path": null, "source": "resource_stack",
     "message": "Resource stack source 1 (fakevanilla) is configured but unavailable: Vanilla cache does not match the selected official target version. Its files were not validated or used to resolve references."}
  ],
  "limitations": ["No verified vanilla resource source is configured for Java 1.21.11: references to vanilla models are reported as Unsupported (unchecked), not as errors. Nova's vanilla texture cache contains textures only."]
}
```

`nova.workflow.validate_changes` is unchanged: it validates selected project assets without the resource stack.

## Version-specific rules

Rules depend on the project's Minecraft version (`minecraftVersion`), and Nova never applies a 1.21.11 fact to
another version:

- **Model and definition resolution** uses Nova's profile for the version (1.21.11, 26.1, 26.3).
- **Pack format** checks use Nova's version registry (`evidence.source: nova_version_registry`).
- **Animation** checks use Nova's animation parser for the version.
- **Equipment and armor companions** use a path and naming convention Nova verified against the official 1.21.11
  client (the facts behind the `armor` guide). For other versions those links are `possible`, and validation does
  not produce `companion_suggestion`.
- Vanilla data is never assumed: without a verified vanilla resource source, Nova says it cannot check vanilla
  references.

## Errors

Errors are reported as the text block `{"error": {"code", "message", "details"}}` with `isError: true` and **no
structured content**, so a client never validates an error against a success schema. `details.pointer` names the
argument at fault.

| Code | Tools | Meaning |
| --- | --- | --- |
| `invalid_arguments` | all | An argument breaks the schema, or a path has empty, `.` or `..` segments |
| `stale_project` | all | The project changed or this connection's generation/projectId is not current |
| `unsupported_version` | vanilla, relationships | Nova does not support that Minecraft version |
| `vanilla_not_cached` | vanilla | No verified entry for that version (or identity). Nova never downloads; `details.nextStep` names the workflow in Nova |
| `vanilla_cache_invalid` | vanilla | Entries exist but every one is incomplete or failed verification; `details.entries`, `details.damaged` |
| `texture_not_in_cache` | `nova.vanilla.texture` | The path is not in the selected entry |
| `vanilla_integrity_failed` | `nova.vanilla.texture` | The cached file failed its SHA-256 check; nothing is returned or changed |
| `texture_too_large` | `nova.vanilla.texture` | Larger than 262,144 bytes inline; use `include: "metadata"` |
| `version_unavailable` | `nova.asset.relationships` | A `version` other than the project's target |
| `operation_canceled` | all | Canceled or past the deadline (30 s; 60 s for validation) |
| `busy`, `validation_unavailable` | all | Another request is running, or the index is updating: retry |

## Privacy and licensing

The tools return resource paths, hashes, dimensions and Mojang's public metadata hashes, never local paths. Cached
vanilla textures are the user's own local copies, made by Nova on the user's request from Mojang's official
servers; Nova does not include Mojang assets in its packages, source or documentation. The examples above use
synthetic test data.

## Exact tool definitions

These are the definitions `tools/list` returns (checked by the `mcp-relationships` test).

### `nova.vanilla.list`

```json
{
  "name": "nova.vanilla.list",
  "description": "List textures in Nova's verified local vanilla texture cache for one Minecraft version: path, size, SHA-256 and dimensions. Textures only (no models). Never downloads; reports vanilla_not_cached instead.",
  "annotations": {
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "animated": {
        "description": "Only textures with .png.mcmeta animation metadata.",
        "type": "boolean"
      },
      "generation": {
        "description": "The generation from nova.project.info.",
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "identity": {
        "description": "A cache entry identity from nova.vanilla.list; defaults to the project's baseline entry for that version, else the newest verified one.",
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "limit": {
        "maximum": 200,
        "minimum": 1,
        "type": "integer"
      },
      "offset": {
        "maximum": 100000,
        "minimum": 0,
        "type": "integer"
      },
      "prefix": {
        "description": "Only paths under assets/minecraft/textures/<prefix>, such as block/ or item/.",
        "maxLength": 128,
        "pattern": "^[a-z0-9_./-]*$",
        "type": "string"
      },
      "projectId": {
        "description": "The projectId from nova.project.info.",
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "search": {
        "description": "Only paths containing this text.",
        "maxLength": 64,
        "pattern": "^[a-z0-9_./-]+$",
        "type": "string"
      },
      "version": {
        "description": "A Minecraft Java version such as 1.21.11.",
        "maxLength": 16,
        "pattern": "^[0-9]{1,3}(\\.[0-9]{1,3}){1,2}$",
        "type": "string"
      }
    },
    "required": [
      "generation",
      "projectId",
      "version"
    ],
    "type": "object"
  },
  "outputSchema": {
    "additionalProperties": false,
    "properties": {
      "cached": {
        "type": "string"
      },
      "entryFiles": {
        "type": "integer"
      },
      "generation": {
        "type": "string"
      },
      "identity": {
        "type": "string"
      },
      "matched": {
        "type": "integer"
      },
      "matchesProjectVersion": {
        "type": "boolean"
      },
      "nextOffset": {
        "type": [
          "integer",
          "null"
        ]
      },
      "projectId": {
        "type": "string"
      },
      "projectVersion": {
        "type": "string"
      },
      "provenance": {
        "type": "object"
      },
      "selection": {
        "type": "string"
      },
      "textures": {
        "type": "array"
      },
      "verification": {
        "type": "object"
      },
      "version": {
        "type": "string"
      }
    },
    "required": [
      "projectId",
      "generation",
      "version",
      "identity",
      "selection",
      "cached",
      "projectVersion",
      "matchesProjectVersion",
      "provenance",
      "verification",
      "textures",
      "matched",
      "entryFiles",
      "nextOffset"
    ],
    "type": "object"
  }
}
```

### `nova.vanilla.texture`

```json
{
  "name": "nova.vanilla.texture",
  "description": "Return one texture from Nova's verified local vanilla texture cache as a PNG image block, with its SHA-256 (checked on read), dimensions, animation metadata and provenance. Never downloads.",
  "annotations": {
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "generation": {
        "description": "The generation from nova.project.info.",
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "identity": {
        "description": "A cache entry identity from nova.vanilla.list; defaults to the project's baseline entry for that version, else the newest verified one.",
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "include": {
        "description": "image (default): include the PNG (at most 262144 bytes); metadata: omit it.",
        "enum": [
          "image",
          "metadata"
        ],
        "type": "string"
      },
      "path": {
        "description": "A cached texture path from nova.vanilla.list, such as assets/minecraft/textures/block/stone.png.",
        "maxLength": 256,
        "pattern": "^assets/minecraft/textures/[a-z0-9_./-]+\\.png$",
        "type": "string"
      },
      "projectId": {
        "description": "The projectId from nova.project.info.",
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "version": {
        "description": "A Minecraft Java version such as 1.21.11.",
        "maxLength": 16,
        "pattern": "^[0-9]{1,3}(\\.[0-9]{1,3}){1,2}$",
        "type": "string"
      }
    },
    "required": [
      "generation",
      "projectId",
      "version",
      "path"
    ],
    "type": "object"
  },
  "outputSchema": {
    "additionalProperties": false,
    "properties": {
      "animationMetadata": {
        "type": [
          "object",
          "null"
        ]
      },
      "bytes": {
        "type": "integer"
      },
      "cached": {
        "type": "string"
      },
      "generation": {
        "type": "string"
      },
      "height": {
        "type": [
          "integer",
          "null"
        ]
      },
      "identity": {
        "type": "string"
      },
      "imageIncluded": {
        "type": "boolean"
      },
      "limits": {
        "type": "object"
      },
      "matchesProjectVersion": {
        "type": "boolean"
      },
      "mimeType": {
        "type": "string"
      },
      "origin": {
        "type": "string"
      },
      "path": {
        "type": "string"
      },
      "projectId": {
        "type": "string"
      },
      "projectOverride": {
        "type": "boolean"
      },
      "projectVersion": {
        "type": "string"
      },
      "provenance": {
        "type": "object"
      },
      "selection": {
        "type": "string"
      },
      "sha256": {
        "type": "string"
      },
      "verification": {
        "type": "object"
      },
      "version": {
        "type": "string"
      },
      "width": {
        "type": [
          "integer",
          "null"
        ]
      }
    },
    "required": [
      "projectId",
      "generation",
      "version",
      "identity",
      "selection",
      "cached",
      "projectVersion",
      "matchesProjectVersion",
      "provenance",
      "verification",
      "path",
      "sha256",
      "origin",
      "mimeType",
      "bytes",
      "width",
      "height",
      "imageIncluded",
      "projectOverride",
      "limits",
      "animationMetadata"
    ],
    "type": "object"
  }
}
```

### `nova.asset.relationships`

```json
{
  "name": "nova.asset.relationships",
  "description": "Explain how one resource-pack asset is selected and used: item definitions and blockstates, model parent chains, texture variables, faces and UVs, display contexts, animation metadata and companion assets, each with its source (project, open draft, resource stack) and evidence. Unresolvable links are reported, never guessed.",
  "annotations": {
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "generation": {
        "description": "The generation from nova.project.info.",
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "path": {
        "description": "A resource-pack path such as assets/minecraft/textures/item/diamond_sword.png.",
        "maxLength": 256,
        "pattern": "^assets/[a-z0-9_.-]+/[a-z0-9_./-]+$",
        "type": "string"
      },
      "projectId": {
        "description": "The projectId from nova.project.info.",
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "version": {
        "description": "The Minecraft version; must be the project's target version (the default).",
        "maxLength": 16,
        "pattern": "^[0-9]{1,3}(\\.[0-9]{1,3}){1,2}$",
        "type": "string"
      }
    },
    "required": [
      "generation",
      "projectId",
      "path"
    ],
    "type": "object"
  },
  "outputSchema": {
    "additionalProperties": false,
    "properties": {
      "detail": {
        "type": "object"
      },
      "found": {
        "type": "boolean"
      },
      "generation": {
        "type": "string"
      },
      "kind": {
        "type": "string"
      },
      "limitations": {
        "type": "array"
      },
      "minecraftVersion": {
        "type": "string"
      },
      "path": {
        "type": "string"
      },
      "projectId": {
        "type": "string"
      },
      "providers": {
        "type": "array"
      },
      "relationships": {
        "type": "array"
      },
      "sources": {
        "type": "object"
      },
      "truncated": {
        "type": "boolean"
      },
      "versionSource": {
        "type": "string"
      }
    },
    "required": [
      "generation",
      "projectId",
      "path",
      "kind",
      "minecraftVersion",
      "versionSource",
      "found",
      "providers",
      "relationships",
      "detail",
      "sources",
      "limitations",
      "truncated"
    ],
    "type": "object"
  }
}
```

### `nova.pack.validate`

```json
{
  "name": "nova.pack.validate",
  "description": "Run read-only validation of saved files and open unsaved drafts. References resolve through the project, the resource stack the user configured, and Nova's verified vanilla texture cache (textures only); each finding has a rule, source, evidence and next step. Never downloads.",
  "annotations": {
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "assetId": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "generation": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "limit": {
        "maximum": 200,
        "minimum": 1,
        "type": "integer"
      }
    },
    "required": [
      "generation"
    ],
    "type": "object"
  },
  "outputSchema": {
    "additionalProperties": false,
    "properties": {
      "counts": {
        "type": "object"
      },
      "diagnostics": {
        "type": "array"
      },
      "drafts": {
        "type": "object"
      },
      "files": {
        "type": "integer"
      },
      "generation": {
        "type": "string"
      },
      "inspected": {
        "type": "integer"
      },
      "limitations": {
        "type": "array"
      },
      "milliseconds": {
        "type": "integer"
      },
      "minecraftVersion": {
        "type": "string"
      },
      "sources": {
        "type": "object"
      },
      "truncated": {
        "type": "boolean"
      }
    },
    "required": [
      "drafts",
      "minecraftVersion",
      "sources",
      "limitations",
      "generation",
      "diagnostics",
      "counts",
      "files",
      "inspected",
      "milliseconds",
      "truncated"
    ],
    "type": "object"
  }
}
```
