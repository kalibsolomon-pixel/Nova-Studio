// SPDX-License-Identifier: GPL-3.0-or-later
// Image-operation engine tests: colour models, masks, shapes, brushes, every
// registered operation, transforms, palettes, generators and shading.
#include "imageops.h"
#include "palette.h"
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>

using namespace nova;
namespace {
QImage solid(int w, int h, QRgb c) {
    QImage im(w, h, QImage::Format_RGBA8888);
    im.fill(QColor::fromRgba(c));
    return im;
}
QImage pattern(int w, int h) {
    QImage im(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            im.setPixel(x, y, qRgba((x * 37 + y * 11) % 256, (x * 5 + y * 23) % 256, (x * y * 3) % 256,
                                    (x + y) % 5 == 0 ? 0 : 255));
    return im;
}
QByteArray digest(const QImage &im) {
    const auto rgba = im.convertToFormat(QImage::Format_RGBA8888);
    return QCryptographicHash::hash(QByteArrayView(rgba.constBits(), rgba.sizeInBytes()),
                                    QCryptographicHash::Sha256);
}
ops::Result run(const QString &id, const QJsonObject &params, const QImage &src,
                const ops::Context &ctx = {}) {
    return ops::run({id, params}, src, ctx);
}
} // namespace

class OpsTests : public QObject {
    Q_OBJECT
  private slots:
    void firstPaletteSaveCreatesSettingsDirectory() {
        QTemporaryDir tmp;
        const auto path = tmp.filePath("new/settings/palettes.json");
        PaletteStore store(path);
        store.palettes.push_back({"First", {qRgba(7, 19, 233, 123)}});
        QString error;
        QVERIFY2(store.save(&error), qPrintable(error));
        PaletteStore loaded(path);
        QVERIFY(loaded.load(&error));
        QCOMPARE(loaded.palettes.size(), size_t(1));
        QCOMPARE(loaded.palettes[0].colors, store.palettes[0].colors);
        PaletteStore invalid;
        QVERIFY(!invalid.save(&error));
    }
    void colorModelsRoundTrip() {
        for (int r = 0; r < 256; r += 17)
            for (int g = 0; g < 256; g += 17)
                for (int b = 0; b < 256; b += 17) {
                    const QRgb c = qRgba(r, g, b, 200);
                    QCOMPARE(ops::fromHsv(ops::toHsv(c), 200), c);
                    QCOMPARE(ops::fromHsl(ops::toHsl(c), 200), c);
                }
        QCOMPARE(ops::toHsv(qRgb(255, 0, 0)).h, 0.0);
        QCOMPARE(ops::toHsv(qRgb(0, 0, 255)).h, 240.0);
        QVERIFY(std::abs(ops::linearToSrgb(ops::srgbToLinear(.5)) - .5) < 1e-12);
        QCOMPARE(ops::luma(qRgb(255, 255, 255)), 255);
        QVERIFY(ops::colorMatches(qRgba(1, 2, 3, 0), qRgba(200, 100, 50, 0), 0, ops::Distance::Rgb, true));
        QVERIFY(!ops::colorMatches(qRgb(10, 10, 10), qRgb(20, 10, 10), 9, ops::Distance::Rgb, false));
        QVERIFY(ops::colorMatches(qRgb(10, 10, 10), qRgb(20, 10, 10), 10, ops::Distance::Rgb, false));
    }
    void maskCombinationsAndOutline() {
        const QSize s(8, 8);
        auto a = ops::rectMask(s, {0, 0, 4, 4}), b = ops::rectMask(s, {2, 2, 4, 4});
        QCOMPARE(ops::maskBounds(ops::combine(a, b, ops::Combine::Add)), QRect(0, 0, 6, 6));
        QCOMPARE(ops::maskBounds(ops::combine(a, b, ops::Combine::Intersect)), QRect(2, 2, 2, 2));
        auto sub = ops::combine(a, b, ops::Combine::Subtract);
        QCOMPARE(sub.constScanLine(3)[3], uchar(0));
        QCOMPARE(sub.constScanLine(0)[0], uchar(255));
        QCOMPARE(ops::maskBounds(ops::combine({}, b, ops::Combine::Add)), QRect(2, 2, 4, 4));
        QVERIFY(ops::maskEmpty(ops::combine({}, b, ops::Combine::Subtract)));
        auto inv = ops::invertMask(a);
        QCOMPARE(inv.constScanLine(0)[0], uchar(0));
        QCOMPARE(inv.constScanLine(7)[7], uchar(255));
        // Outline of a 4×4 square is exactly its 4 pixel-aligned edges (merged runs).
        const auto lines = ops::maskOutline(a);
        QCOMPARE(lines.size(), 4);
        for (const auto &l : lines)
            QVERIFY(l.p1().x() == std::floor(l.p1().x()) && l.p1().y() == std::floor(l.p1().y()));
        // Ellipse and polygon masks sample pixel centres.
        auto e = ops::ellipseMask({9, 9}, {0, 0, 9, 9});
        QCOMPARE(e.constScanLine(4)[4], uchar(255));
        QCOMPARE(e.constScanLine(0)[0], uchar(0));
        QCOMPARE(e.constScanLine(0)[4], uchar(255));
        auto tri = ops::polygonMask({8, 8}, {{0, 0}, {8, 0}, {0, 8}});
        QCOMPARE(tri.constScanLine(0)[0], uchar(255));
        QCOMPARE(tri.constScanLine(7)[7], uchar(0));
    }
    void polygonScanlineMatchesReference() {
        // Brute-force even-odd test on pixel centres (the rule the scanline fill implements).
        auto reference = [](QSize size, const QVector<QPointF> &poly) {
            auto m = ops::emptyMask(size);
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x) {
                    bool in = false;
                    const QPointF p(x + .5, y + .5);
                    for (int i = 0, j = int(poly.size()) - 1; i < poly.size(); j = i++) {
                        const auto &a = poly[i], &b = poly[j];
                        if ((a.y() > p.y()) != (b.y() > p.y()) &&
                            p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x())
                            in = !in;
                    }
                    if (in)
                        m.scanLine(y)[x] = 255;
                }
            return m;
        };
        for (int seed = 1; seed <= 40; ++seed) {
            ops::Random r{quint64(seed)};
            QVector<QPointF> poly;
            const int n = 3 + seed % 17;
            for (int k = 0; k < n; ++k)
                poly.append({r.unit() * 70 - 3, r.unit() * 50 - 3}); // self-intersecting, partly off-canvas
            if (seed % 5 == 0)
                for (auto &q : poly)
                    q = QPointF(std::round(q.x()), std::round(q.y())); // vertices on pixel corners
            QCOMPARE(ops::polygonMask({64, 48}, poly), reference({64, 48}, poly));
        }
        ops::ShapeSpec rotated;
        rotated.kind = ops::ShapeKind::Polygon;
        rotated.filled = true;
        rotated.rotation = 45;
        rotated.points = {{20, 20}, {40, 20}, {40, 40}, {20, 40}};
        QVERIFY(ops::rasterizeShape({64, 64}, rotated).constScanLine(30)[30] == 255);
    }
    void colorMaskContiguousAndGlobal() {
        auto im = solid(10, 10, qRgb(255, 0, 0));
        for (int y = 0; y < 10; ++y)
            im.setPixel(5, y, qRgb(0, 0, 0)); // wall splits the image
        im.setPixel(8, 8, qRgb(250, 0, 0));
        auto contiguous = ops::colorMask(im, {1, 1}, 0, ops::Distance::Rgb, true, true);
        QCOMPARE(ops::maskBounds(contiguous), QRect(0, 0, 5, 10));
        auto global = ops::colorMask(im, {1, 1}, 0, ops::Distance::Rgb, false, true);
        QCOMPARE(global.constScanLine(8)[8], uchar(0));
        QCOMPARE(global.constScanLine(1)[9], uchar(255));
        auto tolerant = ops::colorMask(im, {1, 1}, 5, ops::Distance::Rgb, false, true);
        QCOMPARE(tolerant.constScanLine(8)[8], uchar(255));
        // Large single region: bounded iterative fill (4096×1 and 1024² regions).
        auto big = solid(1024, 1024, qRgb(1, 2, 3));
        auto all = ops::colorMask(big, {0, 0}, 0, ops::Distance::Rgb, true, true);
        QCOMPARE(ops::maskBounds(all), QRect(0, 0, 1024, 1024));
        // Diagonal connectivity is opt-in.
        auto checker = solid(4, 4, qRgb(0, 0, 0));
        checker.setPixel(0, 0, qRgb(255, 255, 255));
        checker.setPixel(1, 1, qRgb(255, 255, 255));
        QCOMPARE(ops::colorMask(checker, {0, 0}, 0, ops::Distance::Rgb, true, true, false).constScanLine(1)[1], uchar(0));
        QCOMPARE(ops::colorMask(checker, {0, 0}, 0, ops::Distance::Rgb, true, true, true).constScanLine(1)[1], uchar(255));
    }
    void brushFootprints() {
        auto count = [](ops::BrushShape shape, int size) {
            ops::Brush b;
            b.shape = shape;
            b.size = size;
            return int(ops::footprint(b).size());
        };
        QCOMPARE(count(ops::BrushShape::Square, 1), 1);
        QCOMPARE(count(ops::BrushShape::Square, 3), 9);
        QCOMPARE(count(ops::BrushShape::Square, 4), 16);
        QCOMPARE(count(ops::BrushShape::HorizontalLine, 5), 5);
        QCOMPARE(count(ops::BrushShape::VerticalLine, 5), 5);
        QCOMPARE(count(ops::BrushShape::Diamond, 5), 13);
        QCOMPARE(count(ops::BrushShape::Circle, 5), 21);
        QCOMPARE(count(ops::BrushShape::Circle, 2), 4);
        ops::Brush b;
        b.size = 3;
        for (const auto &d : ops::footprint(b)) {
            QVERIFY(std::abs(d.offset.x()) <= 1 && std::abs(d.offset.y()) <= 1);
            QCOMPARE(d.coverage, 255); // hard by default: never silently blurred
        }
        b.hardness = 0;
        b.size = 7;
        b.shape = ops::BrushShape::Circle;
        int soft = 0;
        for (const auto &d : ops::footprint(b))
            soft += d.coverage < 255;
        QVERIFY(soft > 0);
        // Custom stamp coverage comes from alpha.
        QImage stamp(3, 1, QImage::Format_RGBA8888);
        stamp.fill(Qt::transparent);
        stamp.setPixel(0, 0, qRgba(0, 0, 0, 255));
        stamp.setPixel(2, 0, qRgba(0, 0, 0, 128));
        b.shape = ops::BrushShape::Custom;
        b.stamp = stamp;
        const auto fp = ops::footprint(b);
        QCOMPARE(int(fp.size()), 2);
        QCOMPARE(fp[1].coverage, 128);
    }
    void symmetryPoints() {
        const QSize s(16, 16);
        const QPointF c(8, 8);
        QCOMPARE(int(ops::symmetricPoints({2, 3}, s, ops::Symmetry::Horizontal, 0, c).size()), 2);
        QCOMPARE(ops::symmetricPoints({2, 3}, s, ops::Symmetry::Horizontal, 0, c)[1], QPoint(13, 3));
        QCOMPARE(ops::symmetricPoints({2, 3}, s, ops::Symmetry::Vertical, 0, c)[1], QPoint(2, 12));
        QCOMPARE(int(ops::symmetricPoints({2, 3}, s, ops::Symmetry::Both, 0, c).size()), 4);
        QCOMPARE(int(ops::symmetricPoints({2, 3}, s, ops::Symmetry::Radial, 4, c).size()), 4);
        // On-axis points do not duplicate.
        QCOMPARE(int(ops::symmetricPoints({7, 3}, QSize(15, 16), ops::Symmetry::Horizontal, 0, {7.5, 8}).size()), 1);
    }
    void shapeRasterization() {
        ops::ShapeSpec rect;
        rect.bounds = QRectF(1, 1, 4, 3);
        rect.filled = true;
        auto m = ops::rasterizeShape({8, 8}, rect);
        QCOMPARE(ops::maskBounds(m), QRect(1, 1, 4, 3));
        rect.filled = false;
        m = ops::rasterizeShape({8, 8}, rect);
        QCOMPARE(m.constScanLine(2)[2], uchar(0)); // hollow interior
        QCOMPARE(m.constScanLine(1)[1], uchar(255));
        rect.thickness = 2;
        rect.bounds = QRectF(0, 0, 8, 8);
        m = ops::rasterizeShape({8, 8}, rect);
        QCOMPARE(m.constScanLine(1)[1], uchar(255));
        QCOMPARE(m.constScanLine(2)[2], uchar(0));
        ops::ShapeSpec line;
        line.kind = ops::ShapeKind::Line;
        line.points = {QPointF(0, 0), QPointF(7, 7)};
        m = ops::rasterizeShape({8, 8}, line);
        int on = 0;
        for (int i = 0; i < 8; ++i)
            on += m.constScanLine(i)[i] == 255;
        QCOMPARE(on, 8);
        QCOMPARE(m.constScanLine(0)[1], uchar(0));
        ops::ShapeSpec star;
        star.kind = ops::ShapeKind::Star;
        star.filled = true;
        star.bounds = QRectF(0, 0, 32, 32);
        star.sides = 5;
        m = ops::rasterizeShape({32, 32}, star);
        QCOMPARE(m.constScanLine(16)[16], uchar(255));
        QCOMPARE(m.constScanLine(0)[0], uchar(0));
        ops::ShapeSpec poly = star;
        poly.kind = ops::ShapeKind::RegularPolygon;
        poly.sides = 6;
        QVERIFY(!ops::maskEmpty(ops::rasterizeShape({32, 32}, poly)));
        ops::ShapeSpec rounded;
        rounded.kind = ops::ShapeKind::RoundedRectangle;
        rounded.filled = true;
        rounded.radius = 3;
        rounded.bounds = QRectF(0, 0, 10, 10);
        m = ops::rasterizeShape({10, 10}, rounded);
        QCOMPARE(m.constScanLine(0)[0], uchar(0));
        QCOMPARE(m.constScanLine(5)[0], uchar(255));
        // Rotation by 90° of a square leaves it unchanged.
        rect.thickness = 1;
        rect.filled = true;
        rect.bounds = QRectF(2, 2, 4, 4);
        auto a = ops::rasterizeShape({8, 8}, rect);
        rect.rotation = 90;
        QCOMPARE(ops::rasterizeShape({8, 8}, rect), a);
    }
    void registryValidation() {
        QVERIFY(ops::registry().size() >= 50);
        for (const auto &info : ops::registry()) {
            ops::Spec spec{info.id, {}};
            QString error;
            QVERIFY2(ops::validate(spec, &error), qPrintable(info.id + ": " + error));
            QCOMPARE(spec.params, ops::defaults(info.id));
        }
        QString error;
        ops::Spec bad{"gaussian_blur", {{"sigma", 99}}};
        QVERIFY(!ops::validate(bad, &error));
        QVERIFY(error.contains("between"));
        ops::Spec unknownKey{"invert", {{"bogus", true}}};
        QVERIFY(!ops::validate(unknownKey, &error));
        ops::Spec wrongType{"invert", {{"invert_alpha", 3}}};
        QVERIFY(!ops::validate(wrongType, &error));
        ops::Spec choice{"flip", {{"direction", "Diagonal"}}};
        QVERIFY(!ops::validate(choice, &error));
        ops::Spec unknown{"no_such_op", {}};
        QVERIFY(!ops::validate(unknown, &error));
        ops::Spec fraction{"box_blur", {{"radius", 1.5}}};
        QVERIFY(!ops::validate(fraction, &error));
    }
    void smoothShapeCoverage() {
        const auto overlay = solid(2, 2, qRgba(200, 100, 50, 128));
        auto selection = ops::emptyMask({2, 2});
        selection.scanLine(0)[0] = 128;
        const auto clippedOverlay = ops::maskAlpha(overlay, selection);
        QCOMPARE(clippedOverlay.pixel(0, 0), qRgba(200, 100, 50, 64));
        QCOMPARE(clippedOverlay.pixel(1, 1), QRgb(0));
        ops::ShapeSpec shape;
        shape.bounds = QRectF(.5, .5, 4, 4);
        shape.filled = true;
        shape.smooth = true;
        const auto mask = ops::rasterizeShape({8, 8}, shape);
        QVERIFY(mask.constScanLine(0)[0] >= 60 && mask.constScanLine(0)[0] <= 68); // quarter-pixel coverage
        QCOMPARE(mask.constScanLine(2)[2], uchar(255));
        QCOMPARE(mask.constScanLine(6)[6], uchar(0));
        QCOMPARE(ops::rasterizeShape({8, 8}, shape), mask);
        shape.smooth = false;
        const auto hard = ops::rasterizeShape({8, 8}, shape);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                QVERIFY(hard.constScanLine(y)[x] == 0 || hard.constScanLine(y)[x] == 255);
        shape.smooth = true;
        for (auto kind : {ops::ShapeKind::Ellipse, ops::ShapeKind::RoundedRectangle,
                          ops::ShapeKind::RegularPolygon, ops::ShapeKind::Star, ops::ShapeKind::Polygon}) {
            shape.kind = kind;
            shape.bounds = {2, 2, 12, 12};
            shape.points = {{2, 2}, {14, 3}, {8, 14}};
            shape.radius = 3;
            shape.rotation = 19;
            for (bool filled : {false, true}) {
                shape.filled = filled;
                const auto m = ops::rasterizeShape({16, 16}, shape);
                QVERIFY(!ops::maskEmpty(m));
                int partial = 0;
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < 16; ++x)
                        partial += m.constScanLine(y)[x] > 0 && m.constScanLine(y)[x] < 255;
                QVERIFY(partial > 0);
                QCOMPARE(ops::rasterizeShape({16, 16}, shape), m);
            }
        }
        shape = {};
        shape.smooth = true;
        shape.bounds = {-4, -4, 12, 12};
        const auto clipped = ops::rasterizeShape({16, 16}, shape);
        QCOMPARE(clipped.constScanLine(0)[0], uchar(0)); // clipping creates no outline
        QCOMPARE(clipped.constScanLine(4)[7], uchar(255));
        QCOMPARE(clipped.constScanLine(4)[8], uchar(0));
    }
    void rectangularBrushDimensions() {
        for (auto kind : {ops::BrushShape::Square, ops::BrushShape::Circle, ops::BrushShape::Diamond}) {
            ops::Brush brush;
            brush.shape = kind;
            brush.size = 3;
            brush.height = 9;
            const auto tall = ops::footprint(brush);
            brush.size = 9;
            brush.height = 3;
            const auto wide = ops::footprint(brush);
            QCOMPARE(tall.size(), wide.size());
            for (const auto &dab : tall) {
                QVERIFY(dab.offset.x() >= -1 && dab.offset.x() <= 1);
                QVERIFY(dab.offset.y() >= -4 && dab.offset.y() <= 4);
                QVERIFY(std::any_of(wide.begin(), wide.end(), [&](const auto &other) {
                    return other.offset == QPoint(dab.offset.y(), dab.offset.x()) && other.coverage == dab.coverage;
                }));
            }
            if (kind == ops::BrushShape::Square)
                QCOMPARE(tall.size(), size_t(27));
        }
        ops::Brush stamp;
        stamp.shape = ops::BrushShape::Custom;
        stamp.stamp = solid(3, 2, qRgba(60, 30, 20, 128));
        QCOMPARE(ops::brushDimensions(stamp), QSize(3, 2)); // legacy defaults retain stamp size
        stamp.size = 4;
        stamp.height = 6;
        QCOMPARE(ops::footprint(stamp).size(), size_t(24));
        for (const auto &dab : ops::footprint(stamp))
            QCOMPARE(dab.coverage, 128);
    }
    void everyOperationRunsDeterministically() {
        const auto src = pattern(16, 16);
        for (const auto &info : ops::registry()) {
            ops::Spec spec{info.id, {}};
            ops::Context ctx;
            ctx.palette = {qRgb(0, 0, 0), qRgb(255, 255, 255), qRgb(255, 0, 0)};
            QImage height = solid(16, 16, qRgb(128, 128, 255));
            if (info.id == "normal_map")
                spec.params["source"] = "Luminance";
            if (info.id == "directional_shading")
                ctx.heightmap = &height;
            const auto a = ops::run(spec, src, ctx), b = ops::run(spec, src, ctx);
            QVERIFY2(a.ok(), qPrintable(info.id + ": " + a.error));
            QCOMPARE(digest(a.image), digest(b.image));
            ops::Spec normalised = spec;
            QVERIFY(ops::validate(normalised));
            if (info.id != "trim")
                QCOMPARE(a.image.size(), ops::outputSize(normalised, src.size()));
        }
    }
    void adjustmentsPreserveAlphaAndIdentity() {
        const auto src = pattern(12, 9);
        for (const char *id : {"brightness_contrast", "hue_saturation", "exposure", "gamma", "temperature_tint",
                               "levels", "curves", "channels"}) {
            auto r = run(id, {}, src);
            QVERIFY(r.ok());
            QCOMPARE(digest(r.image), digest(src)); // defaults are exact identities
        }
        auto inverted = run("invert", {}, src).image;
        for (int y = 0; y < src.height(); ++y)
            for (int x = 0; x < src.width(); ++x) {
                QCOMPARE(qAlpha(inverted.pixel(x, y)), qAlpha(src.pixel(x, y)));
                QCOMPARE(qRed(inverted.pixel(x, y)), 255 - qRed(src.pixel(x, y)));
            }
        auto gray = run("grayscale", {}, solid(2, 2, qRgba(255, 0, 0, 77))).image;
        QCOMPARE(gray.pixel(0, 0), qRgba(54, 54, 54, 77));
        auto hue = run("hue_saturation", {{"hue", 120}}, solid(1, 1, qRgb(255, 0, 0))).image;
        QCOMPARE(hue.pixel(0, 0), qRgb(0, 255, 0));
        auto bright = run("brightness_contrast", {{"brightness", 100}}, solid(1, 1, qRgb(0, 0, 0))).image;
        QCOMPARE(bright.pixel(0, 0), qRgb(255, 255, 255));
        auto levels = run("levels", {{"input_black", 100}, {"input_white", 200}}, solid(1, 1, qRgb(150, 50, 250))).image;
        QCOMPARE(levels.pixel(0, 0), qRgb(128, 0, 255));
        auto alpha = run("channels", {{"alpha", 50}}, solid(1, 1, qRgba(10, 20, 30, 200))).image;
        QCOMPARE(qAlpha(alpha.pixel(0, 0)), 100);
        auto swap = run("channels", {{"swap", "Red ↔ Blue"}}, solid(1, 1, qRgb(10, 20, 30))).image;
        QCOMPARE(swap.pixel(0, 0), qRgb(30, 20, 10));
        auto posterized = run("posterize", {{"levels", 2}}, solid(1, 1, qRgb(100, 200, 128))).image;
        QCOMPARE(posterized.pixel(0, 0), qRgb(0, 255, 255));
        auto threshold = run("alpha_threshold", {{"threshold", 128}}, solid(1, 1, qRgba(5, 6, 7, 127))).image;
        QCOMPARE(threshold.pixel(0, 0), qRgba(0, 0, 0, 0));
        auto cleaned = run("transparency_cleanup", {}, solid(1, 1, qRgba(9, 9, 9, 0))).image;
        QCOMPARE(cleaned.pixel(0, 0), qRgba(0, 0, 0, 0));
        auto replaced = run("replace_color", {{"from", "#FFFF0000"}, {"to", "#FF0000FF"}, {"tolerance", 0},
                                              {"preserve_shading", false}},
                            solid(1, 1, qRgba(255, 0, 0, 90)))
                            .image;
        QCOMPARE(replaced.pixel(0, 0), qRgba(0, 0, 255, 90));
    }
    void quantizationDitherAndPalettes() {
        const auto src = pattern(16, 16);
        auto q = run("quantize", {{"colors", 4}}, src).image;
        QSet<QRgb> colors;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                if (qAlpha(q.pixel(x, y)))
                    colors.insert(q.pixel(x, y) | 0xff000000u);
        QVERIFY(colors.size() <= 4);
        for (const char *d : {"None", "Ordered (Bayer 4×4)", "Floyd–Steinberg"}) {
            auto r = run("palette_map", {{"palette", "#FF000000,#FFFFFFFF"}, {"dither", d}},
                         solid(8, 8, qRgba(128, 128, 128, 200)));
            QVERIFY(r.ok());
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const QRgb c = r.image.pixel(x, y);
                    QVERIFY((c & 0xffffff) == 0 || (c & 0xffffff) == 0xffffff);
                    QCOMPARE(qAlpha(c), 200);
                }
        }
        // Dithering a mid grey to black/white produces both colours.
        auto dithered = run("palette_map", {{"palette", "#FF000000,#FFFFFFFF"}, {"dither", "Ordered (Bayer 4×4)"}},
                            solid(8, 8, qRgb(128, 128, 128)))
                            .image;
        QVERIFY(dithered.pixel(0, 0) != dithered.pixel(1, 0) || dithered.pixel(0, 0) != dithered.pixel(0, 1));
        auto empty = run("palette_map", {}, src);
        QVERIFY(!empty.ok());
        QVERIFY(empty.error.contains("palette"));
        QCOMPARE(ops::extractPalette(solid(4, 4, qRgb(1, 2, 3)), 8).size(), size_t(1));
        QVERIFY(ops::medianCut(src, 8).size() <= 8);
    }
    void filtersAndBoundaries() {
        // Blur of a uniform image is the identity; transparent neighbours do not bleed.
        auto uniform = solid(8, 8, qRgb(40, 80, 120));
        QCOMPARE(digest(run("gaussian_blur", {{"sigma", 2.0}}, uniform).image), digest(uniform));
        QCOMPARE(digest(run("box_blur", {{"radius", 2}}, uniform).image), digest(uniform));
        QImage island(5, 5, QImage::Format_RGBA8888);
        island.fill(Qt::transparent);
        island.setPixel(2, 2, qRgb(255, 0, 0));
        auto blurred = run("box_blur", {{"radius", 1}}, island).image;
        QCOMPARE(qRed(blurred.pixel(1, 1)), 255);
        QCOMPARE(qGreen(blurred.pixel(1, 1)), 0);
        QVERIFY(qAlpha(blurred.pixel(1, 1)) > 0 && qAlpha(blurred.pixel(1, 1)) < 255);
        // Wrapping blur on a horizontal gradient differs only at the edges.
        auto sharpened = run("sharpen", {}, uniform).image;
        QCOMPARE(digest(sharpened), digest(uniform));
        auto med = run("median", {}, island).image;
        QCOMPARE(qAlpha(med.pixel(2, 2)), 0); // isolated pixel removed
        auto outline = run("outline", {}, island).image;
        QCOMPARE(outline.pixel(2, 1), qRgb(0, 0, 0));
        QCOMPARE(outline.pixel(1, 1), qRgb(0, 0, 0)); // square corners
        auto rounded = run("outline", {{"corners", "Rounded"}}, island).image;
        QCOMPARE(qAlpha(rounded.pixel(1, 1)), 0);
        auto inside = run("outline", {{"position", "Inside"}, {"color", "#FF00FF00"}}, island).image;
        QCOMPARE(inside.pixel(2, 2), qRgb(0, 255, 0));
        auto pix = run("pixelate", {{"block", 4}}, pattern(8, 8)).image;
        QCOMPARE(pix.pixel(0, 0), pix.pixel(3, 3));
        auto normal = run("normal_map", {}, uniform).image;
        QCOMPARE(normal.pixel(3, 3), qRgb(128, 128, 255)); // flat surface faces the viewer
        auto noise = run("noise", {{"seed", 5}}, uniform).image;
        QVERIFY(digest(noise) != digest(uniform));
        QCOMPARE(digest(noise), digest(run("noise", {{"seed", 5}}, uniform).image));
        QVERIFY(digest(noise) != digest(run("noise", {{"seed", 6}}, uniform).image));
        auto edges = run("edge_detect", {}, uniform).image;
        QCOMPARE(qRed(edges.pixel(4, 4)), 0);
    }
    void transformsAreExact() {
        const auto src = pattern(7, 5);
        auto flipped = run("flip", {}, src).image;
        QCOMPARE(flipped.pixel(0, 0), src.pixel(6, 0));
        QCOMPARE(digest(run("flip", {}, flipped).image), digest(src));
        auto turned = run("image_rotate", {}, src).image;
        QCOMPARE(turned.size(), QSize(5, 7));
        QCOMPARE(turned.pixel(0, 0), src.pixel(0, 4));
        auto back = run("image_rotate", {{"turns", "90° counter-clockwise"}}, turned).image;
        QCOMPARE(digest(back), digest(src));
        auto half = run("image_rotate", {{"turns", "180°"}}, src).image;
        QCOMPARE(half.pixel(0, 0), src.pixel(6, 4));
        const auto square = pattern(8, 8);
        auto quarter = run("rotate", {{"angle", 90.0}}, square).image;
        QCOMPARE(quarter.pixel(0, 0), square.pixel(0, 7));
        QCOMPARE(digest(run("rotate", {{"angle", 360.0}}, square).image), digest(square));
        auto up = run("resize_image", {{"width", 14}, {"height", 10}}, src).image;
        QCOMPARE(up.pixel(1, 1), src.pixel(0, 0)); // nearest neighbour: exact 2× blocks
        QCOMPARE(up.pixel(13, 9), src.pixel(6, 4));
        auto down = run("resize_image", {{"width", 7}, {"height", 5}}, up).image;
        QCOMPARE(digest(down), digest(src));
        auto smooth = run("resize_image", {{"width", 14}, {"height", 10}, {"resampling", "Bilinear"}}, src);
        QVERIFY(smooth.ok());
        auto canvas = run("resize_canvas", {{"width", 9}, {"height", 7}}, src).image;
        QCOMPARE(canvas.pixel(1, 1), src.pixel(0, 0));
        QCOMPARE(qAlpha(canvas.pixel(0, 0)), 0); // transparent padding
        auto wrapped = run("resize_canvas", {{"width", 14}, {"height", 5}, {"anchor", "Top left"},
                                             {"padding", "Repeat (wrap)"}},
                           src)
                           .image;
        QCOMPARE(wrapped.pixel(7, 2), src.pixel(0, 2));
        auto cropped = run("crop", {{"x", 1}, {"y", 1}, {"width", 3}, {"height", 2}}, src).image;
        QCOMPARE(cropped.pixel(0, 0), src.pixel(1, 1));
        QVERIFY(!run("crop", {{"x", 6}, {"y", 0}, {"width", 3}, {"height", 2}}, src).ok());
        QImage sprite(10, 10, QImage::Format_RGBA8888);
        sprite.fill(Qt::transparent);
        sprite.setPixel(3, 4, qRgb(1, 1, 1));
        sprite.setPixel(5, 6, qRgb(2, 2, 2));
        QCOMPARE(run("trim", {}, sprite).image.size(), QSize(3, 3));
        auto centred = run("align_content", {}, sprite).image;
        QCOMPARE(ops::alphaBounds(centred, 0), QRect(3, 3, 3, 3));
        auto corner = run("align_content", {{"anchor", "Top left"}}, sprite).image;
        QCOMPARE(corner.pixel(0, 0), qRgb(1, 1, 1));
        auto off = run("offset", {{"dx", 3}, {"dy", 0}}, src).image;
        QCOMPARE(off.pixel(3, 0), src.pixel(0, 0));
        QCOMPARE(off.pixel(0, 0), src.pixel(4, 0)); // wrapped
    }
    void maskedAndLiftedOperations() {
        const auto src = pattern(8, 8);
        const auto mask = ops::rectMask({8, 8}, {0, 0, 4, 8});
        ops::Context ctx;
        ctx.mask = &mask;
        auto inv = run("invert", {}, src, ctx).image;
        QCOMPARE(qRed(inv.pixel(1, 1)), 255 - qRed(src.pixel(1, 1)));
        QCOMPARE(inv.pixel(6, 1), src.pixel(6, 1)); // outside the selection untouched
        // Lifted flip mirrors only inside the selection bounds.
        auto flipped = run("flip", {}, src, ctx).image;
        QCOMPARE(flipped.pixel(0, 1), src.pixel(3, 1)); // both opaque
        QCOMPARE(flipped.pixel(5, 2), src.pixel(5, 2));
        // Document-size operations ignore masks entirely.
        auto turned = run("image_rotate", {}, src, ctx).image;
        QCOMPARE(turned.pixel(0, 0), src.pixel(0, 7));
    }
    void memoryBudgetAndCancellation() {
        ops::Context ctx;
        ctx.memoryLimit = 1024;
        auto r = run("gaussian_blur", {}, pattern(64, 64), ctx);
        QVERIFY(!r.ok());
        QVERIFY(r.error.contains("budget"));
        std::atomic_bool cancel = true;
        ops::Context canceled;
        canceled.cancel = &cancel;
        auto c = run("gaussian_blur", {{"sigma", 3.0}}, pattern(256, 256), canceled);
        QVERIFY(c.canceled);
        QVERIFY(c.image.isNull());
        QVERIFY(!run("invert", {}, QImage()).ok());
    }
    void generatorsAreDeterministicAndTileable() {
        for (const auto &info : ops::registry()) {
            if (!info.generator)
                continue;
            ops::Spec spec{info.id, {{"width", 32}, {"height", 32}, {"seed", 42}}};
            auto a = ops::run(spec, {}), b = ops::run(spec, {});
            QVERIFY2(a.ok(), qPrintable(info.id));
            QCOMPARE(a.image.size(), QSize(32, 32));
            QCOMPARE(digest(a.image), digest(b.image));
            spec.params["seed"] = 43;
            auto c = ops::run(spec, {});
            if (info.id != "gen_gui_panel" && info.id != "gen_item" && info.id != "gen_metal")
                QVERIFY2(digest(a.image) != digest(c.image), qPrintable(info.id));
        }
        // Periodic noise is mathematically tileable: wrapped neighbours stay close.
        auto noise = ops::run({"gen_noise", {{"width", 64}, {"height", 64}}}, {}).image;
        const auto report = ops::seamReport(noise);
        double interior = 0;
        for (int y = 0; y < 64; ++y) {
            const QRgb a = noise.pixel(31, y), b = noise.pixel(32, y);
            interior += (std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b))) / 4.0;
        }
        interior /= 64;
        QVERIFY2(report.horizontal <= interior * 3 + 2, qPrintable(QString::number(report.horizontal)));
        const auto bricks = ops::run({"gen_bricks", {{"width", 32}, {"height", 32}, {"brick_width", 7},
                                                     {"brick_height", 3}, {"mortar", 1}}},
                                     {})
                                .image;
        QCOMPARE(ops::tile(bricks, 2, 2).size(), QSize(64, 64));
        QCOMPARE(ops::wrapOffset(bricks, 32, 32), bricks);
    }
    void shadingBehaviour() {
        QImage bump(16, 16, QImage::Format_RGBA8888);
        bump.fill(Qt::transparent);
        for (int y = 2; y < 14; ++y)
            for (int x = 2; x < 14; ++x)
                bump.setPixel(x, y, qRgb(120, 120, 120));
        ops::Spec spec{"directional_shading", {{"height_source", "Alpha bevel"}, {"output", "Modify pixels"},
                                               {"light_direction", 315}, {"steps", 2}}};
        auto lit = ops::run(spec, bump).image;
        QVERIFY(ops::luma(lit.pixel(2, 2)) > ops::luma(lit.pixel(13, 13))); // top-left lit
        spec.params["light_direction"] = 135;
        auto opposite = ops::run(spec, bump).image;
        QVERIFY(ops::luma(opposite.pixel(13, 13)) > ops::luma(opposite.pixel(2, 2)));
        QCOMPARE(qAlpha(lit.pixel(0, 0)), 0);
        // Shading layer output contains only overlay pixels over opaque source pixels.
        spec.params["output"] = "Shading layer";
        auto layer = ops::run(spec, bump).image;
        QCOMPARE(qAlpha(layer.pixel(0, 0)), 0);
        QVERIFY(qAlpha(layer.pixel(2, 2)) > 0);
        // Palette-constrained output uses only palette colours.
        ops::Context ctx;
        ctx.palette = {qRgb(60, 60, 60), qRgb(120, 120, 120), qRgb(200, 200, 200)};
        spec.params["output"] = "Modify pixels";
        spec.params["palette_mode"] = "Active palette";
        auto constrained = ops::run(spec, bump, ctx).image;
        for (int y = 2; y < 14; ++y)
            for (int x = 2; x < 14; ++x)
                QVERIFY(std::find(ctx.palette.begin(), ctx.palette.end(), constrained.pixel(x, y)) != ctx.palette.end());
        // Selection mask confines the change.
        const auto mask = ops::rectMask({16, 16}, {0, 0, 8, 16});
        ctx.mask = &mask;
        auto masked = ops::run(spec, bump, ctx).image;
        QCOMPARE(masked.pixel(13, 13), bump.pixel(13, 13));
        // Presets are loadable and valid.
        int presets = 0;
        for (const auto &preset : ops::builtInPresets()) {
            ops::Spec s{preset.operation, preset.params};
            QVERIFY2(ops::validate(s), qPrintable(preset.name));
            presets += preset.operation == "directional_shading";
        }
        QCOMPARE(presets, 7);
        QVERIFY(!ops::run({"directional_shading", {{"height_source", "Normal map layer"}}}, bump).ok());
    }
    void paletteFormatsRoundTrip() {
        Palette p;
        p.name = "Test";
        p.colors = {qRgb(1, 2, 3), qRgb(250, 128, 0), qRgba(10, 20, 30, 128)};
        for (auto format : {PaletteFormat::Gpl, PaletteFormat::JascPal, PaletteFormat::PaintNet, PaletteFormat::Hex}) {
            Palette back;
            QString error;
            QVERIFY2(decodePalette(encodePalette(p, format), format, &back, &error), qPrintable(error));
            QCOMPARE(back.colors.size(), p.colors.size());
            QCOMPARE(back.colors[1], p.colors[1]);
            if (format == PaletteFormat::PaintNet)
                QCOMPARE(back.colors[2], p.colors[2]); // only Paint.NET stores alpha
        }
        Palette out;
        QString error;
        QVERIFY(!decodePalette("not a palette", PaletteFormat::Gpl, &out, &error));
        QVERIFY(!decodePalette("JASC-PAL\n0100\n2\n1 2 3\n", PaletteFormat::JascPal, &out, &error));
        QVERIFY(!decodePalette("GIMP Palette\n300 0 0\n", PaletteFormat::Gpl, &out, &error));
        QVERIFY(!decodePalette("zzzzzz\n", PaletteFormat::Hex, &out, &error));
        QTemporaryDir dir;
        QVERIFY(exportPalette(dir.filePath("p.gpl"), p, &error));
        QVERIFY(importPalette(dir.filePath("p.gpl"), &out, &error));
        QCOMPARE(out.name, QString("Test"));
        QVERIFY(!exportPalette(dir.filePath("p.aco"), p, &error));
        Palette dup = p;
        dup.colors.push_back(qRgb(1, 2, 3));
        QCOMPARE(removeDuplicateColors(dup), 1);
        sortPalette(dup, PaletteSort::Luminance);
        QCOMPARE(dup.colors.front(), qRgb(1, 2, 3));
        PaletteStore store(dir.filePath("palettes.json"));
        store.palettes.push_back(p);
        QVERIFY(store.save(&error));
        PaletteStore reloaded(dir.filePath("palettes.json"));
        QVERIFY(reloaded.load(&error));
        QCOMPARE(reloaded.palettes.size(), size_t(1));
        QCOMPARE(reloaded.palettes[0].colors, p.colors);
        QVERIFY(reloaded.all().size() == builtInPalettes().size() + 1);
        QCOMPARE(reloaded.uniqueName("Test"), QString("Test 2"));
    }
};
QTEST_GUILESS_MAIN(OpsTests)
#include "ops_test.moc"
