// SPDX-License-Identifier: GPL-3.0-or-later
// Fill modes: Contiguous keeps its original matching rule; Universal Fill replaces
// every pixel matching the seed under the documented contract (max |dR|,|dG|,|dB| <=
// tolerance, alpha exactly equal, fully transparent pixels always match each other),
// gated by the selection, reading the active layer or the visible composite and
// writing only the active layer.
#include "session.h"
#include <QtTest>

using namespace nova;
namespace {
QImage solid(int w, int h, QRgb c) {
    QImage im(w, h, QImage::Format_RGBA8888);
    im.fill(QColor::fromRgba(c));
    return im;
}
std::shared_ptr<Document> doc(const QImage &pixels) { return std::make_shared<Document>(QString(), pixels); }
int count(const QImage &mask) {
    int n = 0;
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x)
            n += mask.constScanLine(y)[x] == 255;
    return n;
}
// Two regions of `c` separated by a full-height wall: x in [0,2] and [4,7] (x = 3 is the wall).
QImage walled(QRgb c, QRgb wall) {
    auto im = solid(8, 4, c);
    for (int y = 0; y < 4; ++y)
        im.setPixel(3, y, wall);
    return im;
}
} // namespace

class FillModesTests : public QObject {
    Q_OBJECT
  private slots:
    void contractMatcher() {
        const QRgb ref = qRgba(100, 150, 200, 255);
        // Tolerance 0: exact RGB and alpha for visible pixels.
        QVERIFY(ops::universalFillMatches(ref, ref, 0));
        QVERIFY(!ops::universalFillMatches(qRgba(101, 150, 200, 255), ref, 0));
        QVERIFY(!ops::universalFillMatches(qRgba(100, 150, 200, 254), ref, 0));
        // Boundary is inclusive on each channel, and the maximum channel decides.
        for (int channel = 0; channel < 3; ++channel)
            for (int sign : {-1, 1}) {
                int c[3] = {100, 150, 200};
                c[channel] += sign * 10;
                QVERIFY(ops::universalFillMatches(qRgba(c[0], c[1], c[2], 255), ref, 10));
                c[channel] += sign;
                QVERIFY(!ops::universalFillMatches(qRgba(c[0], c[1], c[2], 255), ref, 10));
            }
        QVERIFY(ops::universalFillMatches(qRgba(110, 140, 210, 255), ref, 10));
        QVERIFY(!ops::universalFillMatches(qRgba(110, 140, 211, 255), ref, 10));
        // Alpha must be exactly equal at any tolerance, including 255.
        QVERIFY(!ops::universalFillMatches(qRgba(100, 150, 200, 254), ref, 255));
        QVERIFY(ops::universalFillMatches(qRgba(0, 0, 0, 128), qRgba(255, 255, 255, 128), 255));
        QVERIFY(!ops::universalFillMatches(qRgba(0, 0, 0, 128), qRgba(255, 255, 255, 128), 254));
        // Fully transparent pixels match each other whatever their hidden RGB, at tolerance 0.
        QVERIFY(ops::universalFillMatches(qRgba(255, 0, 0, 0), qRgba(0, 0, 255, 0), 0));
        // ... but never a visible pixel, even one with alpha 1.
        QVERIFY(!ops::universalFillMatches(qRgba(0, 0, 0, 1), qRgba(0, 0, 0, 0), 255));
        // Out-of-range tolerances clamp to 0..255.
        QVERIFY(!ops::universalFillMatches(qRgba(101, 150, 200, 255), ref, -5));
        QVERIFY(ops::universalFillMatches(qRgba(0, 0, 0, 255), qRgba(255, 255, 255, 255), 1000));
        // The Contiguous matcher is unchanged: alpha within tolerance still matches there.
        QVERIFY(ops::colorMatches(qRgba(100, 150, 200, 250), ref, 10, ops::Distance::Rgb, true));
        QVERIFY(!ops::universalFillMatches(qRgba(100, 150, 200, 250), ref, 10));
    }
    void disconnectedRegionsOnlyInUniversal() {
        const QRgb blue = qRgb(0, 0, 255), black = qRgb(0, 0, 0), green = qRgb(0, 255, 0);
        auto contiguous = doc(walled(blue, black));
        QVERIFY(contiguous->fill(contiguous->activeLayer(), {0, 0}, green, 0, ops::Distance::Rgb, true, false));
        QCOMPARE(contiguous->image.pixel(2, 3), green);
        QCOMPARE(contiguous->image.pixel(5, 1), blue); // the far side of the wall is untouched
        QCOMPARE(contiguous->image.pixel(3, 0), black);
        QCOMPARE(contiguous->historyLabels().last(), QString("Fill"));
        auto universal = doc(walled(blue, black));
        QVERIFY(universal->universalFill(universal->activeLayer(), {0, 0}, green, 0, false));
        QCOMPARE(universal->image.pixel(2, 3), green);
        QCOMPARE(universal->image.pixel(5, 1), green);
        QCOMPARE(universal->image.pixel(7, 3), green);
        QCOMPARE(universal->image.pixel(3, 0), black);
        QCOMPARE(universal->historyLabels().last(), QString("Universal Fill"));
        QCOMPARE(universal->historyOperations(), 1);
    }
    void toleranceBoundariesAcrossTheImage() {
        auto pixels = solid(5, 1, qRgb(100, 100, 100));
        pixels.setPixel(1, 0, qRgb(110, 90, 100));  // max diff 10: inside tolerance 10
        pixels.setPixel(2, 0, qRgb(111, 100, 100)); // 11: just outside
        pixels.setPixel(3, 0, qRgba(100, 100, 100, 254)); // alpha differs: never matches
        pixels.setPixel(4, 0, qRgb(100, 100, 89));  // 11 on blue: just outside
        auto mask = ops::universalFillMask(pixels, {0, 0}, 10);
        QCOMPARE(mask.constScanLine(0)[0], uchar(255));
        QCOMPARE(mask.constScanLine(0)[1], uchar(255));
        QCOMPARE(mask.constScanLine(0)[2], uchar(0));
        QCOMPARE(mask.constScanLine(0)[3], uchar(0));
        QCOMPARE(mask.constScanLine(0)[4], uchar(0));
        QCOMPARE(count(ops::universalFillMask(pixels, {0, 0}, 11)), 4); // both 11-step pixels join
        QCOMPARE(count(ops::universalFillMask(pixels, {0, 0}, 0)), 1);
        // Seed outside the image: empty mask; Document refuses without history.
        QCOMPARE(count(ops::universalFillMask(pixels, {9, 0}, 255)), 0);
        auto d = doc(pixels);
        QString error;
        QVERIFY(!d->universalFill(d->activeLayer(), {-1, 0}, qRgb(1, 1, 1), 0, false, 255, &error));
        QVERIFY(error.contains("outside"));
        QCOMPARE(d->historyOperations(), 0);
    }
    void transparentPixelsWithHiddenRgb() {
        QImage pixels(4, 1, QImage::Format_RGBA8888);
        pixels.setPixel(0, 0, qRgba(255, 0, 0, 0));
        pixels.setPixel(1, 0, qRgba(0, 255, 0, 0));
        pixels.setPixel(2, 0, qRgba(0, 0, 0, 1));
        pixels.setPixel(3, 0, qRgba(9, 9, 9, 0));
        auto d = doc(pixels);
        QVERIFY(d->universalFill(d->activeLayer(), {0, 0}, qRgb(7, 7, 7), 0, false));
        QCOMPARE(d->image.pixel(1, 0), qRgb(7, 7, 7));
        QCOMPARE(d->image.pixel(3, 0), qRgb(7, 7, 7));
        QCOMPARE(d->image.pixel(2, 0), qRgba(0, 0, 0, 1)); // visible alpha-1 pixel stays
    }
    void selectionLayersAndSampling() {
        const QRgb blue = qRgb(0, 0, 255), black = qRgb(0, 0, 0), red = qRgb(255, 0, 0);
        // Selection gates painting; matches outside it are left alone.
        auto s = doc(walled(blue, black));
        QVERIFY(s->select(ops::rectMask({8, 4}, {5, 0, 2, 2}), ops::Combine::Replace));
        QVERIFY(s->universalFill(s->activeLayer(), {0, 3}, red, 0, false));
        QCOMPARE(s->image.pixel(5, 0), red);
        QCOMPARE(s->image.pixel(6, 1), red);
        QCOMPARE(s->image.pixel(7, 0), blue);
        QCOMPARE(s->image.pixel(0, 3), blue); // the clicked pixel is outside the selection
        // An empty but allocated selection paints nothing and records nothing.
        auto e = doc(walled(blue, black));
        QVERIFY(e->select(ops::emptyMask({8, 4}), ops::Combine::Replace));
        const int before = e->historyOperations();
        QVERIFY(e->universalFill(e->activeLayer(), {0, 0}, red, 0, false));
        QCOMPARE(e->image.pixel(0, 0), blue);
        QCOMPARE(e->historyOperations(), before);

        // Two layers: the bottom has the pattern, the top is transparent and active.
        auto d = doc(walled(blue, black));
        const auto bottom = d->activeLayer();
        QVERIFY(d->editLayer({LayerCommand::Add, bottom, "Top"}));
        const auto top = d->activeLayer();
        QVERIFY(top != bottom);
        const auto bottomBefore = d->layers()[0].pixels;
        // Active layer only: the transparent top layer matches everywhere.
        QVERIFY(d->universalFill(top, {0, 0}, red, 0, false));
        QCOMPARE(d->layers()[1].pixels.pixel(3, 0), red);
        QCOMPARE(d->layers()[0].pixels, bottomBefore);
        d->undo();
        // Sample all layers: matching reads the composite, writes still go to the top layer.
        QVERIFY(d->universalFill(top, {0, 0}, red, 0, true));
        QCOMPARE(d->layers()[1].pixels.pixel(5, 2), red);
        QCOMPARE(d->layers()[1].pixels.pixel(3, 0), QRgb(0)); // wall colour differs in the composite
        QCOMPARE(d->layers()[0].pixels, bottomBefore);
        // Filling a layer that is not active is refused.
        QString error;
        QVERIFY(!d->universalFill(bottom, {0, 0}, red, 0, false, 255, &error));
        QVERIFY(error.contains("Select the target layer"));
    }
    void undoRedoDeterminismOpacityAndEdges() {
        const QRgb blue = qRgb(0, 0, 255), black = qRgb(0, 0, 0), green = qRgb(0, 255, 0);
        auto d = doc(walled(blue, black));
        const auto original = d->image;
        QVERIFY(d->universalFill(d->activeLayer(), {7, 3}, green, 0, false)); // corner seed
        const auto first = d->image;
        QCOMPARE(first.pixel(0, 0), green);
        QCOMPARE(first.pixel(7, 0), green);
        d->undo();
        QCOMPARE(d->image, original);
        d->redo();
        QCOMPARE(d->image, first);
        // Same input, same output.
        auto again = doc(walled(blue, black));
        QVERIFY(again->universalFill(again->activeLayer(), {7, 3}, green, 0, false));
        QCOMPARE(again->image, first);
        // Partial opacity blends straight-alpha source-over, like Contiguous fill.
        auto half = doc(walled(blue, black));
        QVERIFY(half->universalFill(half->activeLayer(), {0, 0}, green, 0, false, 128));
        auto halfContiguous = doc(walled(blue, black));
        QVERIFY(halfContiguous->fill(halfContiguous->activeLayer(), {0, 0}, green, 0, ops::Distance::Rgb, true, false, 128));
        QCOMPARE(half->image.pixel(0, 0), halfContiguous->image.pixel(0, 0));
        QVERIFY(half->image.pixel(0, 0) != blue && half->image.pixel(0, 0) != green);
        // Nothing to change (fill colour equals the matched colour): no history entry.
        auto same = doc(walled(blue, black));
        QVERIFY(same->universalFill(same->activeLayer(), {0, 0}, blue, 0, false));
        QCOMPARE(same->historyOperations(), 0);
    }
    void supportedPixelFormats() {
        // Documents store straight-alpha RGBA8888. Other inputs are converted once on load;
        // the mask helper converts too, so the contract is applied to straight 8-bit values.
        QImage straight(3, 1, QImage::Format_RGBA8888);
        straight.setPixel(0, 0, qRgba(200, 100, 50, 128));
        straight.setPixel(1, 0, qRgba(200, 100, 50, 128));
        straight.setPixel(2, 0, qRgba(200, 100, 50, 255));
        for (auto format : {QImage::Format_ARGB32, QImage::Format_RGBA8888,
                            QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA8888_Premultiplied}) {
            const auto input = straight.convertToFormat(format);
            QVERIFY2(count(ops::universalFillMask(input, {0, 0}, 0)) == 2, qPrintable(QString("format %1").arg(int(format))));
            auto d = doc(input);
            QCOMPARE(d->image.format(), QImage::Format_RGBA8888);
            QVERIFY(d->universalFill(d->activeLayer(), {0, 0}, qRgb(1, 2, 3), 0, false));
            QCOMPARE(d->image.pixel(1, 0), qRgb(1, 2, 3));
            QCOMPARE(qAlpha(d->image.pixel(2, 0)), 255);
            QVERIFY(d->image.pixel(2, 0) != qRgb(1, 2, 3)); // different alpha: not matched
        }
        // Opaque 24-bit input: every pixel is alpha 255, so only RGB decides.
        QImage rgb(2, 1, QImage::Format_RGB888);
        rgb.setPixel(0, 0, qRgb(10, 20, 30));
        rgb.setPixel(1, 0, qRgb(10, 20, 31));
        QCOMPARE(count(ops::universalFillMask(rgb, {0, 0}, 0)), 1);
        QCOMPARE(count(ops::universalFillMask(rgb, {0, 0}, 1)), 2);
    }
    void sessionEmitsChanged() {
        DocumentSession session;
        auto d = doc(walled(qRgb(0, 0, 255), qRgb(0, 0, 0)));
        QVERIFY(session.adopt(d));
        QSignalSpy spy(&session, &DocumentSession::changed);
        QVERIFY(session.universalFill(d, d->activeLayer(), {0, 0}, qRgb(0, 255, 0), 0, false));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(d->image.pixel(6, 0), qRgb(0, 255, 0));
    }
};

QTEST_GUILESS_MAIN(FillModesTests)
#include "fill_modes_test.moc"
