// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Phase 3-B design system: colour tokens, the application palette, one scoped
// stylesheet and Material Symbols Outlined icons. Every surface reads from these tokens so
// light and dark themes stay consistent (see PHASE_3B_DESIGN_SYSTEM.md).
#include <QColor>
#include <QEvent>
#include <QIcon>
#include <functional>
#include <QPalette>
#include <QString>

class QLabel;
class QToolButton;
class QWidget;
namespace nova::ui {
struct Tokens {
    bool dark = true;
    QColor window, surface, raised, control, base, border, borderStrong;
    QColor text, muted, disabled, placeholder;
    QColor accent, selection, selectionText;
    QColor warning, danger, success;
    // Transparency checkerboard and pixel grid on the editing canvas. The squares are
    // mid-tones so dark and light texture pixels both stand out, they differ from each
    // other less than the grid line does, and the canvas surround (base) is clearly
    // darker (dark theme) or lighter (light theme) than both, so the texture edge reads.
    QColor checkerDark, checkerLight, pixelGrid;
};
// Spacing scale (device-independent pixels).
constexpr int SpaceXS = 2, SpaceS = 4, SpaceM = 6, SpaceL = 8, SpaceXL = 12;
constexpr int ControlHeight = 24; // buttons, combos, spin boxes, line edits
constexpr int IconSize = 18;      // command icons in toolbars and panels
constexpr int ToolIconSize = 22;  // left tool strip
// Toolbar, button and panel metrics. styleSheet() and the toolbar/panel layouts read
// these instead of per-widget pixel values (documented in docs/editor-ui-refinement).
constexpr int ButtonPadding = SpaceXS;       // inside icon buttons, around the icon
constexpr int ButtonBorder = 1;              // transparent until hover/focus/checked
// Hit target of a tool-strip button: icon + padding + border on both sides (28 px).
constexpr int ToolHitTarget = ToolIconSize + 2 * (ButtonPadding + ButtonBorder);
constexpr int ToolbarSpacing = SpaceS;       // between items in a toolbar
constexpr int ToolbarPaddingV = 3, ToolbarPaddingH = SpaceM;
constexpr int GroupGap = SpaceXS;            // extra gap between related option groups
constexpr int SeparatorMarginMajor = SpaceM; // separator inset along the toolbar
constexpr int SeparatorMarginMinor = SpaceS; // separator inset across the toolbar
constexpr int MenuArrowWidth = 10;           // drop-down segment of split buttons
constexpr int MenuArrowGap = SpaceXS;        // space between a button's icon and its arrow
constexpr int PanelPadding = SpaceL;         // dock titles, dialog sections, control panels
constexpr int PanelPaddingV = 5;             // dock title rows
constexpr int ToolStripPaddingV = SpaceS, ToolStripPaddingH = 3; // around the tool strip
constexpr int ToolStripSeparatorMargin = 5;  // separator inset along the tool strip

Tokens tokens(bool dark, const QString &accent);
QPalette palette(const Tokens &t);
QString styleSheet(const Tokens &t);
// The tokens most recently applied by Window::applyTheme(); widgets that
// paint custom content read these instead of hard-coding colours.
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
QToolButton *iconButton(const QString &icon, const QString &tooltip, const QString &objectName = {},
                        QWidget *parent = nullptr);
// Runs callback on the next event-loop turn after target receives one of types
// (used to follow canvas zoom without adding signals to the renderer).
void afterEvents(QObject *target, std::initializer_list<QEvent::Type> types, std::function<void()> callback);
} // namespace nova::ui
