// SPDX-License-Identifier: GPL-3.0-or-later
#include "models.h"
#include "vanilla/cache.h"
#include "vanilla/metadata.h"
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
using namespace nova;
#include "model_test.h"
void ModelTest::write(const QString &root, const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(root + '/' + path).absolutePath());
    QFile f(root + '/' + path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}
QByteArray ModelTest::box() {
    return R"({"textures":{"all":"demo:block/check"},"elements":[{"from":[0,0,0],"to":[16,16,16],"faces":{"north":{"texture":"#all"},"south":{"texture":"#all"},"east":{"texture":"#all"},"west":{"texture":"#all"},"up":{"texture":"#all"},"down":{"texture":"#all"}}}]})";
}
void ModelTest::locations() {
    QCOMPARE(mc::Location::parse("block/stone").id(), QString("minecraft:block/stone"));
    QCOMPARE(mc::Location::parse("mod:item/a").asset("textures"),
             QString("assets/mod/textures/item/a.png"));
    for (const auto &s : {"../x", "foo:../a", "Bad:a", "a:b:c", "a:/x", "a:x//y", "a:x\\y", "a:"})
        QVERIFY(!mc::Location::parse(s).valid());
}
void ModelTest::inheritanceAliasesGraphAndIncremental() {
    QTemporaryDir dir;
    const auto r = dir.path();
    write(r, "assets/demo/models/block/base.json", box());
    write(
        r, "assets/demo/models/block/child.json",
        R"({"parent":"demo:block/base","textures":{"other":"demo:block/new","all":"#other"},"display":{"gui":{"rotation":[0,90,0]}}})");
    write(r, "assets/demo/blockstates/check.json",
          R"({"variants":{"":{"model":"demo:block/child"}}})");
    mc::Library library;
    library.update(r, Index::scan(r));
    auto m = library.model("demo:block/child");
    QVERIFY(m.valid);
    QCOMPARE(m.parents.size(), 1);
    QCOMPARE(m.textures["all"], QString("assets/demo/textures/block/new.png"));
    QCOMPARE(mc::mesh(m).faces.size(), 6);
    QVERIFY(library.dependents("assets/demo/textures/block/new.png")
                .contains("assets/demo/blockstates/check.json"));
    QVERIFY(library.modelsForTexture("assets/demo/textures/block/new.png")
                .contains("demo:block/child"));
    const auto parses = library.parsedFiles;
    library.update(r, Index::scan(r));
    QCOMPARE(library.parsedFiles, parses);
    write(r, "assets/demo/models/block/base.json", R"({"elements":[]})");
    library.update(r, Index::scan(r), {"assets/demo/models/block/base.json"});
    QCOMPARE(library.parsedFiles, parses + 1);
    QCOMPARE(mc::mesh(library.model("demo:block/child")).faces.size(), 0);
}
void ModelTest::failuresAndProfiles() {
    QTemporaryDir dir;
    const auto r = dir.path();
    write(r, "assets/demo/models/a.json", R"({"parent":"demo:b"})");
    write(r, "assets/demo/models/b.json", R"({"parent":"demo:a"})");
    write(r, "assets/demo/models/t.json", R"({"textures":{"x":"#y","y":"#x","z":"#missing"}})");
    write(r, "assets/demo/models/o.json",
          R"({"textures":{"x":{"sprite":"demo:t","force_translucent":true}}})");
    write(r, "assets/demo/models/bad.json", "{invalid");
    write(r, "assets/demo/models/huge.json", QByteArray(2 * 1024 * 1024 + 1, ' '));
    write(r, "assets/demo/models/deep.json", QByteArray(70, '[') + QByteArray(70, ']'));
    mc::Library library;
    library.update(r, Index::scan(r));
    QVERIFY(!library.model("demo:a").valid);
    QVERIFY(library.model("demo:a").diagnostics.join(' ').contains("Cyclic"));
    QVERIFY(library.model("demo:t").diagnostics.join(' ').contains("Cyclic texture"));
    QVERIFY(!library.model("demo:bad").valid);
    QVERIFY(!library.model("demo:huge").valid);
    QVERIFY(!library.model("demo:deep").valid);
    QCOMPARE(library.model("demo:o").textures.size(), 1);
    library.setVersion("1.21.11");
    QVERIFY(library.model("demo:o").textures.isEmpty());
    library.setVersion("unknown");
    QVERIFY(!library.model("demo:t").valid);
}
void ModelTest::geometryAndUvs() {
    QTemporaryDir dir;
    write(dir.path(), "assets/demo/models/a.json", box());
    mc::Library library;
    library.update(dir.path(), Index::scan(dir.path()));
    auto m = library.model("demo:a");
    auto mesh = mc::mesh(m);
    QCOMPARE(mesh.faces.size(), 6);
    QCOMPARE(mesh.minimum, QVector3D(-1, -1, -1));
    QCOMPARE(mesh.maximum, QVector3D(1, 1, 1));
    for (const auto &f : mesh.faces) {
        QCOMPARE(f.uvRect(), QRectF(0, 0, 16, 16));
        QVERIFY(QVector3D::dotProduct(f.normal, f.points[0]) > .99f);
    }
    m.resolved =
        QJsonDocument::fromJson(
            R"({"elements":[{"from":[0,0,0],"to":[16,8,16],"rotation":{"origin":[8,8,8],"x":90,"z":90},"faces":{"south":{"texture":"demo:t","uv":[16,0,0,8],"rotation":90}}}]})")
            .object();
    mesh = mc::mesh(m);
    QCOMPARE(mesh.faces.size(), 1);
    const auto &f = mesh.faces[0];
    QCOMPARE(f.uv[0], QVector2D(1, .5));
    QCOMPARE(f.uvRect(), QRectF(0, 0, 16, 8));
    QVERIFY(std::abs(f.normal.x() - 1) < .001f); // X rotates +Z toward -Y, then Z to +X.
}
void ModelTest::blockstatesAndItems() {
    QTemporaryDir dir;
    const auto r = dir.path();
    write(
        r, "assets/demo/blockstates/a.json",
        R"({"variants":{"facing=north":[{"model":"demo:a","weight":1},{"model":"demo:b","weight":3,"z":90}]}})");
    write(
        r, "assets/demo/blockstates/m.json",
        R"({"multipart":[{"apply":{"model":"demo:base"}},{"when":{"OR":[{"on":"true"},{"facing":"north|west"}]},"apply":{"model":"demo:extra"}}]})");
    write(
        r, "assets/demo/items/a.json",
        R"({"model":{"type":"minecraft:composite","models":[{"type":"minecraft:model","model":"demo:base"},{"type":"minecraft:condition","property":"minecraft:using_item","on_true":{"type":"minecraft:model","model":"demo:active"},"on_false":{"type":"minecraft:empty"}}]}})");
    write(
        r, "assets/demo/items/r.json",
        R"({"model":{"type":"minecraft:range_dispatch","property":"minecraft:damage","entries":[{"threshold":0,"model":{"type":"minecraft:model","model":"demo:a"}},{"threshold":0.5,"model":{"type":"minecraft:model","model":"demo:b"}}]}})");
    mc::Library library;
    library.update(r, Index::scan(r));
    auto d = library.dispatch("assets/demo/blockstates/a.json", {{"facing", "north"}}, 1);
    QCOMPARE(d.instances.size(), 1);
    QCOMPARE(d.instances[0].model, QString("demo:b"));
    QCOMPARE(d.instances[0].z, 90);
    d = library.dispatch("assets/demo/blockstates/m.json", {{"on", true}, {"facing", "south"}});
    QCOMPARE(d.instances.size(), 2);
    d = library.dispatch("assets/demo/items/a.json", {{"minecraft:using_item", true}});
    QCOMPARE(d.instances.size(), 2);
    d = library.dispatch("assets/demo/items/r.json", {{"minecraft:damage", .7}});
    QCOMPARE(d.instances[0].model, QString("demo:b"));
    QVERIFY(!library.dispatch("assets/demo/items/r.json").diagnostics.isEmpty());
    // An unset select property is a default stack (no trim): the game draws the fallback.
    write(r, "assets/demo/items/s.json",
          R"({"model":{"type":"minecraft:select","property":"minecraft:trim_material","cases":[{"when":"minecraft:iron","model":{"type":"minecraft:model","model":"demo:b"}}],"fallback":{"type":"minecraft:model","model":"demo:a"}}})");
    library.update(r, Index::scan(r));
    d = library.dispatch("assets/demo/items/s.json");
    QCOMPARE(d.instances.size(), 1);
    QCOMPARE(d.instances[0].model, QString("demo:a"));
    QVERIFY(d.diagnostics.join(' ').contains("select fallback"));
    d = library.dispatch("assets/demo/items/s.json", {{"minecraft:trim_material", "minecraft:iron"}});
    QCOMPARE(d.instances[0].model, QString("demo:b"));
}
void ModelTest::rescaleUvlockAndModernShading() {
    QTemporaryDir dir;
    write(dir.path(), "assets/demo/models/a.json", box());
    mc::Library library;
    library.update(dir.path(), Index::scan(dir.path()));
    const auto model = library.model("demo:a");
    const auto base = mc::mesh(model);
    mc::Instance state;
    state.y = 90;
    state.uvlock = true;
    const auto locked = mc::transformed(base, state);
    QCOMPARE(locked.faces.size(), base.faces.size());
    for (int i = 0; i < base.faces.size(); ++i)
        if (base.faces[i].name == "up") {
            QCOMPARE(locked.faces[i].uv[0], QVector2D(1, 0));
            QCOMPARE(locked.faces[i].uvRect(), QRectF(0, 0, 16, 16));
        }
    auto modified = model;
    modified.resolved =
        QJsonDocument::fromJson(
            R"json({"elements":[{"from":[0,0,0],"to":[16,16,16],"rotation":{"origin":[8,8,8],"x":90,"y":0,"z":0,"rescale":true},"faces":{"south":{"texture":"demo:t"}}}]})json")
            .object();
    auto rotated = mc::mesh(modified);
    QCOMPARE(rotated.faces.size(), 1);
    QVERIFY(std::abs(rotated.faces[0].normal.y() + 1) < .001f);
    modified.version = "26.3";
    modified.resolved =
        QJsonDocument::fromJson(
            R"json({"elements":[{"from":[0,0,0],"to":[16,16,16],"shade_direction_override":"up","faces":{"south":{"texture":"demo:t"}}}]})json")
            .object();
    QCOMPARE(mc::mesh(modified).faces.size(), 1);
    modified.version = "26.1";
    QVERIFY(mc::mesh(modified).faces.isEmpty());
}
void ModelTest::vanillaParentsFromBaselineCache() {
    QTemporaryDir dir;
    const auto r = dir.path();
    write(r, "pack.mcmeta", "{}");
    write(r, "assets/demo/models/item/gem.json",
          R"({"parent":"minecraft:item/generated","textures":{"layer0":"minecraft:item/apple"}})");
    // A synthetic baseline-cache entry (hand-written JSON and a generated PNG; never a real
    // Mojang file) standing in for a verified schema-2 cache, as the installer would build it.
    vanilla::VersionMetadata m;
    m.id = "1.21.11";
    m.client = {QUrl("https://piston-data.mojang.com/v1/objects/x/client.jar"), QString(40, '1'), 4096};
    m.assetIndexId = "29";
    m.assetIndex = {QUrl("https://piston-meta.mojang.com/v1/packages/x/29.json"), QString(40, '2'), 512};
    vanilla::BaselineEntry entry;
    entry.root = dir.path() + "/cache";
    entry.version = "1.21.11";
    entry.identity = vanilla::identityKey(m);
    entry.created = QDateTime::currentDateTimeUtc();
    entry.schema = 2;
    entry.source = QJsonObject{
        {"manifest_url", QString(vanilla::defaultManifestUrl)},
        {"version_url", "https://piston-meta.mojang.com/v1/packages/x/1.21.11.json"},
        {"version_sha1", QString(40, '3')},
        {"client", QJsonObject{{"url", m.client.url.toString()}, {"sha1", m.client.sha1}, {"size", m.client.size}}},
        {"asset_index", QJsonObject{{"id", m.assetIndexId}, {"url", m.assetIndex.url.toString()},
                                    {"sha1", m.assetIndex.sha1}, {"size", m.assetIndex.size}}}};
    const vanilla::BaselineCache cache(entry.root);
    auto add = [&](const QString &rel, const QByteArray &bytes) {
        QString error;
        QVERIFY2(cache.publishObject(bytes, vanilla::sha256Hex(bytes), rel, &error), qPrintable(error));
        entry.files.insert(rel, {vanilla::sha256Hex(bytes), bytes.size(), "jar"});
    };
    add("assets/minecraft/models/item/generated.json", R"({"parent":"builtin/generated"})");
    add("assets/minecraft/blockstates/stone.json", R"({"variants":{"":{"model":"minecraft:block/stone"}}})");
    add("assets/minecraft/items/apple.json", R"({"model":{"type":"minecraft:model","model":"minecraft:item/generated"}})");
    QImage sprite(16, 16, QImage::Format_ARGB32);
    sprite.fill(Qt::red);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    sprite.save(&buffer, "PNG");
    add("assets/minecraft/textures/item/apple.png", png);
    QString error;
    QVERIFY2(cache.writeManifest(entry, &error), qPrintable(error));
    QVERIFY(entry.hasModelData());

    // No user vanilla folder configured: the vanilla parent cannot resolve, as today.
    mc::Library plain("1.21.11");
    plain.update(r, Index::scan(r));
    QVERIFY(!plain.model("demo:item/gem").valid);

    // With the cache entry as the model source, item/generated resolves from the verified
    // objects and the texture reference lands on the cache's copy.
    mc::Library library("1.21.11");
    library.update(r, Index::scan(r));
    library.setFallbacks({mc::ResourceSource::fromBaseline(entry)}, "1.21.11");
    const auto model = library.model("demo:item/gem");
    QVERIFY2(model.valid, qPrintable(model.diagnostics.join(' ')));
    QVERIFY(model.parents.contains("minecraft:item/generated"));
    QCOMPARE(model.textures["layer0"], QString("assets/minecraft/textures/item/apple.png"));
    const auto path = library.assetPath("assets/minecraft/models/item/generated.json");
    QVERIFY(path.startsWith(dir.path() + "/cache/objects/"));
    QVERIFY(path.endsWith(".json"));
    QVERIFY(!library.editable("assets/minecraft/models/item/generated.json"));
    QCOMPARE(library.dispatch("assets/minecraft/items/apple.json").instances.size(), 1);
}
QTEST_GUILESS_MAIN(ModelTest)
