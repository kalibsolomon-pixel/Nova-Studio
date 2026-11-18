// SPDX-License-Identifier: GPL-3.0-or-later
// Explorer model-icon invalidation: editing one texture rebuilds only the icons whose
// recipe depends on it (the models using it and the definitions dispatching them), a
// whole-library change (resource-stack reload) still rebuilds everything, and rebuilt
// icons keep exactly the same pixels as the originals.
#include "assets.h"
#include "models.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

using namespace nova;

class ExplorerIconTests : public QObject {
    Q_OBJECT
    QTemporaryDir tmp;
    QString pack;
    QStringList rows;
    QModelIndex row(const AssetModel &model, const QString &path) const {
        return model.indexFor(path);
    }
    QImage iconImage(const AssetModel &model, const QString &path) const {
        // Explicit device pixel ratio and a canonical format: the screen's ratio and
        // the icon's internal representation would otherwise vary between two
        // otherwise identical icons.
        return qvariant_cast<QIcon>(model.data(model.indexFor(path), Qt::DecorationRole))
            .pixmap(QSize(64, 64), 1.0)
            .toImage()
            .convertToFormat(QImage::Format_ARGB32);
    }
    // Drive the decoration role like the visible Explorer page until every started
    // thumbnail settled; false on timeout.
    bool settle(AssetModel &model, int timeoutMs = 30000) {
        int stable = -1;
        QElapsedTimer guard;
        guard.start();
        while (guard.elapsed() < timeoutMs) {
            for (const QString &path : rows)
                model.data(model.indexFor(path), Qt::DecorationRole);
            QApplication::processEvents();
            QApplication::processEvents(QEventLoop::WaitForMoreEvents, 5);
            if (model.pendingCount() == 0 && stable == model.iconStats().captured)
                return true;
            stable = model.iconStats().captured;
        }
        return model.pendingCount() == 0;
    }
    void write(const QString &path, const QJsonObject &value) const {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(value).toJson(QJsonDocument::Compact));
    }
    void writeTexture(const QString &path, const QColor &color) const {
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(color);
        QVERIFY(image.save(path));
    }
    QJsonObject cubeModel(const QString &texture) const {
        QJsonObject faces;
        for (const char *side : {"north", "south", "east", "west", "up", "down"})
            faces[side] = QJsonObject{{"texture", "#layer0"}, {"uv", QJsonArray{0, 0, 16, 16}}};
        return QJsonObject{{"textures", QJsonObject{{"layer0", texture}}},
                           {"elements", QJsonArray{QJsonObject{
                                            {"from", QJsonArray{0, 0, 0}},
                                            {"to", QJsonArray{16, 16, 16}},
                                            {"faces", faces}}}}};
    }

  private slots:
    void init() {
        pack = tmp.filePath("pack");
        const QString models = pack + "/assets/nova/models/item";
        const QString items = pack + "/assets/nova/items";
        const QString textures = pack + "/assets/nova/textures/item";
        for (const QString &dir : {models, items, textures})
            QVERIFY(QDir().mkpath(dir));
        writeTexture(textures + "/ta.png", QColor(Qt::red));
        writeTexture(textures + "/tb.png", QColor(Qt::blue));
        for (const char *name : {"ma1", "ma2", "ma3"})
            write(models + '/' + name + ".json", cubeModel("nova:item/ta"));
        for (const char *name : {"mb1", "mb2"})
            write(models + '/' + name + ".json", cubeModel("nova:item/tb"));
        write(items + "/holder.json",
              QJsonObject{{"model", QJsonObject{{"type", "minecraft:model"},
                                                {"model", "nova:item/ma1"}}}});
        rows = QStringList{"assets/nova/models/item/ma1.json", "assets/nova/models/item/ma2.json",
                           "assets/nova/models/item/ma3.json", "assets/nova/models/item/mb1.json",
                           "assets/nova/models/item/mb2.json", "assets/nova/items/holder.json"};
    }
    void textureEditRebuildsOnlyDependents() {
        const auto index = Index::scan(pack);
        auto catalog = std::make_shared<mc::Library>("1.21.11");
        catalog->update(pack, index);
        AssetModel explorer;
        explorer.setIndex(index, pack);
        explorer.setModelSources(catalog);
        QVERIFY(settle(explorer));
        QCOMPARE(explorer.iconStats().captured, 6);
        const QImage beforeMa1 = iconImage(explorer, "assets/nova/models/item/ma1.json");
        const QImage beforeMb1 = iconImage(explorer, "assets/nova/models/item/mb1.json");
        QVERIFY(!beforeMa1.isNull());

        // One texture edit used by ma1..ma3; the holder definition dispatches ma1.
        writeTexture(pack + "/assets/nova/textures/item/ta.png", QColor(Qt::green));
        catalog->update(pack, Index::refresh(pack, index, {"assets/nova/textures/item/ta.png"}),
                        {"assets/nova/textures/item/ta.png"});
        explorer.resetIconStats();
        explorer.setModelSources(catalog);
        QCOMPARE(explorer.iconStats().invalidated, 4);
        QVERIFY(settle(explorer));
        QCOMPARE(explorer.iconStats().captured, 4);
        QCOMPARE(explorer.pendingCount(), 0);
        // Dependent icons reflect the edit; unrelated icons were never rebuilt.
        QVERIFY(iconImage(explorer, "assets/nova/models/item/ma1.json") != beforeMa1);
        QVERIFY(iconImage(explorer, "assets/nova/models/item/mb1.json") == beforeMb1);
        QCOMPARE(iconImage(explorer, "assets/nova/models/item/ma1.json"),
                 iconImage(explorer, "assets/nova/models/item/ma2.json"));
    }
    void previewEditScopesToTheEditedAsset() {
        const auto index = Index::scan(pack);
        auto catalog = std::make_shared<mc::Library>("1.21.11");
        catalog->update(pack, index);
        AssetModel explorer;
        explorer.setIndex(index, pack);
        explorer.setModelSources(catalog);
        QVERIFY(settle(explorer));
        QCOMPARE(explorer.iconStats().captured, 6);

        // A scoped update first, as the indexing worker delivers on every save; the
        // two tb users rebuild, everything else stays cached.
        catalog->update(pack, index, {"assets/nova/textures/item/tb.png"});
        explorer.resetIconStats();
        explorer.setModelSources(catalog);
        QCOMPARE(explorer.iconStats().invalidated, 2);
        QVERIFY(settle(explorer));
        QCOMPARE(explorer.iconStats().captured, 2);
        const QImage settledMb2 = iconImage(explorer, "assets/nova/models/item/mb2.json");

        // An unsaved preview source change is recorded in the change scope. The scope
        // accumulates until the next whole-library change, so mb1 drops together with
        // tb's other user (rebuilt to identical pixels); nothing else is touched.
        auto source = catalog->source("assets/nova/models/item/mb1.json");
        source["textures"] = QJsonObject{{"layer0", "nova:item/ta"}};
        catalog->previewSource("assets/nova/models/item/mb1.json", source);
        explorer.resetIconStats();
        explorer.setModelSources(catalog);
        QCOMPARE(explorer.iconStats().invalidated, 2);
        QVERIFY(settle(explorer));
        QCOMPARE(explorer.iconStats().captured, 2);
        // mb1 now resolves ta, so its icon matches the ta users; mb2 just re-cached.
        QCOMPARE(iconImage(explorer, "assets/nova/models/item/mb1.json"),
                 iconImage(explorer, "assets/nova/models/item/ma1.json"));
        QCOMPARE(iconImage(explorer, "assets/nova/models/item/mb2.json"), settledMb2);
    }
    void resourceStackChangeRebuildsAll() {
        const auto index = Index::scan(pack);
        auto catalog = std::make_shared<mc::Library>("1.21.11");
        catalog->update(pack, index);
        AssetModel explorer;
        explorer.setIndex(index, pack);
        explorer.setModelSources(catalog);
        QVERIFY(settle(explorer));
        QCOMPARE(explorer.iconStats().captured, 6);
        QList<QImage> before;
        for (const QString &path : rows)
            before << iconImage(explorer, path);

        // A resource-stack reload re-reads everything and flushes every model icon.
        auto reloaded = std::make_shared<mc::Library>(*catalog);
        reloaded->setFallbacks({}, "1.21.11");
        explorer.resetIconStats();
        explorer.setModelSources(reloaded);
        QCOMPARE(explorer.iconStats().invalidated, 6);
        QVERIFY(settle(explorer));
        QCOMPARE(explorer.iconStats().captured, 6);
        for (int i = 0; i < rows.size(); ++i) {
            QVERIFY(!before[i].isNull());
            QCOMPARE(iconImage(explorer, rows[i]), before[i]); // identical pixels
        }
    }
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication application(argc, argv);
    application.setStyle("Fusion");
    ExplorerIconTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "explorer_icon_test.moc"
