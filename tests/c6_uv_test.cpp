// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6_uv_test.h"
#include "edit.h"
#include "modelpanel.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>
using namespace nova;
QString UvTest::fixture(const QString &root) {
    auto path = "assets/nova/models/a.json";
    QDir().mkpath(root + "/assets/nova/models");
    QFile file(root + '/' + path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(
        R"({"credit":"original","unknown":{"keep":[1,2,3]},"textures":{"all":"nova:a"},"elements":[{"from":[0,0,0],"to":[16,16,16],"unknown_element":true,"faces":{"south":{"texture":"#all","custom_face":42}}}]})");
    return path;
}
void UvTest::guardedHistoryAndPreservation() {
    QTemporaryDir root;
    const auto asset = fixture(root.path());
    mc::ModelEdit edit;
    QString error;
    QVERIFY(edit.open(root.path(), asset, &error));
    const auto original = edit.source();
    QVERIFY(edit.previewUv(0, "south", {1, 2, 12, 15}, 90, &error));
    QVERIFY(edit.previewing());
    QVERIFY(!edit.save(&error));
    edit.cancelPreview();
    QCOMPARE(edit.source(), original);
    QVERIFY(edit.previewUv(0, "south", {12, 2, 1, 15}, 270));
    QVERIFY(edit.commitPreview());
    QVERIFY(edit.modified());
    QVERIFY(edit.undo());
    QCOMPARE(edit.source(), original);
    QVERIFY(edit.redo());
    QVERIFY(edit.save(&error));
    QVERIFY(!edit.modified());
    mc::ModelEdit loaded;
    QVERIFY(loaded.open(root.path(), asset, &error));
    QCOMPARE(loaded.source()["unknown"], original["unknown"]);
    auto e = loaded.source()["elements"].toArray()[0].toObject();
    QVERIFY(e["unknown_element"].toBool());
    QCOMPARE(e["faces"].toObject()["south"].toObject()["custom_face"].toInt(), 42);
    QVERIFY(loaded.resetUv(0, "south"));
    loaded.commitPreview();
    QVERIFY(!loaded.source()["elements"]
                 .toArray()[0]
                 .toObject()["faces"]
                 .toObject()["south"]
                 .toObject()
                 .contains("uv"));
    QFile external(root.path() + '/' + asset);
    QVERIFY(external.open(QIODevice::Append));
    external.write(" ");
    external.close();
    QVERIFY(!loaded.save(&error));
    QVERIFY(error.contains("conflict"));
    QVERIFY(!loaded.previewUv(4, "south", {0, 0, 16, 16}, 0));
    QVERIFY(!loaded.previewUv(0, "south", {0, 0, 16, 16}, 45));
}
void UvTest::inheritedConversion() {
    QTemporaryDir root;
    const auto asset = fixture(root.path());
    QFile f(root.path() + '/' + asset);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(R"({"parent":"nova:parent","extension":"keep"})");
    f.close();
    mc::ModelEdit edit;
    QVERIFY(edit.open(root.path(), asset));
    QVERIFY(!edit.hasLocalGeometry());
    QVERIFY(!edit.previewUv(0, "south", {0, 0, 8, 8}, 0));
    auto elements =
        QJsonDocument::fromJson(
            R"({"elements":[{"from":[0,0,0],"to":[16,16,16],"faces":{"south":{"texture":"nova:a"}}}]})")
            .object()["elements"]
            .toArray();
    QVERIFY(edit.localGeometry(elements));
    QVERIFY(edit.hasLocalGeometry());
    QCOMPARE(edit.source()["extension"].toString(), QString("keep"));
    QVERIFY(edit.undo());
    QVERIFY(!edit.hasLocalGeometry());
}
void UvTest::interactivePixelCoordinates() {
    QTemporaryDir root;
    const auto asset = fixture(root.path());
    QDir().mkpath(root.path() + "/assets/nova/textures");
    QImage image(32, 16, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    QVERIFY(image.save(root.path() + "/assets/nova/textures/a.png"));
    DocumentSession session;
    session.setProject(root.path());
    QString error;
    auto doc = session.open(root.path() + "/assets/nova/textures/a.png", &error);
    QVERIFY(doc);
    Metrics metrics;
    Canvas canvas(doc, &metrics);
    canvas.session = &session;
    canvas.resize(1100, 700);
    canvas.zoom = 24;
    canvas.show();
    Cube cube(&metrics);
    ModelPanel panel(&cube, &session);
    auto lib = std::make_shared<mc::Library>();
    lib->update(root.path(), Index::scan(root.path()));
    panel.setLibrary(lib, root.path());
    panel.setDocument(doc, &canvas);
    panel.openAsset(asset);
    panel.selectFace(0);
    panel.findChild<QPushButton *>("uvBegin")->click();
    QVERIFY(canvas.uvEditing);
    const auto p = canvas.widgetPosition({8, 8});
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, p.toPoint());
    QTest::mouseMove(&canvas, (p + QPointF(48, 24)).toPoint());
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, (p + QPointF(48, 24)).toPoint());
    QCOMPARE(panel.findChild<QDoubleSpinBox *>("uvU1")->value(), 1.);
    QCOMPARE(panel.findChild<QDoubleSpinBox *>("uvV1")->value(), 1.);
    panel.findChild<QComboBox *>("uvRotation")->setCurrentIndex(1);
    panel.findChild<QPushButton *>("uvCommit")->click();
    QVERIFY(!canvas.uvEditing);
    panel.findChild<QPushButton *>("uvUndo")->click();
    QCOMPARE(panel.findChild<QDoubleSpinBox *>("uvU1")->value(), 0.);
    panel.findChild<QPushButton *>("uvRedo")->click();
    panel.findChild<QPushButton *>("uvSave")->click();
    mc::ModelEdit loaded;
    QVERIFY(loaded.open(root.path(), asset));
    QCOMPARE(loaded.source()["elements"]
                 .toArray()[0]
                 .toObject()["faces"]
                 .toObject()["south"]
                 .toObject()["rotation"]
                 .toInt(),
             90);
}
QTEST_MAIN(UvTest)
