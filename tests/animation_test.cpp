// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include "minecraft_animation.h"
#include "recovery.h"
#include <QTemporaryDir>
#include <QJsonArray>
#include <QTest>
using namespace nova;
class AnimationTest : public QObject {
    Q_OBJECT
    static QImage pixels(QSize size = {16, 16}, QColor color = Qt::red) {
        QImage out(size, QImage::Format_RGBA8888); out.fill(color); return out;
    }
  private slots:
    void frameIdentityCowHistory() {
        Document doc("test.png", pixels());
        auto first = doc.activeFrame();
        QVERIFY(doc.convertToAnimation());
        QVERIFY(doc.editFrame(FrameCommand::Duplicate));
        auto second = doc.activeFrame();
        QVERIFY(first != second);
        auto state = doc.animationState();
        QCOMPARE(state.frames[0].layers[0].pixels.constBits(), state.frames[1].layers[0].pixels.constBits());
        QVERIFY(state.frames[0].layers[0].id != state.frames[1].layers[0].id);
        QVERIFY(doc.paint(doc.activeLayer(), {1, 1}, {1, 1}, qRgb(0, 0, 255))); doc.finish();
        QCOMPARE(doc.image.pixelColor(1, 1), QColor(Qt::blue));
        QVERIFY(doc.selectFrame(first));
        QCOMPARE(doc.image.pixelColor(1, 1), QColor(Qt::red));
        doc.undo();
        QCOMPARE(doc.activeFrame(), second);
        QCOMPARE(doc.image.pixelColor(1, 1), QColor(Qt::red));
        doc.redo(); QCOMPARE(doc.image.pixelColor(1, 1), QColor(Qt::blue));
        QVERIFY(doc.editFrame(FrameCommand::Move, second, 0));
        QCOMPARE(doc.animationState().entries[0].frame, first);
        QVERIFY(doc.editFrame(FrameCommand::Delete, first));
        QCOMPARE(doc.animationState().frames.size(), size_t(1));
        doc.undo(); QCOMPARE(doc.animationState().frames.size(), size_t(2));
        QCOMPARE(doc.animationState().frames[0].id, second);
    }
    void invalidStateAndLimits() {
        Document doc("test.png", pixels()); QVERIFY(doc.convertToAnimation());
        auto original = doc.animationState(), state = original;
        state.entries[0].frame = QUuid::createUuid(); QVERIFY(!doc.replaceAnimation(state, "invalid"));
        state = original; state.defaultTicks = 0; QVERIFY(!doc.replaceAnimation(state, "invalid"));
        state = original; state.entries.clear(); QVERIFY(!doc.replaceAnimation(state, "invalid"));
        state = original; state.entries[0].ticks = -1; QVERIFY(!doc.replaceAnimation(state, "invalid"));
        state = original; state.frames.push_back(state.frames[0]); QVERIFY(!doc.replaceAnimation(state, "invalid"));
        auto limits = doc.frameLimits(); limits.sourceFrames = 1; QVERIFY(doc.setAnimationLimits(limits));
        QVERIFY(!doc.editFrame(FrameCommand::Add));
        QCOMPARE(doc.animationState().frames.size(), size_t(1));
    }
    void layeredNativeRecoveryRoundTrip() {
        QTemporaryDir temp;
        Document doc(temp.filePath("texture.png"), pixels()); QVERIFY(doc.convertToAnimation());
        QVERIFY(doc.editLayer({LayerCommand::Add, doc.activeLayer(), "Overlay"}));
        const auto overlay = doc.activeLayer();
        QVERIFY(doc.editLayer({LayerCommand::Opacity, overlay, {}, 128}));
        QVERIFY(doc.editLayer({LayerCommand::Visibility, overlay, {}, 0}));
        QVERIFY(doc.editFrame(FrameCommand::Duplicate));
        auto state = doc.animationState();
        state.entries.push_back({state.frames[0].id, 8}); state.defaultTicks = 2; state.interpolate = true;
        state.metadata = {{"texture", QJsonObject{{"clamp", true}}}, {"unknown", QJsonObject{{"retained", 42}}}};
        QVERIFY(doc.replaceAnimation(state, "Timing"));
        auto bytes = doc.encodeNative("texture.png"); QVERIFY(!bytes.isEmpty());
        Document loaded("texture.png", QImage{}); QString error;
        QVERIFY2(loaded.decodeNative(bytes, "texture.png", &error), qPrintable(error));
        auto roundtrip = loaded.animationState();
        QCOMPARE(roundtrip.active, state.active); QCOMPARE(roundtrip.frames[0].id, state.frames[0].id);
        QCOMPARE(roundtrip.frames[0].layers[1].id, overlay);
        QCOMPARE(roundtrip.frames[0].layers[1].opacity, 128); QVERIFY(!roundtrip.frames[0].layers[1].visible);
        QCOMPARE(roundtrip.entries[2].frame, state.frames[0].id); QCOMPARE(roundtrip.entries[2].ticks, 8);
        QCOMPARE(roundtrip.defaultTicks, 2); QVERIFY(roundtrip.interpolate); QCOMPARE(roundtrip.metadata, state.metadata);
        auto broken = bytes; broken[broken.size() - 1] ^= 1;
        QVERIFY(!loaded.decodeNative(broken, "texture.png"));
        QCOMPARE(loaded.animationState().frames[0].id, state.frames[0].id);
        RecoveryStore recovery(temp.filePath("recovery"));
        auto snapshot = doc.recoverySnapshot();
        QCOMPARE(snapshot->animationState().frames.size(), size_t(2));
        QVERIFY(recovery.writeDocument(*snapshot, temp.path(), &error));
    }
    void gridsTimingMetadataExactPixels() {
        QImage sheet = pixels({32, 16});
        for (int y = 0; y < 16; ++y) for (int x = 16; x < 32; ++x) sheet.setPixelColor(x, y, Qt::blue);
        QJsonObject meta{{"animation", QJsonObject{{"frames", QJsonArray{0, QJsonObject{{"index", 1}, {"time", 8}}, 0}}, {"frametime", 2}}},
                         {"texture", QJsonObject{{"blur", false}}}};
        Document doc("test.png", pixels()); QString error;
        QVERIFY2(doc.importAnimation(sheet, meta, "26.1", &error), qPrintable(error));
        auto state = doc.animationState(); QCOMPARE(state.frames.size(), size_t(2));
        QCOMPARE(state.entries.size(), size_t(3)); QCOMPARE(state.entries[0].frame, state.entries[2].frame);
        auto plan = planAnimationExport(doc, 2, "26.1", &error); QVERIFY2(plan.ok(), qPrintable(error));
        QCOMPARE(plan.sheet, sheet); QCOMPARE(plan.metadata["texture"], meta["texture"]);
        auto animation = meta["animation"].toObject(); animation["width"] = 8; animation["height"] = 4;
        meta["animation"] = animation; QVERIFY(doc.importAnimation(sheet, meta, "26.3"));
        QCOMPARE(doc.animationState().frames.size(), size_t(16)); QCOMPARE(doc.image.size(), QSize(8, 4));
        animation.remove("height"); meta["animation"] = animation;
        QVERIFY(doc.importAnimation(sheet, meta, "1.21.11")); QCOMPARE(doc.image.size(), QSize(8, 16));
        animation["width"] = 7; meta["animation"] = animation; QVERIFY(!doc.importAnimation(sheet, meta, "26.1"));
        animation["width"] = 8; animation["frametime"] = 0; meta["animation"] = animation;
        QVERIFY(!doc.importAnimation(sheet, meta, "26.1"));
        QVERIFY(!planAnimationExport(doc, 1, "1.12").ok());
    }
    void existingOpsScopedToFrames() {
        Document doc("test.png", pixels()); QVERIFY(doc.convertToAnimation());
        QVERIFY(doc.editFrame(FrameCommand::Duplicate));
        const auto active = doc.activeFrame(), first = doc.animationState().frames[0].id;
        QVERIFY(doc.applyFrames({active}, {"invert", {}}));
        QCOMPARE(doc.frameComposite(first).pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(doc.image.pixelColor(0, 0), QColor(Qt::cyan));
        doc.undo(); QCOMPARE(doc.image.pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(doc.applyFrames({first, active}, {"invert", {}}));
        QCOMPARE(doc.frameComposite(first).pixelColor(0, 0), QColor(Qt::cyan));
        QVERIFY(!doc.applyFrames({first}, {"resize_image", {{"width", 32}, {"height", 32}}}));
    }
    void sharedStructuralHistoryAtLargeFrameCount() {
        Document doc("test.png", pixels({256, 256})); auto state = doc.animationState();
        const auto base = state.frames[0].layers[0]; state.enabled = true; state.frames.clear(); state.entries.clear();
        for (int i = 0; i < 64; ++i) {
            SourceFrame frame;
            for (int j = 0; j < 4; ++j) { auto layer = base; layer.id = QUuid::createUuid(); frame.layers.push_back(layer); }
            frame.activeLayer = frame.layers[0].id; state.frames.push_back(frame); state.entries.push_back({frame.id, 0});
        }
        state.active = state.frames[0].id; QVERIFY(doc.replaceAnimation(state, "Large shared frames"));
        QVERIFY(doc.editFrame(FrameCommand::Duplicate)); QVERIFY(doc.canUndo());
        QVERIFY(doc.historyBytes() <= 16LL * 1024 * 1024); doc.undo(); QCOMPARE(doc.animationState().frames.size(), size_t(64));
        QCOMPARE(doc.activeFrame(), state.active);
    }
    void newAndDuplicatedFrameLayerContext() {
        Document doc("test.png", pixels()); QVERIFY(doc.convertToAnimation());
        const auto background = doc.activeLayer(); QVERIFY(doc.editLayer({LayerCommand::Add, background, "Overlay"}));
        QVERIFY(doc.editLayer({LayerCommand::Select, background}));
        QVERIFY(doc.editFrame(FrameCommand::Duplicate)); QCOMPARE(doc.activeLayer(), doc.layers()[0].id);
        QVERIFY(doc.editLayer({LayerCommand::Visibility, doc.layers()[0].id, {}, 0}));
        QVERIFY(doc.editFrame(FrameCommand::Add)); QCOMPARE(doc.layers().size(), size_t(1));
        QVERIFY(doc.layers()[0].visible); QCOMPARE(doc.layers()[0].opacity, 255); QVERIFY(doc.layers()[0].extensions.isEmpty());
        QVERIFY(doc.paint(doc.activeLayer(), {0, 0}, {0, 0}, qRgb(1, 2, 3)));
    }
};
QTEST_GUILESS_MAIN(AnimationTest)
#include "animation_test.moc"
