// SPDX-License-Identifier: GPL-3.0-or-later
#include "canvas.h"
#include "playback.h"
#include "window.h"
#include <QApplication>
#include <QTabWidget>
#include <QtTest>
using namespace nova;
namespace nova {
class EditorTestAccess {
  public:
    static bool reconcile(Window &w) {
        if (!QMetaObject::invokeMethod(w.reconciliationTimer, "timeout", Qt::DirectConnection))
            return false;
        return w.metrics.counters["native_reconciliation_skips"].toInteger() > 0 && !w.fullRescan;
    }
};
} // namespace nova
class LifecycleTest : public QObject {
    Q_OBJECT
  private slots:
    void hiddenSelectionStops() {
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        auto doc = std::make_shared<Document>("selection.png", image);
        doc->selectAll();
        Metrics metrics;
        Canvas canvas(doc, &metrics);
        canvas.show();
        canvas.activateWindow();
        QTest::qWait(80);
        QApplication::setActiveWindow(&canvas);
        canvas.repaint();
        QVERIFY(canvas.selectionTimerActive());
        canvas.hide();
        QVERIFY(!canvas.selectionTimerActive());
        canvas.show();
        canvas.activateWindow();
        QTest::qWait(80);
        canvas.repaint();
        canvas.hide();
        QVERIFY(!canvas.selectionTimerActive());
    }
    void deadlineTiming() {
        AnimationState s;
        s.enabled = true;
        s.defaultTicks = 10;
        SourceFrame frame;
        frame.id = QUuid::createUuid();
        s.frames.push_back(frame);
        s.entries.push_back({frame.id, 0});
        AnimationSequence seq;
        QVERIFY(seq.set(s));
        QCOMPARE(seq.nextChangeMilliseconds(0, false), 500.);
        QCOMPARE(seq.nextChangeMilliseconds(499, false), 1.);
        QCOMPARE(seq.nextChangeMilliseconds(501, false), 499.);
        QCOMPARE(seq.nextChangeMilliseconds(101, true), 49.);
    }
    void monitorRootReplacement() {
#ifdef Q_OS_WIN
        QTemporaryDir parent;
        QDir dir(parent.path());
        QVERIFY(dir.mkdir("watched"));
        DirectoryMonitor monitor;
        const auto root = parent.filePath("watched");
        QVERIFY(monitor.watch(root));
        QVERIFY(monitor.active());
        QVERIFY(dir.rename("watched", "moved"));
        QVERIFY(dir.mkdir("watched"));
        QVERIFY(!monitor.active());
        QVERIFY(monitor.watch(root));
        QVERIFY(monitor.active());
        QSignalSpy changes(&monitor, &DirectoryMonitor::changed);
        QFile file(root + "/new.txt");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("original fixture");
        file.close();
        QTRY_VERIFY_WITH_TIMEOUT(!changes.isEmpty(), 3000);
        monitor.stop();
        QVERIFY(!monitor.active());
#else
        QSKIP("Windows native monitor identity check");
#endif
    }
    void inactiveWorkspaceTimers() {
        QTemporaryDir root;
        qApp->setProperty("settingsFile", root.filePath("prefs.ini"));
        QElapsedTimer elapsed;
        elapsed.start();
        {
            Window empty({}, {}, elapsed);
            QVERIFY(!empty.findChild<QTimer *>("recoveryAndRevision")->isActive());
            QVERIFY(!empty.findChild<QTimer *>("assetReconciliation")->isActive());
            empty.close();
        }
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        QVERIFY(image.save(root.filePath("texture.png")));
        Window workspace(root.path(), {}, elapsed);
        workspace.show();
        QTRY_COMPARE_WITH_TIMEOUT(workspace.documents().all().size(), size_t(1), 5000);
        QVERIFY(workspace.findChild<QTimer *>("recoveryAndRevision")->isActive());
        QVERIFY(EditorTestAccess::reconcile(workspace));
        auto tabs = workspace.findChild<QTabWidget *>("editorTabs");
        emit tabs->tabCloseRequested(0);
        QVERIFY(workspace.documents().all().empty());
        QVERIFY(!workspace.findChild<QTimer *>("recoveryAndRevision")->isActive());
        QVERIFY(workspace.close());
    }
};
QTEST_MAIN(LifecycleTest)
#include "c6_lifecycle_test.moc"
