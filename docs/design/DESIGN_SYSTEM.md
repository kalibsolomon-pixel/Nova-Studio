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
| `backdrop` | `#0e0f12` | `#dcdfe5` | Gutters between docked panels, main-window background |
| `base` | `#131418` | `#ffffff` | Field wells, lists, trees, tables |
| `canvas` | `#131418` | `#e4e6eb` | Surround of the texture canvas and the 3D viewport |
| `window` | `#191a1f` | `#f5f6f8` | Panel and chrome fill (menu bar, toolbars, dock titles, status bar) |
| `surface` | `#1e1f25` | `#eef0f3` | Cards and grouped sections; hover on document tabs |
| `raised` | `#272830` | `#e6e8ec` | Hover fills, menus, tooltips, popups |
| `control` | `#24252c` | `#ffffff` | Push-button and picker fill |
| `border` / `borderStrong` | `#2a2b33` / `#3a3c46` | `#dfe1e6` / `#c4c8d0` | Hairlines / control outlines |
| `text` / `muted` | `#e3e4e8` / `#9b9ea9` | `#1b1d22` / `#5b5f6b` | Primary / secondary text |
| `disabled` / `placeholder` | `#62656f` / `#767985` | `#9da1ab` / `#6e7280` | Disabled ink / field placeholders (AA on white) |
| `icon` | `#c4c6ce` | `#3d414b` | Default icon ink, one step softer than text |
| `accent` | `#7b6cf0` (blue `#4a8af4`) | `#6550e0` (blue `#2563d9`) | Focus, primary actions, checked state, active tab |
| `accentHover` / `accentMuted` | | | Primary hover / checked toggles, inactive selection |
| `onAccent` | white | white | Text and marks on accent fills |
| `selection` / `selectionText` | `#383461` / white | `#e1dcfc` / `#1b1d22` | Selected rows and text selection |
| `warning`, `danger`, `success`, `info` | | | Status text; pair with a word or icon |
| `warningSurface`, `dangerSurface`, `infoSurface` | | | Tinted backgrounds for inline notices |
| `checkerDark` / `checkerLight` / `pixelGrid` | | | Canvas transparency and grid. `checkerboard_theme_test` guards their contrast against `canvas`. |

Widgets that paint custom content read `ui::current()`. They never hard-code a colour.
`ui::apply(tokens)` installs the style, palette and stylesheet, and records the active tokens.

## Type scale

The type scale uses the platform UI family (Segoe UI on Windows). Body text is 9 pt.

| `ui::Type` | Size | Weight | Use |
| --- | --- | --- | --- |
| `Caption` | 8 pt | Semibold, tracked | Section titles (`ui::sectionTitle`), keycaps, metadata |
| `Body` | 9 pt | Regular | Everything else |
| `Strong` | 9 pt | Semibold | Dock titles, the active tool name, group titles |
| `Title` | 10 pt | Semibold | Dialog headings (`QLabel[role="title"]`) |
| `Heading` | 13 pt | Semibold | Large empty states |
| `Display` | 20 pt | Semibold | The welcome screen (`QLabel[role="display"]`) |

Use `ui::font(Type)` in code, or a stylesheet role on a label.

## Spacing and shape

- **Spacing:** `SpaceXS 2 · SpaceS 4 · SpaceM 6 · SpaceL 8 · SpaceXL 12 · Space2XL 16 · Space3XL 24` (px).
  Panel content uses `PanelPadding` (12). Related controls are 4 apart and groups are 8 or more apart.
- **Radii:** `RadiusS 4` (controls, rows, keycaps), `RadiusM 6` (cards, notices, popups' rows),
  `RadiusL 8` (large cards).
- **Heights:** `ControlHeight 26` for buttons, fields and pickers. `RowHeight 26` for tree, list
  and table rows.
- **Icons:** `IconSize 18` for commands and `ToolIconSize 20` for the tool strip, drawn from
  Material Symbols Outlined (`ui::lineIcon`). `GutterWidth 4` and `ScrollBarWidth 10`.

## Components

| Component | How to get it |
| --- | --- |
| Primary button | `ui::setPrimary(button)`: accent fill and `onAccent` text. Use one per view, for the main action. |
| Secondary button | Plain `QPushButton`: control fill and `borderStrong` outline |
| Icon button | `ui::iconButton(icon, tooltip)`, or `QPushButton[role="icon"]` |
| Text field, spin box, combo box | Plain Qt widgets; `NovaStyle` draws the shared field well and chevrons |
| Checkbox, radio | Plain Qt widgets; `NovaStyle` draws rounded accent indicators |
| Section title | `ui::sectionTitle("Document")` |
| Hint / secondary text | `ui::hint(text)`, or `QLabel[role="hint"]` |
| Empty state | `ui::emptyState(text)` |
| Inline warning notice | `QLabel[role="notice"]`, or `QFrame[role="notice"]` |
| Card | `QFrame[role="card"]`, or a titled `QGroupBox` |
| Status colour | `ui::current().danger` / `.warning` / `.success`, always beside text |

Rules that a stylesheet cannot express live in `NovaStyle` (`theme.cpp`). These are field
geometry, arrows, branch chevrons, check marks, dock and folder icons, and metrics.

Never call `setStyleSheet` on an individual widget for colours. If a component needs a new
look, add a role to `styleSheet()` and document it here.

## Checking a change

`tests/phase3b_gallery.cpp` drives the real window through every major view in both themes.
Set `NOVA_PHASE3B_GALLERY_DIR` to a folder and run `nova-phase3b-gallery.exe`. Compare the
dark and light captures before and after the change.
