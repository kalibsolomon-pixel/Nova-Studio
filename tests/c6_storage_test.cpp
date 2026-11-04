// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6_storage_test.h"
#include "core.h"
#include "recovery.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace nova;
void StreamingTest::uniqueSpoolRoundtripAndRecovery() {
    QTemporaryDir root;
    QImage base(128, 128, QImage::Format_RGBA8888);
    base.fill(Qt::transparent);
    Document doc(root.filePath("texture.png"), base);
    AnimationState state;
    state.enabled = true;
    state.metadata = {{"unknown", QJsonObject{{"keep", true}}}};
    quint32 random = 0x12345;
    for (int frame = 0; frame < 40; ++frame) {
        SourceFrame source;
        source.name = QString("Frame %1").arg(frame);
        for (int layer = 0; layer < 4; ++layer) {
            QImage pixels = base;
            for (int y = 0; y < 128; ++y) {
                auto row = pixels.scanLine(y);
                for (int x = 0; x < 512; ++x) {
                    random ^= random << 13;
                    random ^= random >> 17;
                    random ^= random << 5;
                    row[x] = uchar(random);
                }
            }
            RasterLayer raster;
            raster.id = QUuid::createUuid();
            raster.name = QString("Layer %1").arg(layer);
            raster.pixels = pixels;
            raster.visible = layer != 2;
            raster.opacity = layer == 1 ? 123 : 255;
            raster.extensions = {{"custom", frame}};
            source.layers.push_back(raster);
        }
        source.activeLayer = source.layers[1].id;
        state.entries.push_back({source.id, frame % 5 + 1});
        state.frames.push_back(source);
    }
    state.active = state.frames[0].id;
    QString error;
    QVERIFY2(doc.replaceAnimation(state, "Unique fixture", &error), qPrintable(error));
    const auto bytes = doc.encodeNative("texture.png", &error);
    QVERIFY2(bytes.size() > 8 * 1024 * 1024, qPrintable(error));
    QVERIFY(bytes.startsWith("NVAN0002"));
    Document loaded(doc.path, {});
    QVERIFY2(loaded.decodeNative(bytes, "texture.png", &error), qPrintable(error));
    auto actual = loaded.animationState();
    QCOMPARE(actual.metadata, state.metadata);
    QCOMPARE(actual.entries.size(), state.entries.size());
    QCOMPARE(actual.frames[29].layers[2].pixels, state.frames[29].layers[2].pixels);
    QCOMPARE(actual.frames[29].layers[2].extensions, state.frames[29].layers[2].extensions);
    QVERIFY(!actual.frames[29].layers[2].visible);
    auto corrupt = bytes;
    corrupt[corrupt.size() - 100] = char(corrupt[corrupt.size() - 100] ^ 1);
    const auto before = loaded.image;
    QVERIFY(!loaded.decodeNative(corrupt, "texture.png", &error));
    QCOMPARE(loaded.image, before);
    RecoveryStore recovery(root.filePath("recovery"));
    QVERIFY2(recovery.writeDocument(*doc.recoverySnapshot(), root.path(), &error),
             qPrintable(error));
    auto restored = recovery.load(doc.id());
    QVERIFY2(restored.error.isEmpty(), qPrintable(restored.error));
    Document recovered(doc.path, {});
    QVERIFY(recovered.decodeNative(restored.nativeData, "recovery.png", &error));
    QCOMPARE(recovered.animationState().frames[39].layers[3].pixels,
             state.frames[39].layers[3].pixels);
}
void StreamingTest::guardedStreamFailureAndConflict() {
    QTemporaryDir root;
    const auto path = root.filePath("guarded.bin");
    QString error;
    QVERIFY(atomicWrite(path, "original", {}, &error));
    const auto expected = Revision::read(path);
    QVERIFY(!atomicWriteStream(
        path, expected,
        [](QIODevice &file) {
            file.write("partial");
            return false;
        },
        &error));
    QVERIFY(expected.sameContent(Revision::read(path)));
    QVERIFY(!atomicWriteStream(
        path, expected, [](QIODevice &file) { return file.write("local") == 5; }, &error,
        [&] {
            QFile external(path);
            QVERIFY(external.open(QIODevice::WriteOnly));
            external.write("external");
        }));
    QVERIFY(error.contains("conflict"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("external"));
}
QTEST_GUILESS_MAIN(StreamingTest)
