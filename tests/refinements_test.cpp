// SPDX-License-Identifier: GPL-3.0-or-later
#include "presets.h"
#include "session.h"
#include "tools.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace nova;
class RefinementsTest : public QObject {
    Q_OBJECT
    static QImage blank(int n = 8) {
        QImage i(n, n, QImage::Format_RGBA8888);
        i.fill(Qt::transparent);
        return i;
    }
  private slots:
    void namedShapePresetsAndOriginalTemplates() {
        QTemporaryDir directory;
        PresetStore saved(directory.filePath("presets.json"));
        QJsonObject shape{{"kind", int(ops::ShapeKind::Star)},
                          {"x", 2},
                          {"y", 3},
                          {"width", 8},
                          {"height", 6},
                          {"rotation", 30},
                          {"thickness", 2},
                          {"points", 7},
                          {"radius", 2},
                          {"inner", .4},
                          {"filled", true}};
        saved.put({"shape", "Inset star", {}, shape});
        QString error;
        QVERIFY(saved.save(&error));
        PresetStore reopened(saved.path());
        QVERIFY(reopened.load(&error));
        const auto preset = reopened.find("shape", "Inset star");
        QVERIFY(preset);
        QCOMPARE(preset->data, shape);
        int templates = 0;
        for (const auto &preset : ops::builtInPresets())
            if (preset.name.startsWith("C5")) {
                ops::Spec spec{preset.operation, preset.params};
                QVERIFY2(ops::validate(spec, &error), qPrintable(error));
                auto result = ops::run(spec, blank(16));
                QVERIFY2(result.ok(), qPrintable(result.error));
                ++templates;
            }
        QCOMPARE(templates, 4);
    }
    void rgbaStampAlphaPaletteAndMigration() {
        Document doc("test.png", blank());
        StrokeStyle style;
        style.brush.shape = ops::BrushShape::Custom;
        style.brush.stamp = QImage(2, 1, QImage::Format_RGBA8888);
        style.brush.stamp.setPixel(0, 0, qRgba(230, 20, 30, 255));
        style.brush.stamp.setPixel(1, 0, qRgba(10, 210, 90, 128));
        style.brush.sourceColors = true;
        QVERIFY(doc.stroke(doc.activeLayer(), {2, 2}, {2, 2}, style));
        doc.finish();
        QCOMPARE(doc.image.pixel(2, 2), qRgba(230, 20, 30, 255));
        QCOMPARE(qAlpha(doc.image.pixel(3, 2)), 128);
        QCOMPARE(qGreen(doc.image.pixel(3, 2)), 210);
        doc.undo();
        QCOMPARE(doc.image, blank());
        doc.redo();
        QCOMPARE(qAlpha(doc.image.pixel(3, 2)), 128);
        ToolState tools;
        tools.brush(Tool::Brush) = style.brush;
        auto preset = tools.brushPreset(Tool::Brush);
        QCOMPARE(preset["schema"].toInt(), 3);
        ToolState loaded;
        loaded.applyBrushPreset(Tool::Brush, preset);
        QVERIFY(loaded.brush(Tool::Brush).sourceColors);
        QCOMPARE(loaded.brush(Tool::Brush).stamp, style.brush.stamp);
        preset["schema"] = 2;
        loaded.applyBrushPreset(Tool::Brush, preset);
        QVERIFY(!loaded.brush(Tool::Brush).sourceColors);
        doc.undo();
        style.palette = {qRgb(0, 0, 255)};
        style.brush.opacity = 128;
        doc.stroke(doc.activeLayer(), {2, 2}, {2, 2}, style);
        doc.finish();
        QCOMPARE(qBlue(doc.image.pixel(2, 2)), 255);
        QCOMPARE(qAlpha(doc.image.pixel(2, 2)), 128);
    }
    void multiLayerAtomicScope() {
        auto image = blank();
        image.setPixelColor(1, 2, Qt::red);
        auto doc = std::make_shared<Document>("test.png", image);
        DocumentSession session;
        QVERIFY(session.adopt(doc));
        QVERIFY(session.edit(doc, {LayerCommand::Duplicate, doc->activeLayer()}));
        const auto first = doc->layers()[0].id, second = doc->layers()[1].id;
        const auto before = doc->historyCursor();
        QString error;
        QVERIFY(session.applyLayers(doc, {first, second},
                                    {"offset", {{"dx", 2}, {"dy", 1}, {"wrap", false}}}, &error));
        QCOMPARE(doc->historyCursor(), before + 1);
        for (const auto &l : doc->layers()) {
            QCOMPARE(l.pixels.pixelColor(3, 3), QColor(Qt::red));
            QCOMPARE(qAlpha(l.pixels.pixel(1, 2)), 0);
        }
        doc->undo();
        for (const auto &l : doc->layers())
            QCOMPARE(l.pixels, image);
        doc->redo();
        const auto pixels = doc->image;
        const auto cursor = doc->historyCursor();
        QVERIFY(!session.applyLayers(doc, {first, QUuid::createUuid()}, {"invert", {}}, &error));
        QCOMPARE(doc->image, pixels);
        QCOMPARE(doc->historyCursor(), cursor);
        QVERIFY(!session.applyLayers(doc, {first}, {"resize_image", {{"width", 4}, {"height", 4}}},
                                     &error));
        QCOMPARE(doc->image.size(), QSize(8, 8));
        QVERIFY(session.applyLayers(doc, {first, second},
                                    {"scale_content", {{"scale_x", 50}, {"scale_y", 50}}}, &error));
        QCOMPARE(doc->layers()[0].id, first);
        QCOMPARE(doc->layers()[1].id, second);
    }
    void nativeCacheNeverStales() {
        auto i = blank(64);
        i.fill(Qt::red);
        Document doc("test.png", i);
        QString error;
        const auto one = doc.encodeNative("test.png", &error);
        QVERIFY(!one.isEmpty());
        QCOMPARE(doc.encodeNative("test.png", &error), one);
        doc.draw({0, 0}, {0, 0}, qRgb(0, 255, 0));
        doc.finish();
        const auto two = doc.encodeNative("test.png", &error);
        QVERIFY(two != one);
        Document decoded("test.png", {});
        QVERIFY(decoded.decodeNative(two, "test.png", &error));
        QCOMPARE(decoded.image, doc.image);
        doc.undo();
        const auto three = doc.encodeNative("test.png", &error);
        QVERIFY(decoded.decodeNative(three, "test.png", &error));
        QCOMPARE(decoded.image, i);
        QVERIFY(doc.convertToAnimation());
        QVERIFY(doc.editFrame(FrameCommand::Duplicate));
        const auto animated = doc.encodeNative("test.png", &error);
        QVERIFY(decoded.decodeNative(animated, "test.png", &error));
        QCOMPARE(decoded.animationState().frames.size(), size_t(2));
    }
};
QTEST_MAIN(RefinementsTest)
#include "refinements_test.moc"
