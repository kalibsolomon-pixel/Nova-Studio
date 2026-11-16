# UI polish council record

Independent screenshot reviews. Reviewers receive the same captures and rubric; they do not receive other scores. Scores are subjective and are not averaged into approval. Every participating reviewer must give at least 9/10 overall.

## Latest completed round: 13

| Model | Hierarchy | Colour | Type | Spacing | Controls | Icons | Consistency | Overall |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| opencode/glm-5.3-flash-free | 8 | 8 | 7 | 7 | 7 | 8 | 8 | 7 |
| opencode/longcat-2.5-free | 7 | 7 | 7 | 7 | 6 | 7 | 7 | 7 |
| t3-hub/k3.1-pro,thinking | 7 | 8 | 7 | 7 | 7 | 7 | 7 | 7 |

**Acceptance remains unmet.** All three reviewers gave 7/10 overall.

Round 13 included 13 logical-pixel working text and 12-pixel captions, filename-focused tabs, matching settings-menu titles, disabled onion labels with their fields, larger frame tiles and elided paths. Changes for the next review: consistent dock-body surfaces; compact texture layouts give colour controls more vertical room; uniform asset selection fills with accurate thumbnail colours; overlapping UV labels are suppressed and use theme tokens. Paint colours and artwork were not changed to satisfy UI-colour suggestions.

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

## Verification

- Native Qt gallery: dark/light, welcome/editor, selection, animation, models, batch, palette and settings. Gallery/test windows use the secondary portrait display.
- Targeted workspace, tool-placement (including 150% and 200%) and tool-options checks: 6/6 passed.
- Regression run before the latest border/spacing pass: 66/66 passed with MCP explicitly excluded.
- Full regression run with official MCP SDK 1.26.0: 67/67 suites passed. MCP pixel assertions now match the documented canvas token; the standalone MCP check passed 24 checks with one optional benchmark skipped.
- Later font, frame-size, path and Welcome-layout changes: four targeted GUI suites passed. Subsequent surface/UV changes: workspace, UV, animation and tool-options checks passed; final gallery and persistence checks are recorded separately in the task evidence.
- Token contrast calculations: dark secondary text on window 7.24:1; light secondary 5.80:1; light selected-row text 10.31:1. These do not establish whole-app accessibility compliance.
- Performance was not measured.

Evidence remains under `build/ui-polish`: baseline, latest captures, scores and review JSON. This record is provisional until the next council completes.
