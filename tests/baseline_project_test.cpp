// SPDX-License-Identifier: GPL-3.0-or-later
// Vanilla baseline project data: .nova/project.json, virtual (not yet on disk) baseline documents,
// asset folder creation, the Reset to Vanilla / Remove from Project journal and sparse export.
// Every texture is a synthetic PNG generated here; no game file or network access is used.
#include "animation_export.h"
#include "baseline_reset.h"
#include "minecraft_animation.h"
#include "project.h"
#include "project_config.h"
#include "recovery.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace nova;
namespace {
QByteArray png(int w, int h, int seed) {
    QImage image(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            image.setPixel(x, y, qRgba((x * 7 + seed) & 255, (y * 13 + seed) & 255, seed & 255, 255));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}
QImage decode(const QByteArray &bytes) {
    return QImage::fromData(bytes, "PNG").convertToFormat(QImage::Format_RGBA8888);
}
QByteArray animationMeta() { return R"({"animation":{"frametime":2},"x-vanilla":1})"; }
BaselineRef ref(const QString &version) {
    BaselineRef r;
    r.version = version;
    r.identity = QString(64, 'a');
    r.clientSha1 = QString(40, 'b');
    r.assetIndexId = "29";
    r.assetIndexSha1 = QString(40, 'c');
    return r;
}
QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}
bool write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QString sha(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
// The complete recursive tree: folders end in '/', files map to their bytes.
QMap<QString, QByteArray> tree(const QString &root) {
    QMap<QString, QByteArray> out;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const auto relative = QDir(root).relativeFilePath(it.filePath());
        if (it.fileInfo().isDir())
            out[relative + '/'] = {};
        else
            out[relative] = read(it.filePath());
    }
    return out;
}
int fileCount(const QString &root) {
    int n = 0;
    for (const auto &key : tree(root).keys())
        n += !key.endsWith('/');
    return n;
}
QString state(const QString &root, const QString &id) {
    for (const auto &j : resetJournals(root))
        if (j.id == id)
            return j.state;
    return "<none>";
}
QStringList keys(const QMap<QString, QByteArray> &map) { return map.keys(); }
QStringList filesOnly(const QMap<QString, QByteArray> &map) {
    QStringList out;
    for (const auto &k : map.keys())
        if (!k.endsWith('/'))
            out << k;
    return out;
}
QByteArray compact(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
// A schema-1 file whose baseline has one field replaced (invalid values are rejected on load).
QByteArray withBaseline(const QString &key, const QString &value) {
    auto baseline = ref("26.1").json();
    baseline[key] = value;
    return compact(QJsonObject{{"schema", 1}, {"baseline", baseline}});
}
const BaselineCheck available = [](const QString &, QString *) { return true; };
} // namespace

class BaselineProjectTests : public QObject {
    Q_OBJECT
    QString create(const QString &parent, const QString &name, const QString &version,
                   bool baseline, const QString &ns = "minecraft", bool folders = true) {
        QString error;
        GameVersion target;
        for (const auto &v : gameVersions())
            if (v.id == version)
                target = v;
        const auto b = ref(version);
        const auto root = parent + '/' + name;
        if (!Project::create(root, "Pack", target, "Description", ns, {}, folders, &error,
                             baseline ? &b : nullptr))
            qWarning() << error;
        return QFileInfo(root).canonicalFilePath();
    }
    QMap<QString, QByteArray> exported(const QString &root, const QString &destination) {
        QString error;
        if (!Project::exportMinecraft(root, destination, &error)) {
            qWarning() << error;
            return {{"<export failed>", error.toUtf8()}};
        }
        return tree(destination);
    }
    // A saved override created through the virtual baseline document path.
    void saveOverride(const QString &root, const QString &asset, const QByteArray &baseline,
                      QByteArray *saved) {
        QString error;
        auto doc = Document::fromBaseline(root + '/' + asset, baseline, {}, "26.1", &error);
        QVERIFY2(doc, qPrintable(error));
        doc->draw({0, 0}, {1, 1}, qRgba(1, 2, 3, 255));
        doc->finish();
        QVERIFY(ensureAssetDirectory(root, asset, &error));
        QVERIFY2(doc->save(&error), qPrintable(error));
        *saved = read(root + '/' + asset);
    }

  private slots:
    void projectConfigRoundTrip() {
        QTemporaryDir tmp;
        const auto root = tmp.path();
        auto missing = ProjectConfig::load(root);
        QVERIFY(!missing.baseline);
        QVERIFY(missing.diagnostic.isEmpty());
        QVERIFY(!missing.revision.exists);
        QVERIFY(!QFileInfo::exists(root + "/.nova"));
        missing.baseline = ref("1.21.11");
        QString error;
        QVERIFY2(missing.save(root, &error), qPrintable(error));
        auto loaded = ProjectConfig::load(root);
        QVERIFY(loaded.diagnostic.isEmpty());
        QVERIFY(loaded.baseline && *loaded.baseline == ref("1.21.11"));
        const auto json = QJsonDocument::fromJson(read(ProjectConfig::path(root))).object();
        QCOMPARE(json["schema"].toInt(), 1);
        QCOMPARE(json["baseline"].toObject()["asset_index"].toObject()["id"].toString(), QString("29"));
        // Unknown keys (top level, in baseline and in asset_index) survive a save.
        const auto extended = compact(QJsonObject{
            {"schema", 1},
            {"future", QJsonObject{{"x", QJsonArray{1, 2}}}},
            {"baseline", QJsonObject{{"edition", "java"}, {"version", "26.1"}, {"identity", QString(64, 'd')},
                                     {"client_sha1", QString(40, 'e')}, {"note", "keep"},
                                     {"asset_index", QJsonObject{{"id", "30"}, {"sha1", QString(40, 'f')}, {"extra", true}}}}}});
        QVERIFY(write(ProjectConfig::path(root), extended));
        auto unknown = ProjectConfig::load(root);
        QVERIFY2(unknown.diagnostic.isEmpty(), qPrintable(unknown.diagnostic));
        QCOMPARE(unknown.baseline->version, QString("26.1"));
        unknown.baseline->version = "26.3";
        QVERIFY(unknown.save(root, &error));
        const auto saved = QJsonDocument::fromJson(read(ProjectConfig::path(root))).object();
        QCOMPARE(saved["future"].toObject()["x"].toArray().size(), 2);
        QCOMPARE(saved["baseline"].toObject()["note"].toString(), QString("keep"));
        QVERIFY(saved["baseline"].toObject()["asset_index"].toObject()["extra"].toBool());
        QCOMPARE(saved["baseline"].toObject()["version"].toString(), QString("26.3"));
        // A stale revision never overwrites a newer file.
        QVERIFY(write(ProjectConfig::path(root), extended));
        QVERIFY(!unknown.save(root, &error));
        QCOMPARE(read(ProjectConfig::path(root)), extended);
        // Unknown schema and invalid content: no baseline, a diagnostic, never rewritten.
        for (const QByteArray bad :
             {compact(QJsonObject{{"schema", 2}, {"baseline", ref("26.1").json()}}), QByteArray("{broken"),
              compact(QJsonObject{{"schema", 1}, {"baseline", QJsonObject{{"edition", "bedrock"}}}}),
              withBaseline("version", "1.8.9"), withBaseline("identity", QString(64, 'A')),
              withBaseline("client_sha1", QString(39, 'b')), withBaseline("edition", "bedrock")}) {
            QVERIFY(write(ProjectConfig::path(root), bad));
            auto config = ProjectConfig::load(root);
            QVERIFY(!config.baseline);
            QVERIFY(!config.diagnostic.isEmpty());
            config.baseline = ref("26.1");
            QVERIFY(!config.save(root, &error));
            QCOMPARE(read(ProjectConfig::path(root)), bad);
            QCOMPARE(Project::inspect(root).configDiagnostic, config.diagnostic);
        }
    }
    void legacyProjectUnchanged() {
        QTemporaryDir tmp;
        QString error;
        const auto legacy = tmp.path() + "/legacy";
        QVERIFY(Project::create(legacy, "Pack", gameVersions()[0], "Description", "minecraft", {},
                                true, &error));
        QVERIFY(!QFileInfo::exists(legacy + "/.nova"));
        const auto before = tree(legacy);
        const auto p = Project::inspect(legacy);
        QVERIFY(!p.baseline);
        QVERIFY(p.configDiagnostic.isEmpty());
        QCOMPARE(p.kind, Project::Kind::ResourcePack);
        QCOMPARE(p.diagnostics, QStringList({"Pack format is in the verified version registry",
                                             "Metadata checks do not validate all Minecraft assets"}));
        QCOMPARE(tree(legacy), before);
        // A baseline project differs only by .nova/project.json.
        const auto withBaseline = create(tmp.path(), "baseline", gameVersions()[0].id, true);
        auto expected = before;
        expected[".nova/"] = {};
        expected[".nova/project.json"] = read(withBaseline + "/.nova/project.json");
        QCOMPARE(keys(tree(withBaseline)), keys(expected));
        QCOMPARE(read(withBaseline + "/pack.mcmeta"), read(legacy + "/pack.mcmeta"));
        const auto inspected = Project::inspect(withBaseline);
        QVERIFY(inspected.baseline && *inspected.baseline == ref(gameVersions()[0].id));
        QCOMPARE(inspected.diagnostics, p.diagnostics);
        // Mismatched or invalid baselines never create a project.
        auto wrong = ref("1.21.4");
        QVERIFY(!Project::create(tmp.path() + "/wrong", "Pack", gameVersions()[0], "d", "minecraft",
                                 {}, true, &error, &wrong));
        wrong = ref(gameVersions()[0].id);
        wrong.identity = "short";
        QVERIFY(!Project::create(tmp.path() + "/wrong", "Pack", gameVersions()[0], "d", "minecraft",
                                 {}, true, &error, &wrong));
        QVERIFY(!QFileInfo::exists(tmp.path() + "/wrong"));
    }
    void packFormatDiagnostic() {
        QTemporaryDir tmp;
        QString error;
        for (const auto &v : gameVersions()) {
            const auto root = create(tmp.path(), "p" + QString(v.id).remove('.'), v.id, true);
            const auto p = Project::inspect(root);
            QVERIFY2(!p.diagnostics.join('\n').contains("vanilla baseline"), qPrintable(v.id));
        }
        const auto root = create(tmp.path(), "mismatch", "26.1", true);
        auto p = Project::inspect(root);
        const QByteArray other = R"({"pack":{"description":"x","min_format":[75,0],"max_format":[75,0]}})";
        QVERIFY(p.saveMetadata(other, &error));
        const auto config = read(root + "/.nova/project.json");
        p = Project::inspect(root);
        QVERIFY(p.diagnostics.join('\n').contains("does not target the vanilla baseline (Java 26.1"));
        QVERIFY(p.baseline);
        QCOMPARE(read(root + "/pack.mcmeta"), other);
        QCOMPARE(read(root + "/.nova/project.json"), config);
        // A range covering the baseline format matches; a legacy pack_format must equal it.
        QVERIFY(p.saveMetadata(R"({"pack":{"description":"x","min_format":[75,0],"max_format":[90,0]}})", &error));
        QVERIFY(!Project::inspect(root).diagnostics.join('\n').contains("vanilla baseline"));
        const auto legacy = create(tmp.path(), "legacy4", "1.21.4", true);
        p = Project::inspect(legacy);
        QVERIFY(p.saveMetadata(R"({"pack":{"description":"x","pack_format":34}})", &error));
        QVERIFY(Project::inspect(legacy).diagnostics.join('\n').contains("Java 1.21.4"));
    }
    void assetDirectoryContainment() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "26.1", true);
        QString error;
        QVERIFY2(ensureAssetDirectory(root, "assets/minecraft/textures/entity/chest/normal.png", &error),
                 qPrintable(error));
        QVERIFY(QFileInfo(root + "/assets/minecraft/textures/entity/chest").isDir());
        QVERIFY(!QFileInfo::exists(root + "/assets/minecraft/textures/entity/chest/normal.png"));
        QVERIFY(ensureAssetDirectory(root, "assets/newns/textures/item/gem.png", &error));
        const auto before = tree(tmp.path());
        for (const auto &bad :
             {QString("assets/../escape/x.png"), QString("assets/minecraft/../../x.png"),
              QString("../outside/assets/x.png"), QString("/assets/minecraft/x.png"),
              QString("C:/assets/minecraft/x.png"), QString("assets\\minecraft\\x.png"),
              QString("assets/minecraft/.nova/x.png"), QString("pack/assets/x.png"),
              QString("assets/x.png"), QString("assets/nul/x.png"), QString("assets/a:b/x.png"),
              QString("assets/minecraft/textures./x.png"), QString("assets//minecraft/x.png")}) {
            QVERIFY2(!ensureAssetDirectory(root, bad, &error), qPrintable(bad));
            QVERIFY(!error.isEmpty());
        }
        QCOMPARE(tree(tmp.path()), before);
        // A file where a folder is needed is never replaced.
        QVERIFY(write(root + "/assets/minecraft/textures/blocker", "file"));
        QVERIFY(!ensureAssetDirectory(root, "assets/minecraft/textures/blocker/x/y.png", &error));
        QCOMPARE(read(root + "/assets/minecraft/textures/blocker"), QByteArray("file"));
        QVERIFY(!ensureAssetDirectory(root, "assets/minecraft/textures/entity", &error));
#ifdef Q_OS_WIN
        const auto outside = tmp.path() + "/outside";
        QVERIFY(QDir().mkdir(outside));
        const auto junction = root + "/assets/minecraft/textures/linked";
        QCOMPARE(QProcess::execute("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(junction),
                                               QDir::toNativeSeparators(outside)}), 0);
        QVERIFY(QFileInfo(junction).isJunction());
        QVERIFY(reparsePoint(junction));
        QVERIFY(!ensureAssetDirectory(root, "assets/minecraft/textures/linked/block/x.png", &error));
        QVERIFY(error.contains("Linked"));
        QVERIFY(QDir(outside).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        // Resets refuse linked folders too; a linked .nova disables the config.
        QVERIFY(write(outside + "/x.png", png(4, 4, 1)));
        QVERIFY(!applyReset(root, {"assets/minecraft/textures/linked/x.png"},
                            ResetKind::RemoveFromProject, {}, nullptr, &error));
        QCOMPARE(read(outside + "/x.png"), png(4, 4, 1));
        QVERIFY(QDir().rmdir(junction)); // removes the junction, never its target
        QVERIFY(QFileInfo::exists(outside + "/x.png"));
        const auto linkedNova = tmp.path() + "/linked-nova";
        QVERIFY(QDir().mkpath(linkedNova));
        QVERIFY(write(tmp.path() + "/real-nova/project.json", read(root + "/.nova/project.json")));
        QCOMPARE(QProcess::execute("cmd.exe", {"/c", "mklink", "/J",
                                               QDir::toNativeSeparators(linkedNova + "/.nova"),
                                               QDir::toNativeSeparators(tmp.path() + "/real-nova")}), 0);
        const auto config = ProjectConfig::load(linkedNova);
        QVERIFY(!config.baseline);
        QVERIFY(config.diagnostic.contains("linked"));
        QVERIFY(!ensureProjectDirectory(linkedNova, ".nova/baseline-resets/x/journal.json", &error));
        QVERIFY(QDir().rmdir(linkedNova + "/.nova"));
#endif
    }
    void virtualRasterDocument() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "26.1", true);
        const auto asset = "assets/minecraft/textures/entity/pig/pig.png";
        const auto target = root + '/' + asset;
        const auto baseline = png(16, 16, 40);
        const auto before = tree(root);
        QString error;
        auto doc = Document::fromBaseline(target, baseline, R"({"texture":{"blur":true}})", "26.1", &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(!doc->modified());
        QVERIFY(!doc->animated());
        QVERIFY(!doc->minecraftOutputPending());
        QVERIFY(!doc->revision().exists);
        QVERIFY(!doc->mcmetaRevision().exists);
        QVERIFY(!doc->externalChanged());
        QCOMPARE(doc->image, decode(baseline));
        QCOMPARE(doc->path, target);
        QCOMPARE(tree(root), before); // opening wrote nothing, not even folders
        QVERIFY(!Document::fromBaseline(target, "not a png", {}, "26.1", &error));
        QVERIFY(!Document::fromBaseline(target, baseline, "{bad", "26.1", &error));
        doc->draw({2, 2}, {5, 2}, qRgba(200, 10, 10, 255));
        doc->finish();
        QVERIFY(doc->modified());
        // A file that appears at the target before the first save is never overwritten.
        auto rival = Document::fromBaseline(target, baseline, {}, "26.1", &error);
        QVERIFY(rival);
        rival->draw({0, 0}, {0, 0}, qRgba(9, 9, 9, 255));
        rival->finish();
        QVERIFY(!doc->save(&error)); // the folder does not exist yet
        QVERIFY(ensureAssetDirectory(root, asset, &error));
        QVERIFY(write(target, "external"));
        QVERIFY(!doc->save(&error));
        QCOMPARE(read(target), QByteArray("external"));
        QVERIFY(QFile::rename(target, root + "/external.bin"));
        QVERIFY2(doc->save(&error), qPrintable(error));
        QVERIFY(!doc->modified());
        QCOMPARE(loadPng(target), doc->image);
        QVERIFY(Revision::read(target).sameContent(doc->revision()));
        QVERIFY(!Document::fromBaseline(target, baseline, {}, "26.1", &error)); // now a project file
        QVERIFY(!rival->save(&error));
        QVERIFY(Revision::read(target).sameContent(doc->revision()));
        QVERIFY(!QFileInfo::exists(target + ".mcmeta"));
    }
    void virtualAnimatedDocument() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "1.21.11", true);
        const auto asset = "assets/minecraft/textures/block/water_still.png";
        const auto target = root + '/' + asset;
        const auto sheet = png(16, 48, 70);
        const auto before = tree(root);
        QString error;
        auto doc = Document::fromBaseline(target, sheet, animationMeta(), "1.21.11", &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(doc->animated());
        QCOMPARE(int(doc->animationState().frames.size()), 3);
        QCOMPARE(doc->animationState().target, QString("1.21.11"));
        QCOMPARE(doc->animationState().defaultTicks, 2);
        QVERIFY(!doc->modified());
        QVERIFY(!doc->minecraftOutputPending());
        QVERIFY(!doc->externalChanged());
        QCOMPARE(tree(root), before);
        // Versions without an animation profile keep Document::open's fixed profile.
        auto legacy = Document::fromBaseline(target, sheet, animationMeta(), "1.21.4", &error);
        QVERIFY2(legacy && legacy->animationState().target == "26.1", qPrintable(error));
        doc->draw({0, 0}, {3, 0}, qRgba(255, 0, 0, 255));
        doc->finish();
        QVERIFY(doc->modified());
        QVERIFY(doc->minecraftOutputPending());
        QVERIFY(!doc->save(&error)); // animated textures only leave through the pair export
        QVERIFY(ensureAssetDirectory(root, asset, &error));
        const auto plan = planAnimationExport(*doc, 1, {}, &error);
        QVERIFY2(plan.ok(), qPrintable(error));
        // An external .mcmeta appearing first blocks the create-only pair; it stays untouched.
        QVERIFY(write(target + ".mcmeta", "{}"));
        QVERIFY(stageAnimationPair(target, plan.png, plan.mcmeta, doc->revision(), doc->mcmetaRevision(), &error).isEmpty());
        QCOMPARE(read(target + ".mcmeta"), QByteArray("{}"));
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(QFile::rename(target + ".mcmeta", root + "/external.mcmeta"));
        const auto journal = stageAnimationPair(target, plan.png, plan.mcmeta, doc->revision(),
                                                doc->mcmetaRevision(), &error);
        QVERIFY2(!journal.isEmpty(), qPrintable(error));
        QVERIFY2(finishAnimationPair(journal, PairAction::Retry, &error), qPrintable(error));
        QCOMPARE(read(target), plan.png);
        QCOMPARE(read(target + ".mcmeta"), plan.mcmeta);
        QCOMPARE(QJsonDocument::fromJson(plan.mcmeta).object()["x-vanilla"].toInt(), 1);
        doc->acceptAnimationExport(Revision::read(target), Revision::read(target + ".mcmeta"));
        QVERIFY(!doc->minecraftOutputPending());
        auto reopened = Document::open(target, &error);
        QVERIFY2(reopened && reopened->animated(), qPrintable(error));
        QCOMPARE(reopened->frameComposite(reopened->animationState().frames[0].id),
                 doc->frameComposite(doc->animationState().frames[0].id));
        // The pair is a project override now: a second virtual document is refused.
        QVERIFY(!Document::fromBaseline(target, sheet, animationMeta(), "1.21.11", &error));
        QVERIFY(Document::fromBaseline(root + "/assets/minecraft/textures/block/lava_still.png", sheet,
                                       animationMeta(), "1.21.11", &error));
    }
    void recoveryKeepsCreateOnly() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "26.1", true);
        const auto asset = "assets/minecraft/textures/item/apple.png";
        const auto target = root + '/' + asset;
        QString error;
        auto doc = Document::fromBaseline(target, png(16, 16, 5), {}, "26.1", &error);
        QVERIFY(doc);
        doc->draw({1, 1}, {4, 4}, qRgba(0, 255, 0, 255));
        doc->finish();
        const auto edited = doc->image;
        {
            RecoveryStore store(tmp.path() + "/recovery");
            QVERIFY2(store.writeDocument(*doc->recoverySnapshot(), root, &error), qPrintable(error));
        }
        RecoveryStore store(tmp.path() + "/recovery");
        auto entries = store.entries();
        QCOMPARE(entries.size(), 1);
        auto entry = store.load(entries[0].id);
        QVERIFY(entry.error.isEmpty());
        QCOMPARE(entry.path, target);
        QVERIFY(!entry.original.exists);
        QVERIFY(!QFileInfo::exists(target));
        auto restored = std::make_shared<Document>(entry.path, entry.image);
        restored->restorePixels(entry.image, entry.original);
        restored->setRecoveryIdentity(entry.id);
        QVERIFY(restored->modified());
        QVERIFY(!restored->revision().exists);
        QCOMPARE(restored->image, edited);
        QVERIFY(ensureAssetDirectory(root, asset, &error));
        QVERIFY(write(target, "external"));
        QVERIFY(!restored->save(&error));
        QCOMPARE(read(target), QByteArray("external"));
        QVERIFY(QFile::rename(target, root + "/external.bin"));
        QVERIFY2(restored->save(&error), qPrintable(error));
        QCOMPARE(loadPng(target), edited);
        store.restored(entry.id);
        store.discard(entry.id);
    }
    void exportBlankAndBaselineProjects() {
        QTemporaryDir tmp;
        const auto legacy = create(tmp.path(), "legacy", "26.1", false);
        const auto baseline = create(tmp.path(), "baseline", "26.1", true);
        const auto expected = QStringList{"assets/",
                                          "assets/minecraft/",
                                          "assets/minecraft/blockstates/",
                                          "assets/minecraft/font/",
                                          "assets/minecraft/models/",
                                          "assets/minecraft/sounds/",
                                          "assets/minecraft/textures/",
                                          "assets/minecraft/textures/block/",
                                          "assets/minecraft/textures/item/",
                                          "pack.mcmeta"};
        const auto a = exported(legacy, tmp.path() + "/out-legacy");
        const auto b = exported(baseline, tmp.path() + "/out-baseline");
        QCOMPARE(keys(a), expected);
        QCOMPARE(keys(b), expected);
        QCOMPARE(a["pack.mcmeta"], read(legacy + "/pack.mcmeta"));
        QCOMPARE(b["pack.mcmeta"], read(baseline + "/pack.mcmeta"));
        QCOMPARE(a["pack.mcmeta"], b["pack.mcmeta"]);
        // Without common folders only the namespace folder exists, and it is exported empty.
        const auto bare = create(tmp.path(), "bare", "26.1", true, "example", false);
        QCOMPARE(keys(exported(bare, tmp.path() + "/out-bare")),
                 QStringList({"assets/", "assets/example/", "pack.mcmeta"}));
    }
    void exportOverridesAndNewTextures_data() {
        QTest::addColumn<QString>("version");
        QTest::newRow("range 26.1") << QString("26.1");
        QTest::newRow("range 26.3") << QString("26.3");
        QTest::newRow("legacy 1.21.4") << QString("1.21.4");
    }
    void exportOverridesAndNewTextures() {
        QFETCH(QString, version);
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", version, true, "example", false);
        QByteArray override, gem;
        saveOverride(root, "assets/minecraft/textures/entity/chest/normal.png", png(16, 16, 3), &override);
        const auto gemPath = root + "/assets/example/textures/item/gem.png";
        QString error;
        QVERIFY(ensureAssetDirectory(root, "assets/example/textures/item/gem.png", &error));
        QVERIFY(savePng(gemPath, decode(png(8, 8, 9)), &error));
        gem = read(gemPath);
        const auto out = exported(root, tmp.path() + "/out");
        QCOMPARE(filesOnly(out), QStringList({"assets/example/textures/item/gem.png",
                                              "assets/minecraft/textures/entity/chest/normal.png",
                                              "pack.mcmeta"}));
        QCOMPARE(out["assets/minecraft/textures/entity/chest/normal.png"], override);
        QCOMPARE(out["assets/example/textures/item/gem.png"], gem);
        const auto pack = QJsonDocument::fromJson(out["pack.mcmeta"]).object()["pack"].toObject();
        for (const auto &v : gameVersions())
            if (v.id == version) {
                if (v.rangeSchema) {
                    QCOMPARE(pack["min_format"].toArray(), v.format.json().toArray());
                    QCOMPARE(pack["max_format"].toArray(), v.format.json().toArray());
                    QVERIFY(!pack.contains("pack_format"));
                } else {
                    QCOMPARE(pack["pack_format"].toInt(), 46);
                    QVERIFY(!pack.contains("min_format") && !pack.contains("max_format"));
                }
            }
        QVERIFY(!out.contains(".nova/") && !out.contains(".nova/project.json"));
    }
    void resetUndoRedoAndExport() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "26.1", true);
        QString error, id;
        // (1) override saved from a virtual document, (2) override byte-identical to vanilla,
        // (3) layered override with an editable sidecar, (4) animated override with .mcmeta.
        const auto chest = QString("assets/minecraft/textures/entity/chest/normal.png");
        QByteArray chestBytes;
        saveOverride(root, chest, png(16, 16, 3), &chestBytes);
        const auto identical = QString("assets/minecraft/textures/block/stone.png");
        const auto vanillaStone = png(16, 16, 11);
        QVERIFY(atomicWrite(root + '/' + identical, vanillaStone, Revision{}, &error));
        const auto layered = QString("assets/minecraft/textures/item/stick.png");
        QVERIFY(savePng(root + '/' + layered, decode(png(16, 16, 21))));
        {
            auto doc = Document::open(root + '/' + layered, &error);
            QVERIFY(doc);
            QVERIFY(doc->editLayer({LayerCommand::Add, {}, "Ink"}));
            QVERIFY2(doc->saveNative(root, &error), qPrintable(error));
        }
        const auto sidecar = QDir(root).relativeFilePath(nativeDocumentPath(root, root + '/' + layered));
        QVERIFY(sidecar.startsWith(".nova/documents/"));
        const auto lava = QString("assets/minecraft/textures/block/lava_still.png");
        QVERIFY(write(root + '/' + lava, png(16, 32, 50)));
        QVERIFY(write(root + '/' + lava + ".mcmeta", animationMeta()));
        const auto project = tree(root);
        const auto first = exported(root, tmp.path() + "/e1");
        QCOMPARE(filesOnly(first), QStringList({lava, lava + ".mcmeta", identical, chest, layered, "pack.mcmeta"}));
        QCOMPARE(first[identical], vanillaStone); // ownership, not byte comparison
        QCOMPARE(first[chest], chestBytes);
        const QStringList all{chest, identical, layered, lava};
        QVERIFY2(applyReset(root, all, ResetKind::ResetToVanilla, available, &id, &error), qPrintable(error));
        for (const auto &asset : all)
            QVERIFY2(!QFileInfo::exists(root + '/' + asset), qPrintable(asset));
        QVERIFY(!QFileInfo::exists(root + '/' + lava + ".mcmeta"));
        QVERIFY(!QFileInfo::exists(root + '/' + sidecar));
        QVERIFY(!QFileInfo::exists(root + "/assets/minecraft/textures/entity")); // emptied folders
        QVERIFY(QFileInfo(root + "/assets/minecraft").isDir());
        auto journals = resetJournals(root, &error);
        QCOMPARE(journals.size(), 1);
        QCOMPARE(journals[0].id, id);
        QCOMPARE(journals[0].state, QString("applied"));
        QCOMPARE(journals[0].files.size(), 6);
        const auto second = exported(root, tmp.path() + "/e2");
        QCOMPARE(filesOnly(second), QStringList({"pack.mcmeta"}));
        for (const auto &k : second.keys())
            QVERIFY2(!k.contains(".nova"), qPrintable(k));
        QVERIFY(QFileInfo(root + "/.nova/baseline-resets/" + id + "/journal.json").isFile());
        // Undo restores every file byte for byte; export includes them again.
        QVERIFY2(undoReset(root, id, &error), qPrintable(error));
        auto restored = tree(root);
        for (const auto &k : project.keys())
            QVERIFY2(restored.contains(k) && restored[k] == project[k], qPrintable(k));
        QCOMPARE(filesOnly(exported(root, tmp.path() + "/e3")), filesOnly(first));
        QCOMPARE(exported(root, tmp.path() + "/e3b"), first);
        QCOMPARE(state(root, id), QString("undone"));
        QVERIFY(!undoReset(root, id, &error));
        // Redo removes them again.
        QVERIFY2(redoReset(root, id, &error, available), qPrintable(error));
        QCOMPARE(filesOnly(exported(root, tmp.path() + "/e4")), QStringList({"pack.mcmeta"}));
        QVERIFY(!redoReset(root, id, &error));
        QVERIFY(undoReset(root, id, &error));
        // Redo is refused once a restored file changed; nothing moves.
        QVERIFY(write(root + '/' + identical, "edited"));
        QVERIFY(!redoReset(root, id, &error));
        QVERIFY(error.contains(identical));
        QCOMPARE(read(root + '/' + identical), QByteArray("edited"));
        QVERIFY(QFileInfo::exists(root + '/' + chest));
        // A redo of a reset whose baseline vanished is refused as well.
        QVERIFY(atomicWrite(root + '/' + identical, vanillaStone, Revision::read(root + '/' + identical), &error));
        QVERIFY(!redoReset(root, id, &error, [](const QString &, QString *e) { *e = "corrupt"; return false; }));
        QVERIFY(QFileInfo::exists(root + '/' + identical));
    }
    void baselineUnavailableChangesNothing() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "26.1", true);
        QByteArray a, b;
        saveOverride(root, "assets/minecraft/textures/block/dirt.png", png(16, 16, 1), &a);
        saveOverride(root, "assets/minecraft/textures/block/sand.png", png(16, 16, 2), &b);
        const auto before = tree(root);
        QString error, id;
        const BaselineCheck sandMissing = [](const QString &asset, QString *e) {
            if (asset.endsWith("sand.png")) {
                *e = "object hash mismatch";
                return false;
            }
            return true;
        };
        QVERIFY(!applyReset(root, {"assets/minecraft/textures/block/dirt.png",
                                   "assets/minecraft/textures/block/sand.png"},
                            ResetKind::ResetToVanilla, sandMissing, &id, &error));
        QVERIFY(error.contains("the override was not changed"));
        QVERIFY(error.contains("object hash mismatch"));
        QVERIFY(id.isEmpty());
        QCOMPARE(tree(root), before);
        QVERIFY(!applyReset(root, {"assets/minecraft/textures/block/dirt.png"},
                            ResetKind::ResetToVanilla, {}, &id, &error));
        QCOMPARE(tree(root), before);
        // Invalid requests are refused before anything is touched.
        for (const auto &bad : {QString("assets/minecraft/textures/block/missing.png"),
                                QString("pack.mcmeta"), QString("assets/../pack.png"),
                                QString(".nova/project.json"), QString("assets/minecraft/x.txt")})
            QVERIFY2(!applyReset(root, {bad}, ResetKind::RemoveFromProject, {}, &id, &error), qPrintable(bad));
        QCOMPARE(tree(root), before);
        // Remove from Project needs no baseline.
        QVERIFY2(applyReset(root, {"assets/minecraft/textures/block/sand.png"},
                            ResetKind::RemoveFromProject, {}, &id, &error), qPrintable(error));
        QCOMPARE(resetJournals(root)[0].kind, ResetKind::RemoveFromProject);
        QVERIFY(QFileInfo::exists(root + "/assets/minecraft/textures/block/dirt.png"));
    }
    void undoNeverOverwrites() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "26.1", true);
        const auto asset = QString("assets/minecraft/textures/block/dirt.png");
        QByteArray saved;
        saveOverride(root, asset, png(16, 16, 1), &saved);
        QString error, id;
        QVERIFY(applyReset(root, {asset}, ResetKind::ResetToVanilla, available, &id, &error));
        // A new override created after the reset occupies the path.
        QByteArray newer;
        saveOverride(root, asset, png(16, 16, 2), &newer);
        QVERIFY(newer != saved);
        QVERIFY(!undoReset(root, id, &error));
        QVERIFY(error.contains(asset));
        QCOMPARE(read(root + '/' + asset), newer);
        QCOMPARE(state(root, id), QString("applied"));
        QCOMPARE(read(root + "/.nova/baseline-resets/" + id + "/0.retained"), saved);
        // Once the newer file was itself reset, the first journal can be undone again.
        QString second;
        QVERIFY(applyReset(root, {asset}, ResetKind::ResetToVanilla, available, &second, &error));
        QVERIFY2(undoReset(root, id, &error), qPrintable(error));
        QCOMPARE(read(root + '/' + asset), saved);
        QVERIFY(!undoReset(root, second, &error));
        QCOMPARE(read(root + "/.nova/baseline-resets/" + second + "/0.retained"), newer);
    }
    void externalChangeDuringResetIsRetained() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "26.1", true);
        const auto asset = QString("assets/minecraft/textures/block/dirt.png");
        QByteArray saved;
        saveOverride(root, asset, png(16, 16, 1), &saved);
        const QByteArray external = "changed by another program between read and rename";
        QString error, id;
        QVERIFY2(applyReset(root, {asset}, ResetKind::ResetToVanilla, available, &id, &error, [&] {
                     QVERIFY(write(root + '/' + asset, external));
                 }),
                 qPrintable(error));
        QCOMPARE(read(root + "/.nova/baseline-resets/" + id + "/0.retained"), external);
        const auto f = resetJournals(root)[0].files[0].toObject();
        QCOMPARE(f["sha256"].toString(), sha(external));
        QCOMPARE(f["before_sha256"].toString(), sha(saved));
        QVERIFY(undoReset(root, id, &error));
        QCOMPARE(read(root + '/' + asset), external);
    }
    void interruptedJournalRecovers() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "26.1", true);
        const QStringList assets{"assets/minecraft/textures/block/water_still.png",
                                 "assets/minecraft/textures/block/dirt.png"};
        QVERIFY(write(root + '/' + assets[0], png(16, 32, 7)));
        QVERIFY(write(root + '/' + assets[0] + ".mcmeta", animationMeta()));
        QVERIFY(write(root + '/' + assets[1], png(16, 16, 8)));
        const auto before = tree(root);
        QString error, id;
        QVERIFY(applyReset(root, assets, ResetKind::ResetToVanilla, available, &id, &error));
        // Simulate a crash after the first rename: the journal still says "prepared".
        const auto folder = root + "/.nova/baseline-resets/" + id;
        auto journal = QJsonDocument::fromJson(read(folder + "/journal.json")).object();
        journal["state"] = "prepared";
        QVERIFY(write(folder + "/journal.json", QJsonDocument(journal).toJson()));
        const auto files = journal["files"].toArray();
        QCOMPARE(files.size(), 3);
        for (int i = 1; i < files.size(); ++i) {
            const auto path = root + '/' + files[i].toObject()["path"].toString();
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QVERIFY(QFile::rename(folder + '/' + files[i].toObject()["retained"].toString(), path));
        }
        auto journals = resetJournals(root, &error);
        QCOMPARE(journals.size(), 1);
        QCOMPARE(journals[0].state, QString("prepared"));
        QVERIFY(journals[0].interrupted());
        QVERIFY(!undoReset(root, id, &error));
        QVERIFY(!applyReset(root, {assets[1]}, ResetKind::ResetToVanilla, available, nullptr, &error));
        QVERIFY(error.contains("Recover"));
        QVERIFY2(recoverReset(root, id, &error), qPrintable(error));
        QCOMPARE(state(root, id), QString("rolled_back"));
        auto after = tree(root);
        for (const auto &k : before.keys())
            QVERIFY2(after.contains(k) && after[k] == before[k], qPrintable(k));
        QVERIFY(recoverReset(root, id, &error)); // stable journals are left alone
        // Rolling back would overwrite a file that appeared meanwhile: the reset completes instead.
        QString second;
        QVERIFY(applyReset(root, {assets[1]}, ResetKind::ResetToVanilla, available, &second, &error));
        const auto folder2 = root + "/.nova/baseline-resets/" + second;
        journal = QJsonDocument::fromJson(read(folder2 + "/journal.json")).object();
        journal["state"] = "prepared";
        QVERIFY(write(folder2 + "/journal.json", QJsonDocument(journal).toJson()));
        QVERIFY(write(root + '/' + assets[1], "newcomer"));
        QVERIFY(recoverReset(root, second, &error));
        QCOMPARE(state(root, second), QString("applied"));
        QCOMPARE(read(root + '/' + assets[1]), QByteArray("newcomer"));
        QCOMPARE(read(folder2 + "/0.retained"), before[assets[1]]);
        // An interrupted undo is rolled back to "applied" with every file retained.
        QVERIFY(QFile::rename(root + '/' + assets[1], root + "/newcomer.bin"));
        QVERIFY(undoReset(root, second, &error));
        journal = QJsonDocument::fromJson(read(folder2 + "/journal.json")).object();
        journal["state"] = "undoing";
        QVERIFY(write(folder2 + "/journal.json", QJsonDocument(journal).toJson()));
        QVERIFY(recoverReset(root, second, &error));
        QCOMPARE(state(root, second), QString("applied"));
        QCOMPARE(read(folder2 + "/0.retained"), before[assets[1]]);
        QVERIFY(!QFileInfo::exists(root + '/' + assets[1]));
        // A journal naming files outside its own textures is rejected, never followed.
        journal["files"] = QJsonArray{QJsonObject{{"path", "pack.mcmeta"}, {"retained", "0.retained"},
                                                  {"sha256", QString(64, '0')}, {"size", 1}}};
        QVERIFY(write(folder2 + "/journal.json", QJsonDocument(journal).toJson()));
        QCOMPARE(state(root, second), QString("invalid"));
        QVERIFY(!recoverReset(root, second, &error));
        QVERIFY(!undoReset(root, second, &error));
        QVERIFY(QFileInfo::exists(root + "/pack.mcmeta"));
    }
    void animationJournalBlocksReset() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "26.1", true);
        const auto asset = QString("assets/minecraft/textures/block/water_still.png");
        QString error;
        auto doc = Document::fromBaseline(root + '/' + asset, png(16, 32, 4), animationMeta(), "26.1", &error);
        QVERIFY(doc);
        doc->draw({0, 0}, {0, 0}, qRgba(1, 1, 1, 255));
        doc->finish();
        QVERIFY(ensureAssetDirectory(root, asset, &error));
        const auto plan = planAnimationExport(*doc, 1, {}, &error);
        const auto journal = stageAnimationPair(root + '/' + asset, plan.png, plan.mcmeta, {}, {}, &error);
        QVERIFY(!journal.isEmpty());
        QVERIFY(finishAnimationPair(journal, PairAction::Retry, &error));
        // A second, unfinished pair export beside it blocks the reset.
        const auto pending = stageAnimationPair(root + '/' + asset, plan.png, plan.mcmeta,
                                                Revision::read(root + '/' + asset),
                                                Revision::read(root + '/' + asset + ".mcmeta"), &error);
        QVERIFY(!pending.isEmpty());
        const auto before = tree(root);
        QVERIFY(!applyReset(root, {asset}, ResetKind::ResetToVanilla, available, nullptr, &error));
        QVERIFY(error.contains("animation export"));
        QCOMPARE(tree(root), before);
        QVERIFY(finishAnimationPair(pending, PairAction::Rollback, &error));
        QString id;
        QVERIFY2(applyReset(root, {asset}, ResetKind::ResetToVanilla, available, &id, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(root + '/' + asset) && !QFileInfo::exists(root + '/' + asset + ".mcmeta"));
        // Completed pair journals stay beside the folder (so it is kept) and are never exported.
        const auto out = exported(root, tmp.path() + "/out");
        QCOMPARE(filesOnly(out), QStringList({"pack.mcmeta"}));
    }
    void pruneOnlyRemovesStableJournals() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "26.1", true);
        QString error;
        QStringList ids;
        for (int i = 0; i < 4; ++i) {
            const auto asset = QString("assets/minecraft/textures/block/b%1.png").arg(i);
            QVERIFY(write(root + '/' + asset, png(4, 4, i)));
            QString id;
            QVERIFY(applyReset(root, {asset}, ResetKind::ResetToVanilla, available, &id, &error));
            if (i == 1)
                QVERIFY(undoReset(root, id, &error));
            ids << id;
            QTest::qWait(5);
        }
        // An interrupted journal, a journal folder holding a subfolder and foreign data survive.
        const auto folder = root + "/.nova/baseline-resets/";
        auto interrupted = QJsonDocument::fromJson(read(folder + ids[0] + "/journal.json")).object();
        interrupted["state"] = "prepared";
        QVERIFY(write(folder + ids[0] + "/journal.json", QJsonDocument(interrupted).toJson()));
        QVERIFY(QDir().mkpath(folder + ids[2] + "/nested"));
        QVERIFY(write(folder + "notes/keep.txt", "foreign"));
        QVERIFY(write(root + "/.nova/other.json", "{}"));
        const auto project = tree(root + "/assets");
        pruneResets(root, 1, 30);
        QVERIFY(QFileInfo::exists(folder + ids[3] + "/journal.json")); // newest kept
        QVERIFY(QFileInfo::exists(folder + ids[0] + "/0.retained"));   // interrupted kept
        QVERIFY(QFileInfo::exists(folder + ids[2] + "/0.retained"));   // not plain files: kept
        QVERIFY(!QFileInfo::exists(folder + ids[1]));                   // stable beyond the limit
        QCOMPARE(read(folder + "notes/keep.txt"), QByteArray("foreign"));
        QCOMPARE(read(root + "/.nova/other.json"), QByteArray("{}"));
        QCOMPARE(read(root + "/.nova/project.json").isEmpty(), false);
        QCOMPARE(tree(root + "/assets"), project);
        QVERIFY(QFileInfo::exists(root + "/assets/minecraft/textures/block/b1.png"));
        // Age: a stable journal older than 30 days is pruned even within the count limit.
        auto old = QJsonDocument::fromJson(read(folder + ids[3] + "/journal.json")).object();
        old["updated"] = QDateTime::currentDateTimeUtc().addDays(-40).toString(Qt::ISODateWithMs);
        QVERIFY(write(folder + ids[3] + "/journal.json", QJsonDocument(old).toJson()));
        pruneResets(root, 64, 30);
        QVERIFY(!QFileInfo::exists(folder + ids[3]));
        QVERIFY(QFileInfo::exists(folder + ids[0] + "/journal.json"));
        QVERIFY2(recoverReset(root, ids[0], &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(root + "/assets/minecraft/textures/block/b0.png"));
    }
    void largeBaselineIsNeverMaterialized() {
        QTemporaryDir tmp;
        const auto root = create(tmp.path(), "pack", "26.1", true);
        const auto before = tree(root);
        // ~11,000 baseline entries exist only as a path -> object map.
        QMap<QString, QString> baseline;
        for (int i = 0; i < 11000; ++i)
            baseline.insert(QString("assets/minecraft/textures/block/generated_%1.png").arg(i, 5, 10, QChar('0')),
                            sha(QByteArray::number(i)));
        const auto bytes = png(16, 16, 77);
        int opened = 0;
        for (auto it = baseline.cbegin(); it != baseline.cend(); ++it) {
            if (opened++ % 500)
                continue;
            QString error;
            auto doc = Document::fromBaseline(root + '/' + it.key(), bytes, {}, "26.1", &error);
            QVERIFY2(doc && !doc->modified(), qPrintable(error));
        }
        const BaselineCheck lookup = [&](const QString &asset, QString *) { return baseline.contains(asset); };
        QString error;
        QVERIFY(!applyReset(root, {baseline.firstKey()}, ResetKind::ResetToVanilla, lookup, nullptr, &error));
        QCOMPARE(tree(root), before);
        QCOMPARE(fileCount(root), 2); // pack.mcmeta and .nova/project.json
        QCOMPARE(filesOnly(exported(root, tmp.path() + "/out")), QStringList({"pack.mcmeta"}));
    }
};
QTEST_GUILESS_MAIN(BaselineProjectTests)
#include "baseline_project_test.moc"
