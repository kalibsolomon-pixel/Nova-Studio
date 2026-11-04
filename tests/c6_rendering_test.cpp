// SPDX-License-Identifier: GPL-3.0-or-later
#include "models.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace nova;
class RenderingTest : public QObject {
    Q_OBJECT
  private slots:
    void transformationCodec() {
        QMatrix4x4 m;
        QString error;
        QVERIFY(mc::itemTransform(
            QJsonDocument::fromJson(
                R"({"translation":[1,2,3],"scale":[2,3,4],"left_rotation":[0,0,0,1]})")
                .object(),
            m, &error));
        QCOMPARE(m.map(QVector3D(1, 1, 1)), QVector3D(3, 5, 7));
        QVERIFY(mc::itemTransform(QJsonArray{1, 0, 0, 1, 0, 1, 0, 2, 0, 0, 1, 3, 0, 0, 0, 1}, m));
        QCOMPARE(m.map(QVector3D()), QVector3D(1, 2, 3));
        QVERIFY(!mc::itemTransform(QJsonArray{1, 2}, m, &error));
    }
    void subtreeAndTint() {
        QTemporaryDir root;
        QDir().mkpath(root.path() + "/assets/nova/items");
        QFile f(root.path() + "/assets/nova/items/a.json");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(
            R"({"model":{"type":"minecraft:composite","transformation":{"translation":[1,0,0]},"models":[{"type":"minecraft:model","model":"nova:a","transformation":{"scale":[2,2,2]},"tints":[{"type":"minecraft:constant","value":16711680},{"type":"minecraft:grass"}]}]}})");
        f.close();
        mc::Library lib;
        lib.update(root.path(), Index::scan(root.path()));
        auto d = lib.dispatch("assets/nova/items/a.json", {{"_tints", QJsonArray{0, 65280}}});
        QCOMPARE(d.instances.size(), 1);
        QCOMPARE(d.instances[0].transformation.map(QVector3D(1, 0, 0)), QVector3D(3, 0, 0));
        QCOMPARE(d.instances[0].tints[0], qRgb(255, 0, 0));
        QCOMPARE(d.instances[0].tints[1], qRgb(0, 255, 0));
        QVERIFY(d.diagnostics.join(' ').contains("Simulated"));
        lib.setVersion("1.21.11");
        QVERIFY(lib.dispatch("assets/nova/items/a.json").instances.isEmpty());
    }
    void contours() {
        mc::Model model;
        model.valid = true;
        model.generated = true;
        model.version = "26.1";
        model.id = "nova:generated";
        model.textures["layer0"] = "assets/nova/textures/a.png";
        model.resolved["textures"] = QJsonObject{{"layer0", "nova:a"}};
        QImage image(3, 3, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        auto geometry = mc::generatedMesh(model, {{model.textures["layer0"], image}});
        QCOMPARE(geometry.faces.size(), 6);
        QCOMPARE(geometry.minimum.z(), -.0625f);
        QCOMPARE(geometry.maximum.z(), .0625f);
        image.setPixelColor(1, 1, Qt::transparent);
        geometry = mc::generatedMesh(model, {{model.textures["layer0"], image}});
        QCOMPARE(geometry.faces.size(), 10);
        for (const auto &face : geometry.faces) {
            QVERIFY(face.normal.length() > .99f);
            QCOMPARE(face.elementIndex, -1);
        }
        QMatrix4x4 mirror;
        mirror.scale(-1, 2, 1);
        auto reflected = mc::applyTransform(geometry, mirror);
        QCOMPARE(reflected.faces.size(), 10);
        QVERIFY(reflected.faces[0].normal.length() > .99f);
    }
};
QTEST_GUILESS_MAIN(RenderingTest)
#include "c6_rendering_test.moc"
