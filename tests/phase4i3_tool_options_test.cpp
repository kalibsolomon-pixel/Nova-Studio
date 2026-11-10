// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-I.3 regression: the tool options row must show its controls fully in
// the vertical direction whenever a horizontal scrollbar is present. A page
// pre-built while hidden inside the stacked widget used to keep the bar-less
// fixed height from construction (QStackedLayout never resizes hidden pages and
// scrollbar children report isVisible() == false inside a hidden page), so the
// shape and transform pages were clipped by exactly the scrollbar height.
#include "tools.h"
#include "window.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>

namespace {
QScrollArea *optionsRow(nova::Window &window) {
    auto bar = window.findChild<nova::ToolOptionsBar *>("toolOptions");
    auto stack = bar ? bar->findChild<QStackedWidget *>() : nullptr;
    auto page = stack ? stack->currentWidget() : nullptr;
    return page ? qobject_cast<QScrollArea *>(page) : nullptr;
}
int requiredHeight(const QScrollArea *row) {
    const int content = row->widget()->sizeHint().height();
    const int bar = row->horizontalScrollBar()->isVisible() ? row->horizontalScrollBar()->sizeHint().height() : 0;
    return content + bar;
}
// A fixed logical window size, narrower than every page these tests expect to scroll (the narrowest,
// pencil, is about 1240 px wide) and small enough for any test display, so whether a scrollbar is
// needed does not depend on a monitor clamping the window.
const QSize narrowWindow(1000, 700);
// The production bar builds every page lazily: the first during the first
// event-loop turn, the rest during idle time two seconds later.
void waitForLazilyBuiltPages() { QTest::qWait(2600); }
} // namespace

class Phase4i3ToolOptionsTests : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;

    // The demonstrated failure: a page whose horizontal scrollbar is needed
    // must reserve the scrollbar height on top of its content height.
    void assertRowShowsControlsFully(nova::Window &window, const char *tool) {
        auto row = optionsRow(window);
        QVERIFY2(row, "no options row page for the active tool");
        const int content = row->widget()->sizeHint().height();
        QVERIFY2(row->horizontalScrollBar()->maximum() > 0,
                 qPrintable(QString("%1: expected the horizontal scrollbar to be needed at this width "
                                    "(row width %2, content %3)")
                                .arg(tool).arg(row->width()).arg(row->widget()->sizeHint().width())));
        QTest::qWait(150);
        row = optionsRow(window);
        const int height = row->height();
        const int viewport = row->viewport()->height();
        const int required = requiredHeight(row);
        QVERIFY2(height >= required && viewport >= content,
                 qPrintable(QString("%1: options row height %2 (viewport %3) does not fully show content %4 plus the "
                                    "visible scrollbar (%5 required)")
                                .arg(tool).arg(height).arg(viewport).arg(content).arg(required)));
    }

  private slots:
    void pagesNeedingAScrollbarShowTheirControlsFully() {
        qApp->setProperty("settingsFile", temporary.filePath("studio.ini"));
        QElapsedTimer started;
        started.start();
        nova::Window window({}, {}, started);
        window.resize(narrowWindow);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.size(), narrowWindow);
        waitForLazilyBuiltPages();
        // Pencil was never clipped (it is the start-up page); shape and transform
        // were the clipped pages. The second round re-shows already-built pages.
        for (int round = 0; round < 2; ++round)
            for (auto tool : {nova::Tool::Pencil, nova::Tool::Shape, nova::Tool::Transform}) {
                window.tools().setTool(tool);
                QTest::qWait(250);
                assertRowShowsControlsFully(window, nova::toolInfo(tool).id.toLatin1().constData());
            }
    }

    void pageFirstShownNeedingAScrollbarIsNotClipped() {
        qApp->setProperty("settingsFile", temporary.filePath("studio.ini"));
        QElapsedTimer started;
        started.start();
        nova::Window window({}, {}, started);
        window.resize(narrowWindow);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.size(), narrowWindow);
        waitForLazilyBuiltPages();
        // Every page except the start-up one is pre-built hidden; selecting it now
        // shows it for the first time already needing a scrollbar.
        for (auto tool : {nova::Tool::Shape, nova::Tool::Transform}) {
            window.tools().setTool(tool);
            QTest::qWait(250);
            assertRowShowsControlsFully(window, nova::toolInfo(tool).id.toLatin1().constData());
        }
    }

    void pagesWithoutAScrollbarKeepTheCompactRow() {
        qApp->setProperty("settingsFile", temporary.filePath("studio.ini"));
        QElapsedTimer started;
        started.start();
        nova::Window window({}, {}, started);
        window.resize(1280, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        waitForLazilyBuiltPages();
        for (QSize size : {QSize(1280, 800), QSize(800, 550)}) {
            window.resize(size);
            QTest::qWait(400);
            for (auto tool : {nova::Tool::RectSelect, nova::Tool::Fill, nova::Tool::Gradient, nova::Tool::Eyedropper}) {
                window.tools().setTool(tool);
                QTest::qWait(250);
                auto row = optionsRow(window);
                QVERIFY2(row, "no options row page for the active tool");
                QCOMPARE(row->horizontalScrollBar()->maximum(), 0);
                QCOMPARE(row->height(), row->widget()->sizeHint().height());
            }
        }
    }
};

int main(int argc, char **argv) {
    // Match the production application start-up (Fusion style, 9 pt font, shared
    // OpenGL contexts); control metrics and therefore scrollbar need depend on it.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication application(argc, argv);
    application.setStyle("Fusion");
    QFont font = application.font();
    font.setPointSize(9);
    application.setFont(font);
    Phase4i3ToolOptionsTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "phase4i3_tool_options_test.moc"
