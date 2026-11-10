// SPDX-License-Identifier: GPL-3.0-or-later
// Transparency checkerboard: theme tokens keep the texture edge distinct from the canvas
// surround in both themes, keep dark and light texture pixels visible, stay quieter than
// the pixel grid, and the canvas paints exactly those tokens. With NOVA_EVIDENCE_DIR set,
// the canvas is captured at representative zoom levels in both themes.
#include "canvas.h"
#include "theme.h"
#include <QApplication>
#include <QtTest>
#include <cmath>

using namespace nova;
namespace {
double luminance(QColor c) {
    auto channel = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}
double contrast(QColor a, QColor b) {
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}
QColor over(QColor top, QColor bottom) { // straight-alpha source-over on an opaque colour
    const double a = top.alphaF();
    return QColor::fromRgbF(top.redF() * a + bottom.redF() * (1 - a), top.greenF() * a + bottom.greenF() * (1 - a),
                            top.blueF() * a + bottom.blueF() * (1 - a));
}
} // namespace

class CheckerboardThemeTests : public QObject {
    Q_OBJECT
  private slots:
    void tokensKeepEdgesAndPixelsVisible() {
        for (bool dark : {true, false}) {
            const auto t = ui::tokens(dark, "violet");
            const auto name = dark ? "dark" : "light";
            const double edgeDark = contrast(t.base, t.checkerDark), edgeLight = contrast(t.base, t.checkerLight);
            const double pattern = contrast(t.checkerDark, t.checkerLight);
            const double grid = contrast(over(t.pixelGrid, t.checkerLight), t.checkerLight);
            qInfo().noquote() << name << "edge" << edgeDark << edgeLight << "pattern" << pattern << "grid" << grid
                              << "black" << contrast(Qt::black, t.checkerDark) << "white"
                              << contrast(Qt::white, t.checkerLight);
            // The transparent texture edge stands apart from the canvas surround.
            QVERIFY2(edgeDark >= 2.0 && edgeLight >= 1.7, name);
            // A quiet pattern: visible, but less prominent than the pixel grid.
            QVERIFY2(pattern >= 1.1 && pattern <= 1.5, name);
            QVERIFY2(grid > pattern, name);
            // Dark and light texture pixels both remain clearly visible on it.
            QVERIFY2(contrast(Qt::black, t.checkerDark) >= 3.0, name);
            QVERIFY2(contrast(Qt::white, t.checkerLight) >= 1.7, name);
            // Lighter than the former fixed 43/53 grey squares.
            QVERIFY2(luminance(t.checkerDark) > luminance(QColor(53, 53, 53)), name);
            // Opaque tokens (the brush tile is RGB32).
            QCOMPARE(t.checkerDark.alpha(), 255);
            QCOMPARE(t.checkerLight.alpha(), 255);
        }
        // The two themes differ (one shared value would suit neither surround).
        QVERIFY(ui::tokens(true, "violet").checkerDark != ui::tokens(false, "violet").checkerDark);
    }
    void canvasPaintsTheThemeTokens() {
        QImage pixels(16, 16, QImage::Format_RGBA8888);
        pixels.fill(Qt::transparent);
        for (int i = 0; i < 4; ++i) {
            pixels.setPixel(12 + i % 2, 12 + i / 2, i % 2 ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
            pixels.setPixel(i, 15, qRgba(220, 60, 60, 128));
        }
        Metrics metrics;
        Canvas canvas(std::make_shared<Document>(QString(), pixels), &metrics);
        canvas.resize(520, 520);
        canvas.grid = true;
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        const auto evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        for (bool dark : {true, false}) {
            const auto t = ui::tokens(dark, "violet");
            ui::setCurrent(t);
            qApp->setPalette(ui::palette(t));
            canvas.zoom = 24;
            canvas.update();
            QTest::qWait(40);
            const auto shot = canvas.grab().toImage();
            const auto dpr = shot.devicePixelRatio();
            const QPointF origin = QPointF(canvas.width() / 2.0, canvas.height() / 2.0) - QPointF(8 * 24, 8 * 24);
            auto at = [&](double x, double y) { return QColor(shot.pixel(((origin + QPointF(x, y) * 24) * dpr).toPoint())); };
            QCOMPARE(at(0.5, 0.5).rgb(), t.checkerDark.rgb());  // cell (0,0)
            QCOMPARE(at(1.5, 0.5).rgb(), t.checkerLight.rgb()); // cell (1,0)
            QCOMPARE(QColor(shot.pixel(QPoint(4, 4) * dpr)).rgb(), t.base.rgb()); // surround
            QCOMPARE(at(12.5, 12.5).rgb(), QColor(Qt::black).rgb());
            QCOMPARE(at(13.5, 12.5).rgb(), QColor(Qt::white).rgb());
            if (!evidence.isEmpty()) {
                QDir().mkpath(evidence);
                for (double zoom : {4.0, 8.0, 24.0}) {
                    canvas.zoom = zoom;
                    canvas.update();
                    QTest::qWait(30);
                    QVERIFY(canvas.grab().save(QString("%1/checker-%2-zoom%3.png")
                                                   .arg(evidence, dark ? "dark" : "light").arg(int(zoom * 100))));
                }
            }
        }
        ui::setCurrent(ui::tokens(true, "violet"));
    }
};

QTEST_MAIN(CheckerboardThemeTests)
#include "checkerboard_theme_test.moc"
