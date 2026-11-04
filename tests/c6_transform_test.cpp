// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6_transform_test.h"
#include "canvas.h"
#include "recovery.h"
#include "session.h"
#include "tools.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace nova;
namespace {
std::shared_ptr<Document> fixture(const QString &path) {
    QImage a(32, 16, QImage::Format_RGBA8888);
    a.fill(Qt::transparent);
    a.setPixelColor(3, 3, Qt::red);
    auto doc = std::make_shared<Document>(path, a);
    QImage b = a;
    b.fill(Qt::transparent);
    b.setPixelColor(9, 3, Qt::blue);
    doc->editLayer({LayerCommand::Add, doc->activeLayer(), "B", 0, b});
    return doc;
}
} // namespace
void GroupTransformTest::previewCommitHistoryAndRecovery() {
    QTemporaryDir root;
    auto doc = fixture(root.filePath("texture.png"));
    auto a = doc->layers()[0].id, b = doc->layers()[1].id;
    DocumentSession session;
    QVERIFY(session.adopt(doc));
    Metrics metrics;
    ToolState tools;
    tools.tool = Tool::Transform;
    Canvas canvas(doc, &metrics, nullptr, &tools);
    canvas.session = &session;
    const auto beforeA = doc->layer(a)->pixels, beforeB = doc->layer(b)->pixels;
    const auto cursor = doc->historyCursor();
    QVERIFY(canvas.beginLayerTransform({a, b}));
    canvas.moveFloating({2, 1});
    QCOMPARE(doc->layer(a)->pixels, beforeA);
    QCOMPARE(doc->layer(b)->pixels, beforeB);
    QVERIFY(canvas.displayed() != doc->image);
    canvas.cancelFloating();
    QCOMPARE(doc->historyCursor(), cursor);
    QVERIFY(canvas.beginLayerTransform({a, b}));
    canvas.moveFloating({2, 1});
    QVERIFY(canvas.commitFloating());
    QCOMPARE(doc->historyCursor(), cursor + 1);
    QCOMPARE(doc->layer(a)->pixels.pixelColor(5, 4), QColor(Qt::red));
    QCOMPARE(doc->layer(b)->pixels.pixelColor(11, 4), QColor(Qt::blue));
    doc->undo();
    QCOMPARE(doc->layer(a)->pixels, beforeA);
    QCOMPARE(doc->layer(b)->pixels, beforeB);
    doc->redo();
    QVERIFY(doc->saveNative(root.path()));
    Document loaded(doc->path, {});
    QVERIFY(loaded.loadNative(root.path()));
    QCOMPARE(loaded.layer(a)->pixels, doc->layer(a)->pixels);
    QCOMPARE(loaded.layer(b)->pixels, doc->layer(b)->pixels);
    RecoveryStore recovery(root.filePath("recovery"));
    QVERIFY(recovery.writeDocument(*doc, root.path()));
    const auto restored = recovery.load(doc->id());
    QVERIFY(restored.error.isEmpty());
    Document recovered(doc->path, {});
    QVERIFY(recovered.decodeNative(restored.nativeData, "recovery.png"));
    QCOMPARE(recovered.layer(b)->pixels, doc->layer(b)->pixels);
}
void GroupTransformTest::sharedPivotMaskAndHidden() {
    auto doc = fixture("texture.png");
    auto a = doc->layers()[0].id, b = doc->layers()[1].id;
    QImage hidden = doc->image;
    hidden.fill(Qt::yellow);
    doc->editLayer({LayerCommand::Add, b, "Hidden", 0, hidden});
    auto h = doc->activeLayer();
    doc->editLayer({LayerCommand::Visibility, h, {}, 0});
    Metrics metrics;
    ToolState tools;
    tools.tool = Tool::Transform;
    Canvas canvas(doc, &metrics, nullptr, &tools);
    const auto hiddenBefore = doc->layer(h)->pixels;
    QVERIFY(canvas.beginLayerTransform({a, h, b}));
    QCOMPARE(canvas.floatingState().layers.size(), 2);
    tools.transformFlipH = true;
    emit tools.transformChanged();
    QVERIFY(canvas.commitFloating());
    QCOMPARE(doc->layer(a)->pixels.pixelColor(9, 3), QColor(Qt::red));
    QCOMPARE(doc->layer(b)->pixels.pixelColor(3, 3), QColor(Qt::blue));
    QCOMPARE(doc->layer(h)->pixels, hiddenBefore);
    doc->undo();
    QVERIFY(canvas.beginLayerTransform({h, b}, true));
    QCOMPARE(canvas.floatingState().layers.size(), 2);
    canvas.moveFloating({1, 0});
    QVERIFY(canvas.commitFloating());
    QVERIFY(!doc->layer(h)->visible);
    QVERIFY(doc->layer(h)->pixels != hiddenBefore);
    doc->undo();
    QVERIFY(doc->setSelection(ops::rectMask(doc->image.size(), QRect(0, 0, 8, 8))));
    QVERIFY(canvas.beginLayerTransform({a, b}));
    canvas.moveFloating({1, 1});
    QVERIFY(canvas.commitFloating());
    QCOMPARE(doc->layer(b)->pixels.pixelColor(9, 3), QColor(Qt::blue));
    QCOMPARE(doc->layer(a)->pixels.pixelColor(4, 4), QColor(Qt::red));
}
void GroupTransformTest::budgetsAndStalePreview() {
    auto doc = fixture("texture.png");
    auto a = doc->layers()[0].id, b = doc->layers()[1].id;
    Metrics metrics;
    ToolState tools;
    tools.tool = Tool::Transform;
    Canvas canvas(doc, &metrics, nullptr, &tools);
    QString error;
    canvas.error = [&](const QString &value) { error = value; };
    QVERIFY(canvas.beginLayerTransform({a, b}));
    canvas.moveFloating({4, 0});
    const auto before = doc->image;
    doc->setHistoryLimits(128, 2048);
    QVERIFY(!canvas.commitFloating());
    QCOMPARE(doc->image, before);
    QVERIFY(error.contains("history budget"));
    canvas.cancelFloating();
    doc->setHistoryLimits(16 * 1024 * 1024, 2048);
    QVERIFY(canvas.beginLayerTransform({a, b}));
    doc->editLayer({LayerCommand::Rename, a, "Changed"});
    QVERIFY(!canvas.commitFloating());
    QVERIFY(error.contains("stale"));
    canvas.cancelFloating();
    QVERIFY(canvas.beginLayerTransform({a, b}));
    tools.scaleX = tools.scaleY = 1600;
    emit tools.transformChanged();
    QVERIFY(canvas.floatingState().valid);
    canvas.cancelFloating();
    QImage large(2048, 2048, QImage::Format_RGBA8888);
    large.fill(Qt::white);
    auto big = std::make_shared<Document>("large.png", large);
    big->editLayer({LayerCommand::Add, big->activeLayer(), "Second", 0, large});
    Canvas bounded(big, &metrics);
    QVERIFY(!bounded.beginLayerTransform({big->layers()[0].id, big->layers()[1].id}));
    QVERIFY(!bounded.hasFloating());
}
QTEST_MAIN(GroupTransformTest)
