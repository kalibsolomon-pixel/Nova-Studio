// SPDX-License-Identifier: GPL-3.0-or-later
// 3D preview notice: at any view size and display scale the banner keeps to a quarter of the
// view's height (at least one line), leaves the centre of the view free, states "No
// Minecraft model" first, and keeps the full text in the accessible name, the view's
// accessible description and tooltip. Registered at 100%, 150% and 200% (QT_SCALE_FACTOR).
// With NOVA_EVIDENCE_DIR set, the view is captured at each size.
#include "cube.h"
#include <QApplication>
#include <QLabel>
#include <QSurfaceFormat>
#include <QtTest>

using namespace nova;
namespace {
const QString shortReason = "No Minecraft model · No model in the resource pack or resource stack uses this "
                            "texture.\nShowing the texture flat; this is not a model render.";
const QString longReason =
    "No Minecraft model · minecraft:block/a_very_long_model_name_used_for_layout_checks could not be "
    "resolved: Malformed model JSON: unterminated object at offset 4096 in the third nested element's "
    "faces table, after the north face's uv array.\nShowing the texture flat; this is not a model render.";
QString scale() { return qEnvironmentVariable("QT_SCALE_FACTOR", "1"); }
} // namespace

class PreviewNoticeTests : public QObject {
    Q_OBJECT
    Metrics metrics;
    Cube *cube = nullptr;
    QString evidence;

    QLabel *notice() { return cube->findChild<QLabel *>("previewNotice"); }
    void show(QSize size, const QString &text) {
        cube->resize(size);
        cube->setNotice(text);
        QTest::qWait(30);
        QApplication::processEvents();
    }

  private slots:
    void initTestCase() {
        evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        cube = new Cube(&metrics);
        cube->show();
        QVERIFY(QTest::qWaitForWindowExposed(cube));
    }
    void cleanupTestCase() { delete cube; }

    void staysWithinItsShareAtEverySize_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QString>("text");
        // 200x150 is the view's minimum size; 204x174 is the preview measured at 150% and
        // 200% in the default workspace; the rest are common and large views.
        for (QSize size : {QSize(200, 150), QSize(204, 174), QSize(320, 200), QSize(503, 174), QSize(640, 360),
                           QSize(900, 600)}) {
            const auto name = QString("%1x%2").arg(size.width()).arg(size.height()).toLatin1();
            QTest::newRow(name + " short") << size << shortReason;
            QTest::newRow(name + " long") << size << longReason;
        }
    }
    void staysWithinItsShareAtEverySize() {
        QFETCH(QSize, size);
        QFETCH(QString, text);
        show(size, text);
        // At 150%/200% the test display can be too small for the larger views; the test
        // display pins windows inside it, so check against the size actually given.
        if (cube->size() != size)
            qInfo() << "view clamped by the test display:" << size << "->" << cube->size();
        size = cube->size();
        QVERIFY(size.width() >= 200 && size.height() >= 150);
        auto label = notice();
        QVERIFY(label && label->isVisible());
        const auto g = label->geometry();
        const auto lines = label->text().split('\n');
        const int line = label->fontMetrics().lineSpacing();
        // A quarter of the height, or one line when a quarter is smaller than that.
        const int oneLine = g.height() - (lines.size() - 1) * line;
        QVERIFY2(g.height() <= std::max(int(size.height() * Cube::NoticeMaxShare) + 1, oneLine),
                 qPrintable(QString("%1 lines, %2 px of %3").arg(lines.size()).arg(g.height()).arg(size.height())));
        QVERIFY(lines.size() <= 4);
        // The centre of the view, where the fallback square is drawn, stays clear.
        const QRect centre(size.width() / 5, size.height() * 2 / 5, size.width() * 3 / 5, size.height() / 2);
        QVERIFY2(!g.intersects(centre), qPrintable(QString("notice bottom %1, centre top %2").arg(g.bottom()).arg(centre.top())));
        QVERIFY(cube->rect().contains(g));
        // Truthful and complete where it matters.
        QVERIFY(label->text().startsWith("No Minecraft model"));
        QCOMPARE(label->accessibleName(), text);
        QCOMPARE(cube->accessibleDescription(), text);
        QCOMPARE(cube->toolTip(), text);
        QVERIFY(label->testAttribute(Qt::WA_TransparentForMouseEvents)); // orbit and inspect under it
        // Elided exactly when the text did not fit.
        const bool elided = label->property("elided").toBool();
        QCOMPARE(elided, lines.last().endsWith(QChar(0x2026)));
        if (!elided)
            QVERIFY(label->text().contains("this is not a model render"));
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            QVERIFY(cube->grab().save(QString("%1/notice-scale%2-%3x%4-%5.png").arg(evidence, scale())
                                          .arg(size.width()).arg(size.height())
                                          .arg(text == longReason ? "long" : "short")));
        }
    }
    void shortReasonIsCompleteInCommonViews() {
        show({640, 360}, shortReason);
        QVERIFY(!notice()->property("elided").toBool());
        show({900, 600}, longReason);
        QVERIFY(!notice()->property("elided").toBool());
        // The smallest view still names the state, elided.
        show({200, 150}, longReason);
        QCOMPARE(notice()->text().count('\n'), 0);
        QVERIFY(notice()->property("elided").toBool());
        QVERIFY(notice()->text().startsWith("No Minecraft model"));
    }
    void followsResizesAndClears() {
        show({640, 360}, longReason);
        const int large = notice()->height();
        cube->resize(204, 174);
        QApplication::processEvents();
        QVERIFY(notice()->height() < large);
        cube->resize(900, 600);
        QApplication::processEvents();
        QVERIFY(!notice()->property("elided").toBool()); // a large view shows it all again
        cube->setNotice({});
        QVERIFY(!notice()->isVisible());
        QVERIFY(cube->toolTip().isEmpty());
        QVERIFY(cube->accessibleDescription().isEmpty());
        // Another tooltip (for example a shader log) is left alone.
        cube->setToolTip("shader log");
        cube->setNotice(shortReason);
        QCOMPARE(cube->toolTip(), QString("shader log"));
        cube->setNotice({});
        QCOMPARE(cube->toolTip(), QString("shader log"));
        cube->setToolTip({});
    }
    void lineBreaking() {
        QFont font = cube->font();
        const QFontMetrics metrics(font);
        // One line: everything elided onto it, paragraphs joined.
        const auto one = Cube::noticeLines(shortReason, font, 200, 1);
        QCOMPARE(one.size(), 1);
        QVERIFY(one[0].endsWith(QChar(0x2026)));
        QVERIFY(metrics.horizontalAdvance(one[0]) <= 200);
        // Wide enough: one line per paragraph, nothing lost.
        const auto wide = Cube::noticeLines(shortReason, font, 4000, 4);
        QCOMPARE(wide, shortReason.split('\n'));
        // A word longer than the width still breaks instead of overflowing.
        const auto word = Cube::noticeLines(QString(80, 'x'), font, 60, 4);
        QVERIFY(word.size() > 1);
        for (const auto &line : word)
            QVERIFY(metrics.horizontalAdvance(line) <= 60);
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
    PreviewNoticeTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "preview_notice_test.moc"
