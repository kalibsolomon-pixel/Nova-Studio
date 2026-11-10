// SPDX-License-Identifier: GPL-3.0-or-later
// Movable tool strip: product default on the left edge, drag with visible snap targets
// (including directly above the Colours panel), keyboard positions, free floating that
// stays reachable, per-workspace persistence across restart, Reset Workspace, and
// recovery from unknown or off-screen saved positions. With NOVA_EVIDENCE_DIR set, the
// drag feedback and each position are captured.
#include "settings.h"
#include "tool_placement.h"
#include "theme.h"
#include "tools.h"
#include "window.h"
#include <QApplication>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>

namespace nova {
class EditorTestAccess {
  public:
    static ToolStripPlacement *placement(Window &w) { return w.toolPlacement; }
    static Preferences &preferences(Window &w) { return w.preferences; }
    static void reset(Window &w) { w.resetWorkspace(); }
    static QDockWidget *dock(Window &w, const char *name) { return w.findChild<QDockWidget *>(name); }
    static qint64 rechecks(Window &w) { return w.metrics.counters.value("screen_change_rechecks").toInteger(); }
};
} // namespace nova
using namespace nova;
using Access = EditorTestAccess;

namespace {
void mouse(QWidget *w, QEvent::Type type, QPoint global, Qt::MouseButton button, Qt::MouseButtons buttons) {
    const auto local = w->mapFromGlobal(global);
    QMouseEvent e(type, QPointF(local), QPointF(global), button, buttons, {});
    QApplication::sendEvent(w, &e);
}
QRect inWindow(QMainWindow &w, QWidget *child) { return QRect(child->mapTo(&w, QPoint()), child->size()); }
bool onSomeScreen(const QRect &frame) {
    for (auto screen : QGuiApplication::screens())
        if (screen->availableGeometry().contains(frame))
            return true;
    return false;
}
} // namespace

class ToolPlacementTests : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QString evidence;

    std::unique_ptr<Window> open(const QString &settings, QSize size = {1280, 800}) {
        qApp->setProperty("settingsFile", temporary.filePath(settings));
        QElapsedTimer timer;
        timer.start();
        auto window = std::make_unique<Window>(QString(), QString(), timer);
        window->resize(size);
        window->show();
        if (!QTest::qWaitForWindowExposed(window.get()))
            return {};
        QTest::qWait(60);
        return window;
    }
    void capture(QWidget &w, const QString &name) {
        if (evidence.isEmpty())
            return;
        QDir().mkpath(evidence);
        QTest::qWait(60);
        QVERIFY(w.grab().save(evidence + "/tools-" + name + ".png"));
    }
    // Drags the grip with real mouse events to the centre of target `wanted`.
    void dragTo(Window &window, ToolPlacement wanted, const QString &shot = {}) {
        auto placement = Access::placement(window);
        auto grip = placement->grip;
        QVERIFY(grip->isVisible());
        const auto start = grip->mapToGlobal(grip->rect().center());
        mouse(grip, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        mouse(grip, QEvent::MouseMove, start + QPoint(20, 20), Qt::NoButton, Qt::LeftButton);
        QVERIFY(placement->dragging());
        auto overlay = window.findChild<QWidget *>("toolSnapOverlay");
        QVERIFY(overlay && overlay->isVisible());
        QRect target;
        for (const auto &t : placement->targets())
            if (t.placement == wanted)
                target = t.rect;
        QVERIFY2(target.isValid(), qPrintable(toolPlacementId(wanted)));
        const auto end = window.mapToGlobal(target.center());
        mouse(grip, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        QVERIFY(placement->hoveredTarget() >= 0);
        QCOMPARE(placement->targets()[placement->hoveredTarget()].placement, wanted);
        if (!shot.isEmpty())
            capture(window, shot);
        mouse(grip, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        QVERIFY(!placement->dragging());
        QTRY_VERIFY(!window.findChild<QWidget *>("toolSnapOverlay"));
        QCOMPARE(placement->placement(), wanted);
    }
    // Every strip item is shown, inside the tools panel (or toolbar), not overlapping another
    // item, and no QToolBar overflow button is in use.
    void checkEveryItemReachable(Window &window) {
        const QStringList names = {"toolPlacementGrip", "toolGroup-Draw",   "toolGroup-Select", "toolGroup-Transform",
                                   "toolGroup-Shapes",  "toolGroup-Fill",   "toolGroup-Colour", "effectsButton",
                                   "toolColorChip"};
        auto box = window.findChild<ToolBox *>("toolBox");
        QTRY_VERIFY(box->isVisible());
        QApplication::processEvents();
        const QRect strip(box->mapTo(&window, QPoint()), box->size());
        QList<QRect> placed;
        for (const auto &name : names) {
            auto w = window.findChild<QWidget *>(name);
            QVERIFY2(w, qPrintable(name));
            QTRY_VERIFY2(w->isVisible() && !w->visibleRegion().isEmpty(), qPrintable(name));
            const QRect r(w->mapTo(&window, QPoint()), w->size());
            const auto flow = window.findChild<QWidget *>("toolStripFlow");
            QVERIFY2(strip.contains(r), qPrintable(name + QString(" outside the strip (%1,%2 %3x%4) strip %5,%6 %7x%8 parent %9 flow %10x%11 max %12")
                                                       .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())
                                                       .arg(strip.x()).arg(strip.y()).arg(strip.width()).arg(strip.height())
                                                       .arg(w->parentWidget() ? w->parentWidget()->objectName() : QString("-"))
                                                       .arg(flow ? flow->width() : -1).arg(flow ? flow->height() : -1)
                                                       .arg(box->maximumHeight())));
            for (const auto &other : placed)
                QVERIFY2(!other.intersects(r), qPrintable(name + " overlaps another item"));
            placed << r;
            QVERIFY2(!w->accessibleName().isEmpty() || !w->toolTip().isEmpty(), qPrintable(name));
        }
        for (auto b : box->findChildren<QToolButton *>("qt_toolbar_ext_button"))
            QVERIFY2(!b->isVisible(), "the toolbar overflow button is shown");
    }
    void checkAboveColours(Window &window) {
        auto tools = Access::dock(window, "toolsDock");
        auto colours = Access::dock(window, "colors");
        QTRY_VERIFY(tools->isVisible() && colours->isVisible());
        QCOMPARE(window.dockWidgetArea(tools), Qt::RightDockWidgetArea);
        QCOMPARE(window.dockWidgetArea(colours), Qt::RightDockWidgetArea);
        QTRY_VERIFY(!colours->visibleRegion().isEmpty());
        // The main window lays docks out on the next event-loop turns.
        QTRY_VERIFY(inWindow(window, tools).bottom() < inWindow(window, colours).top() && inWindow(window, tools).top() >= 0);
        const auto t = inWindow(window, tools), c = inWindow(window, colours);
        QVERIFY2(t.bottom() < c.top() && c.top() - t.bottom() <= 12,
                 qPrintable(QString("tools %1,%2 %3x%4 colours %5,%6 %7x%8").arg(t.x()).arg(t.y()).arg(t.width())
                                .arg(t.height()).arg(c.x()).arg(c.y()).arg(c.width()).arg(c.height())));
        QVERIFY(std::abs(t.left() - c.left()) <= 2);
        QCOMPARE(window.findChild<ToolBox *>("toolBox")->parentWidget()->objectName(), QString("toolsDockHost"));
    }

  private slots:
    void initTestCase() { evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR"); }

    void defaultIsTheLeftEdge() {
        auto window = open("default/studio.ini");
        QVERIFY(window);
        auto placement = Access::placement(*window);
        QCOMPARE(placement->placement(), ToolPlacement::LeftEdge);
        auto box = window->findChild<ToolBox *>("toolBox");
        QCOMPARE(window->toolBarArea(box), Qt::LeftToolBarArea);
        QCOMPARE(box->orientation(), Qt::Vertical);
        QVERIFY(!Access::dock(*window, "toolsDock")->isVisible());
        auto grip = window->findChild<QToolButton *>("toolPlacementGrip");
        QVERIFY(grip && grip->isVisible());
        QCOMPARE(grip->accessibleName(), QString("Move tools"));
        QVERIFY(grip->focusPolicy() & Qt::TabFocus);
        QVERIFY(!grip->icon().isNull());
        QCOMPARE(placement->menu()->actions().size(), 6);
        QVERIFY(window->findChild<QAction *>("toolPlacement-left")->isChecked());
        // Reachable from the menu bar (and so the command palette).
        auto panels = window->findChild<QMenu *>("panelsMenu");
        QVERIFY(panels->actions().contains(placement->menu()->menuAction()));
        // The Tools panel toggle moves the strip into the panel and back; it never shows
        // an empty panel.
        auto panelToggle = Access::dock(*window, "toolsDock")->toggleViewAction();
        QVERIFY(panels->actions().contains(panelToggle));
        QVERIFY(!panelToggle->isChecked());
        panelToggle->trigger();
        QCOMPARE(placement->placement(), ToolPlacement::AboveColours);
        QTRY_VERIFY(panelToggle->isChecked());
        checkAboveColours(*window);
        panelToggle->trigger();
        QCOMPARE(placement->placement(), ToolPlacement::LeftEdge);
        QTRY_VERIFY(!Access::dock(*window, "toolsDock")->isVisible());
        QVERIFY(window->findChild<ToolBox *>("toolBox")->isVisible());
        capture(*window, "01-default-left");
    }
    void dragSnapsAboveColoursWithFeedback() {
        auto window = open("drag/studio.ini");
        QVERIFY(window);
        // Every useful target is offered: above Colours and Assets, top, left, right.
        QStringList offered;
        for (const auto &t : Access::placement(*window)->targets())
            offered << toolPlacementId(t.placement);
        QCOMPARE(offered, QStringList({"above-colours", "above-assets", "top", "left", "right"}));
        // Targets are on screen and never overlap.
        const auto list = Access::placement(*window)->targets();
        for (int i = 0; i < list.size(); ++i) {
            QVERIFY2(window->rect().contains(list[i].rect), qPrintable(toolPlacementId(list[i].placement)));
            for (int j = i + 1; j < list.size(); ++j)
                QVERIFY2(!list[i].rect.intersects(list[j].rect),
                         qPrintable(toolPlacementId(list[i].placement) + " / " + toolPlacementId(list[j].placement)));
        }
        dragTo(*window, ToolPlacement::AboveColours, "02-drag-feedback-above-colours");
        checkAboveColours(*window);
        // Every tool and the colour chip stay on screen above Colours (the strip wraps).
        checkEveryItemReachable(*window);
        capture(*window, "03-above-colours");
        // Tools keep working from the new place: shortcut action, buttons and focus.
        window->findChild<QAction *>("tool-fill")->trigger();
        QCOMPARE(window->tools().tool, Tool::Fill);
        auto group = window->findChild<QToolButton *>("toolGroup-Draw");
        QVERIFY(group && group->isVisible() && group->isEnabled());
        group->click();
        QCOMPARE(window->tools().tool, Tool::Pencil);
        // Back to the left edge by dragging, too.
        dragTo(*window, ToolPlacement::LeftEdge);
        QCOMPARE(window->toolBarArea(window->findChild<ToolBox *>("toolBox")), Qt::LeftToolBarArea);
        QTRY_VERIFY(window->findChild<QWidget *>("toolColorChip")->isVisible());
        QTRY_VERIFY(!Access::dock(*window, "toolsDock")->isVisible());
    }
    void escapeCancelsAndKeyboardListsPositions() {
        auto window = open("keys/studio.ini");
        QVERIFY(window);
        auto placement = Access::placement(*window);
        auto grip = placement->grip;
        const auto start = grip->mapToGlobal(grip->rect().center());
        mouse(grip, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        mouse(grip, QEvent::MouseMove, start + QPoint(300, 200), Qt::NoButton, Qt::LeftButton);
        QVERIFY(placement->dragging());
        QTest::keyClick(grip, Qt::Key_Escape);
        QVERIFY(!placement->dragging());
        mouse(grip, QEvent::MouseButtonRelease, start + QPoint(300, 200), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(placement->placement(), ToolPlacement::LeftEdge);
        // Keyboard: focus the grip, Enter lists the positions; choose one from the menu.
        QTimer::singleShot(150, [] {
            auto popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            QVERIFY(popup);
            QCOMPARE(popup->objectName(), QString("toolPlacementMenu"));
            for (auto a : popup->actions())
                if (a->objectName() == "toolPlacement-right")
                    popup->setActiveAction(a);
            QTest::keyClick(popup, Qt::Key_Return);
        });
        grip->setFocus(Qt::TabFocusReason);
        QTest::keyClick(grip, Qt::Key_Return);
        QTRY_COMPARE(placement->placement(), ToolPlacement::RightEdge);
        QCOMPARE(window->toolBarArea(window->findChild<ToolBox *>("toolBox")), Qt::RightToolBarArea);
        capture(*window, "04-right-edge");
    }
    void everyPositionAndFreeFloating() {
        auto window = open("positions/studio.ini");
        QVERIFY(window);
        auto placement = Access::placement(*window);
        auto box = window->findChild<ToolBox *>("toolBox");
        window->findChild<QAction *>("toolPlacement-top")->trigger();
        QCOMPARE(window->toolBarArea(box), Qt::TopToolBarArea);
        QVERIFY(window->toolBarBreak(box));
        QCOMPARE(box->orientation(), Qt::Horizontal);
        auto options = window->findChild<QToolBar *>("toolOptions");
        QTRY_VERIFY(box->geometry().top() >= options->geometry().bottom()); // its own row below
        capture(*window, "05-top");
        window->findChild<QAction *>("toolPlacement-above-assets")->trigger();
        auto tools = Access::dock(*window, "toolsDock"), assets = Access::dock(*window, "assets");
        QTRY_VERIFY(tools->isVisible());
        QCOMPARE(window->dockWidgetArea(tools), Qt::LeftDockWidgetArea);
        QVERIFY(!window->toolBarBreak(box));
        QTRY_VERIFY(inWindow(*window, tools).bottom() < inWindow(*window, assets).top());
        capture(*window, "06-above-assets");
        // Free movement: drop away from every target.
        auto grip = placement->grip;
        const auto start = grip->mapToGlobal(grip->rect().center());
        const auto free = window->mapToGlobal(window->rect().center());
        mouse(grip, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        mouse(grip, QEvent::MouseMove, free, Qt::NoButton, Qt::LeftButton);
        QCOMPARE(placement->hoveredTarget(), -1);
        capture(*window, "07-drag-free");
        mouse(grip, QEvent::MouseButtonRelease, free, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(placement->placement(), ToolPlacement::Floating);
        QTRY_VERIFY(tools->isFloating() && tools->isVisible());
        QVERIFY(onSomeScreen(tools->frameGeometry()));
        QVERIFY((tools->frameGeometry().center() - free).manhattanLength() < 400);
        // Display size and DPI changes trigger a reachability check (the test display
        // harness pins windows on screen, so the off-screen rule itself is checked in
        // reachableFrameRecovers below).
        const auto before = Access::rechecks(*window);
        emit QGuiApplication::primaryScreen()->availableGeometryChanged(QGuiApplication::primaryScreen()->availableGeometry());
        emit QGuiApplication::primaryScreen()->logicalDotsPerInchChanged(QGuiApplication::primaryScreen()->logicalDotsPerInch());
        QCOMPARE(Access::rechecks(*window), before + 2);
        QTest::qWait(30);
        QVERIFY(onSomeScreen(tools->frameGeometry()));
        // View > Panels > Tool Strip still hides and shows the strip in the dock.
        box->toggleViewAction()->trigger();
        QTRY_VERIFY(!tools->isVisible());
        box->toggleViewAction()->trigger();
        QTRY_VERIFY(tools->isVisible());
    }
    void persistsPerWorkspaceAcrossRestart() {
        {
            auto window = open("persist/studio.ini");
            QVERIFY(window);
            QCOMPARE(window->workspacePreset(), QString("essentials"));
            window->findChild<QAction *>("toolPlacement-above-colours")->trigger();
            checkAboveColours(*window);
            // Another workspace starts from its product default...
            window->findChild<QAction *>("workspacePreset-texture")->trigger();
            QTest::qWait(50);
            QCOMPARE(Access::placement(*window)->placement(), ToolPlacement::LeftEdge);
            window->findChild<QAction *>("toolPlacement-right")->trigger();
            QTest::qWait(50);
            // ...and each workspace gets its own arrangement back.
            window->findChild<QAction *>("workspacePreset-essentials")->trigger();
            QTest::qWait(50);
            QCOMPARE(Access::placement(*window)->placement(), ToolPlacement::AboveColours);
            checkAboveColours(*window);
            window->findChild<QAction *>("workspacePreset-texture")->trigger();
            QTest::qWait(50);
            QCOMPARE(Access::placement(*window)->placement(), ToolPlacement::RightEdge);
            window->findChild<QAction *>("workspacePreset-essentials")->trigger();
            QTest::qWait(50);
            QVERIFY(window->close());
        }
        Settings settings(temporary.filePath("persist/studio.ini"));
        const auto saved = settings.load();
        QVERIFY(saved.layout.startsWith("NOVALAYOUT1 above-colours\n"));
        QVERIFY(saved.presetLayouts.value("texture").startsWith("NOVALAYOUT1 right\n"));
        {
            auto window = open("persist/studio.ini");
            QVERIFY(window);
            QCOMPARE(Access::placement(*window)->placement(), ToolPlacement::AboveColours);
            checkAboveColours(*window);
            window->findChild<QAction *>("workspacePreset-texture")->trigger();
            QTest::qWait(50);
            QCOMPARE(Access::placement(*window)->placement(), ToolPlacement::RightEdge);
            // Untouched workspaces keep the product default.
            window->findChild<QAction *>("workspacePreset-model")->trigger();
            QTest::qWait(50);
            QCOMPARE(Access::placement(*window)->placement(), ToolPlacement::LeftEdge);
            // Reset Workspace restores the product default everywhere.
            window->findChild<QAction *>("resetWorkspaceCommand")->trigger();
            QTest::qWait(50);
            QCOMPARE(window->workspacePreset(), QString("essentials"));
            QCOMPARE(Access::placement(*window)->placement(), ToolPlacement::LeftEdge);
            QCOMPARE(window->toolBarArea(window->findChild<ToolBox *>("toolBox")), Qt::LeftToolBarArea);
            QVERIFY(Access::preferences(*window).presetLayouts.isEmpty());
            window->findChild<QAction *>("workspacePreset-texture")->trigger();
            QTest::qWait(50);
            QCOMPARE(Access::placement(*window)->placement(), ToolPlacement::LeftEdge);
            QVERIFY(window->close());
        }
    }
    void invalidAndOffScreenSavedPositionsRecover() {
        // Unknown position and corrupt state: the default layout, tools on the left.
        {
            QSettings raw(temporary.filePath("bad/studio.ini"), QSettings::IniFormat);
            raw.setValue("workspace/layout", QByteArray("NOVALAYOUT1 sideways\ncorrupt"));
            raw.setValue("presetLayouts/texture", QByteArray("NOVALAYOUT1 ??\n"));
            raw.sync();
        }
        {
            auto window = open("bad/studio.ini");
            QVERIFY(window);
            QCOMPARE(Access::placement(*window)->placement(), ToolPlacement::LeftEdge);
            QCOMPARE(window->toolBarArea(window->findChild<ToolBox *>("toolBox")), Qt::LeftToolBarArea);
            window->findChild<QAction *>("workspacePreset-texture")->trigger();
            QTest::qWait(50);
            QCOMPARE(Access::placement(*window)->placement(), ToolPlacement::LeftEdge);
            QVERIFY(window->findChild<ToolBox *>("toolBox")->isVisible());
            // A floating strip saved off-screen comes back onto a screen after restart.
            window->findChild<QAction *>("workspacePreset-essentials")->trigger();
            window->findChild<QAction *>("toolPlacement-floating")->trigger();
            auto tools = Access::dock(*window, "toolsDock");
            QTRY_VERIFY(tools->isFloating());
            QVERIFY(window->close());
        }
        auto window = open("bad/studio.ini");
        QVERIFY(window);
        QCOMPARE(Access::placement(*window)->placement(), ToolPlacement::Floating);
        auto tools = Access::dock(*window, "toolsDock");
        QTRY_VERIFY(tools->isVisible());
        QTRY_VERIFY(onSomeScreen(tools->frameGeometry()));
        // Older layouts without a position prefix still restore (tools on the left).
        ToolPlacement placement;
        QByteArray state;
        QVERIFY(unpackLayout(window->saveState(1), &placement, &state));
        QCOMPARE(placement, ToolPlacement::LeftEdge);
        QVERIFY(!unpackLayout("NOVALAYOUT1 nowhere\nx", &placement, &state));
        QCOMPARE(placement, ToolPlacement::LeftEdge);
        QCOMPARE(state, QByteArray("x"));
        QVERIFY(window->close());
    }
    void reachableFrameRecovers() {
        const QRect primary(0, 0, 1920, 1040), second(1920, 0, 2560, 1400);
        const QRect strip(100, 100, 360, 44);
        // On a screen: unchanged.
        QCOMPARE(ToolStripPlacement::reachableFrame(strip, {primary, second}), strip);
        QCOMPARE(ToolStripPlacement::reachableFrame(strip.translated(2000, 0), {primary, second}), strip.translated(2000, 0));
        // Far off-screen: onto the nearest screen, whole.
        const auto back = ToolStripPlacement::reachableFrame(strip.translated(-30000, 40000), {primary, second});
        QVERIFY(primary.contains(back));
        QCOMPARE(back.size(), strip.size());
        // The second screen was removed: the strip moves onto the remaining one.
        const auto removed = ToolStripPlacement::reachableFrame(strip.translated(3000, 600), {primary});
        QVERIFY(primary.contains(removed));
        // Straddling two screens counts as not reachable as a whole.
        QVERIFY(primary.contains(ToolStripPlacement::reachableFrame(QRect(1800, 10, 360, 44), {primary})));
        // DPI change: the same physical screen at 150% has a smaller logical area.
        const QRect scaled(0, 0, 1280, 693);
        const auto rescaled = ToolStripPlacement::reachableFrame(QRect(1500, 900, 360, 44), {scaled});
        QVERIFY(scaled.contains(rescaled));
        // Larger than the screen: shrunk to fit.
        const auto shrunk = ToolStripPlacement::reachableFrame(QRect(0, 0, 3000, 44), {scaled});
        QVERIFY(scaled.contains(shrunk));
        QCOMPARE(shrunk.width(), scaled.width());
        // No screens reported: unchanged rather than guessed.
        QCOMPARE(ToolStripPlacement::reachableFrame(strip, {}), strip);
    }
    void narrowNormalAndWideColumnsKeepEveryToolReachable() {
        auto window = open("columns/studio.ini");
        QVERIFY(window);
        window->findChild<QAction *>("toolPlacement-above-colours")->trigger();
        checkAboveColours(*window);
        auto tools = Access::dock(*window, "toolsDock");
        auto colours = Access::dock(*window, "colors");
        auto box = window->findChild<ToolBox *>("toolBox");
        QVERIFY(box->wrapping());
        int previousRows = 0;
        for (int requested : {520, 300, 200}) { // wide, normal, narrow
            window->resizeDocks({tools}, {requested}, Qt::Horizontal);
            QTest::qWait(120);
            const int strip = window->findChild<QWidget *>("toolStripFlow")->height();
            const int rowHeight = window->findChild<QWidget *>("toolColorChip")->height();
            const int rows = (strip + ui::ToolbarSpacing) / (rowHeight + ui::ToolbarSpacing);
            qInfo() << "column" << requested << "->" << tools->width() << "px," << rows << "row(s)";
            checkEveryItemReachable(*window);
            QVERIFY(rows >= previousRows); // narrower columns wrap onto more rows, never fewer
            previousRows = rows;
            // The panel is only as tall as its rows (it does not take height from Colours), no
            // overlap with the Colours panel below, nothing outside the window.
            QTRY_VERIFY2(tools->height() <= box->height() + 4,
                         qPrintable(QString("tools panel %1 px for a %2 px strip").arg(tools->height()).arg(box->height())));
            QTRY_VERIFY(inWindow(*window, tools).bottom() < inWindow(*window, colours).top());
            QVERIFY(window->rect().contains(inWindow(*window, tools)));
            capture(*window, QString("09-above-colours-column-%1").arg(requested));
        }
        QVERIFY(previousRows >= 2);
        // Tools are usable from a wrapped row; the chip opens the Colours panel from the keyboard.
        window->findChild<QToolButton *>("toolGroup-Fill")->click();
        QVERIFY(window->tools().tool == Tool::Fill || window->tools().tool == Tool::Gradient);
        auto chip = window->findChild<QWidget *>("toolColorChip");
        QVERIFY(chip->focusPolicy() & Qt::TabFocus);
        colours->hide();
        chip->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(chip->hasFocus());
        QTest::keyClick(chip, Qt::Key_Space);
        QTRY_VERIFY(colours->isVisible());
        // Back to an edge and into the panel again: plain toolbar, then wrapping again.
        window->findChild<QAction *>("toolPlacement-left")->trigger();
        QVERIFY(!box->wrapping());
        QCOMPARE(window->toolBarArea(box), Qt::LeftToolBarArea);
        checkEveryItemReachable(*window);
        window->findChild<QAction *>("toolPlacement-above-assets")->trigger();
        QVERIFY(box->wrapping());
        checkEveryItemReachable(*window);
        window->findChild<QAction *>("toolPlacement-floating")->trigger();
        QTRY_VERIFY(tools->isFloating());
        checkEveryItemReachable(*window);
        QVERIFY(onSomeScreen(tools->frameGeometry()));
        QVERIFY(window->close());
    }
    void keyboardTraversalVisitsEveryItem() {
        auto window = open("keyboard/studio.ini");
        QVERIFY(window);
        window->findChild<QAction *>("toolPlacement-above-colours")->trigger();
        checkAboveColours(*window);
        Access::dock(*window, "toolsDock")->activateWindow();
        auto grip = Access::placement(*window)->grip;
        grip->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(grip->hasFocus());
        QStringList order{grip->objectName()};
        for (int i = 0; i < 12; ++i) {
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
            order << QApplication::focusWidget()->objectName();
        }
        const QStringList expected = {"toolPlacementGrip", "toolGroup-Draw",   "toolGroup-Select", "toolGroup-Transform",
                                      "toolGroup-Shapes",  "toolGroup-Fill",   "toolGroup-Colour", "effectsButton",
                                      "toolColorChip"};
        QVERIFY2(order.mid(0, expected.size()) == expected, qPrintable(order.join(", ")));
        QVERIFY(window->close());
    }
    void compactAndLargeWindowsKeepTargetsInside() {
        for (QSize size : {QSize(800, 550), QSize(1280, 800), QSize(1920, 1080)}) {
            auto window = open(QString("size-%1/studio.ini").arg(size.width()), size);
            QVERIFY(window);
            window->findChild<QAction *>("toolPlacement-above-colours")->trigger();
            QTest::qWait(60);
            auto tools = Access::dock(*window, "toolsDock");
            QTRY_VERIFY(tools->isVisible());
            QVERIFY(window->rect().contains(inWindow(*window, tools)));
            checkEveryItemReachable(*window);
            for (const auto &t : Access::placement(*window)->targets())
                QVERIFY2(window->rect().contains(t.rect), qPrintable(toolPlacementId(t.placement)));
            capture(*window, QString("08-above-colours-%1x%2").arg(size.width()).arg(size.height()));
            QVERIFY(window->close());
        }
    }
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication application(argc, argv);
    application.setStyle("Fusion");
    QFont font = application.font();
    font.setPointSize(9);
    application.setFont(font);
    ToolPlacementTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tool_placement_test.moc"
