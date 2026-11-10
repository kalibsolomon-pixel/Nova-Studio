// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Where the tool strip lives. A small extension of the main window's own toolbar and dock
// layout, not a docking framework: edge placements keep the strip a QMainWindow toolbar
// (the left edge is the product default), the other placements host the same strip in one
// "toolsDock" QDockWidget split directly above the Colours or Assets panel, or floating.
// The strip's grip starts a drag that shows every snap target; Enter/Space on the grip,
// and View > Panels > Tool Strip Position, list the same positions for the keyboard.
#include <QByteArray>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QToolButton>
#include <QVector>

class QDockWidget;
class QMainWindow;
class QMenu;
class QActionGroup;
namespace nova {
class ToolBox;
enum class ToolPlacement { LeftEdge, RightEdge, TopEdge, AboveColours, AboveAssets, Floating };
QString toolPlacementId(ToolPlacement placement);
QString toolPlacementLabel(ToolPlacement placement);
// Unknown or empty ids give the default (left edge) and set *ok to false.
ToolPlacement toolPlacementFromId(const QString &id, bool *ok = nullptr);

// Saved layouts carry the placement in front of QMainWindow::saveState() bytes:
// "NOVALAYOUT1 <placement>\n<state>". Older blobs without the prefix mean the left edge.
QByteArray packLayout(ToolPlacement placement, const QByteArray &state);
// Returns false only for a prefixed blob whose placement is unknown (state still set).
bool unpackLayout(const QByteArray &blob, ToolPlacement *placement, QByteArray *state);

class ToolStripPlacement;
class ToolGrip : public QToolButton {
    ToolStripPlacement *owner;
    QPoint pressed;
    bool dragging = false, cancelled = false; // a cancelled drag's release is not a click

  public:
    ToolGrip(ToolStripPlacement *owner, QWidget *parent);

  protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
};

class ToolStripPlacement : public QObject {
    Q_OBJECT
    QMainWindow *window;
    ToolBox *box;
    QPointer<QDockWidget> colours, assets;
    QDockWidget *dock;
    QWidget *host;
    QWidget *overlay = nullptr;
    QMenu *positions;
    QActionGroup *choices;
    ToolPlacement current = ToolPlacement::LeftEdge;
    int hovered = -1;
    QPoint cursor;
    void syncMenu();

  public:
    struct Target {
        ToolPlacement placement;
        QRect rect; // main-window coordinates
        QString label;
    };
    ToolStripPlacement(QMainWindow *window, ToolBox *box, QDockWidget *colours, QDockWidget *assets);
    QDockWidget *dockWidget() const { return dock; }
    QMenu *menu() const { return positions; }
    ToolGrip *grip = nullptr;
    ToolPlacement placement() const { return current; }
    // Moves the strip. Floating uses `at` (global top-left; default: near the window's
    // top-left) and is kept inside a screen's available area.
    void place(ToolPlacement placement, QPoint at = {});
    // Snap targets for the current window arrangement, most specific first.
    QVector<Target> targets() const;
    // Drag feedback, driven by the grip (global cursor positions).
    void beginDrag(QPoint global);
    void moveDrag(QPoint global);
    void finishDrag(QPoint global, bool cancel);
    bool dragging() const { return overlay != nullptr; }
    int hoveredTarget() const { return hovered; }
    QPoint dragCursor() const { return cursor; }
    // Keeps a floating strip reachable after screen or DPI changes.
    void keepReachable();
    // The frame unchanged if one available screen area contains it; otherwise shrunk to
    // fit and moved onto the nearest one. Screens are in logical (DPI-scaled) coordinates.
    static QRect reachableFrame(QRect frame, const QList<QRect> &available);
  signals:
    void placed(nova::ToolPlacement placement);
};
} // namespace nova
