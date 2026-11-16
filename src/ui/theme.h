// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Nova design system: colour tokens, the type scale, spacing and shape metrics,
// the application palette, one scoped stylesheet, the Nova widget style and Material
// Symbols Outlined icons. Every surface reads from these tokens so light and dark
// themes stay consistent; docs/design/DESIGN_SYSTEM.md describes how to use them.
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QIcon>
#include <functional>
#include <QPalette>
#include <QString>

class QAbstractScrollArea;
class QLabel;
class QStyle;
class QToolButton;
class QWidget;
namespace nova::ui {
struct Tokens {
    bool dark = true;
    // Elevation, from back to front: the backdrop shows in the gutters between
    // panels; base is the deepest working surface (canvas surround, input wells,
    // lists); window is panel and chrome fill; surface lifts headers and cards;
    // raised is hover and popups; control fills buttons. canvas surrounds the texture
    // and 3D viewports: base in dark themes, a soft grey in light themes so a large
    // workspace never glares white.
    QColor backdrop, base, canvas, window, surface, raised, control;
    // Large list, tree and table areas: recessed in dark themes, flush with the panel in
    // light themes so wide white areas never glare. Fields keep the base well.
    QColor list;
    QColor border, borderStrong;
    QColor text, muted, disabled, placeholder;
    // Icon ink is a step softer than text so dense toolbars stay calm; the active
    // tool and checked toggles switch to accent ink.
    QColor icon;
    // Accent fills (primary buttons, checked toggles) carry onAccent text. accentHover
    // brightens a fill under the pointer; accentMuted tints selected-but-unfocused rows.
    QColor accent, accentHover, accentMuted, onAccent, selection, selectionText;
    QColor warning, danger, success, info;
    // Tinted backgrounds for inline notices (warning, danger and info banners).
    QColor warningSurface, dangerSurface, infoSurface;
    // Transparency checkerboard and pixel grid on the editing canvas. The squares are
    // mid-tones so dark and light texture pixels both stand out, they differ from each
    // other less than the grid line does, and the canvas surround (canvas) is clearly
    // darker (dark theme) or lighter (light theme) than both, so the texture edge reads.
    QColor checkerDark, checkerLight, pixelGrid;
};
// Spacing scale (device-independent pixels), on a 4 px rhythm with 2 px half steps.
constexpr int SpaceXS = 2, SpaceS = 4, SpaceM = 6, SpaceL = 8, SpaceXL = 12, Space2XL = 16, Space3XL = 24;
// Shape: controls and list rows use RadiusS, cards and popups RadiusM, large
// surfaces (welcome actions, dialogs' grouped sections) RadiusL.
constexpr int RadiusS = 4, RadiusM = 6, RadiusL = 8;
constexpr int ControlHeight = 26; // buttons, combos, spin boxes, line edits
constexpr int RowHeight = 26;     // tree, list and table rows
constexpr int InsetRowHeight = 24; // rows of short lists inset in a panel (QListWidget[role="inset"])
constexpr int IconSize = 18;      // command icons in toolbars and panels
constexpr int ToolIconSize = 20;  // left tool strip
// Type scale in points over the platform UI family (Segoe UI Variable Text on
// Windows 11, Segoe UI before it). Body is the application font; the others are
// applied through font(Type) or stylesheet roles.
enum class Type { Caption, Body, Strong, Title, Heading, Display };
constexpr double TypeCaption = 8.0, TypeBody = 9.0, TypeTitle = 10.0, TypeHeading = 13.0, TypeDisplay = 20.0;
QFont font(Type type);
// Toolbar, button and panel metrics. styleSheet() and the toolbar/panel layouts read
// these instead of per-widget pixel values (documented in docs/design/DESIGN_SYSTEM.md).
constexpr int ButtonPadding = 3;             // inside icon buttons, around the icon
constexpr int ButtonBorder = 1;              // transparent until hover/focus/checked
constexpr int ToolButtonPadding = SpaceS;    // inside tool-strip buttons
// Hit target of a tool-strip button: icon + padding + border on both sides (30 px).
constexpr int ToolHitTarget = ToolIconSize + 2 * (ToolButtonPadding + ButtonBorder);
constexpr int ToolbarSpacing = SpaceS;       // between items in a toolbar
constexpr int ToolbarPaddingV = SpaceS, ToolbarPaddingH = SpaceL;
constexpr int GroupGap = SpaceS;             // extra gap between related option groups
constexpr int SeparatorMarginMajor = SpaceM; // separator inset along the toolbar
constexpr int SeparatorMarginMinor = SpaceS; // separator inset across the toolbar
constexpr int MenuArrowWidth = 10;           // drop-down segment of split buttons
constexpr int MenuArrowGap = SpaceXS;        // space between a button's icon and its arrow
constexpr int PanelPadding = SpaceXL;        // dock titles, dialog sections, control panels
constexpr int PanelPaddingV = 7;             // dock title rows
constexpr int ToolStripPaddingV = SpaceM, ToolStripPaddingH = SpaceS; // around the tool strip
constexpr int ToolStripSeparatorMargin = SpaceM; // separator inset along the tool strip
constexpr int GutterWidth = 4;               // backdrop gap between docked panels
constexpr int ScrollBarWidth = 10;           // overlay-style scroll bars, no arrow buttons

Tokens tokens(bool dark, const QString &accent);
QPalette palette(const Tokens &t);
QString styleSheet(const Tokens &t);
// The Nova widget style: Fusion with token-driven icons for standard pixmaps (dock
// buttons, folder and file icons), arrows, check marks and metrics. The application
// sets it once at startup; apply() installs it if a test or host has not.
QStyle *createStyle();
// Applies tokens to the whole application: style, palette, stylesheet and the
// active token set. Window::applyTheme() calls this on every theme or accent change.
void apply(const Tokens &t);
// Installs the Nova style and the body font. Window construction calls it first, so
// every widget is created with the final font even when the host has not set one.
void prepareApplication();
// The tokens most recently applied by apply(); widgets that paint custom
// content read these instead of hard-coding colours.
const Tokens &current();
void setCurrent(const Tokens &t);

// Outlined Material Symbols drawn at the requested size and device pixel ratio.
// Accepts the established command names and the editor-tool IDs from toolInfos().
// Unknown names and a missing embedded font return a null icon with a diagnostic.
QIcon lineIcon(const QString &name, const QColor &ink, const QColor &disabledInk = {});
QIcon lineIcon(const QString &name); // uses current() tokens

// Small helpers that keep labels and icon buttons consistent.
QLabel *sectionTitle(const QString &text, QWidget *parent = nullptr);
QLabel *hint(const QString &text, QWidget *parent = nullptr);
// Centred empty-state text for panels with nothing to show yet.
QLabel *emptyState(const QString &text, QWidget *parent = nullptr);
// Marks a push button as the dialog's or view's main action (accent fill).
void setPrimary(QWidget *button);
// Fades the bottom edge of a scroll area into the panel while more content lies below,
// so a panel taller than its dock reads as scrollable rather than cut off.
void addScrollFade(QAbstractScrollArea *area);
QToolButton *iconButton(const QString &icon, const QString &tooltip, const QString &objectName = {},
                        QWidget *parent = nullptr);
// Runs callback on the next event-loop turn after target receives one of types
// (used to follow canvas zoom without adding signals to the renderer).
void afterEvents(QObject *target, std::initializer_list<QEvent::Type> types, std::function<void()> callback);
} // namespace nova::ui
