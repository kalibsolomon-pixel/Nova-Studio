// SPDX-License-Identifier: GPL-3.0-or-later
// Fill options row: explicit Contiguous / Universal Fill mode control, keyboard access,
// mode help on the controls, canvas dispatch, and Contiguous as the default of every
// new session. With NOVA_EVIDENCE_DIR set, the options row is captured in both modes.
#include "canvas.h"
#include "tools.h"
#include "window.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>

namespace {
void click(nova::Canvas *c, double x, double y) {
    const auto p = c->widgetPosition(QPointF(x + .5, y + .5));
    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent e(type, p, c->mapToGlobal(p), Qt::LeftButton,
                      type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, {});
        QApplication::sendEvent(c, &e);
    }
}
} // namespace

class FillModeGuiTests : public QObject {
    Q_OBJECT
    QTemporaryDir tmp;
    QString root;
    QString evidence;

  private slots:
    void initTestCase() {
        evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        root = tmp.path() + "/Fill Pack";
        const auto path = root + "/assets/minecraft/textures/block/stone.png";
        QDir().mkpath(QFileInfo(path).absolutePath());
        // Two blue regions split by a black wall at x = 7.
        QImage pixels(16, 16, QImage::Format_RGBA8888);
        pixels.fill(QColor(0, 0, 255));
        for (int y = 0; y < 16; ++y)
            pixels.setPixel(7, y, qRgb(0, 0, 0));
        QVERIFY(nova::savePng(path, pixels));
        QFile meta(root + "/pack.mcmeta");
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write(R"({"pack":{"pack_format":34,"description":"Fill"}})");
        meta.close();
        qApp->setProperty("settingsFile", tmp.path() + "/prefs/studio.ini");
    }
    void modesKeyboardHelpAndDispatch() {
        QElapsedTimer started;
        started.start();
        nova::Window window(root, {}, started);
        window.resize(1280, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto tabs = window.findChild<QTabWidget *>("editorTabs");
        QTRY_VERIFY(tabs->count() >= 1);
        auto canvas = static_cast<nova::Canvas *>(tabs->currentWidget());
        auto doc = canvas->document;
        canvas->zoom = 20;
        window.findChild<QAction *>("tool-fill")->trigger();
        QTRY_VERIFY(window.findChild<QToolButton *>("fillModeUniversalButton"));
        auto contiguous = window.findChild<QToolButton *>("fillModeContiguousButton");
        auto universal = window.findChild<QToolButton *>("fillModeUniversalButton");
        auto tolerance = window.findChild<QSpinBox *>("fillTolerance");
        auto metric = window.findChild<QComboBox *>("fillMetric");
        QVERIFY(contiguous && tolerance && metric);
        QTRY_VERIFY(universal->isVisible());
        // Default: Contiguous; tolerance visible with an exact-match default of 0.
        QVERIFY(window.tools().contiguous);
        QVERIFY(contiguous->isChecked());
        QVERIFY(!universal->isChecked());
        QCOMPARE(tolerance->value(), 0);
        QCOMPARE(tolerance->minimum(), 0);
        QCOMPARE(tolerance->maximum(), 255);
        QVERIFY(tolerance->isVisible());
        QCOMPARE(tolerance->accessibleName(), QString("Tolerance"));
        QCOMPARE(window.findChild<QWidget *>("fillMode")->accessibleName(), QString("Fill mode"));
        QCOMPARE(universal->accessibleName(), QString("Fill mode: Universal Fill"));
        QCOMPARE(contiguous->text(), QString("Contiguous"));
        QCOMPARE(universal->text(), QString("Universal Fill"));
        QVERIFY(universal->toolTip().contains("alpha\nis exactly equal"));
        QVERIFY(universal->toolTip().contains("Tolerance 0 = exact colour"));
        QVERIFY(contiguous->toolTip().contains("connected area"));
        QVERIFY(tolerance->toolTip().startsWith("Contiguous"));
        QVERIFY(metric->isEnabled());
        if (!evidence.isEmpty()) {
            QTest::qWait(500); // the options row settles its height on later event-loop turns
            QVERIFY(window.findChild<QWidget *>("toolOptions")->grab().save(evidence + "/fill-options-contiguous.png"));
            QVERIFY(window.grab().save(evidence + "/fill-window-contiguous.png"));
        }

        // Contiguous on the canvas: only the clicked region changes.
        window.tools().setForeground(qRgb(0, 200, 0));
        click(canvas, 2, 2);
        QCOMPARE(doc->image.pixel(2, 2), qRgb(0, 200, 0));
        QCOMPARE(doc->image.pixel(12, 2), qRgb(0, 0, 255));
        doc->undo();
        QCOMPARE(doc->image.pixel(2, 2), qRgb(0, 0, 255));

        // Keyboard: focus the Universal button and press Space.
        universal->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(universal->hasFocus());
        QTest::keyClick(universal, Qt::Key_Space);
        QTRY_VERIFY(!window.tools().contiguous);
        QVERIFY(universal->isChecked());
        QVERIFY(!contiguous->isChecked());
        QTRY_VERIFY(tolerance->toolTip().startsWith("Universal Fill"));
        QVERIFY(!metric->isEnabled()); // Universal Fill always compares R, G and B
        // Tab order reaches the mode buttons and the tolerance box.
        contiguous->setFocus(Qt::TabFocusReason);
        QVERIFY(contiguous->focusPolicy() & Qt::TabFocus);
        QVERIFY(tolerance->focusPolicy() & Qt::TabFocus);
        if (!evidence.isEmpty()) {
            QTest::qWait(500);
            QVERIFY(window.findChild<QWidget *>("toolOptions")->grab().save(evidence + "/fill-options-universal.png"));
            QVERIFY(window.grab().save(evidence + "/fill-window-universal.png"));
        }

        // Universal on the canvas: both regions change, the wall does not; one history step.
        click(canvas, 2, 2);
        QCOMPARE(doc->image.pixel(2, 2), qRgb(0, 200, 0));
        QCOMPARE(doc->image.pixel(12, 2), qRgb(0, 200, 0));
        QCOMPARE(doc->image.pixel(7, 2), qRgb(0, 0, 0));
        // One step; it replaces the undone Contiguous step on the redo branch.
        QCOMPARE(doc->historyLabels().count("Universal Fill"), 1);
        QCOMPARE(doc->historyLabels().last(), QString("Universal Fill"));
        QVERIFY(!doc->historyLabels().contains("Fill"));
        doc->undo();
        QCOMPARE(doc->image.pixel(12, 2), qRgb(0, 0, 255));
        // Tolerance from the shared box: 254 still keeps the black wall (blue differs by 255).
        tolerance->setValue(254);
        QCOMPARE(window.tools().tolerance, 254);
        click(canvas, 2, 2);
        QCOMPARE(doc->image.pixel(7, 5), qRgb(0, 0, 0));
        doc->undo();
        tolerance->setValue(0);
        window.close();
    }
    void newSessionStartsContiguous() {
        QElapsedTimer started;
        started.start();
        nova::Window window(root, {}, started);
        window.resize(1000, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.tools().contiguous);
        window.findChild<QAction *>("tool-fill")->trigger();
        QTRY_VERIFY(window.findChild<QToolButton *>("fillModeContiguousButton"));
        QTRY_VERIFY(window.findChild<QToolButton *>("fillModeContiguousButton")->isChecked());
        window.close();
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
    FillModeGuiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "fill_mode_gui_test.moc"
