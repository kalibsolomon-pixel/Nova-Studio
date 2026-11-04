// SPDX-License-Identifier: GPL-3.0-or-later
#include "batch.h"
#include "creation.h"
#include "edit.h"
#include "modelauthor.h"
#include "resourcetools.h"
#include "validation.h"
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
using namespace nova;
namespace {
bool write(const QString &root, const QString &asset, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(root + '/' + asset).absolutePath());
    QFile f(root + '/' + asset);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
QJsonObject original() {
    auto o = mc::modelTemplate(mc::ModelTemplate::Cube, "nova:block/a");
    o["extension"] = QJsonObject{{"preserve", 42}};
    auto a = o["elements"].toArray();
    auto e = a[0].toObject();
    e["custom"] = true;
    auto f = e["faces"].toObject();
    auto face = f["south"].toObject();
    face["face_extension"] = 17;
    f["south"] = face;
    e["faces"] = f;
    a[0] = e;
    o["elements"] = a;
    return o;
}
QString makeModel(const QString &root) {
    QString path = "assets/nova/models/block/a.json";
    write(root, path, QJsonDocument(original()).toJson());
    return path;
}
mc::Library library(const QString &root, QString version = "26.1") {
    mc::Library l(version);
    l.update(root, Index::scan(root));
    return l;
}
bool has(const mc::ValidationReport &r, const QString &text) {
    for (const auto &d : r.diagnostics)
        if (d.message.contains(text, Qt::CaseInsensitive) || d.code == text)
            return true;
    return false;
}
} // namespace
class C7Test : public QObject {
    Q_OBJECT
  private slots:
    void faceAssignmentsWriteMinecraftAliases() {
        mc::ModelEdit edit;
        edit.create(original());
        mc::FaceEdit change;
        change.directions = {"south", "east"};
        change.texture = "nova:block/other";
        QVERIFY(edit.previewFaces(0, change));
        const auto source = edit.source();
        const auto face =
            source["elements"].toArray()[0].toObject()["faces"].toObject()["south"].toObject();
        const auto reference = face["texture"].toString();
        QVERIFY(reference.startsWith('#'));
        QCOMPARE(source["textures"].toObject()[reference.mid(1)].toString(), change.texture);
        auto raw = original();
        auto elements = raw["elements"].toArray();
        auto e = elements[0].toObject(), faces = e["faces"].toObject();
        auto south = faces["south"].toObject();
        south["texture"] = "nova:block/other";
        faces["south"] = south;
        e["faces"] = faces;
        elements[0] = e;
        raw["elements"] = elements;
        QTemporaryDir root;
        const QString asset = "assets/nova/models/block/literal.json";
        QVERIFY(write(root.path(), asset, QJsonDocument(raw).toJson()));
        mc::ValidationEngine engine;
        auto report = engine.run(root.path(), Index::scan(root.path()), library(root.path()));
        QVERIFY(has(report, "face_texture_reference"));
        mc::Diagnostic repair;
        for (const auto &d : report.diagnostics)
            if (d.code == "face_texture_reference")
                repair = d;
        QVERIFY(!repair.replacement.isEmpty());
        QVERIFY(mc::applyRepair(root.path(), repair).applied);
        QCOMPARE(repair.replacement["extension"], raw["extension"]);
    }
    void directFaceMaterialAppearsInAuthorPreview() {
        QTemporaryDir root;
        auto asset = makeModel(root.path());
        QDir().mkpath(root.path() + "/assets/nova/textures/block");
        QImage pixels(16, 16, QImage::Format_RGBA8888);
        pixels.fill(Qt::red);
        QVERIFY(pixels.save(root.path() + "/assets/nova/textures/block/a.png"));
        pixels.fill(Qt::green);
        QVERIFY(pixels.save(root.path() + "/assets/nova/textures/block/b.png"));
        auto edit = std::make_shared<mc::ModelEdit>();
        QVERIFY(edit->open(root.path(), asset));
        auto catalog = std::make_shared<mc::Library>(library(root.path()));
        ModelAuthorDialog dialog(edit, catalog, root.path());
        dialog.show();
        QTest::qWait(60);
        auto cube = dialog.findChild<Cube *>("authorPreview");
        QVERIFY(cube);
        QVERIFY(!cube->grabFramebuffer().isNull());
        const auto initialTextures = cube->resourceCounts()["textures"].toInt();
        dialog.findChild<QListWidget *>("authorDirections")->setCurrentRow(3);
        dialog.findChild<QComboBox *>("authorTexture")->setCurrentText("nova:block/b");
        dialog.findChild<QPushButton *>("authorAssign")->click();
        QVERIFY(!cube->grabFramebuffer().isNull());
        QCOMPARE(cube->resourceCounts()["textures"].toInt(), initialTextures + 1);
    }
    void inMemoryValidationAndAtomicMultiFaceFailure() {
        QTemporaryDir root;
        auto asset = makeModel(root.path());
        auto source = original();
        auto elements = source["elements"].toArray();
        auto e = elements[0].toObject();
        e["from"] = QJsonArray{40, 0, 0};
        elements[0] = e;
        source["elements"] = elements;
        auto l = library(root.path());
        mc::ValidationEngine engine;
        auto r =
            engine.run(root.path(), Index::scan(root.path()), l, {}, {}, {}, {}, {{asset, source}});
        QVERIFY(has(r, "unsaved_model"));
        QVERIFY(has(r, "invalid cuboid"));
        for (const auto &d : r.diagnostics)
            QVERIFY(d.replacement.isEmpty());
        auto saved = engine.run(root.path(), Index::scan(root.path()), l);
        QVERIFY(!has(saved, "unsaved_model"));
        QVERIFY(!has(saved, "invalid cuboid"));
        mc::ModelEdit edit;
        edit.create(original());
        const auto before = edit.source();
        QVERIFY(!edit.previewUvs({{0, "south"}, {99, "north"}}, {1, 2, 3, 4}, 90));
        QCOMPARE(edit.source(), before);
        QVERIFY(!edit.previewing());
    }
    void renamePreservesReferencesAndRecovery() {
        QTemporaryDir root;
        auto asset = makeModel(root.path());
        const QString renamed = "assets/nova/models/block/renamed.json";
        mc::ModelEdit edit;
        QVERIFY(edit.open(root.path(), asset));
        QString error;
        QVERIFY2(edit.renameKeepingAlias(root.path(), renamed, &error), qPrintable(error));
        QCOMPARE(edit.assetPath(), renamed);
        auto l = library(root.path());
        auto old = l.model("nova:block/a"), now = l.model("nova:block/renamed");
        QVERIFY(old.valid);
        QVERIFY(now.valid);
        QCOMPARE(old.resolved["elements"], now.resolved["elements"]);
        QCOMPARE(now.source["extension"], original()["extension"]);
        auto journals =
            QDir(root.path() + "/.nova/creation").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(journals.size(), 1);
        QVERIFY2(mc::rollbackCreation(root.path(), ".nova/creation/" + journals.first(), &error),
                 qPrintable(error));
        QVERIFY(!QFileInfo::exists(root.path() + '/' + renamed));
        mc::ModelEdit restored;
        QVERIFY(restored.open(root.path(), asset));
        QCOMPARE(restored.source(), original());
    }
    void multiFaceUvTransactions() {
        mc::ModelEdit e;
        e.create(original());
        QVector<mc::UvTarget> targets{{0, "south"}, {0, "east"}};
        QVERIFY(e.transformUvs(targets, mc::UvOperation::Move, {0.125, 0.25}, {32, 16}));
        auto before = e.source();
        QVERIFY(e.transformUvs(targets, mc::UvOperation::MirrorU));
        QVERIFY(e.transformUvs(targets, mc::UvOperation::Rotate));
        QVERIFY(e.transformUvs(targets, mc::UvOperation::Snap, {}, {32, 16}));
        auto valid = e.source();
        QVERIFY(!e.transformUvs({{0, "south"}, {900, "east"}}, mc::UvOperation::Move, {1, 2}));
        QCOMPARE(e.source(), valid);
        QVERIFY(e.transformUvs(targets, mc::UvOperation::Align, {2.5, 3.25}));
        QVERIFY(e.commitPreview());
        QVERIFY(e.undo());
        QCOMPARE(e.source(), original());
        Q_UNUSED(before);
    }
    void modelRecoveryChecksumAndExternalConflict() {
        QTemporaryDir root;
        auto asset = makeModel(root.path());
        mc::ModelEdit edit;
        QVERIFY(edit.open(root.path(), asset));
        QVERIFY(edit.labelElement(0, "Recovered"));
        QVERIFY(edit.commitPreview());
        auto path = root.path() + "/snapshot.json";
        QVERIFY(edit.writeRecovery(path));
        mc::ModelEdit restored;
        QVERIFY(restored.restoreRecovery(root.path(), path));
        QCOMPARE(restored.source(), edit.source());
        QVERIFY(restored.modified());
        QVERIFY(write(root.path(), asset, "{\"external\":true}"));
        QVERIFY(restored.restoreRecovery(root.path(), path));
        QVERIFY(restored.externalChange());
        QVERIFY(!restored.save());
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto bytes = f.readAll();
        f.close();
        bytes.replace("Recovered", "Corrupted");
        QVERIFY(write(root.path(), "snapshot.json", bytes));
        QVERIFY(!restored.restoreRecovery(root.path(), path));
    }
    void failureDuringAtomicSavePreservesExternalData() {
        QTemporaryDir root;
        auto asset = makeModel(root.path());
        auto path = root.path() + '/' + asset;
        auto rev = Revision::read(path);
        QString error;
        QVERIFY(!atomicWrite(path, "new", rev, &error,
                             [&] { write(root.path(), asset, "external revision"); }));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("external revision"));
    }
    void nativeBatchSourceProtection() {
        QTemporaryDir root, backup;
        auto path = "assets/nova/textures/a.png";
        QDir().mkpath(root.path() + "/assets/nova/textures");
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        QVERIFY(image.save(root.path() + '/' + path));
        auto native = nativeDocumentPath(root.path(), root.path() + '/' + path);
        QDir().mkpath(QFileInfo(native).absolutePath());
        QFile file(native);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("native source remains protected");
        file.close();
        batch::Pipeline pipeline;
        pipeline.steps = {{"invert", {}}};
        batch::Output out;
        out.inPlace = true;
        out.backupDirectory = backup.path();
        auto plan = batch::plan(pipeline, root.path(), {path}, out);
        QCOMPARE(plan.ready, 0);
        QCOMPARE(plan.invalid, 1);
        QVERIFY(plan.items[0].message.contains("native source"));
    }
    void lifecycleAndRoundTrip() {
        QTemporaryDir root;
        mc::ModelEdit e;
        QVERIFY(e.create(original()));
        QVERIFY(e.modified());
        QString error;
        QVERIFY2(e.saveAs(root.path(), "assets/nova/models/block/new.json", &error),
                 qPrintable(error));
        QVERIFY(!e.modified());
        mc::ModelEdit reopened;
        QVERIFY(reopened.open(root.path(), e.assetPath()));
        QCOMPARE(reopened.source(), original());
        QVERIFY(!e.saveAs(root.path(), e.assetPath(), &error));
        QVERIFY(!e.saveAs(root.path(), "../escape.json", &error));
    }
    void geometryTransaction() {
        QTemporaryDir root;
        auto path = makeModel(root.path());
        mc::ModelEdit e;
        QVERIFY(e.open(root.path(), path));
        const auto baseline = e.source();
        mc::ElementTransform t;
        t.from = {1, 2, 3};
        t.to = {12, 13, 14};
        t.rotation = true;
        t.angles = {30, 45, -60};
        t.origin = {3, 4, 5};
        QVERIFY(e.previewElement(0, t));
        QCOMPARE(e.authoritative(), baseline);
        e.cancelPreview();
        QCOMPARE(e.source(), baseline);
        QVERIFY(e.previewElement(0, t));
        QVERIFY(e.commitPreview());
        QVERIFY(e.undo());
        QCOMPARE(e.source(), baseline);
        QVERIFY(e.redo());
        auto element = e.source()["elements"].toArray()[0].toObject();
        QVERIFY(element["custom"].toBool());
        QCOMPARE(e.source()["extension"], baseline["extension"]);
        QVERIFY(e.save());
        auto model = library(root.path()).model("nova:block/a");
        QCOMPARE(mc::mesh(model).faces.size(), 6);
    }
    void invalidTransformsAreUnchanged() {
        QTemporaryDir root;
        mc::ModelEdit e;
        QVERIFY(e.open(root.path(), makeModel(root.path())));
        auto before = e.source();
        mc::ElementTransform t;
        t.from = {20, 0, 0};
        QVERIFY(!e.previewElement(0, t));
        t.from = {-17, 0, 0};
        QVERIFY(!e.previewElement(0, t));
        t.from = {0, 0, 0};
        t.angles = {std::numeric_limits<float>::infinity(), 0, 0};
        QVERIFY(!e.previewElement(0, t));
        QCOMPARE(e.source(), before);
    }
    void elementLifecycle() {
        mc::ModelEdit e;
        e.create(original());
        QVERIFY(e.addElement());
        QVERIFY(e.labelElement(1, "Inset"));
        QVERIFY(e.duplicateElement(1));
        QCOMPARE(e.source()["elements"].toArray().size(), 3);
        QVERIFY(e.deleteElement(0));
        QVERIFY(e.commitPreview());
        QCOMPARE(e.source()["elements"].toArray().size(), 2);
        QVERIFY(e.undo());
        QCOMPARE(e.source()["elements"].toArray().size(), 1);
    }
    void facesAndBindings() {
        mc::ModelEdit e;
        e.create(original());
        mc::FaceEdit f;
        f.directions = {"south", "east"};
        f.texture = "nova:block/b";
        f.tint = 2;
        f.cullface = "south";
        QVERIFY(e.previewFaces(0, f));
        auto a = e.source()["elements"].toArray()[0].toObject()["faces"].toObject();
        QCOMPARE(a["south"].toObject()["face_extension"].toInt(), 17);
        QVERIFY(e.previewUv(0, "south", {15.5f, 1.25f, 2.5f, 12.25f}, 270));
        auto copied =
            e.source()["elements"].toArray()[0].toObject()["faces"].toObject()["south"].toObject();
        QVERIFY(e.pasteFaces(0, {"north", "up"}, copied));
        QVERIFY(e.textureBinding("all", "nova:block/b"));
        QVERIFY(!e.textureBinding("../bad", "nova:block/b"));
        f.enabled = false;
        QVERIFY(e.previewFaces(0, f));
        QVERIFY(
            !e.source()["elements"].toArray()[0].toObject()["faces"].toObject().contains("south"));
        e.cancelPreview();
        QCOMPARE(e.source(), original());
    }
    void conflictAndFailedSave() {
        QTemporaryDir root;
        auto path = makeModel(root.path());
        mc::ModelEdit e;
        QVERIFY(e.open(root.path(), path));
        QVERIFY(e.previewUv(0, "south", {0, 0, 8, 8}, 0));
        QVERIFY(!e.save());
        QVERIFY(e.commitPreview());
        QVERIFY(write(root.path(), path, "{}"));
        QVERIFY(e.externalChange());
        QVERIFY(!e.save());
        QFile f(root.path() + '/' + path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("{}"));
    }
    void historyBudget() {
        mc::ModelEdit e;
        e.create(original());
        for (int i = 0; i < 200; ++i) {
            QVERIFY(e.labelElement(0, QString::number(i)));
            QVERIFY(e.commitPreview());
        }
        QVERIFY(e.historyBytes() <= 16 * 1024 * 1024);
        int count = 0;
        while (e.undo())
            ++count;
        QCOMPARE(count, 128);
    }
    void allTemplatesAndProfiles() {
        for (const auto &version : {"1.21.11", "26.1", "26.3"})
            for (int i = 0; i < 6; ++i) {
                QTemporaryDir root;
                mc::CreationRequest r;
                r.id = "nova:block/original";
                r.texture = "nova:block/check";
                r.shape = mc::ModelTemplate(i);
                r.type = mc::AssetType::ModelTexturePair;
                r.version = version;
                auto p = mc::planCreation(root.path(), r);
                QVERIFY(p.valid());
                auto out = mc::executeCreation(p);
                QVERIFY2(out.complete, qPrintable(out.error));
                auto m = library(root.path(), version).model(r.id);
                if (i == 3 || i == 4) {
                    QVERIFY(m.diagnostics.join(' ').contains("Missing parent"));
                } else {
                    QVERIFY(m.valid);
                    QVERIFY(!mc::mesh(m).faces.isEmpty());
                }
            }
    }
    void creationTypesAndRecovery() {
        for (int type = 0; type < 7; ++type) {
            QTemporaryDir root;
            mc::CreationRequest r;
            r.id = "nova:block/new";
            r.type = mc::AssetType(type);
            auto p = mc::planCreation(root.path(), r);
            QVERIFY(p.valid());
            auto out = mc::executeCreation(p);
            QVERIFY(out.complete);
            QVERIFY(!mc::planCreation(root.path(), r).valid());
            QString error;
            QVERIFY2(mc::rollbackCreation(root.path(), out.journal, &error), qPrintable(error));
            for (const auto &f : p.files)
                QVERIFY(!QFileInfo::exists(root.path() + '/' + f.relative));
            QVERIFY(QFileInfo::exists(root.path() + '/' + out.journal + "/manifest.json"));
        }
    }
    void partialCreationAndRecoveryConflict() {
        QTemporaryDir root;
        mc::CreationRequest r;
        r.id = "nova:block/new";
        r.type = mc::AssetType::ModelTexturePair;
        auto p = mc::planCreation(root.path(), r);
        auto out = mc::executeCreation(p, [&](int i) {
            if (i == 1)
                write(root.path(), p.files[i].relative, "external");
        });
        QVERIFY(!out.complete);
        QCOMPARE(out.written.size(), 1);
        QString error;
        QVERIFY(!mc::rollbackCreation(root.path(), out.journal, &error));
        QVERIFY(QFileInfo::exists(root.path() + '/' + p.files[0].relative));
        QFile f(root.path() + '/' + p.files[1].relative);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("external"));
    }
    void interruptedCreationRecovery() {
        QTemporaryDir root;
        mc::CreationRequest r;
        r.id = "nova:block/new";
        auto p = mc::planCreation(root.path(), r);
        auto out = mc::executeCreation(p);
        QVERIFY(out.complete);
        auto manifest = root.path() + '/' + out.journal + "/manifest.json";
        QFile f(manifest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto j = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        j["written"] = QJsonArray{};
        j["state"] = "prepared";
        QVERIFY(write(root.path(), out.journal + "/manifest.json", QJsonDocument(j).toJson()));
        QVERIFY(mc::rollbackCreation(root.path(), out.journal));
        QVERIFY(!QFileInfo::exists(root.path() + '/' + p.files[0].relative));
    }
    void invalidCreationPaths() {
        QTemporaryDir root;
        QString abs, error;
        for (const auto &s : {"../outside", "assets/CON/a.json", "assets/nova/../x", "C:/x",
                              "assets/nova/x:stream", "assets/nova/test./a"})
            QVERIFY2(!mc::safeDestination(root.path(), s, &abs, &error), s);
        mc::CreationRequest r;
        r.id = "nova:../bad";
        QVERIFY(!mc::planCreation(root.path(), r).valid());
        r.id = "nova:ok";
        r.version = "unknown";
        QVERIFY(!mc::planCreation(root.path(), r).valid());
    }
    void dependencyAndMalformedValidation() {
        QTemporaryDir root;
        makeModel(root.path());
        write(root.path(), "assets/nova/models/cycle1.json", R"({"parent":"nova:cycle2"})");
        write(root.path(), "assets/nova/models/cycle2.json", R"({"parent":"nova:cycle1"})");
        write(root.path(), "assets/nova/models/bad.json", "{\n invalid");
        write(root.path(), "assets/nova/blockstates/a.json",
              R"({"variants":{"":{"model":"nova:missing"}}})");
        write(root.path(), "assets/nova/items/a.json", "{}");
        mc::ValidationEngine engine;
        auto r = engine.run(root.path(), Index::scan(root.path()), library(root.path()));
        QVERIFY(has(r, "cyclic"));
        QVERIFY(has(r, "missing texture"));
        QVERIFY(has(r, "missing_model"));
        QVERIFY(has(r, "item_definition"));
        QVERIFY(has(r, "malformed_json"));
        auto json = r.json();
        QVERIFY(json["diagnostics"].isArray());
        QVERIFY(r.text().contains("cannot certify"));
    }
    void versionRepairAndStaleGuard() {
        QTemporaryDir root;
        auto path = makeModel(root.path());
        auto o = original();
        auto a = o["elements"].toArray();
        auto e = a[0].toObject();
        e["shade"] = false;
        a[0] = e;
        o["elements"] = a;
        write(root.path(), path, QJsonDocument(o).toJson());
        mc::ValidationEngine engine;
        auto older =
            engine.run(root.path(), Index::scan(root.path()), library(root.path(), "26.1"));
        QVERIFY(!has(older, "shade_migration"));
        auto current =
            engine.run(root.path(), Index::scan(root.path()), library(root.path(), "26.3"));
        QVERIFY(has(current, "shade_migration"));
        mc::Diagnostic fix;
        for (const auto &d : current.diagnostics)
            if (d.code == "shade_migration")
                fix = d;
        QVERIFY(!fix.replacement.isEmpty());
        auto out = mc::applyRepair(root.path(), fix);
        QVERIFY2(out.applied, qPrintable(out.error));
        QVERIFY(QFileInfo::exists(root.path() + '/' + out.backup));
        QCOMPARE(fix.replacement["extension"], o["extension"]);
        QVERIFY(!mc::applyRepair(root.path(), fix).applied);
        auto after =
            engine.run(root.path(), Index::scan(root.path()), library(root.path(), "26.3"));
        QVERIFY(!has(after, "shade_migration"));
    }
    void textureRepairAndIncrementalInvalidation() {
        QTemporaryDir root;
        auto path = makeModel(root.path());
        QDir().mkpath(root.path() + "/assets/nova/textures/relocated");
        QImage im(16, 16, QImage::Format_RGBA8888);
        im.fill(Qt::white);
        QVERIFY(im.save(root.path() + "/assets/nova/textures/relocated/a.png"));
        mc::ValidationEngine engine;
        auto l = library(root.path());
        auto first = engine.run(root.path(), Index::scan(root.path()), l);
        mc::Diagnostic fix;
        for (const auto &d : first.diagnostics)
            if (d.code == "texture_path")
                fix = d;
        QVERIFY(!fix.replacement.isEmpty());
        auto second = engine.run(root.path(), Index::scan(root.path()), l);
        QVERIFY(second.reused > 0);
        auto out = mc::applyRepair(root.path(), fix);
        QVERIFY(out.applied);
        auto index = Index::scan(root.path());
        l.update(root.path(), index, {path});
        auto third = engine.run(root.path(), index, l, {}, {path});
        QVERIFY(!has(third, "texture_path"));
        QVERIFY(!has(third, "Missing texture:"));
    }
    void animationValidation() {
        QTemporaryDir root;
        write(root.path(), "assets/nova/textures/a.png.mcmeta", R"({"animation":{"frames":[4]}})");
        mc::ValidationEngine engine;
        auto first = engine.run(root.path(), Index::scan(root.path()), library(root.path()));
        QVERIFY(has(first, "missing_spritesheet"));
        QImage im(16, 32, QImage::Format_RGBA8888);
        im.fill(Qt::white);
        im.save(root.path() + "/assets/nova/textures/a.png");
        auto next = engine.run(root.path(), Index::scan(root.path()), library(root.path()), {},
                               {"assets/nova/textures/a.png.mcmeta"});
        QVERIFY(has(next, "animation"));
    }
    void cancelAndScope() {
        QTemporaryDir root;
        makeModel(root.path());
        write(root.path(), "assets/other/items/a.json", "{}");
        mc::ValidationEngine engine;
        auto cancellation = std::make_shared<std::atomic_bool>(true);
        auto r = engine.run(root.path(), Index::scan(root.path()), library(root.path()), {}, {},
                            cancellation);
        QVERIFY(r.canceled);
        auto scoped = engine.run(root.path(), Index::scan(root.path()), library(root.path()),
                                 {"assets/other"});
        QCOMPARE(scoped.files, 1);
        QVERIFY(has(scoped, "item_definition"));
    }
    void modelAwareBatchUsesSharedEngine() {
        QTemporaryDir root, output;
        mc::CreationRequest request;
        request.id = "nova:block/a";
        request.type = mc::AssetType::ModelTexturePair;
        auto p = mc::planCreation(root.path(), request);
        QVERIFY(mc::executeCreation(p).complete);
        auto l = library(root.path());
        auto scope = mc::modelTextureScope(l, p.files.first().relative, false);
        QCOMPARE(scope, QStringList{"assets/nova/textures/block/a.png"});
        batch::Pipeline pipeline;
        ops::Spec spec;
        spec.id = "invert";
        pipeline.steps.push_back(spec);
        QString error;
        QVERIFY2(pipeline.validate(&error), qPrintable(error));
        batch::Output target;
        target.directory = output.path();
        auto plan = batch::plan(pipeline, root.path(), scope, target);
        QCOMPARE(plan.ready, 1);
        ops::Context context;
        auto out = batch::execute(pipeline, plan, target, context, nullptr);
        QCOMPARE(out.done, 1);
        QVERIFY(QFileInfo::exists(output.path() + '/' + scope[0]));
    }
    void nativeWidgetAuthoring() {
        QTemporaryDir root;
        auto path = makeModel(root.path());
        auto catalog = std::make_shared<mc::Library>(library(root.path()));
        auto edit = std::make_shared<mc::ModelEdit>();
        QVERIFY(edit->open(root.path(), path));
        ModelAuthorDialog d(edit, catalog, root.path());
        d.show();
        QTest::qWait(80);
        auto add = d.findChild<QPushButton *>("authorAdd");
        QVERIFY(add);
        add->click();
        QCOMPARE(edit->source()["elements"].toArray().size(), 2);
        QCOMPARE(edit->authoritative()["elements"].toArray().size(), 1);
        d.findChild<QPushButton *>("authorApply")->click();
        QCOMPARE(edit->authoritative()["elements"].toArray().size(), 2);
        d.findChild<QPushButton *>("authorUndo")->click();
        QCOMPARE(edit->source()["elements"].toArray().size(), 1);
        d.findChild<QPushButton *>("authorRedo")->click();
        QCOMPARE(edit->source()["elements"].toArray().size(), 2);
        d.findChild<QPushButton *>("authorSave")->click();
        QVERIFY(!edit->modified());
        auto coords = d.findChild<QDoubleSpinBox *>("authorNumber0");
        coords->setValue(2);
        QVERIFY(edit->previewing());
        d.findChild<QPushButton *>("authorCancel")->click();
        QVERIFY(!edit->previewing());
        auto directory = qEnvironmentVariable("NOVA_C7_EVIDENCE_DIR");
        if (!directory.isEmpty()) {
            QDir().mkpath(directory);
            d.grab().save(directory + "/author-widget.png");
        }
        d.close();
    }
    void handlesNudgeAndCancel() {
        ElementHandles h;
        h.resize(300, 250);
        h.show();
        mc::ElementTransform received;
        bool change = false;
        h.changed = [&](const auto &t) {
            received = t;
            change = true;
            h.setTransform(t);
        };
        h.setFocus();
        QTest::keyClick(&h, Qt::Key_Right);
        QVERIFY(change);
        QCOMPARE(received.from.x(), 1.f);
        QCOMPARE(received.to.x(), 17.f);
    }
    void problemsAsync() {
        QTemporaryDir root;
        makeModel(root.path());
        auto l = std::make_shared<mc::Library>(library(root.path()));
        ResourceTools panel;
        panel.context = [&] {
            return ResourceContext{root.path(), "assets/nova/models/block/a.json", "assets/nova",
                                   Index::scan(root.path()), l};
        };
        panel.show();
        panel.validate();
        auto list = panel.findChild<QTreeWidget *>("problemsList");
        QTRY_VERIFY_WITH_TIMEOUT(list->topLevelItemCount() > 0, 10000);
        auto directory = qEnvironmentVariable("NOVA_C7_EVIDENCE_DIR");
        if (!directory.isEmpty())
            panel.grab().save(directory + "/problems-widget.png");
    }
    void largeValidationResponsiveness() {
        const auto root = qEnvironmentVariable("NOVA_LARGE_FIXTURE");
        if (root.isEmpty())
            QSKIP("NOVA_LARGE_FIXTURE not supplied");
        auto index = Index::scan(root);
        QVERIFY(index.files.size() >= 50000);
        auto catalog = std::make_shared<mc::Library>();
        catalog->update(root, index);
        ResourceTools panel;
        panel.context = [&] { return ResourceContext{root, {}, {}, index, catalog}; };
        panel.show();
        QElapsedTimer time;
        time.start();
        qint64 previous = 0, worst = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, &panel, [&] {
            auto now = time.elapsed();
            if (previous)
                worst = std::max(worst, now - previous);
            previous = now;
        });
        heartbeat.start(10);
        panel.validate();
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QPushButton *>("runValidation")->isEnabled(),
                                 90000);
        heartbeat.stop();
        qInfo() << "c7-large-validation responsiveness worst-gap-ms" << worst;
#ifdef NDEBUG
        QVERIFY2(worst < 500, qPrintable(QString::number(worst)));
#else
        QVERIFY2(worst < 1500, qPrintable(QString::number(worst)));
#endif
    }
};
QTEST_MAIN(C7Test)
#include "c7_test.moc"
