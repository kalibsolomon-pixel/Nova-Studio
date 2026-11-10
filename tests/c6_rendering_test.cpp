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
    void generatedLayerGeometryAndLimits() {
        mc::Model model;
        model.valid = model.generated = true;
        model.version = "26.1";
        model.id = "nova:item/layers";
        QJsonObject textures;
        QMap<QString, QImage> sprites;
        QImage image(8, 16, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        image.setPixelColor(3, 7, Qt::transparent); // interior hole, including non-square sprites
        for (int layer = 0; layer < 5; ++layer) {
            const auto key = "layer" + QString::number(layer);
            const auto id = "nova:item/layer" + QString::number(layer);
            const auto path = mc::Location::parse(id).asset("textures");
            textures[key] = id;
            model.textures[key] = path;
            sprites[path] = image;
        }
        model.resolved["textures"] = textures;
        const auto geometry = mc::generatedMesh(model, sprites);
        QVERIFY(geometry.previewLimitation.isEmpty());
        QCOMPARE(geometry.faces.size(), 50); // front/back, four outside edges and four hole walls per layer
        QCOMPARE(geometry.minimum, QVector3D(-1, -1, -.0625f));
        QCOMPARE(geometry.maximum, QVector3D(1, 1, .0625f));
        for (int i = 0; i < geometry.faces.size(); ++i) {
            const auto &face = geometry.faces[i];
            QCOMPARE(face.generatedLayer, i / 10);
            QCOMPARE(face.texture, model.textures["layer" + QString::number(i / 10)]);
            QCOMPARE(face.tint, face.generatedLayer);
        }
        QMatrix4x4 pose;
        pose.rotate(50, 0, 0, 1);
        pose.translate(.2f, .3f, .4f);
        const auto moved = mc::applyTransform(geometry, pose);
        for (int i = 0; i < moved.faces.size(); ++i)
            QCOMPARE(moved.faces[i].generatedLayer, geometry.faces[i].generatedLayer);

        auto unavailable = sprites;
        unavailable.remove(model.textures["layer1"]);
        const auto missing = mc::generatedMesh(model, unavailable);
        QVERIFY(missing.previewLimitation.contains(model.textures["layer1"]));
        QVERIFY(missing.previewLimitation.contains("unavailable"));
        QVERIFY(!missing.faces.isEmpty()); // retain the explicit flat approximation
        unavailable = sprites;
        unavailable[model.textures["layer0"]] = QImage(1025, 1, QImage::Format_RGBA8888);
        QVERIFY(mc::generatedMesh(model, unavailable).previewLimitation.contains("1024-pixel"));
        QImage busy(64, 64, QImage::Format_RGBA8888);
        busy.fill(Qt::transparent);
        for (int y = 0; y < busy.height(); ++y)
            for (int x = 0; x < busy.width(); ++x)
                if ((x + y) % 2 == 0)
                    busy.setPixelColor(x, y, Qt::white);
        unavailable[model.textures["layer0"]] = busy;
        QVERIFY(mc::generatedMesh(model, unavailable).previewLimitation.contains("4096-span"));
        model.valid = false;
        QVERIFY(mc::generatedMesh(model, {}).faces.isEmpty());
    }

};
QTEST_GUILESS_MAIN(RenderingTest)
#include "c6_rendering_test.moc"
