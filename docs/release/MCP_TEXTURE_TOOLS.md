# Texture authoring tools (MCP)

Four MCP tools change many pixels of an open texture in one call:

| Tool | Does | Layer | Selection |
| --- | --- | --- | --- |
| `nova.texture.put_pixels` | Writes a rectangle of pixels from a palette and index rows | active layer only | clips; partial coverage mixes |
| `nova.texture.palette_remap` | Replaces colours that match a list of mappings | any visible layer | clips; partial coverage mixes |
| `nova.texture.gradient_map` | Recolours pixels by luma through gradient stops | any visible layer | clips; partial coverage mixes |
| `nova.texture.erase` | Makes a region transparent, like Nova's eraser | active layer only | clips; partial coverage erases partially |

They need an editing authorization (**Allow editing…** in Resources › MCP Integration). Read-only
connections get `permission_denied`.

**Edited, not saved.** Each tool changes the open document in Nova Studio's memory as one undoable
step. Nothing is written to the pack. The result's `dirty: true` means the document now has unsaved
changes; a result never says an edit was saved. To save, use `nova.drafts.save` /
`nova.drafts.save_all`, or Save / Save All in Nova Studio (see [MCP_SETUP.md](MCP_SETUP.md)).

## Common rules

**Target.** Every call names an open texture with the same four fields as `nova.texture.edit`:

| Field | Meaning |
| --- | --- |
| `generation`, `projectId` | From `nova.project.info` for this connection. A project switch or another connection's values return `stale_project`. |
| `assetId` | The document's id for this connection, from `nova.drafts.list` or `nova.assets.list`. Only indexed, open textures of the authorized pack qualify; anything else returns `document_not_open`. No filesystem path is accepted anywhere. |
| `expectedRevision` | The current `revision` from `nova.asset.open`, `nova.texture.read` on a live document, `nova.edit.capabilities`, `nova.drafts.list` or the previous edit's result. A changed document returns `revision_conflict`. |
| `layer` (optional) | A layer id from the document state. Defaults to the active layer. |
| `returnPreview` (optional) | Set to `true` to include a result PNG in the same call. |

Use `nova.asset.open` to get the edit revision for a saved texture that isn't open. Nova adds the
document to a background tab and records the open in **Review changes**.

**Documents.** Textures up to 256 × 256 pixels and 4 MiB of layer pixels (larger documents return
`edit_limit`). Animated textures: the edit applies to the active frame. Model documents return
`invalid_asset_type`.

**Coordinates.** Pixels, origin at the top-left, x to the right, y down. A rectangle is
`{x, y, width, height}` with `x`, `y` 0–255 and `width`, `height` 1–256; `x + width` and `y + height`
may not exceed the canvas (checked in 64-bit arithmetic, so no value can overflow).

**Colours.** `#RRGGBB` (alpha FF) or `#RRGGBBAA`, hexadecimal, case-insensitive. The alpha byte comes
last, as in CSS: `#ff000080` is red at alpha 128. All arithmetic is integer, on straight
(non-premultiplied) 8-bit encoded sRGB channels; there is no colour-space conversion and no floating
point, so results are identical on every platform and build.

**Zero alpha.** A pixel that a tool writes with alpha 0 is stored as `#00000000` (RGB 0), as Nova's
eraser and Clear do. Pixels a tool does not write keep their stored bytes, including any RGB under
zero alpha.

**Layers.** `put_pixels` and `erase` paint, so — like `fill_rect` and strokes — they only change the
active layer; naming another layer returns `edit_rejected` ("Select the target layer before drawing").
`palette_remap` and `gradient_map` are adjustments and may target any visible layer; as with Nova's
Adjust operations, the edited layer becomes the active layer (the result's `activeLayer` shows it).
Hidden layers return `edit_rejected` ("Show the selected layer before editing it"). Other layers never
change.

**Selection.** Nova's selection is a coverage mask (0–255 per pixel). With no selection, the whole
region is edited. Selected pixels (255) take the full result; unselected pixels (0) do not change; an
empty selection changes nothing. For `put_pixels`, `palette_remap` and `gradient_map`, partial
coverage *k* mixes each straight RGBA channel exactly as Nova's Adjust operations do:
`(result × k + original × (255 − k) + 127) / 255`. For `erase`, coverage is the eraser strength
(below).

**One step, all or nothing.** Every call is validated completely — schema, then everything that needs
the canvas (region bounds, row widths, palette indices, stop order, duplicate mappings, the layer) —
before anything changes. The edit is then built on an isolated copy of the document and published as
one history step; a failure at any point (an argument, the history budget, a document change, a
cancellation with `notifications/cancelled` before publication) leaves every pixel unchanged.
`nova.edit.undo` / `nova.edit.redo` (and Undo / Redo in Nova Studio) restore the exact previous and
resulting pixels. An edit that changes no pixel succeeds with `changed: false` and adds no history
step.

**Recovery and drafts.** The edited document is an ordinary unsaved draft: `nova.drafts.list` shows
it with `dirty: true`, `nova.pack.validate` includes it, Nova's recovery checkpoints protect it, and
Save / `nova.drafts.save` write it to its own resource path. A failed save (for example an external
change, `external_change`) keeps the draft and its pixels.

**Limits.** A request is one JSON-RPC line of at most 64 KiB (`-32600` otherwise); `put_pixels`
therefore writes at most about 60,000 pixels per call — for example 256 × 128, so a 256 × 256 texture
takes two calls. Palettes have at most 62 colours, remaps at most 64 mappings, gradients 2–16 stops.
Every list and string is bounded by the schema before it is read, and nothing is allocated from a
size the caller states until it has been checked against the canvas.

**Privacy.** Results identify resources by pack-relative path. Pixel data is available through the
read and preview tools, or by opting into a result PNG. Nova does not log tool arguments or image
payloads.

## Result

Every tool returns the usual edit result (`revision`, `dirty`, `undoAvailable`, `redoAvailable`,
`width`, `height`, `layers`, `frames`, `activeLayer`, `activeFrame`, `animated`, …, `transactionId`,
`appliedOperations`, `changed`, `warnings`, `changeSummary`) plus:

| Field | Meaning |
| --- | --- |
| `documentId` | The document's id across connections, as in `nova.drafts.list`. |
| `path` | Its resource-pack path, such as `assets/minecraft/textures/item/stick.png`. |
| `assetId` | The id used in the call. |
| `layer` | The layer the tool addressed. |
| `region` | The rectangle the tool addressed (the whole canvas when none was given). |
| `changedBounds` | The smallest rectangle containing every changed pixel of that layer, or `null`. |
| `pixelsChanged` | How many pixels of that layer changed (stored bytes compared). |

Use the returned `revision` as `expectedRevision` for the next edit.

Edit tools accept `include: "summary"` to omit document details such as dimensions, layers and
frames from the result. The default `include: "document"` returns the usual state. Both forms keep
the new revision, draft status and undo availability. On `nova.workflow.execute`, `include` applies
to every stage.

Successful tool replies contain the result in `structuredContent` and a compact JSON summary in
the text block. Clients that only consume text can read the summary's IDs and revisions, or pass
`verbosity: "full"` to get the complete result as JSON text. `verbosity` also works on read tools.

## Read exact pixels

`nova.texture.read` accepts `generation` and a pack `assetId` from `nova.assets.list`. For a vanilla
reference, use an ID such as `vanilla:block/stone.png` or
`vanilla:assets/minecraft/textures/block/stone.png`. Vanilla reads use Nova's verified local cache
for the project version by default. Pass `version` or `identity` to select a different cached entry.

- An open texture defaults to its visible composite and active frame. Specify a `layer` UUID to
  read its stored pixels, or a zero-based `frame` number to select a different source frame.
- A closed texture defaults to the saved PNG. An explicit `frame` reads a source frame from the
  PNG and its animation metadata. Frame numbers follow source order.
- Use a `region` of `{x, y, width, height}` to read up to 256 × 256 pixels inside the selected image.
- With up to 62 colours, `pixels` contains the rectangle, `palette` and `rows` for
  `nova.texture.put_pixels`. Use the output location or set the destination `x` and `y` when copying
  into another texture.
- With more than 62 colours, `encoding` is `hex`. Each row concatenates eight hex digits
  `RRGGBBAA` per pixel. Read a smaller region if exact hex rows exceed the response budget.
  `colorCount` reports the full count, and `paletteSummary` reports up to 62 colours with their luma
  and pixel counts, sorted darkest first.

## Preview changes

`nova.asset.preview_image` takes an open asset's `assetId` and `expectedRevision` along with
`generation` and `projectId`. A texture preview uses nearest-neighbour scaling to a 256-pixel
longest edge by default. Set `maxEdge` from 16 to 512 to choose a size. Set `pixelGrid` to draw
texel boundaries when each texel has at least four preview pixels per side.

Set `compare` to get the before and after captures for the latest edit of the requested revision in
this connection. For an animation, the comparison shows the frame captured by that edit. Pass
`assetIds` with up to 16 texture IDs to get a contact sheet from open drafts and saved PNGs. Each
tile defaults to a 128-pixel longest edge, capped at 192 pixels. The response lists the tiles in
request order with their labels and asset paths.

Direct edit calls support `returnPreview: true` to append the new image. The response's `preview`
object reports the image dimensions or an error for unavailable or oversized images. Preview
encoding happens after the edit has committed.

## `nova.texture.put_pixels`

Writes the rectangle `x, y, width, height` from `palette` and `rows`.

- `rows` has exactly `height` strings, top to bottom; each has exactly `width` characters, left to
  right.
- Each character is a palette index — `0`–`9` are 0–9, `a`–`z` are 10–35, `A`–`Z` are 36–61 — or `.`
  to leave that pixel unchanged. An index at or beyond the palette length is an error.
- `mode` `replace` (default) sets each pixel to the palette colour exactly, alpha included (a
  palette alpha of 00 stores `#00000000`). `over` composites the palette colour over the pixel with
  Nova's straight-alpha source-over, rounded: `a = alpha`, `w = dstAlpha × (255 − a)`,
  `channel = (src × a × 255 + dst × w + d / 2) / d` with `d = a × 255 + w`, `alpha = (d + 127) / 255`.

Example — a 4 × 2 patch with a transparent pixel and two kept pixels:

```json
{"generation": "…", "projectId": "…", "assetId": "…", "expectedRevision": "…",
 "x": 2, "y": 3, "width": 4, "height": 2,
 "palette": ["#ff0000", "#0000ff80", "#00000000"],
 "rows": ["0..1", "1200"]}
```

Result (abridged):

```json
{"documentId": "…", "path": "assets/nova/textures/item/plain.png", "region": {"x": 2, "y": 3, "width": 4, "height": 2},
 "changedBounds": {"x": 2, "y": 3, "width": 4, "height": 2}, "pixelsChanged": 6, "changed": true, "dirty": true,
 "undoAvailable": true, "revision": "…"}
```

A 16 × 16 icon is one call of 16 rows of 16 characters plus its palette.

## `nova.texture.palette_remap`

Replaces matching colours inside `region` (default: the whole canvas); unmatched pixels are
unchanged.

- Each pixel is compared with its **original** colour, so `A→B` and `B→A` in one call swap the two
  colours instead of chaining. When several mappings match, the first in the list wins.
- `from` `#RRGGBB` matches visible pixels (alpha 1–255) whose red, green and blue each differ from it
  by at most `tolerance` (default 0: exact). The pixel's alpha is not compared.
- `from` `#RRGGBBAA` also compares alpha with the same tolerance. Fully transparent pixels compare as
  `#00000000` whatever RGB they store, so `#00000000` matches every transparent pixel.
- `to` `#RRGGBB` sets red, green and blue and keeps the pixel's alpha. `to` `#RRGGBBAA` sets all four.
  A `from` with alpha 00 needs an 8-digit `to` (otherwise the result would stay invisible).
- The same `from` twice is an error.

```json
{"generation": "…", "projectId": "…", "assetId": "…", "expectedRevision": "…",
 "mappings": [{"from": "#5a3c1e", "to": "#3c5a1e"}, {"from": "#3c5a1e", "to": "#5a3c1e"},
              {"from": "#ffffff", "to": "#00000000"}],
 "tolerance": 0}
```

## `nova.texture.gradient_map`

Recolours each pixel inside `region` (default: the whole canvas) by its luma.

- `source` is `luma709` (the only value): `luma = (2126 R + 7152 G + 722 B + 5000) / 10000`, integer,
  on the encoded 8-bit sRGB channels, 0–255. This is the same luma as Nova's Grayscale operation.
- `stops` (2–16) have strictly increasing `position` values 0–255. A luma at or below the first stop
  takes its colour; at or above the last stop, the last colour.
- `interpolation` `linear` (default): between stops *p0* < *p1*, each channel is
  `(2 × (c0 × (p1 − luma) + c1 × (luma − p0)) + (p1 − p0)) / (2 × (p1 − p0))` — rounded integer
  interpolation of the encoded values. `step`: the colour of the last stop at or below the luma.
- `alpha` `preserve` (default): each pixel keeps its alpha; stops must be `#RRGGBB`. `multiply`: the
  result's alpha is `(pixel alpha × interpolated stop alpha + 127) / 255`; stops may be `#RRGGBBAA`.
- Fully transparent pixels are never changed (they are not made visible).

```json
{"generation": "…", "projectId": "…", "assetId": "…", "expectedRevision": "…",
 "stops": [{"position": 0, "color": "#1b0f2e"}, {"position": 128, "color": "#7a3fb0"},
           {"position": 255, "color": "#f2e6ff"}],
 "interpolation": "linear", "alpha": "preserve"}
```

## `nova.texture.erase`

Makes `region` of the active layer transparent, clipped by the selection — Nova's eraser:

- coverage = `255 × selection / 255 × strength / 255` (integer, `strength` 1–255, default 255);
- new alpha = `alpha × (255 − coverage) / 255`; RGB is kept while alpha stays above 0;
- a pixel reaching alpha 0 is stored as `#00000000` (this also clears RGB stored under pixels that
  were already transparent inside the region).

It never deletes files, layers or frames. `region` is required; to clear the whole layer, pass the
full canvas.

```json
{"generation": "…", "projectId": "…", "assetId": "…", "expectedRevision": "…",
 "region": {"x": 0, "y": 12, "width": 16, "height": 4}, "strength": 255}
```

## Native adjustments and generators

Use `nova.texture.edit` with an `operation` such as
`{"command":"hue_saturation","params":{"hue":-25,"saturation":10}}`. The same
commands work in `nova.edit.batch`, `nova.edit.preview`, and workflow stages. Each
operation accepts only the parameters that `tools/list` shows under `nova.edit.batch`
(the listing names the other tools' operations by command only). Optional parameters
use Nova's editor defaults. Colours use the same CSS order as the compact tools.

- Colour and tone: `hue_saturation`, `levels`, `colorize`, `quantize`, `palette_map`.
  `levels` requires `input_white` to exceed `input_black`. `palette_map` requires
  an explicit `palette` of 1–256 colours.
- Effects: `outline`, `directional_shading`, `seamless`. For a heightmap or normal
  map source, set `mapLayer` to a layer in the active frame. Set `output` to
  `Modify pixels` to shade the target in place. The default adds a shading layer.
- Generators: `gen_stone`, `gen_bricks`, `gen_tiles`, `gen_wood`, `gen_ore`,
  `gen_metal`, `gen_gui_panel`, `gen_item`, `gen_noise`. They render at the current
  canvas size and replace pixels on the target layer.
- `resize_image` requires `params.width` and `params.height` (1–256). It resizes
  every layer and frame together and uses nearest-neighbour resampling by default.

Except for `resize_image`, choose an optional visible `layer` in the active frame
and an optional `region` inside the canvas. Nova computes the effect at canvas
size and clips the result to the region and the active selection.

## Errors

Errors are tool results with `isError: true` and a text block `{"error": {"code", "message",
"details"?}}`; they never carry `structuredContent`. Argument errors name the JSON pointer of the
field in the message and in `details.pointer`, and never echo the value.

| Code | When | Example message |
| --- | --- | --- |
| `invalid_arguments` | Schema or canvas check failed | `Invalid argument at /rows/1: column 3 uses palette index 4, but the palette has 2 entries.` |
| | | `Invalid argument at /width: the region extends past the canvas width (16 pixels); x + width must be at most 16.` |
| | | `Invalid argument at /rows: expected 16 rows (one per pixel row of the region), got 15.` |
| | | `Invalid argument at /mappings/1/from: the same colour is already mapped by /mappings/0/from.` |
| | | `Invalid argument at /stops/1/position: stop positions must increase strictly.` |
| | | `Invalid argument at /layer: the layer is not in this document.` |
| | | `Missing required argument /region.` |
| `permission_denied` | Read-only connection | `This connection has read-only access.` |
| `stale_project` | Wrong `generation` or `projectId`, project switched, or authorization ended | `The generation does not match this connection. Call nova.project.info and use its generation.` |
| `revision_conflict` | `expectedRevision` is not current, or the document changed before publication | `The document changed. Obtain its current revision.` |
| `document_not_open` | `assetId` is not an open texture of this pack | `The identifier must refer to an indexed open document.` |
| `edit_rejected` | The editor refused the edit: inactive or hidden layer, history budget | `Select the target layer before drawing` |
| `edit_limit` | Document larger than 256 × 256 or 4 MiB | `The document exceeds the editing budget.` |
| `document_busy` | A canvas interaction or dialog is in progress | `Finish the current canvas interaction first.` |
| `operation_canceled` | The client cancelled before publication | `Publication stopped at the next document boundary.` |
| `busy` | Another MCP operation of this connection is running | `Another MCP operation is still running.` |

A revoked authorization closes the connection; no further call reaches Nova Studio.

## Combine edits and recolour multiple textures

In `nova.edit.batch`, `nova.edit.preview` or a workflow stage, use `put_pixels`, `palette_remap`,
`gradient_map` and `erase` as `command` values. Copy each standalone tool's pixel arguments into
the command object. The enclosing batch or stage supplies the target and revision. Commands see
the canvas left by preceding commands, and the whole batch is one undoable step per document.

Use `nova.texture.adjust_bulk` to apply up to eight shared operations to 1–64 distinct pack textures.
It accepts `palette_remap`, `gradient_map`, `hue_saturation`, `levels`, `colorize`, `palette_map`
and `quantize`. Each target uses its own active visible layer and frame, and an optional per-operation
region clips against that target's selection. For example, pass these fields with `generation` and
`projectId`:

```json
{"targets":[{"assetId":"…","expectedRevision":"current"}],
 "operations":[{"command":"palette_remap","mappings":[{"from":"#808080","to":"#325681"}]}]}
```

Use a live revision to require an open document at that revision. The explicit value `current` takes
the revision at preparation and opens a background tab when needed, within Nova's 32-document opening
limit. Each asset commits independently in input order. A failed asset leaves its pixels unchanged
and may leave a newly opened tab. The remaining assets still run. Results report `committed` or
`rejected` with an error code such as `revision_conflict`, `open_limit` or `edit_rejected`.
Cancellation, a project switch or the 30-second deadline stops at an asset boundary and marks the
remaining targets `not_run`. Earlier commits stay available for individual undo and save.

## SDK wire measurements

The SDK fixture uses 16 × 16 textures. Counts are compact UTF-8 JSON-RPC lines; the older
rounds also sampled `o200k_base` tokens, but the SDK virtual environment used for measurement
has no tokenizer installed, so the latest round reports bytes only. Transaction IDs and
revisions vary between runs. The edit compares a one-pixel `put_pixels` with default document
state before and `include: "summary"` after.

| Measurement | Before bytes / tokens | After bytes / tokens |
| --- | ---: | ---: |
| `tools/list`, expanded schemas before deduplication | 239,176 / 68,282 | 187,042 / 50,371 |
| `tools/list`, brief preview and workflow operations | 187,042 / 50,371 | 121,095 / not resampled |
| `tools/list`, trimmed listing, measured by `--dump-tools` (see below) | 121,096 / — | 77,391 / — |
| One edit reply | 2,868 / 1,148 | 1,513 / 566 |
| Edit text block | 1,321 / 556 | 458 / 173 |
| Five-texture recolour requests | 4,060 / 1,815 | 908 / 386 |
| Five-texture recolour replies | 24,240 / 9,525 | 3,296 / 1,292 |

The recolour compares five separate open-and-remap pairs (10 calls) with one bulk call. The
original 42-tool listing was 113,065 bytes. The expanded measurement includes the new command
schemas, and the final listing also includes the analyzer.

The trimmed listing (`nova-mcp --dump-tools <file>` prints the per-tool bytes) removes, from
`tools/list` only:

- the spec-optional `outputSchema` (MCP 2025-11-25 keeps it optional; the official SDKs pass
  `structuredContent` through unvalidated when a tool lists none, and Nova's replies are
  unchanged — 24.7 KB of the 121 KB listing was repeated output schemas);
- `nova.texture.edit` operations and `nova.texture.adjust_bulk` operations, and
  `nova.asset.create_preview` parameters, each named by command only where the full command
  set repeats `nova.edit.batch` or `nova.asset.create`.

`nova.edit.preview`, `nova.workflow.plan`, `nova.workflow.execute`, `nova.texture.edit` and
`nova.texture.adjust_bulk` list each operation by its `command` name only, because their full
schemas repeat `nova.edit.batch`. Nova still checks their calls against the full command
schemas, so an invalid operation fails with the same pointer as in a batch.

## Exact definitions

These are the compact tools' complete definitions. `tools/list` publishes their input schemas
exactly as shown and omits the spec-optional `outputSchema` (the `texture-pixels` test compares
these definitions with the full set Nova validates calls against, which keeps the output
schemas).

### `nova.texture.put_pixels`

```json
{
  "name": "nova.texture.put_pixels",
  "description": "Write a rectangle of pixels from a palette in one edit. rows[r] has one character per pixel: a palette index (0-9 = 0..9, a-z = 10..35, A-Z = 36..61) or '.' to keep that pixel. Replaces RGBA exactly unless mode is \"over\". Edits the open document in memory; it is not saved.",
  "annotations": {
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "assetId": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "expectedRevision": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "generation": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "height": {
        "maximum": 256,
        "minimum": 1,
        "type": "integer"
      },
      "layer": {
        "description": "A layer id from the document state; defaults to the active layer.",
        "maxLength": 36,
        "pattern": "^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$",
        "type": "string"
      },
      "mode": {
        "description": "replace (default): set each pixel to the palette RGBA exactly; over: composite the palette colour over the pixel.",
        "enum": [
          "replace",
          "over"
        ],
        "type": "string"
      },
      "palette": {
        "description": "Up to 62 colours #RRGGBB or #RRGGBBAA.",
        "items": {
          "description": "A colour #RRGGBB (opaque) or #RRGGBBAA (straight alpha, CSS order).",
          "maxLength": 9,
          "pattern": "^#[A-Fa-f0-9]{6}([A-Fa-f0-9]{2})?$",
          "type": "string"
        },
        "maxItems": 62,
        "minItems": 1,
        "type": "array"
      },
      "projectId": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "rows": {
        "description": "One string per pixel row, top to bottom; height rows of width characters.",
        "items": {
          "maxLength": 256,
          "pattern": "^[0-9A-Za-z.]+$",
          "type": "string"
        },
        "maxItems": 256,
        "minItems": 1,
        "type": "array"
      },
      "width": {
        "maximum": 256,
        "minimum": 1,
        "type": "integer"
      },
      "x": {
        "maximum": 255,
        "minimum": 0,
        "type": "integer"
      },
      "y": {
        "maximum": 255,
        "minimum": 0,
        "type": "integer"
      },
      "returnPreview": {
        "type": "boolean",
        "description": "Append an upscaled PNG of the result."
      },
      "include": {
        "description": "summary omits document dimensions, layers, frames and other editor state.",
        "enum": [
          "document",
          "summary"
        ]
      },
      "verbosity": {
        "enum": [
          "summary",
          "full"
        ]
      }
    },
    "required": [
      "x",
      "y",
      "width",
      "height",
      "palette",
      "rows",
      "generation",
      "projectId",
      "assetId",
      "expectedRevision"
    ],
    "type": "object"
  },
  "outputSchema": {
    "additionalProperties": false,
    "properties": {
      "activeFrame": {
        "type": "string"
      },
      "activeLayer": {
        "type": "string"
      },
      "animated": {
        "type": "boolean"
      },
      "appliedOperations": {
        "type": "integer"
      },
      "assetId": {
        "type": "string"
      },
      "changeSummary": {
        "type": "object"
      },
      "changed": {
        "type": "boolean"
      },
      "changedBounds": {
        "oneOf": [
          {
            "additionalProperties": false,
            "properties": {
              "height": {
                "type": "integer"
              },
              "width": {
                "type": "integer"
              },
              "x": {
                "type": "integer"
              },
              "y": {
                "type": "integer"
              }
            },
            "required": [
              "x",
              "y",
              "width",
              "height"
            ],
            "type": "object"
          },
          {
            "type": "null"
          }
        ]
      },
      "defaultTicks": {
        "type": "integer"
      },
      "dirty": {
        "type": "boolean"
      },
      "documentId": {
        "type": "string"
      },
      "elementCount": {
        "type": "integer"
      },
      "frameCount": {
        "type": "integer"
      },
      "frames": {
        "type": "array"
      },
      "height": {
        "type": "integer"
      },
      "interpolate": {
        "type": "boolean"
      },
      "layer": {
        "type": "string"
      },
      "layers": {
        "type": "array"
      },
      "path": {
        "type": "string"
      },
      "pixelsChanged": {
        "type": "integer"
      },
      "playbackCount": {
        "type": "integer"
      },
      "redoAvailable": {
        "type": "boolean"
      },
      "region": {
        "additionalProperties": false,
        "properties": {
          "height": {
            "type": "integer"
          },
          "width": {
            "type": "integer"
          },
          "x": {
            "type": "integer"
          },
          "y": {
            "type": "integer"
          }
        },
        "required": [
          "x",
          "y",
          "width",
          "height"
        ],
        "type": "object"
      },
      "revision": {
        "type": "string"
      },
      "transactionId": {
        "type": "string"
      },
      "undoAvailable": {
        "type": "boolean"
      },
      "warnings": {
        "type": "array"
      },
      "width": {
        "type": "integer"
      },
      "preview": {
        "type": "object"
      }
    },
    "required": [
      "dirty",
      "redoAvailable",
      "revision",
      "undoAvailable",
      "transactionId",
      "appliedOperations",
      "changed",
      "warnings",
      "documentId",
      "path",
      "assetId",
      "layer",
      "region",
      "changedBounds",
      "pixelsChanged"
    ],
    "type": "object"
  }
}
```

### `nova.texture.palette_remap`

```json
{
  "name": "nova.texture.palette_remap",
  "description": "Replace colours in one edit; unmatched pixels are unchanged and each pixel is matched against its original colour (the first matching mapping wins). #RRGGBB keeps each pixel's alpha. Edits the open document in memory; it is not saved.",
  "annotations": {
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "assetId": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "expectedRevision": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "generation": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "layer": {
        "description": "A layer id from the document state; defaults to the active layer.",
        "maxLength": 36,
        "pattern": "^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$",
        "type": "string"
      },
      "mappings": {
        "description": "from #RRGGBB matches visible pixels of that RGB at any alpha; from #RRGGBBAA also matches alpha (fully transparent pixels compare as #00000000). to #RRGGBB keeps the pixel's alpha; to #RRGGBBAA sets it.",
        "items": {
          "additionalProperties": false,
          "properties": {
            "from": {
              "description": "A colour #RRGGBB (opaque) or #RRGGBBAA (straight alpha, CSS order).",
              "maxLength": 9,
              "pattern": "^#[A-Fa-f0-9]{6}([A-Fa-f0-9]{2})?$",
              "type": "string"
            },
            "to": {
              "description": "A colour #RRGGBB (opaque) or #RRGGBBAA (straight alpha, CSS order).",
              "maxLength": 9,
              "pattern": "^#[A-Fa-f0-9]{6}([A-Fa-f0-9]{2})?$",
              "type": "string"
            }
          },
          "required": [
            "from",
            "to"
          ],
          "type": "object"
        },
        "maxItems": 64,
        "minItems": 1,
        "type": "array"
      },
      "projectId": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "region": {
        "additionalProperties": false,
        "description": "A rectangle inside the canvas; x + width and y + height may not exceed its size.",
        "properties": {
          "height": {
            "maximum": 256,
            "minimum": 1,
            "type": "integer"
          },
          "width": {
            "maximum": 256,
            "minimum": 1,
            "type": "integer"
          },
          "x": {
            "maximum": 255,
            "minimum": 0,
            "type": "integer"
          },
          "y": {
            "maximum": 255,
            "minimum": 0,
            "type": "integer"
          }
        },
        "required": [
          "x",
          "y",
          "width",
          "height"
        ],
        "type": "object"
      },
      "tolerance": {
        "description": "Largest allowed per-channel difference, 0 (default) for exact.",
        "maximum": 255,
        "minimum": 0,
        "type": "integer"
      },
      "returnPreview": {
        "type": "boolean",
        "description": "Append an upscaled PNG of the result."
      },
      "include": {
        "description": "summary omits document dimensions, layers, frames and other editor state.",
        "enum": [
          "document",
          "summary"
        ]
      },
      "verbosity": {
        "enum": [
          "summary",
          "full"
        ]
      }
    },
    "required": [
      "mappings",
      "generation",
      "projectId",
      "assetId",
      "expectedRevision"
    ],
    "type": "object"
  },
  "outputSchema": {
    "additionalProperties": false,
    "properties": {
      "activeFrame": {
        "type": "string"
      },
      "activeLayer": {
        "type": "string"
      },
      "animated": {
        "type": "boolean"
      },
      "appliedOperations": {
        "type": "integer"
      },
      "assetId": {
        "type": "string"
      },
      "changeSummary": {
        "type": "object"
      },
      "changed": {
        "type": "boolean"
      },
      "changedBounds": {
        "oneOf": [
          {
            "additionalProperties": false,
            "properties": {
              "height": {
                "type": "integer"
              },
              "width": {
                "type": "integer"
              },
              "x": {
                "type": "integer"
              },
              "y": {
                "type": "integer"
              }
            },
            "required": [
              "x",
              "y",
              "width",
              "height"
            ],
            "type": "object"
          },
          {
            "type": "null"
          }
        ]
      },
      "defaultTicks": {
        "type": "integer"
      },
      "dirty": {
        "type": "boolean"
      },
      "documentId": {
        "type": "string"
      },
      "elementCount": {
        "type": "integer"
      },
      "frameCount": {
        "type": "integer"
      },
      "frames": {
        "type": "array"
      },
      "height": {
        "type": "integer"
      },
      "interpolate": {
        "type": "boolean"
      },
      "layer": {
        "type": "string"
      },
      "layers": {
        "type": "array"
      },
      "path": {
        "type": "string"
      },
      "pixelsChanged": {
        "type": "integer"
      },
      "playbackCount": {
        "type": "integer"
      },
      "redoAvailable": {
        "type": "boolean"
      },
      "region": {
        "additionalProperties": false,
        "properties": {
          "height": {
            "type": "integer"
          },
          "width": {
            "type": "integer"
          },
          "x": {
            "type": "integer"
          },
          "y": {
            "type": "integer"
          }
        },
        "required": [
          "x",
          "y",
          "width",
          "height"
        ],
        "type": "object"
      },
      "revision": {
        "type": "string"
      },
      "transactionId": {
        "type": "string"
      },
      "undoAvailable": {
        "type": "boolean"
      },
      "warnings": {
        "type": "array"
      },
      "width": {
        "type": "integer"
      },
      "preview": {
        "type": "object"
      }
    },
    "required": [
      "dirty",
      "redoAvailable",
      "revision",
      "undoAvailable",
      "transactionId",
      "appliedOperations",
      "changed",
      "warnings",
      "documentId",
      "path",
      "assetId",
      "layer",
      "region",
      "changedBounds",
      "pixelsChanged"
    ],
    "type": "object"
  }
}
```

### `nova.texture.gradient_map`

```json
{
  "name": "nova.texture.gradient_map",
  "description": "Recolour pixels by luma through a gradient in one edit. Luma is (2126 R + 7152 G + 722 B) / 10000 on encoded 8-bit sRGB, 0-255. Fully transparent pixels are unchanged. Edits the open document in memory; it is not saved.",
  "annotations": {
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "alpha": {
        "description": "preserve (default): keep each pixel's alpha, stops are #RRGGBB; multiply: pixel alpha * stop alpha / 255.",
        "enum": [
          "preserve",
          "multiply"
        ],
        "type": "string"
      },
      "assetId": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "expectedRevision": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "generation": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "interpolation": {
        "description": "linear (default): rounded integer interpolation of encoded channels between stops; step: the colour of the last stop at or below the luma.",
        "enum": [
          "linear",
          "step"
        ],
        "type": "string"
      },
      "layer": {
        "description": "A layer id from the document state; defaults to the active layer.",
        "maxLength": 36,
        "pattern": "^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$",
        "type": "string"
      },
      "projectId": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "region": {
        "additionalProperties": false,
        "description": "A rectangle inside the canvas; x + width and y + height may not exceed its size.",
        "properties": {
          "height": {
            "maximum": 256,
            "minimum": 1,
            "type": "integer"
          },
          "width": {
            "maximum": 256,
            "minimum": 1,
            "type": "integer"
          },
          "x": {
            "maximum": 255,
            "minimum": 0,
            "type": "integer"
          },
          "y": {
            "maximum": 255,
            "minimum": 0,
            "type": "integer"
          }
        },
        "required": [
          "x",
          "y",
          "width",
          "height"
        ],
        "type": "object"
      },
      "source": {
        "description": "The source value; only luma709 is supported.",
        "enum": [
          "luma709"
        ],
        "type": "string"
      },
      "stops": {
        "description": "2-16 stops with strictly increasing luma positions; luma outside them takes the end colour.",
        "items": {
          "additionalProperties": false,
          "properties": {
            "color": {
              "description": "A colour #RRGGBB (opaque) or #RRGGBBAA (straight alpha, CSS order).",
              "maxLength": 9,
              "pattern": "^#[A-Fa-f0-9]{6}([A-Fa-f0-9]{2})?$",
              "type": "string"
            },
            "position": {
              "maximum": 255,
              "minimum": 0,
              "type": "integer"
            }
          },
          "required": [
            "position",
            "color"
          ],
          "type": "object"
        },
        "maxItems": 16,
        "minItems": 2,
        "type": "array"
      },
      "returnPreview": {
        "type": "boolean",
        "description": "Append an upscaled PNG of the result."
      },
      "include": {
        "description": "summary omits document dimensions, layers, frames and other editor state.",
        "enum": [
          "document",
          "summary"
        ]
      },
      "verbosity": {
        "enum": [
          "summary",
          "full"
        ]
      }
    },
    "required": [
      "stops",
      "generation",
      "projectId",
      "assetId",
      "expectedRevision"
    ],
    "type": "object"
  },
  "outputSchema": {
    "additionalProperties": false,
    "properties": {
      "activeFrame": {
        "type": "string"
      },
      "activeLayer": {
        "type": "string"
      },
      "animated": {
        "type": "boolean"
      },
      "appliedOperations": {
        "type": "integer"
      },
      "assetId": {
        "type": "string"
      },
      "changeSummary": {
        "type": "object"
      },
      "changed": {
        "type": "boolean"
      },
      "changedBounds": {
        "oneOf": [
          {
            "additionalProperties": false,
            "properties": {
              "height": {
                "type": "integer"
              },
              "width": {
                "type": "integer"
              },
              "x": {
                "type": "integer"
              },
              "y": {
                "type": "integer"
              }
            },
            "required": [
              "x",
              "y",
              "width",
              "height"
            ],
            "type": "object"
          },
          {
            "type": "null"
          }
        ]
      },
      "defaultTicks": {
        "type": "integer"
      },
      "dirty": {
        "type": "boolean"
      },
      "documentId": {
        "type": "string"
      },
      "elementCount": {
        "type": "integer"
      },
      "frameCount": {
        "type": "integer"
      },
      "frames": {
        "type": "array"
      },
      "height": {
        "type": "integer"
      },
      "interpolate": {
        "type": "boolean"
      },
      "layer": {
        "type": "string"
      },
      "layers": {
        "type": "array"
      },
      "path": {
        "type": "string"
      },
      "pixelsChanged": {
        "type": "integer"
      },
      "playbackCount": {
        "type": "integer"
      },
      "redoAvailable": {
        "type": "boolean"
      },
      "region": {
        "additionalProperties": false,
        "properties": {
          "height": {
            "type": "integer"
          },
          "width": {
            "type": "integer"
          },
          "x": {
            "type": "integer"
          },
          "y": {
            "type": "integer"
          }
        },
        "required": [
          "x",
          "y",
          "width",
          "height"
        ],
        "type": "object"
      },
      "revision": {
        "type": "string"
      },
      "transactionId": {
        "type": "string"
      },
      "undoAvailable": {
        "type": "boolean"
      },
      "warnings": {
        "type": "array"
      },
      "width": {
        "type": "integer"
      },
      "preview": {
        "type": "object"
      }
    },
    "required": [
      "dirty",
      "redoAvailable",
      "revision",
      "undoAvailable",
      "transactionId",
      "appliedOperations",
      "changed",
      "warnings",
      "documentId",
      "path",
      "assetId",
      "layer",
      "region",
      "changedBounds",
      "pixelsChanged"
    ],
    "type": "object"
  }
}
```

### `nova.texture.erase`

```json
{
  "name": "nova.texture.erase",
  "description": "Make a region of the active layer transparent in one edit, clipped by the selection, like Nova's eraser: alpha * (255 - coverage) / 255; pixels reaching zero alpha are stored as #00000000. Never deletes files or layers. Edits the open document in memory; it is not saved.",
  "annotations": {
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "assetId": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "expectedRevision": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "generation": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "layer": {
        "description": "A layer id from the document state; defaults to the active layer.",
        "maxLength": 36,
        "pattern": "^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$",
        "type": "string"
      },
      "projectId": {
        "maxLength": 64,
        "pattern": "^[a-f0-9]{64}$",
        "type": "string"
      },
      "region": {
        "additionalProperties": false,
        "description": "A rectangle inside the canvas; x + width and y + height may not exceed its size.",
        "properties": {
          "height": {
            "maximum": 256,
            "minimum": 1,
            "type": "integer"
          },
          "width": {
            "maximum": 256,
            "minimum": 1,
            "type": "integer"
          },
          "x": {
            "maximum": 255,
            "minimum": 0,
            "type": "integer"
          },
          "y": {
            "maximum": 255,
            "minimum": 0,
            "type": "integer"
          }
        },
        "required": [
          "x",
          "y",
          "width",
          "height"
        ],
        "type": "object"
      },
      "strength": {
        "description": "Eraser coverage, 255 (default) for fully transparent.",
        "maximum": 255,
        "minimum": 1,
        "type": "integer"
      },
      "returnPreview": {
        "type": "boolean",
        "description": "Append an upscaled PNG of the result."
      },
      "include": {
        "description": "summary omits document dimensions, layers, frames and other editor state.",
        "enum": [
          "document",
          "summary"
        ]
      },
      "verbosity": {
        "enum": [
          "summary",
          "full"
        ]
      }
    },
    "required": [
      "region",
      "generation",
      "projectId",
      "assetId",
      "expectedRevision"
    ],
    "type": "object"
  },
  "outputSchema": {
    "additionalProperties": false,
    "properties": {
      "activeFrame": {
        "type": "string"
      },
      "activeLayer": {
        "type": "string"
      },
      "animated": {
        "type": "boolean"
      },
      "appliedOperations": {
        "type": "integer"
      },
      "assetId": {
        "type": "string"
      },
      "changeSummary": {
        "type": "object"
      },
      "changed": {
        "type": "boolean"
      },
      "changedBounds": {
        "oneOf": [
          {
            "additionalProperties": false,
            "properties": {
              "height": {
                "type": "integer"
              },
              "width": {
                "type": "integer"
              },
              "x": {
                "type": "integer"
              },
              "y": {
                "type": "integer"
              }
            },
            "required": [
              "x",
              "y",
              "width",
              "height"
            ],
            "type": "object"
          },
          {
            "type": "null"
          }
        ]
      },
      "defaultTicks": {
        "type": "integer"
      },
      "dirty": {
        "type": "boolean"
      },
      "documentId": {
        "type": "string"
      },
      "elementCount": {
        "type": "integer"
      },
      "frameCount": {
        "type": "integer"
      },
      "frames": {
        "type": "array"
      },
      "height": {
        "type": "integer"
      },
      "interpolate": {
        "type": "boolean"
      },
      "layer": {
        "type": "string"
      },
      "layers": {
        "type": "array"
      },
      "path": {
        "type": "string"
      },
      "pixelsChanged": {
        "type": "integer"
      },
      "playbackCount": {
        "type": "integer"
      },
      "redoAvailable": {
        "type": "boolean"
      },
      "region": {
        "additionalProperties": false,
        "properties": {
          "height": {
            "type": "integer"
          },
          "width": {
            "type": "integer"
          },
          "x": {
            "type": "integer"
          },
          "y": {
            "type": "integer"
          }
        },
        "required": [
          "x",
          "y",
          "width",
          "height"
        ],
        "type": "object"
      },
      "revision": {
        "type": "string"
      },
      "transactionId": {
        "type": "string"
      },
      "undoAvailable": {
        "type": "boolean"
      },
      "warnings": {
        "type": "array"
      },
      "width": {
        "type": "integer"
      },
      "preview": {
        "type": "object"
      }
    },
    "required": [
      "dirty",
      "redoAvailable",
      "revision",
      "undoAvailable",
      "transactionId",
      "appliedOperations",
      "changed",
      "warnings",
      "documentId",
      "path",
      "assetId",
      "layer",
      "region",
      "changedBounds",
      "pixelsChanged"
    ],
    "type": "object"
  }
}
```
