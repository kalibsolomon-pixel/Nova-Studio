// SPDX-License-Identifier: GPL-3.0-or-later
// Fixed tool rail: always a vertical strip on the left edge, never movable or floatable,
// in every workspace, after Reset Workspace, and after restoring layouts saved by builds
// that let the strip move (any saved position comes back to the left edge). Every item stays
// reachable by pointer and keyboard in compact and large windows. With NOVA_EVIDENCE_DIR
// set, the rail is captured at each window size.
#include "settings.h"
#include "theme.h"
#include "tools.h"
#include "window.h"
#include "workspace_layout.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>

namespace nova {
class EditorTestAccess {
  public:
    static Preferences &preferences(Window &w) { return w.preferences; }
    static void reset(Window &w) { w.resetWorkspace(); }
};
} // namespace nova
using namespace nova;
using Access = EditorTestAccess;

namespace {
const QStringList railItems = {"toolGroup-Draw",   "toolGroup-Select", "toolGroup-Transform", "toolGroup-Shapes",
                               "toolGroup-Fill",   "toolGroup-Colour", "effectsButton",       "toolColorChip"};
} // namespace

class ToolStripTests : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QString evidence;

    std::unique_ptr<Window> open(const QString &settings, QSize size = {1280, 800}) {
        qApp->setProperty("settingsFile", temporary.filePath(settings));
        QElapsedTimer timer;
        timer.start();
        const auto pack = temporary.filePath("pack");
        QDir().mkpath(pack);
        auto window = std::make_unique<Window>(pack, QString(), timer);
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
    // The strip is a fixed vertical rail on the left edge, in the main window's own layout.
    void checkRail(Window &window) {
        auto box = window.findChild<ToolBox *>("toolBox");
        QVERIFY(box);
        QTRY_VERIFY(box->isVisible());
        QCOMPARE(box->parentWidget(), static_cast<QWidget *>(&window));
        QCOMPARE(window.toolBarArea(box), Qt::LeftToolBarArea);
        QCOMPARE(box->orientation(), Qt::Vertical);
        QVERIFY(!box->isMovable());
        QVERIFY(!box->isFloatable());
        QVERIFY(!box->isFloating());
        QCOMPARE(box->allowedAreas(), Qt::LeftToolBarArea);
        QVERIFY(!window.toolBarBreak(box));
        QVERIFY2(box->x() <= 2, qPrintable(QString("rail at x=%1").arg(box->x())));
    }
    // Every rail item is shown, inside the rail, not overlapping another item, and no
    // QToolBar overflow button hides any of them.
    void checkEveryItemReachable(Window &window) {
        auto box = window.findChild<ToolBox *>("toolBox");
        QTRY_VERIFY(box->isVisible());
        QApplication::processEvents();
        const QRect strip(box->mapTo(&window, QPoint()), box->size());
        QVERIFY(window.rect().contains(strip));
        QList<QRect> placed;
        for (const auto &name : railItems) {
            auto w = window.findChild<QWidget *>(name);
            QVERIFY2(w, qPrintable(name));
            QTRY_VERIFY2(w->isVisible() && !w->visibleRegion().isEmpty(), qPrintable(name));
            const QRect r(w->mapTo(&window, QPoint()), w->size());
            QVERIFY2(strip.contains(r), qPrintable(name + QString(" outside the rail (%1,%2 %3x%4) rail %5,%6 %7x%8")
                                                       .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())
                                                       .arg(strip.x()).arg(strip.y()).arg(strip.width()).arg(strip.height())));
            for (const auto &other : placed)
                QVERIFY2(!other.intersects(r), qPrintable(name + " overlaps another item"));
            placed << r;
            QVERIFY2(!w->accessibleName().isEmpty() || !w->toolTip().isEmpty(), qPrintable(name));
        }
        for (auto b : box->findChildren<QToolButton *>("qt_toolbar_ext_button"))
            QVERIFY2(!b->isVisible(), "the rail overflowed into QToolBar's extension menu");
    }

  private slots:
    void initTestCase() { evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR"); }

    void defaultIsAFixedLeftRail() {
        auto window = open("default/studio.ini");
        QVERIFY(window);
        checkRail(*window);
        checkEveryItemReachable(*window);
        // No position controls remain: no grip, no tools dock, no position menu.
        QVERIFY(!window->findChild<QToolButton *>("toolPlacementGrip"));
        QVERIFY(!window->findChild<QDockWidget *>("toolsDock"));
        QVERIFY(!window->findChild<QMenu *>("toolPlacementMenu"));
        // View > Panels still shows and hides the strip.
        auto box = window->findChild<ToolBox *>("toolBox");
        auto panels = window->findChild<QMenu *>("panelsMenu");
        QVERIFY(panels->actions().contains(box->toggleViewAction()));
        box->toggleViewAction()->trigger();
        QTRY_VERIFY(!box->isVisible());
        box->toggleViewAction()->trigger();
        checkRail(*window);
        capture(*window, "01-default-left");
    }
    void everyWorkspaceKeepsTheRail() {
        auto window = open("workspaces/studio.ini");
        QVERIFY(window);
        for (const auto id : {"texture", "animation", "model", "uv", "essentials"}) {
            auto preset = window->findChild<QAction *>(QString("workspacePreset-") + id);
            QVERIFY2(preset, id);
            preset->trigger();
            QTest::qWait(60);
            checkRail(*window);
            checkEveryItemReachable(*window);
        }
        Access::reset(*window);
        QTest::qWait(60);
        checkRail(*window);
        QVERIFY(window->close());
    }
    // Layouts saved while the strip could move elsewhere restore with the rail on the left;
    // the rest of each saved arrangement is kept.
    void legacyPositionsRestoreOnTheLeft() {
        QByteArray state;
        {
            auto window = open("legacy-source/studio.ini");
            QVERIFY(window);
            auto box = window->findChild<ToolBox *>("toolBox");
            // A state with the strip on the top edge, as a movable build could save it.
            window->addToolBar(Qt::TopToolBarArea, box);
            QTest::qWait(30);
            state = window->saveState(1);
            QVERIFY(window->close());
        }
        for (const auto position : {"left", "right", "top", "above-colours", "above-assets", "floating", "sideways"}) {
            const auto dir = QString("legacy-%1/studio.ini").arg(position);
            {
                QSettings raw(temporary.filePath(dir), QSettings::IniFormat);
                raw.setValue("workspace/layout", QByteArray("NOVALAYOUT1 ") + position + "\n" + state);
                raw.sync();
            }
            auto window = open(dir);
            QVERIFY2(window, position);
            checkRail(*window);
            checkEveryItemReachable(*window);
            QVERIFY(window->close());
        }
    }
    void corruptLayoutsFallBackToTheDefault() {
        {
            QSettings raw(temporary.filePath("bad/studio.ini"), QSettings::IniFormat);
            raw.setValue("workspace/layout", QByteArray("NOVALAYOUT1 sideways\ncorrupt"));
            raw.setValue("presetLayouts/texture", QByteArray("NOVALAYOUT1 ??\n"));
            raw.sync();
        }
        auto window = open("bad/studio.ini");
        QVERIFY(window);
        checkRail(*window);
        window->findChild<QAction *>("workspacePreset-texture")->trigger();
        QTest::qWait(50);
        checkRail(*window);
        QVERIFY(window->close());
    }
    void layoutBlobs() {
        const QByteArray state("state bytes\nwith a newline");
        QCOMPARE(packLayout(state), QByteArray("NOVALAYOUT1 left\n") + state);
        QCOMPARE(unpackLayout(packLayout(state)), state);
        // Older blobs without a prefix are plain saveState() bytes.
        QCOMPARE(unpackLayout(state), state);
        // Any position written by a movable-strip build is accepted and ignored.
        QCOMPARE(unpackLayout("NOVALAYOUT1 above-colours\nx"), QByteArray("x"));
        QCOMPARE(unpackLayout("NOVALAYOUT1 nowhere\nx"), QByteArray("x"));
        // Malformed prefixes are rejected.
        QVERIFY(unpackLayout("NOVALAYOUT1 left").isEmpty());
        QVERIFY(unpackLayout("NOVALAYOUT1 " + QByteArray(40, 'x') + "\nstate").isEmpty());
    }
    void savedLayoutsRoundTrip() {
        {
            auto window = open("roundtrip/studio.ini");
            QVERIFY(window);
            window->findChild<QAction *>("workspacePreset-texture")->trigger();
            QTest::qWait(60);
            QVERIFY(window->close());
        }
        QSettings raw(temporary.filePath("roundtrip/studio.ini"), QSettings::IniFormat);
        QVERIFY(raw.value("workspace/layout").toByteArray().startsWith("NOVALAYOUT1 left\n"));
        auto window = open("roundtrip/studio.ini");
        QVERIFY(window);
        checkRail(*window);
        QVERIFY(window->close());
    }
    void keyboardTraversalVisitsEveryItem() {
        auto window = open("keyboard/studio.ini");
        QVERIFY(window);
        window->activateWindow();
        auto first = window->findChild<QWidget *>(railItems.first());
        QVERIFY(first);
        first->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(first->hasFocus());
        QStringList order{first->objectName()};
        for (int i = 0; i < railItems.size() + 3; ++i) {
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
            order << QApplication::focusWidget()->objectName();
        }
        QVERIFY2(order.mid(0, railItems.size()) == railItems, qPrintable(order.join(", ")));
        QVERIFY(window->close());
    }
    void compactAndLargeWindowsKeepEveryToolReachable() {
        for (QSize size : {QSize(800, 550), QSize(1280, 800), QSize(1920, 1080)}) {
            auto window = open(QString("size-%1/studio.ini").arg(size.width()), size);
            QVERIFY(window);
            checkRail(*window);
            checkEveryItemReachable(*window);
            capture(*window, QString("02-left-%1x%2").arg(size.width()).arg(size.height()));
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
    ToolStripTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tool_strip_test.moc"
