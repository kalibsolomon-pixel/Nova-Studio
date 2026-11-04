// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Phase 3-B design system: colour tokens, the application palette, one scoped
// stylesheet and vector line icons. Every surface reads from these tokens so
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
};
// Spacing scale (device-independent pixels).
constexpr int SpaceXS = 2, SpaceS = 4, SpaceM = 6, SpaceL = 8, SpaceXL = 12;
constexpr int ControlHeight = 24; // buttons, combos, spin boxes, line edits
constexpr int IconSize = 18;      // command icons in toolbars and panels
constexpr int ToolIconSize = 22;  // left tool strip

Tokens tokens(bool dark, const QString &accent);
QPalette palette(const Tokens &t);
QString styleSheet(const Tokens &t);
// The tokens most recently applied by Window::applyTheme(); widgets that
// paint custom content read these instead of hard-coding colours.
const Tokens &current();
void setCurrent(const Tokens &t);

// Line icons drawn on a 24-unit grid at several sizes for crisp DPI scaling.
// Names: save undo redo open folder search close add duplicate delete rename
// up down tree grid sort parent zoom-in zoom-out fit actual center settings
// activity workspace command keyboard eye
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
