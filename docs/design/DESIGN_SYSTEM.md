# Nova Studio design system

Nova Studio has one visual language. Its single source is `src/ui/theme.h` and `src/ui/theme.cpp`.
New UI composes the pieces described here. It does not pick its own colours, sizes or fonts.

## Principles

- **The artwork is the brightest, most colourful thing on screen.** Chrome is low-chroma
  graphite (dark) or cool grey (light). Saturated colour is reserved for the accent and for
  status.
- **Elevation is expressed by tone, not lines.** Panels sit on a darker backdrop, separated by
  gutters. Hairline borders only divide regions inside a panel.
- **One shape per job.** Every text field, spin box and combo box is the same recessed well.
  Every button uses the same radius and height. Every list row uses the same height and
  selection treatment.
- **Hierarchy through weight and tone, not size jumps.** Labels are muted and values are
  bright. Section titles are small, semibold, tracked capitals.
- **Never colour alone.** Status is a colour plus a word or icon. Selection is a fill plus
  contrasting text.

## Colour tokens (`ui::Tokens`)

The table lists each token with its dark and light value. Some values below are blends of
two tokens.

| Token | Dark | Light | Use |
| --- | --- | --- | --- |
| `backdrop` | `#0e0f12` | `#c9cbd2` | Gutters between docked panels, main-window background |
| `base` | `#131418` | `#ffffff` | Field wells, lists, trees, tables |
| `canvas` | `#131418` | `#d5d7dc` | Surround of the texture canvas and the 3D viewport |
| `window` | `#191a1f` | `#f4f4f6` | Panel and chrome fill (menu bar, toolbars, dock titles, status bar) |
| `surface` | `#1e1f25` | `#ececef` | Cards and grouped sections; hover on document tabs |
| `raised` | `#272830` | `#e5e6ea` | Hover fills, menus, tooltips, popups |
| `control` | `#24252c` | `#ffffff` | Push-button and picker fill |
| `border` / `borderStrong` | `#2a2b33` / `#3a3c46` | `#d6d8de` / `#bfc2ca` | Hairlines / control outlines |
| `text` / `muted` | `#e3e4e8` / `#a4a7b2` | `#1b1d22` / `#5b5f6b` | Primary / secondary text |
| `disabled` / `placeholder` | `#7a7d89` / `#7e818d` | `#878a93` / `#6e7280` | Disabled ink / field placeholders (AA on white) |
| `icon` | `#c4c6ce` | `#3d414b` | Default icon ink, one step softer than text |
| `accent` | `#7b6cf0` (blue `#4a8af4`) | `#6550e0` (blue `#2563d9`) | Focus, primary actions, checked state, active tab |
| `accentHover` / `accentMuted` | | | Primary hover / checked toggles, inactive selection |
| `accentFill` | `#6a5ae0` (blue `#2d6ad0`) | = `accent` | Primary-button fill: the accent deepened until white labels meet AA (4.5:1) |
| `onAccent` | white | white | Text and marks on accent fills |
| `selection` / `selectionText` | `#383461` / white | `#c6bcf4` / `#1b1d22` | Selected rows and text selection |
| `warning`, `danger`, `success`, `info` | | | Status text; pair with a word or icon |
| `warningSurface`, `dangerSurface`, `infoSurface` | | | Tinted backgrounds for inline notices |
| `checkerDark` / `checkerLight` / `pixelGrid` | | | Canvas transparency and grid. `checkerboard_theme_test` guards their contrast against `canvas`. |

Widgets that paint custom content read `ui::current()`. They never hard-code a colour.
`ui::apply(tokens)` installs the style, palette and stylesheet, and records the active tokens.

## Type scale

The type scale uses the platform UI family (Segoe UI on Windows). Body text is 9.75 pt
(13 logical pixels), and captions are 9 pt (12 logical pixels).

Dock containers establish the `window` surface before transparent children inherit it;
the main window's `backdrop` is reserved for gutters. Selected asset rows keep the same
fill when focus moves elsewhere, and selection never recolours artwork thumbnails.
In compact texture/UV workspaces below 1000 logical pixels high, 3D Preview shares the
Layers tab group so colour controls retain useful vertical room. UV face labels use
the themed `raised`, `borderStrong` and `text` tokens; overlapping labels give the
selected face priority.

| `ui::Type` | Size | Weight | Use |
| --- | --- | --- | --- |
| `Caption` | 9 pt | Semibold, tracked | Section titles (`ui::sectionTitle`, in `text` so they outrank the muted field labels below them), keycaps, metadata |
| `Body` | 9.75 pt | Regular | Everything else |
| `Strong` | 9.75 pt | Semibold | Dock titles, the active tool name, group titles |
| `Title` | 10.5 pt | Semibold | Dialog headings (`QLabel[role="title"]`) |
| `Heading` | 13 pt | Semibold | Large empty states |
| `Display` | 20 pt | Semibold | The welcome screen (`QLabel[role="display"]`) |

Use `ui::font(Type)` in code, or a stylesheet role on a label.

Tool options use 6px gaps, reducing to 4px below `OptionsCompactWidth` (900 logical
pixels) so short forms remain compact. Secondary settings menus share
`SettingsLabelWidth` (104) and `SettingsValueWidth` (80), right-aligned numbers and
the same title treatment. Gallery capture 20 shows the tool rail with a texture
document at 1280×900.

## Spacing and shape

- **Spacing:** `SpaceXS 2 · SpaceS 4 · SpaceM 6 · SpaceL 8 · SpaceXL 12 · Space2XL 16 · Space3XL 24` (px).
  Panel content uses `PanelPadding` (12). Related controls are 4 apart and groups are 8 or more apart.
- **Radii:** `RadiusS 4` (controls, rows, keycaps), `RadiusM 6` (cards, notices, popups' rows),
  `RadiusL 8` (large cards).
- **Heights:** `ControlHeight 28` for buttons, fields and pickers. `RowHeight 28` for tree, list
  and table rows. `InsetRowHeight 26` for short lists inset in a panel (`QListWidget[role="inset"]`).
- **Icons:** `IconSize 18` for commands and `ToolIconSize 20` for the tool strip, drawn from
  Material Symbols Outlined (`ui::lineIcon`). `GutterWidth 4` and `ScrollBarWidth 10`.

## Components

| Component | How to get it |
| --- | --- |
| Primary button | `ui::setPrimary(button)`: `accentFill` fill and `onAccent` text. Use one per view, for the main action. |
| Secondary button | Plain `QPushButton`: control fill and `borderStrong` outline |
| Icon button | `ui::iconButton(icon, tooltip)`, or `QPushButton[role="icon"]` |
| Text field, spin box, combo box | Plain Qt widgets; `NovaStyle` draws the shared field well and chevrons |
| Checkbox, radio | Plain Qt widgets; `NovaStyle` draws rounded accent indicators |
| Section title | `ui::sectionTitle("Document")` |
| Text command beside a heading | `QPushButton[role="link"]`: frameless `accentHover` text that tints `accentMuted` on hover; a menu on it opens without a drop-down arrow (Welcome's "View all") |
| Hint / secondary text | `ui::hint(text)`, or `QLabel[role="hint"]` |
| Status value | `QLabel[role="warning"]` (needs attention, e.g. unsaved), `"error"`, or `"info"` (an available action, e.g. an export update) |
| Empty state | `ui::emptyState(text, parent, icon)`: centred text with an optional large muted Material glyph above it |
| Inline warning notice | `QLabel[role="notice"]`, or `QFrame[role="notice"]` |
| Card | `QFrame[role="card"]`, or a titled `QGroupBox` |
| Status colour | `ui::current().danger` / `.warning` / `.success`, always beside text. Re-read the tokens on `QEvent::PaletteChange` so a theme switch re-tones existing rows. |
| Icon toggle | `QCheckBox[role="toggle"]` with an icon and no text: a raised square that fills `accentMuted` with an `accent` edge when checked |
| Secondary settings | `ui::settingsMenu(button, content)`: a native menu hosting a form with `PanelPadding`, `SpaceXL` between columns and `SpaceL` between rows. Keep routine controls on the working row; label the settings button by its task. |
| Path field | `ui::pathField(text)`: a native line edit that preserves the first and final path segments when unfocused. Editing and `text()` retain the entire path; the tooltip shows it in full. |
| Continuous numeric setting | `ui::sliderField(spinBox, label)`: a native slider paired with the exact editable value; both stay synchronized and carry the same accessible name. The value box has no step buttons (the slider is the pointer control). Every row of a settings popover uses it, so values line up in one column. |
| Scrolling panel | `ui::addScrollFade(area)`: a soft edge shadow and hairline mark more content below. A row the edge would cut in half is hidden whole, so a panel never ends on half a heading. |
| Card gutter | `QSplitter[role="gutter"]` for wide handles between cards: empty space that tints `accentMuted` on hover |
| Settings of a radio choice | Under the radio, with their labels indented to the radio's text (indicator width plus label spacing), and a `SpaceL` gap before the next choice (see Batch output) |
| Empty list with a first action | Put the action (a primary button) inside the empty state, and hide the list's edit bar until the list has rows (see the batch operation stack). |

Rules that a stylesheet cannot express live in `NovaStyle` (`theme.cpp`). These are field
geometry, arrows, branch chevrons, check marks, dock and folder icons, and metrics.

Never call `setStyleSheet` on an individual widget for colours. If a component needs a new
look, add a role to `styleSheet()` and document it here.

## Editor states

With no pack and no document, the welcome screen offers Create, Open and Recent packs.
Up to four recent packs show their folder name and full-path tooltip in a native,
keyboard-activated list. View all retains the existing Recent Projects menu.
Command search labels these entries by pack name and still matches their full paths.
Editing docks and toolbars, including the path/workspace command bar, are hidden.
The menu bar and keyboard commands remain available. Their packed layout is retained separately and
restored when a pack or document opens; saving preferences during welcome saves that
editor layout rather than the temporary welcome arrangement. Workspace presets still
select the layout that will be restored.

Selected asset and layer rows use a two-pixel accent edge alongside their selection fill.
Document tabs show the filename and unsaved dot. PNG export freshness is described in the
Inspector and the tab tooltip rather than another abbreviation in the tab label.
Animation frame selection outlines the tile without recolouring the artwork thumbnail.
Frame tiles keep eight pixels of content padding, separate timing and source-name
lines, and a visible keyboard-focus outline. Their geometry follows the actual
font metrics and thumbnail size. Search result rows that would cross the footer
are painted only after scrolling brings the complete row into view.
Brush height, hardness, spacing and smoothing use the secondary-settings menu; onion
skin settings use the same component. Their document and tool-state semantics are unchanged.
Dependent onion controls are disabled while adjacent-frame display is off; their values are
retained. Settings-menu titles use the shared strong weight. Text fields share the same
visible resting border as pickers. Embedded lists keep partial rows out of the parent
scroll area's bottom edge.
Unchecked indicators have a stronger outline (calculated 3.28:1 on the dark popup,
3.09:1 on the light panel). Spin-button zones are 24 pixels wide. Material glyphs
use regular outline weight for clearer compact actions. Batch sections use whitespace
and headings; list wells and fields retain their own boundaries. Short target lists
and empty operation stacks keep a bounded height; a populated stack gives its
space back to the list and parameter editor.

The editing toolbar is a fixed vertical rail on the left edge, as in Aseprite and
Photoshop: it cannot be dragged, docked elsewhere or floated, so every workspace and
every user's muscle memory agree on where the tools are. View › Panels › Tool Strip
still hides and shows it. Workspace layouts saved by earlier builds, in which the strip
could move, restore with the rail on the left and the rest of their arrangement intact.
The rail chooses the tool; the Tool Options row above the canvas edits it. The colour
wells share the 30px tool target. A button that groups tool variants (and Effects) is the
same square as every other rail button, marked by a small triangle in its bottom-right
corner (`muted`, or `accent` when checked) rather than a separate arrow segment. Press and
hold, right-click, Alt+Down or the menu key opens its menu; every variant also has its own
shortcut and an Edit › Tool menu entry. Rules separate Effects and the colour wells.

## Checking a change

`tests/phase3b_gallery.cpp` drives the real window through every major view in both themes.
Set `NOVA_PHASE3B_GALLERY_DIR` to a folder and run `nova-phase3b-gallery.exe`. Compare the
dark and light captures before and after the change.
