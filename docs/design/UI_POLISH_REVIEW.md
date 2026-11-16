# UI polish council record

Independent screenshot reviews. Reviewers receive the same captures and rubric; they do not receive other scores. Scores are subjective and are not averaged into approval. Every participating reviewer must give at least 9/10 overall.

## Latest completed whole-application round: 14

| Model | Hierarchy | Colour | Type | Spacing | Controls | Icons | Consistency | Overall |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| opencode/glm-5.3-flash-free | 7 | 7 | 6 | 6 | 7 | 7 | 7 | 7 |
| opencode/longcat-2.5-free | 7 | 7 | 6 | 7 | 6 | 6 | 6 | 6 |
| t3-hub/k3.1-pro,thinking | 7 | 7 | 7 | 6 | 6 | 6 | 8 | 7 |

**Acceptance remains unmet.** Overall ratings are 6–7/10. GLM-5.3 on z.ai could not view images and is not scored.

## Toolbar placement reviews: rounds 15–17

The council reviewed left, right, top, above Colours, above Assets, and floating
placements independently of the whole-application ratings.

| Model | Hierarchy | Colour | Type | Spacing | Controls | Icons | Consistency | Overall |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| GLM free | 6 | 8 | 7 | 5 | 7 | 8 | 7 | 6 |
| Kimi | 7 | 8 | 6 | 6 | 6 | 7 | 7 | 7 |

LongCat reached the five-minute limit and is unscored. The completed reviews
agree that narrow panel placements need a deliberate grid and that the floating
context capture must include its native tool window. Above Colours also exposes
a layout defect: it separates Colours from its tab peers. Fixes are in progress;
these scores do not constitute toolbar or whole-application approval.

Round 16 evaluated the first grid, tab-group, medium-height layout and floating
capture fixes. GLM free and LongCat free both rated it 7/10 overall; Kimi reached
the five-minute limit and is unscored. The next pass keeps all nine strip controls
in a balanced 3×3 panel grid, places rail colour wells beside the tool cluster,
and preserves side-column widths when moving the strip. Findings that confused
tool selection with brush shape or proposed hiding tools were not applied.

Round 17 evaluated the balanced grid, nearby rail swatches and column-width fixes.
GLM free and LongCat free both rated it 6/10 overall. Kimi reached the five-minute
limit and is unscored. The panel grid still takes too much vertical space. Some
review claims conflict with the actual captures (missing chevrons, a far-right
swatch and different row counts); these remain recorded without being treated
as verified defects. The next pass reduces the panel strip to two rows and uses
content-specific image filenames to remove stale-path ambiguity.

Round 14 included consistent dock-body surfaces, more vertical room for colour controls, uniform asset selection fills and non-overlapping UV labels. The next review also covers every supported editing-toolbar placement: left, right, top, above Colours, above Assets and floating. Batch headings now sit inside their sections; Welcome loses its inset gutter; option spacing and popup label columns use consistent geometry. Paint colours and artwork were not changed to satisfy UI-colour suggestions.

## Round history

| Round | Focus | Overall scores |
| --- | --- | --- |
| 0 | baseline (before changes) | GLM-5.3 Flash (free): 6; LongCat 2.5 (free): 6; MiniMax M3.1 Flash (free): 6; Inkling 2 Small (free): 6; GPT-6.1 Sol: 7; GPT-6.1 Terra: 6; Claude Sonnet 5.5: 5; Claude Haiku 5.5: 5 |
| 1 | new design system (tokens, NovaStyle, restyle) | GLM-5.3 Flash (free): 7; GLM-5.3 Flash (z.ai plan): 7; LongCat 2.5 (free): 7; MiniMax M3.1 Flash (free): 6; Inkling 2 Small (free): 7; GPT-6.1 Sol: 7; GPT-6.1 Terra: 6; Claude Sonnet 5.5: 6; Claude Haiku 5.5: 5 |
| 2 | top dock tabs, neutral light theme, clearer panels (1152 px captures) | GLM-5.3 Flash (free): 7; GLM-5.3 Flash (z.ai plan): 7; LongCat 2.5 (free): 7; MiniMax M3.1 Flash (free): 6; Inkling 2 Small (free): 7; GPT-6.1 Sol: 7; GPT-6.1 Terra: 6; Claude Sonnet 5.5: 6; Claude Haiku 5.5: 6 |
| 3 | one body font, colour grid, compact fields, scroll fades | GLM-5.3 Flash (free): 7; GLM-5.3 Flash (z.ai plan): 7; LongCat 2.5 (free): 7; MiniMax M3.1 Flash (free): 6; Inkling 2 Small (free): 8; GPT-6.1 Sol: 7; GPT-6.1 Terra: 7; Claude Sonnet 5.5: 6; Claude Haiku 5.5: 6 |
| 4 | options bar fits, eye toggles, severity dots, inset lists | GLM-5.3 Flash (free): 7; GLM-5.3 Flash (z.ai plan): 7; LongCat 2.5 (free): 7; MiniMax M3.1 Flash (free): 6; Inkling 2 Small (free): 7; GPT-6.1 Sol: 7; GPT-6.1 Terra: 7; Claude Sonnet 5.5: 6; Claude Haiku 5.5: 6 |
| 5 | filled disabled primary, icon toggles, option dividers | GLM-5.3 Flash (free): 8; GLM-5.3 Flash (z.ai plan): 7; LongCat 2.5 (free): 7; MiniMax M3.1 Flash (free): 6; Inkling 2 Small (free): 7; GPT-6.1 Sol: 7; GPT-6.1 Terra: 7; Claude Sonnet 5.5: 7; Claude Haiku 5.5: 6 |
| 6 | dock room, scroll edge, batch first step, palette groups | GLM-5.3 Flash (free): 6; GLM-5.3 Flash (z.ai plan): 7; LongCat 2.5 (free): 7; MiniMax M3.1 Flash (free): 6; Inkling 2 Small (free): 8; GPT-6.1 Sol: 7; GPT-6.1 Terra: 7; Claude Sonnet 5.5: 6; Claude Haiku 5.5: 6 |
| 7 | clean panel edges, batch footer, roomier lists | GLM-5.3 Flash (free): 7; GLM-5.3 Flash (z.ai plan): 7; LongCat 2.5 (free): 8; MiniMax M3.1 Flash (free): 6; Inkling 2 Small (free): 8; GPT-6.1 Sol: 7; GPT-6.1 Terra: 7; Claude Sonnet 5.5: 7; Claude Haiku 5.5: 6 |
| 8 | colour tracks, palette headings, AA primary fill | GLM-5.3 Flash (free): 7; GLM-5.3 Flash (z.ai plan): 8; LongCat 2.5 (free): 6; MiniMax M3.1 Flash (free): 6; Inkling 2 Small (free): 8; GPT-6.1 Sol: 7; GPT-6.1 Terra: 7; Claude Sonnet 5.5: 7; Claude Haiku 5.5: 6 |
| 9 | native editor polish | MiniMax M3.1 Flash (free): 6; GLM-5.3 Flash (free): 7; GLM-5.3 Flash (z.ai plan): 7; LongCat 2.5 (free): 7; Inkling 2 Small (free): 8; GPT-6.1 Sol: 7; GPT-6.1 Terra: 7; Claude Sonnet 5.5: 7; Claude Haiku 5.5: 7 |
| 10 | focused welcome and compact settings | GLM-5.3 Flash (free): 7; LongCat 2.5 (free): 7; GLM-5.3 Flash (z.ai plan): 8 |
| 11 | paths, menu headings and timeline groups | opencode/glm-5.3-flash-free: 7; opencode/longcat-2.5-free: 7; zai-coding-plan/glm-5.3-flash: 7 |
| 12 | readable states and compact layouts | GLM free: 7; LongCat free: 8; GLM z.ai: 7; Kimi: 7 |
| 13 | readable working text and focused welcome | GLM free: 7; LongCat free: 7; Kimi: 7 |
| 14 | consistent panels and readable UV labels | GLM free: 7; LongCat free: 6; Kimi: 7; GLM z.ai: unable to view images |
| 15 | six toolbar placements only | GLM free: 6; Kimi: 7; LongCat: five-minute limit |
| 16 | six toolbar placements, grid/tab-group fixes | GLM free: 7; LongCat free: 7; Kimi: five-minute limit |
| 17 | six toolbar placements, balanced grid and stable columns | GLM free: 6; LongCat free: 6; Kimi: five-minute limit |

## Verification

- Native Qt gallery: dark/light, welcome/editor, selection, animation, models, batch, palette and settings. Gallery/test windows use the secondary portrait display.
- Targeted workspace, tool-placement (including 150% and 200%) and tool-options checks: 6/6 passed.
- Regression run before the latest border/spacing pass: 66/66 passed with MCP explicitly excluded.
- Full regression run with official MCP SDK 1.26.0: 67/67 suites passed. MCP pixel assertions now match the documented canvas token; the standalone MCP check passed 24 checks with one optional benchmark skipped.
- Later font, frame-size, path and Welcome-layout changes: four targeted GUI suites passed. Subsequent surface/UV changes: workspace, UV, animation and tool-options checks passed; final gallery and persistence checks are recorded separately in the task evidence.
- Token contrast calculations: dark secondary text on window 7.24:1; light secondary 5.80:1; light selected-row text 10.31:1. These do not establish whole-app accessibility compliance.
- A later complete regression run passed 67/67 suites in 435.44 seconds. The subsequent two-row panel grid, combined top row and floating default passed focused placement checks; the combined top row also passed compact, normal and large window checks at 150% and 200%. The final native gallery passed 4/4 checks.
- Performance was not measured.

## Screenshot evidence

| Surface | Before | Current |
| --- | --- | --- |
| Welcome | [Before](screenshots/before-01-welcome-dark.png) | [Current](screenshots/01-welcome-dark.png) |
| Editor | [Before](screenshots/before-02-general-dark-1280.png) | [Current](screenshots/02-general-dark-1280.png) |
| Batch | [Before](screenshots/before-11-batch-dark.png) | [Current](screenshots/11-batch-dark.png) |
| Light theme | [Before](screenshots/before-17-light-texture.png) | [Current](screenshots/17-light-texture.png) |

Editing-toolbar positions: [left](screenshots/20-tools-left.png),
[right](screenshots/21-tools-right.png), [top](screenshots/22-tools-top.png),
[above Colours](screenshots/23-tools-above-colours.png),
[above Assets](screenshots/24-tools-above-assets.png),
[floating context](screenshots/25-tools-floating-context.png), and
[floating detail](screenshots/26-tools-floating.png).

The toolbar transition check verifies that the wrapped strip fits its dock
host after moving from the top, retains tab peers and hidden-panel state, and
preserves side-column widths. The native gallery repeats the width check with
a loaded document. The host and flow viewport follow the actual rows;
this prevents tools on a second row from being clipped. Placement checks passed at
normal, 150% and 200% scaling. Toolbar-only scores do not replace the whole-application acceptance requirement.

The current top placement shares a row with Tool Options; its options retain horizontal
scrolling when space is limited. Panel placements use two rows of four tools beside
the grip at normal widths. Floating defaults to the lower canvas margin after
layout settles. Round 18 is reviewing content-addressed copies of all seven captures.

Evidence remains under `build/ui-polish`: baseline, latest captures, scores and review JSON. This record is provisional until the next council completes.
