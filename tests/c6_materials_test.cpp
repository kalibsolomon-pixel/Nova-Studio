// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6_materials_test.h"
#include "cube.h"
#include "material_playback.h"
#include "minecraft_animation.h"
#include "modelpanel.h"
#include "settings.h"
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
using namespace nova;
namespace {
std::shared_ptr<Document> animation(const QList<QColor> &colors, int ticks,
                                    bool interpolate = false) {
    QImage sheet(16, 16 * colors.size(), QImage::Format_RGBA8888);
    for (int i = 0; i < colors.size(); ++i)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                sheet.setPixelColor(x, y + i * 16, colors[i]);
    auto doc = std::make_shared<Document>("material.png", sheet);
    QString error;
    if (!doc->importAnimation(
            sheet, {{"animation", QJsonObject{{"frametime", ticks}, {"interpolate", interpolate}}}},
            "26.1", &error))
        qFatal("%s", qPrintable(error));
    return doc;
}
} // namespace
void MaterialsTest::independentTimingAndLifecycle() {
    auto a = animation({Qt::red, Qt::blue}, 2),
         b = animation({Qt::yellow, Qt::green, Qt::magenta}, 3);
    MaterialPlayback playback;
    playback.setMaterials({{"a", a}, {"b", b}});
    playback.setVisible(true);
    playback.play();
    QVERIFY(playback.timerActive());
    playback.pause();
    QVERIFY(!playback.timerActive());
    playback.seek(200);
    QCOMPARE(playback.image("a").pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(playback.image("b").pixelColor(0, 0), QColor(Qt::green));
    playback.play();
    playback.setVisible(false);
    QVERIFY(playback.playing());
    QVERIFY(!playback.timerActive());
    const auto ms = playback.positionMilliseconds();
    QTest::qWait(60);
    QCOMPARE(playback.positionMilliseconds(), ms);
    playback.setVisible(true);
    QVERIFY(playback.timerActive());
    playback.stop();
    QVERIFY(!playback.timerActive());
    playback.setMaterials({{"a", a}});
    QCOMPARE(playback.count(), 1);
    playback.play();
    playback.setMaterials({});
    QVERIFY(!playback.timerActive());
    QCOMPARE(playback.count(), 0);
    auto c = animation({Qt::red, Qt::blue}, 2, true);
    playback.setMaterials({{"c", c}});
    playback.seek(50);
    QCOMPARE(playback.image("c"),
             interpolateFrames(c->frameComposite(c->animationState().entries[0].frame),
                               c->frameComposite(c->animationState().entries[1].frame), .5));
}
void MaterialsTest::gpuReuseAndCleanup() {
    Metrics metrics;
    {
        Cube cube(&metrics);
        cube.resize(320, 280);
        mc::Model model;
        model.valid = true;
        model.version = "26.1";
        model.resolved =
            QJsonDocument::fromJson(
                R"({"elements":[{"from":[0,0,0],"to":[16,16,16],"faces":{"south":{"texture":"nova:a"},"east":{"texture":"nova:b"}}}]})")
                .object();
        auto geometry = std::make_shared<mc::Mesh>(mc::mesh(model));
        QImage red(16, 16, QImage::Format_RGBA8888), blue = red;
        red.fill(Qt::red);
        blue.fill(Qt::blue);
        cube.setModel(geometry, {},
                      {{"assets/nova/textures/a.png", red}, {"assets/nova/textures/b.png", blue}});
        cube.show();
        QTest::qWait(80);
        auto before = cube.grabFramebuffer();
        auto uploads = metrics.counters["model_geometry_uploads"].toInteger(),
             allocations = metrics.counters["material_texture_allocations"].toInteger();
        cube.setMaterialImage("assets/nova/textures/a.png", blue);
        QTest::qWait(20);
        QVERIFY(before != cube.grabFramebuffer());
        QCOMPARE(metrics.counters["model_geometry_uploads"].toInteger(), uploads);
        QCOMPARE(metrics.counters["material_texture_allocations"].toInteger(), allocations);
        cube.setModel(std::make_shared<mc::Mesh>(*geometry), {},
                      {{"assets/nova/textures/a.png", red}});
        cube.grabFramebuffer();
        QVERIFY(metrics.counters["material_texture_releases"].toInteger() > 0);
    }
    QCOMPARE(metrics.counters["material_texture_allocations"].toInteger(),
             metrics.counters["material_texture_releases"].toInteger());
}
void MaterialsTest::projectState() {
    QTemporaryDir root, other;
    Settings settings(root.filePath("prefs.ini"));
    Metrics metrics;
    Cube cube(&metrics);
    cube.restoreView({{"pitch", 70},
                      {"yaw", 125},
                      {"scale", 2},
                      {"wireframe", true},
                      {"perspective", true},
                      {"pan", QJsonArray{1, 2}}});
    auto state = QJsonObject{
        {"target", "model:nova:a"}, {"context", "gui"}, {"camera", cube.viewSettings()}};
    QVERIFY(settings.saveProjectPreview(root.path(), state));
    QCOMPARE(settings.projectPreview(root.path()), state);
    QVERIFY(settings.projectPreview(other.path()).isEmpty());
    Cube restored(&metrics);
    restored.restoreView(settings.projectPreview(root.path())["camera"].toObject());
    QCOMPARE(restored.viewSettings()["yaw"].toInt(), 125);
    QVERIFY(restored.wireframe && restored.perspective);
    restored.restoreView({{"pitch", 999999}, {"scale", 0}, {"light", QJsonArray{1e300, 0, 0}}});
    QCOMPARE(restored.viewSettings()["pitch"].toInt(), 89);
    QVERIFY(std::isfinite(restored.lightDirection.x()));
}
void MaterialsTest::editableCatalogPriority() {
    QTemporaryDir pack, fallback;
    QDir().mkpath(pack.path() + "/assets/nova/models");
    QDir().mkpath(pack.path() + "/assets/nova/items");
    QDir().mkpath(fallback.path() + "/assets/minecraft/models");
    auto put = [](const QString &path, const QByteArray &bytes) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        return f.write(bytes) == bytes.size();
    };
    QVERIFY(put(pack.path() + "/assets/nova/models/editable.json", R"({"elements":[]})"));
    QVERIFY(put(pack.path() + "/assets/nova/items/editable.json",
                R"({"model":{"type":"minecraft:model","model":"nova:editable"}})"));
    for (int i = 0; i < 2100; ++i)
        QVERIFY(put(fallback.path() + QString("/assets/minecraft/models/fallback_%1.json").arg(i),
                    "{}"));
    auto library = std::make_shared<mc::Library>();
    library->update(pack.path(), Index::scan(pack.path()));
    QString error;
    library->setFallbacks({mc::ResourceSource::open(fallback.path(), "26.1", &error)});
    QVERIFY(error.isEmpty());
    const auto settingsFile = pack.filePath("prefs.ini");
    qApp->setProperty("settingsFile", settingsFile);
    QSettings settings(settingsFile, QSettings::IniFormat);
    const auto key = QString::fromLatin1(
        QCryptographicHash::hash(QFileInfo(pack.path()).canonicalFilePath().toCaseFolded().toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
    settings.setValue("modelResources/" + key + "/26.1", QStringList{fallback.path()});
    settings.sync();
    Metrics metrics;
    Cube cube(&metrics);
    DocumentSession session;
    ModelPanel panel(&cube, &session);
    panel.setLibrary(library, pack.path());
    auto targets = panel.findChild<QComboBox *>("modelTarget");
    QVERIFY(targets->findData("model:nova:editable") > 0);
    QVERIFY(targets->findData("definition:assets/nova/items/editable.json") > 0);
    QVERIFY(targets->count() > 1000);
    QVERIFY(targets->count() < 4096);
}
void MaterialsTest::liveMaterialSwitchAndClose() {
    QTemporaryDir root;
    QDir().mkpath(root.path() + "/assets/nova/models");
    QDir().mkpath(root.path() + "/assets/nova/textures");
    QImage red(16, 16, QImage::Format_RGBA8888), blue = red, green = red;
    red.fill(Qt::red);
    blue.fill(Qt::blue);
    green.fill(Qt::green);
    QVERIFY(red.save(root.path() + "/assets/nova/textures/a.png"));
    QVERIFY(blue.save(root.path() + "/assets/nova/textures/b.png"));
    QFile model(root.path() + "/assets/nova/models/dual.json");
    QVERIFY(model.open(QIODevice::WriteOnly));
    model.write(
        R"({"elements":[{"from":[0,0,0],"to":[16,16,16],"faces":{"south":{"texture":"nova:a"},"east":{"texture":"nova:b"}}}]})");
    model.close();
    DocumentSession session;
    session.setProject(root.path());
    auto a = session.open(root.path() + "/assets/nova/textures/a.png"),
         b = session.open(root.path() + "/assets/nova/textures/b.png");
    QVERIFY(a && b);
    Metrics metrics;
    Cube cube(&metrics);
    Canvas canvasA(a, &metrics), canvasB(b, &metrics);
    ModelPanel panel(&cube, &session);
    auto library = std::make_shared<mc::Library>();
    library->update(root.path(), Index::scan(root.path()));
    panel.setLibrary(library, root.path());
    cube.setDocument(a);
    panel.setDocument(a, &canvasA);
    panel.openAsset("assets/nova/models/dual.json");
    cube.resize(320, 280);
    cube.show();
    QTest::qWait(60);
    const auto original = cube.grabFramebuffer();
    QVERIFY(session.replacePixels(a, a->activeLayer(), green, "Edit A"));
    QTest::qWait(20);
    const auto edited = cube.grabFramebuffer();
    QVERIFY(edited != original);
    cube.setDocument(b);
    panel.setDocument(b, &canvasB);
    QTest::qWait(20);
    QCOMPARE(cube.grabFramebuffer(), edited);
    session.close(a);
    QTest::qWait(20);
    QCOMPARE(cube.grabFramebuffer(), original);
}
void MaterialsTest::tallAnimatedMaterialSheet() {
    QTemporaryDir root;
    QDir().mkpath(root.path() + "/assets/nova/models");
    QDir().mkpath(root.path() + "/assets/nova/textures");
    QImage sheet(16, 5008, QImage::Format_RGBA8888);
    sheet.fill(Qt::cyan);
    QVERIFY(sheet.save(root.path() + "/assets/nova/textures/long.png"));
    QFile metadata(root.path() + "/assets/nova/textures/long.png.mcmeta");
    QVERIFY(metadata.open(QIODevice::WriteOnly));
    metadata.write(R"({"animation":{"frametime":2}})");
    metadata.close();
    QFile model(root.path() + "/assets/nova/models/long.json");
    QVERIFY(model.open(QIODevice::WriteOnly));
    model.write(
        R"({"elements":[{"from":[0,0,0],"to":[16,16,16],"faces":{"south":{"texture":"nova:long"}}}]})");
    model.close();
    auto library = std::make_shared<mc::Library>();
    library->update(root.path(), Index::scan(root.path()));
    Metrics metrics;
    Cube cube(&metrics);
    DocumentSession session;
    ModelPanel panel(&cube, &session);
    panel.setLibrary(library, root.path());
    panel.openAsset("assets/nova/models/long.json");
    auto playback = panel.findChild<MaterialPlayback *>();
    QVERIFY(playback);
    QCOMPARE(playback->count(), 1);
    playback->setVisible(true);
    playback->seek(0);
    QCOMPARE(playback->image("assets/nova/textures/long.png").size(), QSize(16, 16));
}
void MaterialsTest::shadowedResourceInspection() {
    QTemporaryDir pack, fallback;
    const QString base = "assets/nova/models/base.json";
    auto put = [](const QString &path, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
    };
    QVERIFY(put(pack.path() + '/' + base,
                R"({"elements":[],"textures":{"all":"nova:face"},"credit":"editable winner"})"));
    QVERIFY(put(fallback.path() + '/' + base,
                R"({"elements":[],"textures":{"all":"nova:face"},"credit":"shadowed contents"})"));
    const QString texture = "assets/nova/textures/face.png";
    QImage red(16, 16, QImage::Format_RGBA8888), blue = red;
    red.fill(Qt::red);
    blue.fill(Qt::blue);
    QDir().mkpath(pack.path() + "/assets/nova/textures");
    QDir().mkpath(fallback.path() + "/assets/nova/textures");
    QVERIFY(red.save(pack.path() + '/' + texture));
    QVERIFY(blue.save(fallback.path() + '/' + texture));
    QVERIFY(put(pack.path() + "/assets/nova/models/child.json", R"({"parent":"nova:base"})"));
    const auto settingsFile = pack.filePath("prefs.ini");
    qApp->setProperty("settingsFile", settingsFile);
    QSettings settings(settingsFile, QSettings::IniFormat);
    const auto key = QString::fromLatin1(
        QCryptographicHash::hash(QFileInfo(pack.path()).canonicalFilePath().toCaseFolded().toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
    settings.setValue("modelResources/" + key + "/26.1", QStringList{fallback.path()});
    settings.sync();
    auto library = std::make_shared<mc::Library>();
    library->update(pack.path(), Index::scan(pack.path()));
    Metrics metrics;
    Cube cube(&metrics);
    DocumentSession session;
    ModelPanel panel(&cube, &session);
    panel.setLibrary(library, pack.path());
    panel.openAsset("assets/nova/models/child.json");
    auto dependencies = panel.findChild<QListWidget *>("modelDependencies");
    QVERIFY(dependencies);
    QListWidgetItem *item = nullptr;
    for (int i = 0; i < dependencies->count(); ++i)
        if (dependencies->item(i)->data(Qt::UserRole).toString() == base)
            item = dependencies->item(i);
    QVERIFY(item);
    bool inspected = false;
    QTimer::singleShot(0, &panel, [&] {
        auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        auto choice = dialog->findChild<QComboBox *>("resourceProvider");
        auto contents = dialog->findChild<QPlainTextEdit *>("resourceContents");
        if (choice && contents && choice->count() == 2 && contents->isReadOnly() &&
            contents->toPlainText().contains("editable winner")) {
            choice->setCurrentIndex(1);
            inspected = contents->toPlainText().contains("shadowed contents") &&
                        choice->itemText(1).contains("shadowed");
            for (auto button : dialog->findChildren<QPushButton *>())
                if (button->text() == "Import override")
                    inspected = inspected && !button->isEnabled();
        }
        dialog->reject();
    });
    dependencies->itemDoubleClicked(item);
    QVERIFY(inspected);
    QCOMPARE(library->providers(base).size(), 2);
    QVERIFY(library->editable(base));
    QFile original(pack.path() + '/' + base);
    QVERIFY(original.open(QIODevice::ReadOnly));
    QVERIFY(original.readAll().contains("editable winner"));
    QListWidgetItem *textureItem = nullptr;
    for (int i = 0; i < dependencies->count(); ++i)
        if (dependencies->item(i)->data(Qt::UserRole).toString() == texture)
            textureItem = dependencies->item(i);
    QVERIFY(textureItem);
    bool imageInspected = false;
    QTimer::singleShot(0, &panel, [&] {
        auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        auto choice = dialog->findChild<QComboBox *>("resourceProvider");
        auto picture = dialog->findChild<QLabel *>("resourceImage");
        if (choice && picture && choice->count() == 2 &&
            picture->pixmap().toImage().pixelColor(0, 0) == QColor(Qt::red)) {
            choice->setCurrentIndex(1);
            imageInspected = picture->pixmap().toImage().pixelColor(0, 0) == QColor(Qt::blue);
        }
        dialog->reject();
    });
    dependencies->itemDoubleClicked(textureItem);
    QVERIFY(imageInspected);
}
QTEST_MAIN(MaterialsTest)
