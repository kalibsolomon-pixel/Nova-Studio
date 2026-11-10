// SPDX-License-Identifier: GPL-3.0-or-later
#include "tool_placement.h"
#include "theme.h"
#include "tools.h"
#include <QActionGroup>
#include <QApplication>
#include <QDockWidget>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QStatusBar>
#include <QVBoxLayout>

namespace nova {
namespace {
const ToolPlacement allPlacements[] = {ToolPlacement::LeftEdge,     ToolPlacement::RightEdge,
                                       ToolPlacement::TopEdge,      ToolPlacement::AboveColours,
                                       ToolPlacement::AboveAssets,  ToolPlacement::Floating};
constexpr auto layoutPrefix = "NOVALAYOUT1 ";
// Snap-target sizes (device-independent pixels).
constexpr int EdgeTarget = 44, DockTarget = 64;

// Paints the snap targets over the main window while the grip is dragged.
class SnapOverlay : public QWidget {
    const ToolStripPlacement *owner;

  public:
    SnapOverlay(const ToolStripPlacement *o, QWidget *parent) : QWidget(parent), owner(o) {
        setObjectName("toolSnapOverlay");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(parent->rect());
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const auto &t = ui::current();
        const auto targets = owner->targets();
        p.fillRect(rect(), QColor(0, 0, 0, t.dark ? 60 : 30));
        for (int i = 0; i < targets.size(); ++i) {
            const bool hot = i == owner->hoveredTarget();
            QColor fill = t.accent;
            fill.setAlpha(hot ? 120 : 45);
            p.fillRect(targets[i].rect, fill);
            p.setPen(QPen(t.accent, hot ? 3 : 1, hot ? Qt::SolidLine : Qt::DashLine));
            p.drawRect(targets[i].rect.adjusted(0, 0, -1, -1));
            const auto label = p.fontMetrics().boundingRect(targets[i].label).adjusted(-6, -3, 6, 3);
            auto pill = label;
            pill.moveCenter(targets[i].rect.center());
            p.fillRect(pill, hot ? t.accent : t.raised);
            p.setPen(hot ? t.selectionText : t.text);
            p.drawText(pill, Qt::AlignCenter, targets[i].label);
        }
        if (owner->hoveredTarget() < 0) {
            // Free placement: show where the floating strip would appear.
            const auto local = mapFromGlobal(owner->dragCursor());
            QRect ghost(local, QSize(220, 44));
            p.setPen(QPen(t.accent, 2, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(ghost);
            p.setPen(t.text);
            p.drawText(ghost, Qt::AlignCenter, "Release to float here");
        }
    }
};
} // namespace

QString toolPlacementId(ToolPlacement p) {
    switch (p) {
    case ToolPlacement::LeftEdge: return "left";
    case ToolPlacement::RightEdge: return "right";
    case ToolPlacement::TopEdge: return "top";
    case ToolPlacement::AboveColours: return "above-colours";
    case ToolPlacement::AboveAssets: return "above-assets";
    case ToolPlacement::Floating: return "floating";
    }
    return "left";
}
QString toolPlacementLabel(ToolPlacement p) {
    switch (p) {
    case ToolPlacement::LeftEdge: return "Left Edge (Default)";
    case ToolPlacement::RightEdge: return "Right Edge";
    case ToolPlacement::TopEdge: return "Top, Below Tool Options";
    case ToolPlacement::AboveColours: return "Above Colours";
    case ToolPlacement::AboveAssets: return "Above Assets";
    case ToolPlacement::Floating: return "Floating";
    }
    return {};
}
ToolPlacement toolPlacementFromId(const QString &id, bool *ok) {
    for (auto p : allPlacements)
        if (toolPlacementId(p) == id) {
            if (ok)
                *ok = true;
            return p;
        }
    if (ok)
        *ok = false;
    return ToolPlacement::LeftEdge;
}
QByteArray packLayout(ToolPlacement placement, const QByteArray &state) {
    return QByteArray(layoutPrefix) + toolPlacementId(placement).toLatin1() + '\n' + state;
}
bool unpackLayout(const QByteArray &blob, ToolPlacement *placement, QByteArray *state) {
    *placement = ToolPlacement::LeftEdge;
    if (!blob.startsWith(layoutPrefix)) {
        *state = blob;
        return true;
    }
    const auto end = blob.indexOf('\n');
    const auto prefix = qsizetype(qstrlen(layoutPrefix));
    if (end < 0 || end - prefix > 32) {
        *state = {};
        return false;
    }
    bool ok = false;
    *placement = toolPlacementFromId(QString::fromLatin1(blob.mid(prefix, end - prefix)), &ok);
    *state = blob.mid(end + 1);
    return ok;
}

// ---------------------------------------------------------------- grip
ToolGrip::ToolGrip(ToolStripPlacement *o, QWidget *parent) : QToolButton(parent), owner(o) {
    setObjectName("toolPlacementGrip");
    setAutoRaise(true);
    setCursor(Qt::SizeAllCursor);
    setToolTip("Move the tools: drag to a highlighted position or anywhere to float\n"
               "Click, Enter or Space: choose a position");
    setAccessibleName("Move tools");
    setAccessibleDescription("Drag to a highlighted position, or press Enter for the list of positions");
    setPopupMode(QToolButton::InstantPopup);
    setFocusPolicy(Qt::TabFocus);
    setProperty("novaIcon", "drag");
}
void ToolGrip::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return QToolButton::mousePressEvent(e);
    pressed = e->globalPosition().toPoint();
    dragging = cancelled = false;
    setFocus(Qt::MouseFocusReason);
    e->accept();
}
void ToolGrip::mouseMoveEvent(QMouseEvent *e) {
    if (!(e->buttons() & Qt::LeftButton))
        return;
    const auto global = e->globalPosition().toPoint();
    if (!dragging && (global - pressed).manhattanLength() >= QApplication::startDragDistance()) {
        dragging = true;
        owner->beginDrag(global);
    }
    if (dragging)
        owner->moveDrag(global);
}
void ToolGrip::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return QToolButton::mouseReleaseEvent(e);
    if (dragging) {
        dragging = false;
        owner->finishDrag(e->globalPosition().toPoint(), false);
    } else if (cancelled) {
        cancelled = false;
    } else if (menu()) {
        showMenu(); // a plain click lists the positions
    }
}
void ToolGrip::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape && dragging) {
        dragging = false;
        cancelled = true;
        owner->finishDrag(QCursor::pos(), true);
        return;
    }
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space) && menu()) {
        showMenu();
        return;
    }
    QToolButton::keyPressEvent(e);
}

// ---------------------------------------------------------------- placement
ToolStripPlacement::ToolStripPlacement(QMainWindow *w, ToolBox *b, QDockWidget *c, QDockWidget *a)
    : QObject(w), window(w), box(b), colours(c), assets(a) {
    dock = new QDockWidget("Tools", window);
    dock->setObjectName("toolsDock");
    // The grip is the only handle: no native title bar, close or float button.
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setTitleBarWidget(new QWidget(dock));
    host = new QWidget(dock);
    host->setObjectName("toolsDockHost");
    auto layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // Fixed height: the panel is exactly as tall as the strip's rows, so it never takes
    // height from the panel below it (a dock's maximum follows its content's policy).
    host->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    dock->setWidget(host);
    window->addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->hide();
    // The dock's own show/hide action (Panels menu, toolbar context menu) means "tools in
    // the panel": on puts the strip above Colours, off returns it to the left edge. Qt's
    // default would show an empty dock while the strip is at a window edge.
    auto panel = dock->toggleViewAction();
    panel->setText("Tools Panel (Above Colours)");
    panel->setObjectName("toolsPanelToggle");
    QObject::disconnect(panel, &QAction::triggered, dock, nullptr);
    panel->setEnabled(true); // Qt disables it for docks that are not closable
    connect(panel, &QAction::triggered, this, [this](bool on) {
        box->show();
        place(on ? ToolPlacement::AboveColours : ToolPlacement::LeftEdge);
    });

    positions = new QMenu("Tool Strip Position", window);
    positions->setObjectName("toolPlacementMenu");
    choices = new QActionGroup(positions);
    for (auto p : allPlacements) {
        auto a = positions->addAction(toolPlacementLabel(p));
        a->setObjectName("toolPlacement-" + toolPlacementId(p));
        a->setCheckable(true);
        a->setData(int(p));
        choices->addAction(a);
        connect(a, &QAction::triggered, this, [this, p] { place(p); });
    }
    grip = new ToolGrip(this, box);
    grip->setMenu(positions);
    grip->setIcon(ui::lineIcon("drag"));
    // ToolBox::actions (its tool map) hides QWidget::actions().
    box->insertWidget(static_cast<QWidget *>(box)->actions().value(0), grip);
    box->setMovable(false); // the grip replaces Qt's own toolbar handle
    // View > Panels > Tool Strip also shows and hides the strip while the dock hosts it.
    connect(box->toggleViewAction(), &QAction::toggled, this, [this](bool on) {
        if (box->parentWidget() == host)
            dock->setVisible(on);
    });
    syncMenu();
}
void ToolStripPlacement::syncMenu() {
    for (auto a : choices->actions())
        a->setChecked(ToolPlacement(a->data().toInt()) == current);
    grip->setToolTip("Move the tools (now: " + toolPlacementLabel(current) +
                     ")\nDrag to a highlighted position or anywhere to float\n"
                     "Click, Enter or Space: choose a position");
}
void ToolStripPlacement::place(ToolPlacement p, QPoint at) {
    const bool hosted = box->parentWidget() == host;
    const bool edge = p == ToolPlacement::LeftEdge || p == ToolPlacement::RightEdge || p == ToolPlacement::TopEdge;
    const bool visible = !box->isHidden();
    if (!hosted && window->toolBarBreak(box))
        window->removeToolBarBreak(box);
    if (edge) {
        if (hosted)
            host->layout()->removeWidget(box);
        box->setWrapping(false);
        dock->hide();
        box->setOrientation(p == ToolPlacement::TopEdge ? Qt::Horizontal : Qt::Vertical);
        window->addToolBar(p == ToolPlacement::LeftEdge    ? Qt::LeftToolBarArea
                           : p == ToolPlacement::RightEdge ? Qt::RightToolBarArea
                                                           : Qt::TopToolBarArea,
                           box);
        if (p == ToolPlacement::TopEdge)
            window->insertToolBarBreak(box); // its own row below Tool Options
    } else {
        if (!hosted) {
            window->removeToolBar(box);
            static_cast<QVBoxLayout *>(host->layout())->insertWidget(0, box);
        }
        box->setOrientation(Qt::Horizontal);
        box->setWrapping(true); // rows instead of an overflow button in a narrow panel
        dock->setFloating(false);
        QDockWidget *below = p == ToolPlacement::AboveColours ? colours.data()
                             : p == ToolPlacement::AboveAssets ? assets.data()
                                                               : nullptr;
        if (below) {
            const auto area = window->dockWidgetArea(below);
            window->addDockWidget(area == Qt::NoDockWidgetArea ? Qt::RightDockWidgetArea : area, dock);
            dock->show(); // a hidden dock has no layout item to split
            below->setFloating(false);
            below->show();
            // Splitting moves `below` directly under the tools; its former tab
            // neighbours stay together where they were.
            window->splitDockWidget(dock, below, Qt::Vertical);
            below->raise();
        } else {
            window->addDockWidget(Qt::RightDockWidgetArea, dock);
            dock->setFloating(true);
            // One row when the screen allows it; the strip wraps if the user narrows it.
            const int row = box->singleRowWidth() + 2 * ui::ToolStripPaddingH + 4;
            dock->resize(std::max(200, row), 40); // grows to its minimum if it needs more rows
            dock->move(at.isNull() ? window->mapToGlobal(QPoint(80, 120)) : at);
        }
        dock->show();
        dock->raise();
    }
    box->setVisible(visible);
    current = p;
    keepReachable();
    syncMenu();
    emit placed(p);
}
void ToolStripPlacement::keepReachable() {
    if (!dock->isFloating() || !dock->isVisible())
        return;
    QList<QRect> available;
    for (auto screen : QGuiApplication::screens())
        available.append(screen->availableGeometry());
    const auto frame = dock->frameGeometry();
    const auto target = reachableFrame(frame, available);
    if (target == frame)
        return;
    dock->resize(dock->size() - (frame.size() - target.size()));
    dock->move(target.topLeft());
}
QRect ToolStripPlacement::reachableFrame(QRect frame, const QList<QRect> &available) {
    if (available.isEmpty())
        return frame;
    // The whole strip must fit on one screen; otherwise move it onto the nearest one.
    for (const auto &screen : available)
        if (screen.contains(frame))
            return frame;
    QRect best = available.first();
    for (const auto &screen : available)
        if ((screen.center() - frame.center()).manhattanLength() < (best.center() - frame.center()).manhattanLength())
            best = screen;
    frame.setSize(frame.size().boundedTo(best.size()));
    frame.moveLeft(std::clamp(frame.left(), best.left(), best.right() - frame.width() + 1));
    frame.moveTop(std::clamp(frame.top(), best.top(), best.bottom() - frame.height() + 1));
    return frame;
}
QVector<ToolStripPlacement::Target> ToolStripPlacement::targets() const {
    QVector<Target> out;
    // Content area: below the menu bar and top toolbars, above the status bar.
    int top = window->menuBar() && window->menuBar()->isVisible() ? window->menuBar()->geometry().bottom() + 1 : 0;
    for (auto bar : window->findChildren<QToolBar *>())
        if (bar != box && bar->isVisible() && bar->parentWidget() == window &&
            window->toolBarArea(bar) == Qt::TopToolBarArea)
            top = std::max(top, bar->geometry().bottom() + 1);
    const int bottom = window->statusBar() && window->statusBar()->isVisible() ? window->statusBar()->geometry().top()
                                                                               : window->height();
    const QRect area(0, top, window->width(), std::max(EdgeTarget, bottom - top));
    // A docked panel's target: the top band of the visible panel (or of the tab group
    // showing it), labelled with the panel it sits above.
    auto above = [&](QDockWidget *panel, ToolPlacement p) {
        if (!panel || panel->isFloating() || window->dockWidgetArea(panel) == Qt::NoDockWidgetArea || panel->isHidden())
            return;
        // Qt parks background tabs off-screen but still "visible": use the tab on screen.
        const auto onScreen = [](QWidget *w) { return w->isVisible() && !w->visibleRegion().isEmpty(); };
        QWidget *shown = onScreen(panel) ? panel : nullptr;
        if (!shown)
            for (auto sibling : window->tabifiedDockWidgets(panel))
                if (onScreen(sibling))
                    shown = sibling;
        if (!shown)
            return;
        QRect r(shown->mapTo(window, QPoint()), shown->size());
        // Keep clear of the edge strips so targets never overlap.
        r.setLeft(std::max(r.left(), area.left() + EdgeTarget + 4));
        r.setRight(std::min(r.right(), area.right() - EdgeTarget - 4));
        out.append({p, QRect(r.left(), r.top(), r.width(), std::min(DockTarget, r.height() / 2)),
                    toolPlacementLabel(p)});
    };
    above(colours, ToolPlacement::AboveColours);
    above(assets, ToolPlacement::AboveAssets);
    // Top: a band over the canvas column only, so it never covers the side panels' targets.
    QRect canvas = window->centralWidget() ? QRect(window->centralWidget()->mapTo(window, QPoint()),
                                                   window->centralWidget()->size())
                                           : area;
    canvas.setLeft(std::max(canvas.left(), area.left() + EdgeTarget + 4));
    canvas.setRight(std::min(canvas.right(), area.right() - EdgeTarget - 4));
    out.append({ToolPlacement::TopEdge,
                QRect(canvas.left(), area.top(), std::max(EdgeTarget, canvas.width()), EdgeTarget),
                toolPlacementLabel(ToolPlacement::TopEdge)});
    out.append({ToolPlacement::LeftEdge, QRect(area.left(), area.top(), EdgeTarget, area.height()), "Left"});
    out.append({ToolPlacement::RightEdge, QRect(area.right() - EdgeTarget + 1, area.top(), EdgeTarget, area.height()), "Right"});
    return out;
}
void ToolStripPlacement::beginDrag(QPoint global) {
    delete overlay;
    overlay = new SnapOverlay(this, window);
    overlay->show();
    overlay->raise();
    moveDrag(global);
}
void ToolStripPlacement::moveDrag(QPoint global) {
    cursor = global;
    hovered = -1;
    const auto local = window->mapFromGlobal(global);
    const auto list = targets();
    for (int i = 0; i < list.size(); ++i)
        if (list[i].rect.contains(local)) {
            hovered = i;
            break;
        }
    if (overlay)
        overlay->update();
}
void ToolStripPlacement::finishDrag(QPoint global, bool cancel) {
    moveDrag(global);
    const auto list = targets();
    const int chosen = hovered;
    delete overlay;
    overlay = nullptr;
    hovered = -1;
    if (cancel)
        return;
    if (chosen >= 0)
        place(list[chosen].placement);
    else
        place(ToolPlacement::Floating, global - QPoint(12, 12));
}
} // namespace nova
