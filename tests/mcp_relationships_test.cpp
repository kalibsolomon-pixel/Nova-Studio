// SPDX-License-Identifier: GPL-3.0-or-later
// MCP Phase 3: read-only vanilla texture cache tools, asset relationships and source-aware pack
// validation. Every fixture is synthetic: generated PNGs and hand-written JSON, no Mojang assets.
#include "inspection.h"
#include "modelpanel.h"
#include "protocol.h"
#include "relationships.h"
#include "resourcetools.h"
#include "validation.h"
#include "scope.h"
#include "vanilla_access.h"
#include "service.h"
#include "settings.h"
#include "vanilla/cache.h"
#include "window.h"
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace nova::mcp;
using nova::vanilla::BaselineCache;
using nova::vanilla::BaselineEntry;

namespace nova {
class EditorTestAccess {
  public:
    static void open(Window &window, const QString &file) { window.openTexture(file); }
    static std::shared_ptr<Document> active(Window &window) { return window.current() ? window.current()->document : nullptr; }
    static bool indexReady(Window &window) {
        return !window.indexBusy && !window.indexDebounce->isActive() && window.pendingChanges.isEmpty();
    }
};
} // namespace nova

namespace {
constexpr auto T = "assets/minecraft/textures/";
QString bridge() { return QString::fromUtf8(NOVA_MCP_TEST_BRIDGE); }
QByteArray bytes(const QString &file) {
    QFile input(file);
    return input.open(QIODevice::ReadOnly) ? input.readAll() : QByteArray();
}
void write(const QString &file, const QByteArray &data) {
    QVERIFY(QDir().mkpath(QFileInfo(file).path()));
    QFile output(file);
    QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(output.write(data), data.size());
}
QByteArray png(int width, int height, QRgb color) {
    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(color);
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return data;
}
QByteArray noise(int width, int height) {
    QImage image(width, height, QImage::Format_RGBA8888);
    QRandomGenerator random(20261108);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, random.generate() | 0xff000000u);
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return data;
}
QString sha(const QByteArray &data) { return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()); }
// Every file under `root` (hidden included) with its SHA-256: the cache and packs must not change.
QMap<QString, QString> fingerprint(const QString &root) {
    QMap<QString, QString> files;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto file = it.next();
        files.insert(QDir(root).relativeFilePath(file), sha(bytes(file)));
    }
    return files;
}
// A synthetic complete cache entry. `seed` changes the (fake) client hash and therefore the identity.
BaselineEntry populate(const QString &root, const QString &version, const QMap<QString, QByteArray> &files,
                       char seed = '1', QDateTime created = QDateTime::currentDateTimeUtc()) {
    nova::vanilla::VersionMetadata m;
    m.id = version;
    m.client = {QUrl("https://piston-data.mojang.com/v1/objects/x/client.jar"), QString(40, seed), 4096};
    m.assetIndexId = "29";
    m.assetIndex = {QUrl("https://piston-meta.mojang.com/v1/packages/x/29.json"), QString(40, '2'), 512};
    BaselineEntry entry;
    entry.root = root;
    entry.version = version;
    entry.identity = nova::vanilla::identityKey(m);
    entry.created = created;
    entry.source = QJsonObject{
        {"manifest_url", QString(nova::vanilla::defaultManifestUrl)},
        {"version_url", "https://piston-meta.mojang.com/v1/packages/x/" + version + ".json"},
        {"version_sha1", QString(40, '3')},
        {"client", QJsonObject{{"url", m.client.url.toString()}, {"sha1", m.client.sha1}, {"size", m.client.size}}},
        {"asset_index", QJsonObject{{"id", m.assetIndexId}, {"url", m.assetIndex.url.toString()},
                                    {"sha1", m.assetIndex.sha1}, {"size", m.assetIndex.size}}}};
    const BaselineCache cache(root);
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QString error;
        if (!cache.publishObject(it.value(), sha(it.value()), it.key(), &error))
            qWarning() << error;
        entry.files.insert(it.key(), {sha(it.value()), it.value().size(), "jar"});
    }
    QString error;
    if (!cache.writeManifest(entry, &error))
        qWarning() << error;
    return entry;
}
QMap<QString, QByteArray> cacheFiles() {
    return {{QString(T) + "block/stone.png", png(16, 16, qRgb(120, 120, 120))},
            {QString(T) + "block/dirt.png", png(16, 16, qRgb(110, 80, 50))},
            {QString(T) + "block/water_still.png", png(16, 64, qRgb(40, 60, 200))},
            {QString(T) + "block/water_still.png.mcmeta", "{\"animation\":{\"frametime\":2}}"},
            {QString(T) + "item/apple.png", png(16, 16, qRgb(200, 20, 20))},
            {QString(T) + "entity/equipment/humanoid/diamond.png", png(64, 32, qRgb(60, 220, 220))},
            {QString(T) + "gui/big.png", noise(320, 320)}};
}

// A synthetic snapshot of `root`, as Nova's GUI would provide it, for direct inspect() calls.
Snapshot snapshotOf(const QString &root, const QString &version, const QString &cacheRoot) {
    Snapshot s;
    s.root = QFileInfo(root).canonicalFilePath();
    s.rootIdentity = Scope(s.root).identity();
    s.idKey = randomToken().toLatin1();
    s.generation = randomToken();
    s.projectId = randomToken();
    s.version = version;
    s.index = nova::Index::scan(s.root);
    s.diskRecords = s.index.records;
    s.vanillaCacheRoot = cacheRoot;
    return s;
}
// The same stack entries Nova's model panel produces: loaded sources with their folder name, or the error.
void addStack(Snapshot &s, const QString &folder) {
    QString error;
    const auto source = nova::mc::ResourceSource::open(folder, s.version, &error);
    if (error.isEmpty()) {
        s.resourceSources.append(source);
        s.resourceStack.append({source.vanilla ? source.label : QFileInfo(source.root).fileName(), "loaded", {}, source.root,
                                source.vanilla});
    } else {
        s.resourceStack.append({QFileInfo(folder).fileName(), "unavailable", error, {}, false});
    }
}
QJsonObject run(const Snapshot &s, const QString &tool, QJsonObject args = {}) {
    args["generation"] = s.generation;
    args["projectId"] = s.projectId;
    const auto definition = [&] {
        for (const auto &value : toolDefinitions())
            if (value.toObject()["name"] == tool)
                return value.toObject();
        return QJsonObject{};
    }();
    if (!definition.isEmpty() && tool != "nova.pack.validate") {
        const auto invalid = checkArguments(args, definition["inputSchema"].toObject());
        if (!invalid.isEmpty())
            return toolError("invalid_arguments", invalid.message, {{"pointer", invalid.pointer}});
    }
    if (tool == "nova.pack.validate")
        args.remove("projectId");
    return inspect(s, tool, args, std::make_shared<std::atomic_bool>(false));
}
QString code(const QJsonObject &result) { return result["error"].toObject()["code"].toString(); }
// Owner-review samples of real results (NOVA_MCP_EVIDENCE); never needed for the checks.
void keep(const QString &name, const QJsonObject &result) {
    if (const auto folder = qEnvironmentVariable("NOVA_MCP_EVIDENCE"); !folder.isEmpty()) {
        QDir().mkpath(folder);
        QFile file(folder + "/phase3-" + name + ".json");
        if (file.open(QIODevice::WriteOnly))
            file.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    }
}
// A successful result has every required output field with its declared type.
bool conforms(const QString &tool, const QJsonObject &result, QString *why) {
    for (const auto &value : toolDefinitions()) {
        const auto definition = value.toObject();
        if (definition["name"] != tool)
            continue;
        const auto schema = definition["outputSchema"].toObject();
        const auto properties = schema["properties"].toObject();
        for (const auto &key : schema["required"].toArray()) {
            const auto name = key.toString();
            if (!result.contains(name)) {
                *why = "missing " + name;
                return false;
            }
            const auto type = properties[name].toObject()["type"];
            const auto types = type.isArray() ? type.toArray() : QJsonArray{type};
            const auto v = result[name];
            bool ok = false;
            for (const auto &t : types) {
                const auto n = t.toString();
                ok |= (n == "string" && v.isString()) || (n == "boolean" && v.isBool()) ||
                      (n == "object" && v.isObject()) || (n == "array" && v.isArray()) || (n == "null" && v.isNull()) ||
                      (n == "integer" && v.isDouble() && v.toDouble() == std::floor(v.toDouble()));
            }
            if (!ok) {
                *why = "wrong type for " + name;
                return false;
            }
        }
        return true;
    }
    *why = "unknown tool";
    return false;
}
QJsonArray rows(const QJsonObject &result, const QString &type) {
    QJsonArray found;
    for (const auto &value : result["relationships"].toArray())
        if (value.toObject()["type"] == type)
            found.append(value);
    return found;
}
QJsonObject row(const QJsonObject &result, const QString &type, const QString &key, const QString &value) {
    for (const auto &item : rows(result, type))
        if (item.toObject()[key] == value)
            return item.toObject();
    return {};
}
QJsonArray findings(const QJsonObject &report, const QString &rule) {
    QJsonArray found;
    for (const auto &value : report["diagnostics"].toArray())
        if (value.toObject()["rule"] == rule || value.toObject()["code"] == rule)
            found.append(value);
    return found;
}
QJsonObject finding(const QJsonObject &report, const QString &rule, const QString &path) {
    for (const auto &value : findings(report, rule))
        if (value.toObject()["path"] == path)
            return value.toObject();
    return {};
}
// No absolute or user path in what an agent receives.
bool pathFree(const QByteArray &json, const QString &private1, const QString &private2) {
    static const QRegularExpression drive("[A-Za-z]:[\\\\/]");
    const auto text = QString::fromUtf8(json);
    return !text.contains(drive) && !text.contains(private1, Qt::CaseInsensitive) &&
           !text.contains(private2, Qt::CaseInsensitive) && !text.contains(QDir::homePath(), Qt::CaseInsensitive);
}

// Bridge helpers (same protocol as the Phase 1 and 2 suites).
QByteArray request(int id, const QString &method, const QJsonObject &params = {}) {
    QJsonObject data{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (id >= 0)
        data["id"] = id;
    return QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n';
}
QByteArray call(int id, const QString &tool, const QJsonObject &args = {}) {
    return request(id, "tools/call", {{"name", tool}, {"arguments", args}});
}
void start(QProcess &process, const QString &code, const QString &program = bridge(), const QStringList &arguments = {}) {
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("NOVA_MCP_CODE", code);
    process.setProcessEnvironment(env);
    process.start(program, arguments);
    QVERIFY(process.waitForStarted());
}
QByteArray lastLine;
QJsonObject receive(QProcess &process, int timeout = 30000) {
    if (!QTest::qWaitFor([&] { return process.canReadLine() || process.state() == QProcess::NotRunning; }, timeout))
        return {};
    lastLine = process.readLine();
    return QJsonDocument::fromJson(lastLine).object();
}
QJsonObject payload(const QJsonObject &response) {
    const auto result = response["result"].toObject();
    if (!result["isError"].toBool())
        return result["structuredContent"].toObject();
    if (result.contains("structuredContent"))
        return {{"unexpected", "structured content on an error"}};
    return QJsonDocument::fromJson(result["content"].toArray().first().toObject()["text"].toString().toUtf8()).object();
}
void initialize(QProcess &process) {
    process.write(request(1, "initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}},
        {"clientInfo", QJsonObject{{"name", "Nova phase 3 tests"}, {"version", "1"}}}}));
    receive(process);
    process.write(request(-1, "notifications/initialized"));
}
QString authorize(QWidget &window) {
    auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
    if (!dialog) {
        window.findChild<QAction *>("mcpAccess")->trigger();
        dialog = window.findChild<QDialog *>("mcpAccessDialog");
    }
    dialog->findChild<QPushButton *>("mcpAuthorize")->click(); // read-only: these tools need no editing grant
    return window.findChild<Service *>()->connectionCode();
}
QJsonObject projectInfo(QProcess &client) {
    QJsonObject info;
    for (int attempt = 0; attempt < 100; ++attempt) {
        client.write(call(90, "nova.project.info"));
        info = payload(receive(client));
        if (!info.contains("error"))
            break;
        QTest::qWait(50);
    }
    return info;
}
} // namespace

class RelationshipsTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString base, root, cacheRoot, stack, fakeVanilla, other;

    // The synthetic pack: item definitions, blockstates, parent chains, overrides, faces, broken links.
    void makePack(const QString &pack) {
        write(pack + "/pack.mcmeta", "{\"pack\":{\"min_format\":[75,0],\"max_format\":[75,0],\"description\":\"Phase 3\"}}");
        const QMap<QString, QByteArray> json{
            {"assets/nova/items/ruby.json", "{\"model\":{\"type\":\"minecraft:model\",\"model\":\"nova:item/ruby\"}}"},
            {"assets/nova/items/gem.json", "{\"model\":{\"type\":\"minecraft:select\",\"property\":\"minecraft:display_context\", \"cases\":[{\"when\":\"gui\",\"model\":{\"type\":\"minecraft:model\",\"model\":\"nova:item/ruby\"}}], \"fallback\":{\"type\":\"minecraft:model\",\"model\":\"nova:item/gem_held\"}}}"},
            {"assets/nova/items/chest.json", "{\"model\":{\"type\":\"minecraft:special\",\"base\":\"nova:item/ruby\", \"model\":{\"type\":\"minecraft:chest\",\"texture\":\"minecraft:normal\"}}}"},
            {"assets/nova/models/item/base.json", "{\"parent\":\"minecraft:builtin/generated\", \"display\":{\"gui\":{\"rotation\":[0,0,0],\"translation\":[0,0,0],\"scale\":[1,1,1]}}}"},
            {"assets/nova/models/item/ruby.json", "{\"parent\":\"nova:item/base\",\"textures\":{\"layer0\":\"nova:item/ruby\"}}"},
            {"assets/nova/models/item/gem_held.json", "{\"parent\":\"nova:item/ruby\"}"},
            {"assets/nova/models/item/vanilla_parent.json", "{\"parent\":\"minecraft:item/generated\",\"textures\":{\"layer0\":\"nova:item/ruby\"}}"},
            {"assets/nova/blockstates/crystal.json", "{\"variants\":{\"facing=north\":{\"model\":\"nova:block/crystal\"}, \"facing=south\":{\"model\":\"nova:block/crystal\",\"y\":180}}}"},
            {"assets/nova/models/block/crystal_base.json", "{\"textures\":{\"side\":\"nova:block/plain\",\"particle\":\"#side\"}, \"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16],\"faces\":{\"north\":{\"texture\":\"#side\",\"uv\":[0,0,8,16]}, \"south\":{\"texture\":\"#side\"}}}]}"},
            {"assets/nova/models/block/crystal.json", "{\"parent\":\"nova:block/crystal_base\",\"textures\":{\"side\":\"nova:block/crystal\"}}"},
            {"assets/nova/models/block/flat.json", "{\"textures\":{\"all\":\"nova:block/plain\"},\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16], \"faces\":{\"north\":{\"texture\":\"#all\",\"uv\":[4,4,4,12]},\"south\":{\"texture\":\"#all\",\"uv\":[0,0,20,16]}}}]}"},
            {"assets/nova/models/block/broken.json", "{\"parent\":"},
            {"assets/nova/models/block/cycle_a.json", "{\"parent\":\"nova:block/cycle_b\"}"},
            {"assets/nova/models/block/cycle_b.json", "{\"parent\":\"nova:block/cycle_a\"}"},
            {"assets/nova/models/block/entity.json", "{\"parent\":\"minecraft:builtin/entity\"}"},
            {"assets/nova/models/block/uses_vanilla_texture.json", "{\"parent\":\"nova:block/crystal_base\",\"textures\":{\"side\":\"minecraft:block/stone\"}}"},
            {"assets/nova/models/block/shared.json", "{\"parent\":\"nova:block/crystal_base\",\"textures\":{\"side\":\"nova:block/crystal\"}}"},
            {"assets/nova/models/block/uses_stack.json", "{\"parent\":\"nova:block/crystal_base\",\"textures\":{\"side\":\"nova:block/stack_tex\"}}"},
            {"assets/nova/equipment/ruby.json", "{\"layers\":{\"humanoid\":[{\"texture\":\"nova:ruby\"}],\"humanoid_leggings\":[{\"texture\":\"nova:ruby\"}], \"wings\":[{\"use_player_texture\":true}]}}"}};
        for (auto it = json.cbegin(); it != json.cend(); ++it)
            write(pack + '/' + it.key(), it.value());
        for (int i = 0; i < 70; ++i)
            write(pack + QString("/assets/nova/models/block/deep_%1.json").arg(i),
                  i == 69 ? QByteArray("{}") : QString("{\"parent\":\"nova:block/deep_%1\"}").arg(i + 1).toUtf8());
        for (const auto &texture : {"item/ruby", "item/ruby_chestplate", "item/ruby_leggings", "block/plain", "block/crystal",
                                    "block/orphan", "entity/equipment/humanoid/ruby"})
            write(pack + "/assets/nova/textures/" + texture + ".png", png(16, 16, qRgb(200, 30, 60)));
        write(pack + "/assets/nova/textures/block/crystal.png.mcmeta", "{\"animation\":{\"frametime\":4}}");
        write(pack + "/assets/nova/textures/block/crystal.png", png(16, 32, qRgb(30, 200, 200)));
        write(pack + "/assets/minecraft/textures/block/dirt.png", png(16, 16, qRgb(100, 70, 40)));
    }

  private slots:
    void initTestCase() {
        QVERIFY(dir.isValid());
        base = QFileInfo(dir.path()).canonicalFilePath();
    }
    void init() {
        const auto slot = base + '/' + QTest::currentTestFunction();
        root = slot + "/phase3-pack";
        other = slot + "/other-pack";
        cacheRoot = slot + "/vanilla-baseline";
        stack = slot + "/stackpack";
        fakeVanilla = slot + "/fakevanilla";
        makePack(root);
        makePack(other);
        write(stack + "/pack.mcmeta", "{\"pack\":{\"pack_format\":75,\"description\":\"stack\"}}");
        write(stack + "/assets/nova/models/block/shared.json", "{\"textures\":{\"all\":\"nova:block/plain\"}}");
        write(stack + "/assets/nova/models/block/stack_only.json", "{\"parent\":\"nova:block/crystal_base\"}");
        write(stack + "/assets/nova/textures/block/stack_tex.png", png(16, 16, qRgb(1, 2, 3)));
        // Claims to be a vanilla resource source but does not match the pinned official client.
        write(fakeVanilla + "/nova-vanilla.json", "{\"version\":\"1.21.11\",\"client_sha1\":\"0000\",\"files\":{\"a\":\"b\"}}");
        write(fakeVanilla + "/assets/minecraft/models/item/generated.json", "{\"parent\":\"builtin/generated\"}");
        qApp->setProperty("settingsFile", slot + "/settings/studio.ini");
    }

    // List and fetch from a complete verified fixture cache: pagination, filters, identity, hashes.
    void vanillaListAndFetch() {
        const auto entry = populate(cacheRoot, "1.21.11", cacheFiles());
        const auto before = fingerprint(cacheRoot);
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        auto list = run(s, "nova.vanilla.list", {{"version", "1.21.11"}, {"limit", 2}});
        keep("vanilla-list", list);
        QVERIFY2(!list.contains("error"), QJsonDocument(list).toJson().constData());
        QString why;
        QVERIFY2(conforms("nova.vanilla.list", list, &why), qPrintable(why));
        QCOMPARE(list["identity"].toString(), entry.identity);
        QCOMPARE(list["selection"].toString(), "latest_verified");
        QCOMPARE(list["matched"].toInt(), 6); // PNGs only, the .mcmeta is metadata
        QCOMPARE(list["entryFiles"].toInt(), 7);
        QCOMPARE(list["textures"].toArray().size(), 2);
        QCOMPARE(list["nextOffset"].toInt(), 2);
        QCOMPARE(list["provenance"].toObject()["clientSha1"].toString(), QString(40, '1'));
        QCOMPARE(list["verification"].toObject()["objects"].toString(), "present_with_recorded_sizes");
        QCOMPARE(list["textures"].toArray().first().toObject()["path"].toString(), QString(T) + "block/dirt.png");
        auto page = run(s, "nova.vanilla.list", {{"version", "1.21.11"}, {"offset", 4}, {"limit", 2}});
        QCOMPARE(page["textures"].toArray().size(), 2);
        QVERIFY(page["nextOffset"].isNull());
        auto blocks = run(s, "nova.vanilla.list", {{"version", "1.21.11"}, {"prefix", "block/"}});
        QCOMPARE(blocks["matched"].toInt(), 3);
        auto animated = run(s, "nova.vanilla.list", {{"version", "1.21.11"}, {"animated", true}});
        QCOMPARE(animated["matched"].toInt(), 1);
        const auto water = animated["textures"].toArray().first().toObject();
        QCOMPARE(water["width"].toInt(), 16);
        QCOMPARE(water["height"].toInt(), 64);
        QCOMPARE(water["sha256"].toString(), sha(cacheFiles()[QString(T) + "block/water_still.png"]));
        QCOMPARE(water["animationMetadata"].toBool(), true);
        auto search = run(s, "nova.vanilla.list", {{"version", "1.21.11"}, {"search", "apple"}});
        QCOMPARE(search["matched"].toInt(), 1);
        QCOMPARE(run(s, "nova.vanilla.list", {{"version", "1.21.11"}, {"limit", 201}})["error"].toObject()["details"]
                     .toObject()["pointer"].toString(), "/limit");

        // Fetch: the PNG travels as an MCP image block, the metadata as structured content.
        const QString stone = QString(T) + "block/stone.png";
        write(root + '/' + stone, png(16, 16, qRgb(1, 1, 1))); // a project override of the same path
        s = snapshotOf(root, "1.21.11", cacheRoot);
        auto texture = run(s, "nova.vanilla.texture", {{"version", "1.21.11"}, {"path", stone}});
        QVERIFY2(!texture.contains("error"), QJsonDocument(texture).toJson().constData());
        const auto reply = toolReply(7, texture)["result"].toObject();
        keep("vanilla-texture-structured-content", reply["structuredContent"].toObject());
        QVERIFY2(conforms("nova.vanilla.texture", reply["structuredContent"].toObject(), &why), qPrintable(why));
        QVERIFY(!reply["structuredContent"].toObject().contains("_pngImage"));
        const auto content = reply["content"].toArray();
        QCOMPARE(content.size(), 2);
        QCOMPARE(content[1].toObject()["type"].toString(), "image");
        QCOMPARE(content[1].toObject()["mimeType"].toString(), "image/png");
        QCOMPARE(QByteArray::fromBase64(content[1].toObject()["data"].toString().toLatin1()), cacheFiles()[stone]);
        QCOMPARE(texture["sha256"].toString(), sha(cacheFiles()[stone]));
        QCOMPARE(texture["verification"].toObject()["content"].toString(), "sha256_verified");
        QCOMPARE(texture["projectOverride"].toBool(), true);
        QVERIFY(texture["animationMetadata"].isNull());
        auto flowing = run(s, "nova.vanilla.texture", {{"version", "1.21.11"}, {"path", QString(T) + "block/water_still.png"},
                                                       {"include", "metadata"}});
        QVERIFY(!flowing.contains("_pngImage"));
        QCOMPARE(flowing["imageIncluded"].toBool(), false);
        QCOMPARE(flowing["animationMetadata"].toObject()["json"].toObject()["animation"].toObject()["frametime"].toInt(), 2);
        // The cache was only read.
        QCOMPARE(fingerprint(cacheRoot), before);
        QVERIFY(!QFileInfo::exists(cacheRoot + "/cache.lock"));
        QVERIFY(!QFileInfo::exists(cacheRoot + "/staging"));
    }

    // Which entry is used: the project's baseline identity, an explicit identity, else the newest.
    void vanillaSelection() {
        const auto older = populate(cacheRoot, "1.21.11", cacheFiles(), '1', QDateTime::currentDateTimeUtc().addDays(-2));
        const auto newer = populate(cacheRoot, "1.21.11", {{QString(T) + "block/stone.png", png(16, 16, qRgb(5, 5, 5))}}, '4');
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        QCOMPARE(run(s, "nova.vanilla.list", {{"version", "1.21.11"}})["identity"].toString(), newer.identity);
        s.baselineVersion = "1.21.11";
        s.baselineIdentity = older.identity;
        auto bound = run(s, "nova.vanilla.list", {{"version", "1.21.11"}});
        QCOMPARE(bound["identity"].toString(), older.identity);
        QCOMPARE(bound["selection"].toString(), "project_baseline");
        auto pinned = run(s, "nova.vanilla.list", {{"version", "1.21.11"}, {"identity", newer.identity}});
        QCOMPARE(pinned["selection"].toString(), "requested_identity");
        QCOMPARE(pinned["matched"].toInt(), 1);
        auto unknown = run(s, "nova.vanilla.list", {{"version", "1.21.11"}, {"identity", QString(64, 'a')}});
        QCOMPARE(code(unknown), "vanilla_not_cached");
        QCOMPARE(unknown["error"].toObject()["details"].toObject()["pointer"].toString(), "/identity");
        // Version identity is explicit: another version is never substituted.
        auto otherVersion = run(s, "nova.vanilla.list", {{"version", "26.1"}});
        QCOMPARE(code(otherVersion), "vanilla_not_cached");
        QCOMPARE(otherVersion["error"].toObject()["details"].toObject()["version"].toString(), "26.1");
    }

    // Missing, unsupported, incomplete, corrupted and oversized cases; never a download or a change.
    void vanillaFailures() {
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        auto absent = run(s, "nova.vanilla.list", {{"version", "1.21.11"}});
        QCOMPARE(code(absent), "vanilla_not_cached");
        keep("vanilla-not-cached-error", absent);
        QVERIFY(absent["error"].toObject()["message"].toString().contains("never downloads"));
        QVERIFY(!absent["error"].toObject()["details"].toObject()["nextStep"].toString().isEmpty());
        QVERIFY(!QFileInfo::exists(cacheRoot)); // nothing was created, let alone downloaded
        QCOMPARE(code(run(s, "nova.vanilla.list", {{"version", "1.20.1"}})), "unsupported_version");
        QCOMPARE(code(run(s, "nova.vanilla.list", {{"version", "latest"}})), "invalid_arguments");

        auto entry = populate(cacheRoot, "1.21.11", cacheFiles());
        const QString stone = QString(T) + "block/stone.png";
        QCOMPARE(code(run(s, "nova.vanilla.texture", {{"version", "1.21.11"}, {"path", QString(T) + "block/none.png"}})),
                 "texture_not_in_cache");
        QCOMPARE(code(run(s, "nova.vanilla.texture", {{"version", "1.21.11"}, {"path", QString(T) + "block/../../../pack.png"}})),
                 "invalid_arguments");
        QCOMPARE(code(run(s, "nova.vanilla.texture", {{"version", "1.21.11"}, {"path", "C:/Windows/win.ini"}})),
                 "invalid_arguments");
        QCOMPARE(code(run(s, "nova.vanilla.texture", {{"version", "1.21.11"}, {"path", "assets/nova/textures/block/plain.png"}})),
                 "invalid_arguments");

        // Oversized: a clear error with the bounded alternative, which then succeeds.
        const QString big = QString(T) + "gui/big.png";
        QVERIFY(cacheFiles()[big].size() > VanillaImageLimit);
        auto tooLarge = run(s, "nova.vanilla.texture", {{"version", "1.21.11"}, {"path", big}});
        QCOMPARE(code(tooLarge), "texture_too_large");
        keep("vanilla-too-large-error", tooLarge);
        const auto details = tooLarge["error"].toObject()["details"].toObject();
        QCOMPARE(details["maxImageBytes"].toInteger(), VanillaImageLimit);
        QCOMPARE(details["alternative"].toString(), "metadata");
        QCOMPARE(details["width"].toInt(), 320);
        auto bounded = run(s, "nova.vanilla.texture", {{"version", "1.21.11"}, {"path", big}, {"include", "metadata"}});
        QVERIFY(!bounded.contains("error") && !bounded.contains("_pngImage"));
        QCOMPARE(bounded["bytes"].toInteger(), qint64(cacheFiles()[big].size()));

        // Same-size content corruption: listing (sizes) still works, the fetch refuses the bytes.
        const auto object = entry.objectPath(stone);
        QVERIFY(nova::vanilla::fs::setReadOnly(object, false));
        auto damaged = bytes(object);
        damaged[damaged.size() - 5] = char(damaged[damaged.size() - 5] ^ 0x55);
        write(object, damaged);
        const auto damagedBefore = fingerprint(cacheRoot);
        QVERIFY(!run(s, "nova.vanilla.list", {{"version", "1.21.11"}}).contains("error"));
        QCOMPARE(code(run(s, "nova.vanilla.texture", {{"version", "1.21.11"}, {"path", stone}})), "vanilla_integrity_failed");
        QCOMPARE(fingerprint(cacheRoot), damagedBefore); // the damaged object is neither repaired nor removed
        // Incomplete: an object is missing, so the entry is not used and not repaired.
        const auto apple = entry.objectPath(QString(T) + "item/apple.png");
        QVERIFY(nova::vanilla::fs::setReadOnly(apple, false));
        QVERIFY(QFile::remove(apple));
        const auto incompleteBefore = fingerprint(cacheRoot);
        auto incomplete = run(s, "nova.vanilla.list", {{"version", "1.21.11"}});
        QCOMPARE(code(incomplete), "vanilla_cache_invalid");
        keep("vanilla-incomplete-error", incomplete);
        QCOMPARE(incomplete["error"].toObject()["details"].toObject()["damaged"].toInt(), 1);
        QVERIFY(incomplete["error"].toObject()["message"].toString().contains("item/apple.png"));
        QCOMPARE(fingerprint(cacheRoot), incompleteBefore);
        // The damaged entry asked for by identity is reported as invalid, not as absent.
        QCOMPARE(code(run(s, "nova.vanilla.list", {{"version", "1.21.11"}, {"identity", entry.identity}})), "vanilla_cache_invalid");
        // Corrupt manifest (digest mismatch).
        const auto manifest = BaselineCache(cacheRoot).manifestPath("1.21.11", entry.identity);
        nova::vanilla::fs::setReadOnly(manifest, false);
        write(manifest, bytes(manifest).replace(QString(40, '3').toLatin1(), QString(40, '5').toLatin1()));
        QCOMPARE(code(run(s, "nova.vanilla.texture", {{"version", "1.21.11"}, {"path", stone}})), "vanilla_cache_invalid");
        QVERIFY(!QFileInfo::exists(cacheRoot + "/cache.lock"));
        // Errors never expose the cache location.
        QVERIFY(pathFree(QJsonDocument(incomplete).toJson(), cacheRoot, base));
    }

    // Item definition -> model -> parent -> resolved texture, with pointers, definers and conditional branches.
    void itemChain() {
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        auto result = run(s, "nova.asset.relationships", {{"path", "assets/nova/textures/item/ruby.png"}});
        QString why;
        QVERIFY2(conforms("nova.asset.relationships", result, &why), qPrintable(why + QJsonDocument(result).toJson()));
        keep("relationships-item-texture", result);
        QCOMPARE(result["kind"].toString(), "texture");
        QCOMPARE(result["found"].toBool(), true);
        QCOMPARE(result["providers"].toArray().first().toObject()["source"].toString(), "project");
        const auto uses = row(result, "model_uses_texture", "from", "assets/nova/models/item/ruby.json");
        QCOMPARE(uses["status"].toString(), "confirmed");
        const auto evidence = uses["evidence"].toObject();
        QCOMPARE(evidence["variables"].toArray().first().toObject()["definedBy"].toString(), "nova:item/ruby");
        QCOMPARE(evidence["parentChain"].toArray(), (QJsonArray{"minecraft:builtin/generated", "nova:item/base"}));
        QCOMPARE(evidence["faces"].toArray().first().toObject()["generatedLayer"].toInt(), 0);
        // Inherited by a child model that does not override layer0.
        const auto inherited = row(result, "model_uses_texture", "from", "assets/nova/models/item/gem_held.json");
        QCOMPARE(inherited["evidence"].toObject()["inherited"].toBool(), true);
        QCOMPARE(inherited["evidence"].toObject()["variables"].toArray().first().toObject()["definedBy"].toString(), "nova:item/ruby");
        // Selected by item definitions: plainly, and only in some states (select on display context).
        bool plain = false, conditional = false;
        for (const auto &value : rows(result, "selected_by_item_definition")) {
            const auto r = value.toObject();
            if (r["from"] == "assets/nova/items/ruby.json") {
                plain = true;
                QCOMPARE(r["status"].toString(), "confirmed");
                QCOMPARE(r["evidence"].toObject()["pointer"].toString(), "/model/model");
            }
            if (r["from"] == "assets/nova/items/gem.json") {
                conditional = true;
                QCOMPARE(r["status"].toString(), "conditional");
                QVERIFY(r["reason"].toString().contains("cannot evaluate"));
            }
        }
        QVERIFY(plain && conditional);

        // From the definition: the selection with its parent chain and resolved textures.
        auto definition = run(s, "nova.asset.relationships", {{"path", "assets/nova/items/ruby.json"}});
        keep("relationships-item-definition", definition);
        const auto selects = row(definition, "selects_model", "to", "assets/nova/models/item/ruby.json");
        QCOMPARE(selects["status"].toString(), "confirmed");
        const auto steps = selects["evidence"].toObject()["parentChain"].toArray();
        QCOMPARE(steps.size(), 3);
        QCOMPARE(steps.last().toObject()["id"].toString(), "minecraft:builtin/generated");
        QCOMPARE(selects["evidence"].toObject()["textures"].toArray().first().toObject()["texture"].toString(),
                 "assets/nova/textures/item/ruby.png");
        auto gem = run(s, "nova.asset.relationships", {{"path", "assets/nova/items/gem.json"}});
        QCOMPARE(row(gem, "selects_model", "to", "assets/nova/models/item/gem_held.json")["evidence"].toObject()["pointer"]
                     .toString(), "/model/fallback/model");
        QCOMPARE(row(gem, "selects_model", "to", "assets/nova/models/item/ruby.json")["evidence"].toObject()["pointer"]
                     .toString(), "/model/cases/0/model/model");
        // Special renderers are unsupported; their base model is still reported.
        auto chest = run(s, "nova.asset.relationships", {{"path", "assets/nova/items/chest.json"}});
        QVERIFY(!row(chest, "selects_model", "status", "unsupported").isEmpty());
        QCOMPARE(row(chest, "selects_model", "to", "assets/nova/models/item/ruby.json")["evidence"].toObject()["pointer"]
                     .toString(), "/model/base");
        // A model: chain, variables, display origin and dependents.
        auto model = run(s, "nova.asset.relationships", {{"path", "assets/nova/models/item/ruby.json"}});
        QCOMPARE(model["detail"].toObject()["valid"].toBool(), true);
        QCOMPARE(model["detail"].toObject()["display"].toArray().first().toObject()["definedBy"].toString(), "nova:item/base");
        QVERIFY(!row(model, "child_model", "from", "assets/nova/models/item/gem_held.json").isEmpty());
        QVERIFY(!row(model, "selected_by_item_definition", "from", "assets/nova/items/ruby.json").isEmpty());
        QCOMPARE(model["versionSource"].toString(), "project");
        QCOMPARE(code(run(s, "nova.asset.relationships", {{"path", "assets/nova/items/ruby.json"}, {"version", "26.1"}})),
                 "version_unavailable");
    }

    // Blockstate variants, faces with explicit and default UVs, and a parent texture variable overridden.
    void blockstatesFacesAndOverrides() {
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        auto crystal = run(s, "nova.asset.relationships", {{"path", "assets/nova/textures/block/crystal.png"}});
        keep("relationships-blockstate-faces", crystal);
        const auto uses = row(crystal, "model_uses_texture", "from", "assets/nova/models/block/crystal.json");
        QCOMPARE(uses["status"].toString(), "confirmed");
        const auto faces = uses["evidence"].toObject()["faces"].toArray();
        QCOMPARE(faces.size(), 2);
        QCOMPARE(faces[0].toObject()["face"].toString(), "north");
        QCOMPARE(faces[0].toObject()["uv"].toArray(), (QJsonArray{0, 0, 8, 16}));
        QCOMPARE(faces[0].toObject()["elementsDefinedBy"].toString(), "nova:block/crystal_base");
        QVERIFY(faces[1].toObject()["uv"].toString().startsWith("default"));
        // `particle` is "#side" in the parent; both resolve to this texture through the child's `side`.
        QMap<QString, QString> definers;
        for (const auto &value : uses["evidence"].toObject()["variables"].toArray())
            definers.insert(value.toObject()["variable"].toString(), value.toObject()["definedBy"].toString());
        QCOMPARE(definers.value("side"), "nova:block/crystal");
        QCOMPARE(definers.value("particle"), "nova:block/crystal_base");
        const auto blockstates = rows(crystal, "selected_by_blockstate");
        QCOMPARE(blockstates.size(), 2); // two variants, each a runtime state
        QCOMPARE(blockstates.first().toObject()["status"].toString(), "conditional");
        QVERIFY(blockstates.first().toObject()["evidence"].toObject()["pointer"].toString().startsWith("/variants/facing=north"));
        QVERIFY(!row(crystal, "animation_metadata", "to", "assets/nova/textures/block/crystal.png.mcmeta").isEmpty());
        QCOMPARE(crystal["detail"].toObject()["width"].toInt(), 16); // read from the PNG header
        QCOMPARE(crystal["detail"].toObject()["height"].toInt(), 32);
        // plain.png: used by the base model directly, replaced by the child's own `side`.
        auto plain = run(s, "nova.asset.relationships", {{"path", "assets/nova/textures/block/plain.png"}});
        QVERIFY(!row(plain, "model_uses_texture", "from", "assets/nova/models/block/crystal_base.json").isEmpty());
        const auto override = row(plain, "variable_override", "from", "assets/nova/models/block/crystal.json");
        QCOMPARE(override["status"].toString(), "confirmed");
        const auto replaced = override["evidence"].toObject()["overrides"].toArray().first().toObject();
        QCOMPARE(replaced["variable"].toString(), "side");
        QCOMPARE(replaced["inheritedFrom"].toString(), "nova:block/crystal_base");
        QCOMPARE(replaced["replacedWith"].toString(), "assets/nova/textures/block/crystal.png");
        // The model view lists every variable with its definer and the parent value it overrides.
        auto model = run(s, "nova.asset.relationships", {{"path", "assets/nova/models/block/crystal.json"}});
        bool side = false;
        for (const auto &value : model["detail"].toObject()["textureVariables"].toArray())
            if (value.toObject()["variable"] == "side") {
                side = true;
                QCOMPARE(value.toObject()["definedBy"].toString(), "nova:block/crystal");
                QCOMPARE(value.toObject()["overrides"].toArray(), QJsonArray{"nova:block/crystal_base"});
            }
        QVERIFY(side);
        // Animation metadata: frames from the spritesheet with Nova's parser for the version.
        auto meta = run(s, "nova.asset.relationships", {{"path", "assets/nova/textures/block/crystal.png.mcmeta"}});
        QCOMPARE(meta["detail"].toObject()["frames"].toInt(), 2);
        QCOMPARE(meta["detail"].toObject()["frameHeight"].toInt(), 16);
    }

    // Project files win over the resource stack; stack-only data is labelled with its stack position.
    void precedence() {
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        addStack(s, stack);
        addStack(s, fakeVanilla);
        QCOMPARE(s.resourceStack[1].status, "unavailable");
        auto shared = run(s, "nova.asset.relationships", {{"path", "assets/nova/models/block/shared.json"}});
        keep("relationships-precedence", shared);
        const auto providers = shared["providers"].toArray();
        QCOMPARE(providers.size(), 2);
        QCOMPARE(providers[0].toObject()["source"].toString(), "project");
        QCOMPARE(providers[0].toObject()["selected"].toBool(), true);
        QCOMPARE(providers[1].toObject()["source"].toString(), "resource_stack");
        QCOMPARE(providers[1].toObject()["stackIndex"].toInt(), 0);
        QCOMPARE(providers[1].toObject()["label"].toString(), "stackpack");
        QCOMPARE(providers[1].toObject()["selected"].toBool(), false);
        // The project's version is the one resolved (crystal), not the stack's (plain).
        QCOMPARE(row(shared, "texture_reference", "to", "assets/nova/textures/block/crystal.png")["status"].toString(), "confirmed");
        QVERIFY(row(shared, "texture_reference", "to", "assets/nova/textures/block/plain.png").isEmpty());
        auto stackOnly = run(s, "nova.asset.relationships", {{"path", "assets/nova/models/block/stack_only.json"}});
        QCOMPARE(stackOnly["providers"].toArray().first().toObject()["source"].toString(), "resource_stack");
        const auto steps = rows(stackOnly, "parent_chain").first().toObject()["evidence"].toObject()["steps"].toArray();
        QCOMPARE(steps[0].toObject()["source"].toObject()["source"].toString(), "resource_stack");
        QCOMPARE(steps[1].toObject()["source"].toObject()["source"].toString(), "project");
        auto usesStack = run(s, "nova.asset.relationships", {{"path", "assets/nova/models/block/uses_stack.json"}});
        QCOMPARE(row(usesStack, "texture_reference", "to", "assets/nova/textures/block/stack_tex.png")["source"].toObject()["source"]
                     .toString(), "resource_stack");
        const auto stackState = shared["sources"].toObject()["resourceStack"].toArray();
        QCOMPARE(stackState.size(), 2);
        QCOMPARE(stackState[1].toObject()["status"].toString(), "unavailable");
        QVERIFY(stackState[1].toObject()["reason"].toString().contains("does not match"));
        QCOMPARE(shared["sources"].toObject()["vanillaModelSource"].toBool(), false); // the fake was refused
        QVERIFY(pathFree(QJsonDocument(shared).toJson(), stack, base));
        // An open texture draft is noted, but its relationship data comes from the saved file.
        s.drafts.insert("assets/nova/textures/block/crystal.png", {"texture", "r-open", false, {}, true});
        const auto open = run(s, "nova.asset.relationships", {{"path", "assets/nova/textures/block/crystal.png"}})["providers"]
            .toArray().first().toObject();
        QCOMPARE(open["source"].toString(), "project");
        QCOMPARE(open["unsavedDraft"].toBool(), true);
        QCOMPARE(open["revision"].toString(), "r-open");
    }

    // Missing, invalid, cyclic, unsupported and too deep: each is reported with its reason, never guessed.
    void unresolved() {
        populate(cacheRoot, "1.21.11", cacheFiles());
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        auto vanillaParent = run(s, "nova.asset.relationships", {{"path", "assets/nova/models/item/vanilla_parent.json"}});
        keep("relationships-unresolved-vanilla-parent", vanillaParent);
        const auto chain = rows(vanillaParent, "parent_chain").first().toObject();
        QCOMPARE(chain["status"].toString(), "unresolved");
        QVERIFY(chain["reason"].toString().contains("textures only"));
        QCOMPARE(chain["evidence"].toObject()["steps"].toArray().last().toObject()["id"].toString(), "minecraft:item/generated");
        QCOMPARE(vanillaParent["detail"].toObject()["valid"].toBool(), false);
        QVERIFY(!vanillaParent["detail"].toObject().contains("faces")); // no guessed geometry
        // A texture that only the vanilla cache has: no model is invented for it.
        auto apple = run(s, "nova.asset.relationships", {{"path", QString(T) + "item/apple.png"}});
        keep("relationships-cache-only-texture", apple);
        QCOMPARE(apple["providers"].toArray().first().toObject()["source"].toString(), "vanilla_texture_cache");
        const auto unknown = rows(apple, "model_uses_texture");
        QCOMPARE(unknown.size(), 1);
        QCOMPARE(unknown.first().toObject()["status"].toString(), "unresolved");
        QVERIFY(unknown.first().toObject()["reason"].toString().contains("does not guess"));
        QCOMPARE(apple["sources"].toObject()["vanillaTextureCache"].toObject()["providesModels"].toBool(), false);
        QCOMPARE(apple["detail"].toObject()["width"].toInt(), 16);
        // Cache-only animation metadata is labelled with the cache as its source.
        auto water = run(s, "nova.asset.relationships", {{"path", QString(T) + "block/water_still.png"}});
        const auto meta = row(water, "animation_metadata", "to", QString(T) + "block/water_still.png.mcmeta");
        QCOMPARE(meta["status"].toString(), "confirmed");
        QCOMPARE(meta["source"].toObject()["source"].toString(), "vanilla_texture_cache");
        bool limitation = false;
        for (const auto &text : apple["limitations"].toArray())
            limitation |= text.toString().contains("textures only");
        QVERIFY(limitation);
        // Invalid JSON, cycles, built-in renderers and an over-long chain.
        QCOMPARE(rows(run(s, "nova.asset.relationships", {{"path", "assets/nova/models/block/broken.json"}}), "parent_chain")
                     .first().toObject()["status"].toString(), "invalid");
        const auto cycle = rows(run(s, "nova.asset.relationships", {{"path", "assets/nova/models/block/cycle_a.json"}}), "parent_chain")
                               .first().toObject();
        QCOMPARE(cycle["status"].toString(), "cyclic");
        QCOMPARE(cycle["evidence"].toObject()["steps"].toArray().size(), 3);
        QCOMPARE(rows(run(s, "nova.asset.relationships", {{"path", "assets/nova/models/block/entity.json"}}), "parent_chain")
                     .first().toObject()["status"].toString(), "unsupported");
        const auto deep = rows(run(s, "nova.asset.relationships", {{"path", "assets/nova/models/block/deep_0.json"}}), "parent_chain")
                              .first().toObject();
        QCOMPARE(deep["status"].toString(), "unsupported");
        QCOMPARE(deep["evidence"].toObject()["steps"].toArray().size(), ParentChainLimit);
        // A blockstate that no source provides.
        auto missing = run(s, "nova.asset.relationships", {{"path", "assets/minecraft/blockstates/stone.json"}});
        QCOMPARE(missing["found"].toBool(), false);
        QCOMPARE(rows(missing, "definition").first().toObject()["status"].toString(), "unresolved");
        QVERIFY(rows(missing, "definition").first().toObject()["reason"].toString().contains("cannot resolve"));
        QCOMPARE(code(run(s, "nova.asset.relationships", {{"path", "assets/nova/models/../../pack.mcmeta"}})), "invalid_arguments");
        QCOMPARE(code(run(s, "nova.asset.relationships", {{"path", "C:/pack/assets/x.json"}})), "invalid_arguments");
    }

    // Equipment and armor companions: confirmed only by data and only for the verified version.
    void companions() {
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        auto equipment = run(s, "nova.asset.relationships", {{"path", "assets/nova/equipment/ruby.json"}});
        keep("relationships-equipment", equipment);
        QCOMPARE(row(equipment, "equipment_layer_texture", "to", "assets/nova/textures/entity/equipment/humanoid/ruby.png")["status"]
                     .toString(), "confirmed");
        QCOMPARE(row(equipment, "equipment_layer_texture", "to",
                     "assets/nova/textures/entity/equipment/humanoid_leggings/ruby.png")["status"].toString(), "unresolved");
        QVERIFY(!row(equipment, "equipment_layer_texture", "reason", "The layer uses the player's own texture, not a pack file.").isEmpty());
        auto worn = run(s, "nova.asset.relationships", {{"path", "assets/nova/textures/entity/equipment/humanoid/ruby.png"}});
        const auto layer = row(worn, "equipment_layer_texture", "from", "assets/nova/equipment/ruby.json");
        QCOMPARE(layer["status"].toString(), "confirmed");
        QCOMPARE(layer["evidence"].toObject()["pointer"].toString(), "/layers/humanoid/0/texture");
        const auto icon = row(worn, "companion_asset", "to", "assets/nova/textures/item/ruby_chestplate.png");
        QCOMPARE(icon["status"].toString(), "possible");
        auto chestplate = run(s, "nova.asset.relationships", {{"path", "assets/nova/textures/item/ruby_chestplate.png"}});
        QCOMPARE(row(chestplate, "companion_asset", "to", "assets/nova/textures/entity/equipment/humanoid/ruby.png")["status"]
                     .toString(), "possible");
        // Other versions: the same data is only a suggestion.
        auto later = snapshotOf(root, "26.1", cacheRoot);
        auto unverified = run(later, "nova.asset.relationships", {{"path", "assets/nova/equipment/ruby.json"}});
        const auto suggestion = row(unverified, "equipment_layer_texture", "to", "assets/nova/textures/entity/equipment/humanoid/ruby.png");
        QCOMPARE(suggestion["status"].toString(), "possible");
        QVERIFY(suggestion["reason"].toString().contains("1.21.11 only"));
    }

    // Saved files and open drafts, sources, revisions, conflicts, stale index, stack availability, read-only.
    void generatedPreviewClassification_data() {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("errorText");
        QTest::addColumn<bool>("draft");
        const QByteArray valid = "{\"parent\":\"nova:item/base\",\"textures\":{\"layer0\":\"nova:item/test\"}}";
        QTest::newRow("valid-disk") << valid << QString() << false;
        QTest::newRow("valid-open-draft") << valid << QString() << true;
        QTest::newRow("malformed-generated-json")
            << QByteArray("{\"parent\":\"nova:item/base\",\"textures\":") << QString("Malformed JSON") << false;
        QTest::newRow("missing-parent")
            << QByteArray("{\"parent\":\"nova:item/missing\"}") << QString("Missing parent/model:") << false;
        QTest::newRow("missing-generated-texture")
            << QByteArray("{\"parent\":\"nova:item/base\",\"textures\":{\"layer0\":\"nova:item/missing\"}}")
            << QString("Missing texture:") << false;
        QTest::newRow("unresolved-generated-variable")
            << QByteArray("{\"parent\":\"nova:item/base\",\"textures\":{\"layer0\":\"#missing\"}}")
            << QString("Missing texture variable:") << false;
        QTest::newRow("invalid-generated-texture-reference")
            << QByteArray("{\"parent\":\"nova:item/base\",\"textures\":{\"layer0\":\"BAD:invalid\"}}")
            << QString("Invalid texture resource:") << false;
        QTest::newRow("invalid-generated-elements")
            << QByteArray("{\"parent\":\"nova:item/base\",\"elements\":[{}]}")
            << QString("invalid cuboid coordinates/faces") << false;
    }
    void generatedPreviewClassification() {
        QFETCH(QByteArray, json);
        QFETCH(QString, errorText);
        QFETCH(bool, draft);
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        const QString path = "assets/nova/models/item/test.json";
        write(pack.path() + "/pack.mcmeta", "{\"pack\":{\"min_format\":[75,0],\"max_format\":[75,0],\"description\":\"classification\"}}");
        write(pack.path() + "/assets/nova/models/item/base.json", "{\"parent\":\"minecraft:builtin/generated\"}");
        write(pack.path() + "/assets/nova/textures/item/test.png", png(16, 16, qRgb(50, 120, 200)));
        // The open draft must be used instead of this different (but valid) saved model.
        write(pack.path() + '/' + path, draft ? QByteArray("{}") : json);
        auto s = snapshotOf(pack.path(), "1.21.11", cacheRoot);
        if (draft) {
            s.models.insert(path, QJsonDocument::fromJson(json).object());
            s.revisions.insert(path, "r-generated-preview");
            s.drafts.insert(path, {"model", "r-generated-preview", false, QCryptographicHash::hash(QByteArray("{}"), QCryptographicHash::Sha256), true});
        }
        const auto before = fingerprint(pack.path());
        auto library = std::make_shared<nova::mc::Library>(s.version);
        library->update(s.root, s.index);
        const auto native = nova::mc::ValidationEngine().run(s.root, s.index, *library, {path}, {}, {}, {}, s.models);
        const auto severity = errorText.isEmpty() ? nova::mc::Severity::Information : nova::mc::Severity::Error;
        nova::mc::Diagnostic expected;
        bool found = false;
        for (const auto &d : native.diagnostics) {
            QCOMPARE(d.file, path);
            if (errorText.isEmpty())
                QVERIFY(d.severity != nova::mc::Severity::Error);
            if ((errorText.isEmpty() && d.message.contains("flat layers")) ||
                (!errorText.isEmpty() && d.message.contains(errorText))) {
                expected = d;
                found = true;
            }
        }
        QVERIFY2(found, qPrintable(native.text()));
        QCOMPARE(expected.severity, severity);
        if (errorText.isEmpty()) {
            QCOMPARE(expected.code, "preview_limitation");
            QVERIFY(expected.message.contains("3D preview"));
            QVERIFY(expected.message.contains("cannot faithfully render"));
            QVERIFY(expected.message.contains("model may be valid"));
            QVERIFY(expected.message.contains("does not indicate invalid pack data"));
        }
        QVERIFY(native.text().contains(nova::mc::severityName(severity) + " | " + path));
        bool serialized = false;
        for (const auto &value : native.json()["diagnostics"].toArray())
            serialized |= value.toObject() == expected.json();
        QVERIFY(serialized);

        // Exercise the actual native panel's rows, including their file and explanatory text.
        nova::ResourceTools panel;
        panel.context = [&] {
            return nova::ResourceContext{s.root, path, "assets/nova", s.index, library, s.models};
        };
        panel.findChild<QComboBox *>("validationScope")->setCurrentIndex(0);
        panel.validate();
        const auto list = panel.findChild<QTreeWidget *>("problemsList");
        QTRY_COMPARE_WITH_TIMEOUT(list->topLevelItemCount(), native.diagnostics.size(), 10000);
        bool presented = false;
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            const auto row = list->topLevelItem(i);
            if (row->text(3) == expected.message) {
                QCOMPARE(row->text(0), nova::mc::severityName(severity));
                QCOMPARE(row->text(1), path);
                QCOMPARE(row->text(4), expected.suggestion);
                presented = true;
            }
        }
        QVERIFY(presented);

        const auto id = assetIdentifier(s, path);
        const auto packResult = run(s, "nova.pack.validate", {{"assetId", id}, {"limit", 200}});
        const auto changes = run(s, "nova.workflow.validate_changes", {{"assetIds", QJsonArray{id}}, {"limit", 200}});
        for (const auto &result : {packResult, changes}) {
            QVERIFY2(!result.contains("error"), QJsonDocument(result).toJson().constData());
            int matches = 0;
            for (const auto &value : result["diagnostics"].toArray()) {
                const auto d = value.toObject();
                if (errorText.isEmpty())
                    QVERIFY(d["severity"] != "Error");
                if (d["message"] != expected.message)
                    continue;
                ++matches;
                QCOMPARE(d["severity"].toString(), nova::mc::severityName(expected.severity));
                QCOMPARE(d["code"].toString(), expected.code);
                QCOMPARE(d["assetId"].toString(), id);
                if (result.contains("sources")) {
                    QCOMPARE(d["path"].toString(), path);
                    QCOMPARE(d["source"].toString(), draft ? QString("open_draft") : QString("disk"));
                    if (draft)
                        QCOMPARE(d["revision"].toString(), "r-generated-preview");
                    if (errorText.isEmpty())
                        QCOMPARE(d["rule"].toString(), expected.code);
                    QCOMPARE(d["nextStep"].toString(), expected.suggestion);
                }
            }
            QCOMPARE(matches, 1);
        }
        // Verify the MCP structured and text content both preserve the shared classification.
        const auto reply = toolReply(7, packResult)["result"].toObject();
        QCOMPARE(reply["structuredContent"].toObject(), packResult);
        QCOMPARE(QJsonDocument::fromJson(reply["content"].toArray().first().toObject()["text"].toString().toUtf8()).object(), packResult);
        QCOMPARE(changes["validationSources"].toObject()["models"].toString(), "live_when_open");
        if (draft)
            QCOMPARE(changes["revisions"].toArray().first().toObject()["revision"].toString(), "r-generated-preview");
        QCOMPARE(fingerprint(pack.path()), before);
    }


    void validation() {
        populate(cacheRoot, "1.21.11", cacheFiles());
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        addStack(s, stack);
        addStack(s, fakeVanilla);
        const QString crystal = "assets/nova/textures/block/crystal.png";
        const QString flat = "assets/nova/models/block/flat.json";
        // An open texture draft opened from the original bytes; the file then changes on disk.
        s.drafts.insert(crystal, {"texture", "r-texture", false, QCryptographicHash::hash(bytes(root + '/' + crystal),
                                                                                        QCryptographicHash::Sha256), true});
        write(root + '/' + crystal, png(16, 32, qRgb(9, 9, 9)));
        // An open model draft: its live source (not the disk file) is validated.
        auto live = QJsonDocument::fromJson(bytes(root + '/' + flat)).object();
        auto elements = live["elements"].toArray();
        auto element = elements[0].toObject();
        auto faces = element["faces"].toObject();
        faces["east"] = QJsonObject{{"texture", "#all"}, {"uv", QJsonArray{-2, 0, 16, 16}}};
        element["faces"] = faces;
        elements[0] = element;
        live["elements"] = elements;
        s.models.insert(flat, live);
        s.drafts.insert(flat, {"model", "r-model", false, QCryptographicHash::hash(bytes(root + '/' + flat),
                                                                                   QCryptographicHash::Sha256), true});
        s.unavailableDrafts.insert("assets/nova/textures/item/ruby.png", "a canvas interaction is in progress");
        // A draft whose saved file could not be hashed when it was opened: no comparison, a limitation instead.
        s.drafts.insert("assets/nova/textures/block/plain.png", {"texture", "r-unhashed", false, {}, true});
        const auto packBefore = fingerprint(root);
        const auto cacheBefore = fingerprint(cacheRoot);
        auto report = run(s, "nova.pack.validate", {{"limit", 200}});
        QVERIFY2(!report.contains("error"), QJsonDocument(report).toJson().constData());
        QString why;
        QVERIFY2(conforms("nova.pack.validate", report, &why), qPrintable(why));
        keep("validate-drafts-stack-cache", report);
        QCOMPARE(fingerprint(root), packBefore);
        QCOMPARE(fingerprint(cacheRoot), cacheBefore);
        QCOMPARE(report["minecraftVersion"].toString(), "1.21.11");
        QVERIFY(!report["truncated"].toBool());
        int total = 0;
        for (const auto &value : report["counts"].toObject())
            total += value.toInt();
        QCOMPARE(total, report["diagnostics"].toArray().size());
        for (const auto &value : report["diagnostics"].toArray()) {
            const auto f = value.toObject();
            QVERIFY2(!f["rule"].toString().isEmpty() && f.contains("nextStep") && f["evidence"].isArray(),
                     QJsonDocument(f).toJson().constData());
            QVERIFY2(QStringList({"disk", "open_draft", "resource_stack"}).contains(f["source"].toString()),
                     QJsonDocument(f).toJson().constData());
        }
        // Drafts: conflict with disk, live model findings carry the draft revision, unreadable drafts reported.
        const auto conflict = finding(report, "draft_disk_conflict", crystal);
        QCOMPARE(conflict["source"].toString(), "open_draft");
        QCOMPARE(conflict["revision"].toString(), "r-texture");
        QCOMPARE(conflict["severity"].toString(), "Warning");
        const auto uv = finding(report, "uv_outside_texture", flat);
        QVERIFY2(!uv.isEmpty(), QJsonDocument(report).toJson().constData());
        bool eastFace = false;
        for (const auto &value : findings(report, "uv_outside_texture"))
            eastFace |= value.toObject()["field"] == "/elements/0/faces/east/uv" && value.toObject()["source"] == "open_draft" &&
                        value.toObject()["revision"] == "r-model";
        QVERIFY(eastFace); // only present in the draft, not on disk
        QCOMPARE(finding(report, "uv_zero_area", flat)["field"].toString(), "/elements/0/faces/north/uv");
        QCOMPARE(finding(report, "draft_unavailable", "assets/nova/textures/item/ruby.png")["source"].toString(), "open_draft");
        QCOMPARE(report["drafts"].toObject()["complete"].toBool(), false);
        // Stale index entry for the file changed after indexing.
        QCOMPARE(finding(report, "stale_index_entry", crystal)["source"].toString(), "disk");
        // Vanilla references: unchecked, not errors; textures found in the verified cache are information.
        const auto parent = finding(report, "parent_source_unavailable", "assets/nova/models/item/vanilla_parent.json");
        QCOMPARE(parent["severity"].toString(), "Unsupported");
        QVERIFY(parent["message"].toString().contains("textures only"));
        QCOMPARE(parent["code"].toString(), "model"); // the Phase 1 code is kept for compatibility
        const auto stone = finding(report, "texture_in_vanilla_cache", "assets/nova/models/block/uses_vanilla_texture.json");
        QCOMPARE(stone["severity"].toString(), "Information");
        // Project-namespace references still fail honestly.
        QVERIFY(!finding(report, "parent_cycle_or_depth", "assets/nova/models/block/cycle_a.json").isEmpty());
        QCOMPARE(finding(report, "preview_limitation", "assets/nova/models/item/ruby.json")["severity"].toString(), "Information");
        QCOMPARE(finding(report, "draft_unavailable", "assets/nova/textures/item/ruby.png")["nextStep"].toString().isEmpty(), false);
        QVERIFY(!finding(report, "malformed_json", "assets/nova/models/block/broken.json").isEmpty());
        // Unreferenced textures: a warning for the pack's own namespace only.
        QCOMPARE(finding(report, "unreferenced_texture", "assets/nova/textures/block/orphan.png")["severity"].toString(), "Warning");
        QVERIFY(finding(report, "unreferenced_texture", "assets/minecraft/textures/block/dirt.png").isEmpty());
        // Resource stack: the refused source is reported as unavailable, not silently skipped.
        const auto unavailable = findings(report, "resource_stack_unavailable");
        QCOMPARE(unavailable.size(), 1);
        QCOMPARE(unavailable.first().toObject()["source"].toString(), "resource_stack");
        QCOMPARE(report["sources"].toObject()["resourceStackComplete"].toBool(), false);
        // Companion suggestion (1.21.11): leggings icon without its worn texture; labelled a suggestion.
        const auto companion = finding(report, "companion_suggestion", "assets/nova/textures/item/ruby_leggings.png");
        QCOMPARE(companion["severity"].toString(), "Information");
        QCOMPARE(companion["evidence"].toArray().first().toObject()["confidence"].toString(), "suggestion");
        QVERIFY(finding(report, "companion_suggestion", "assets/nova/textures/item/ruby_chestplate.png").isEmpty());
        QStringList limitations;
        for (const auto &text : report["limitations"].toArray())
            limitations << text.toString();
        QVERIFY2(limitations.join('\n').contains("1 minecraft block/item texture"), qPrintable(limitations.join('\n')));
        QVERIFY(limitations.join('\n').contains("assets/nova/textures/block/plain.png was opened from a file Nova could not hash"));
        QVERIFY(finding(report, "draft_disk_conflict", "assets/nova/textures/block/plain.png").isEmpty());
        QVERIFY(pathFree(QJsonDocument(report).toJson(), stack, base));
        // Another version: pack metadata evidence names the version and Nova's registry.
        auto later = snapshotOf(root, "26.1", cacheRoot);
        auto target = run(later, "nova.pack.validate");
        const auto pack = findings(target, "pack_target").first().toObject();
        QCOMPARE(pack["evidence"].toArray().first().toObject()["selectedVersion"].toString(), "26.1");
        QCOMPARE(pack["evidence"].toArray().first().toObject()["source"].toString(), "nova_version_registry");
        // Without the vanilla cache entry, a vanilla texture reference is unchecked rather than an error.
        auto uncached = snapshotOf(root, "1.21.11", base + "/no-cache");
        auto report2 = run(uncached, "nova.pack.validate", {{"limit", 200}});
        QCOMPARE(finding(report2, "texture_source_unavailable", "assets/nova/models/block/uses_vanilla_texture.json")["severity"]
                     .toString(), "Unsupported");
        QVERIFY(!QFileInfo::exists(base + "/no-cache"));
    }

    // Tool definitions, schema compliance of errors and the documentation's exact definitions.
    void schemas() {
        const auto definitions = toolDefinitions();
        QCOMPARE(definitions.size(), 39);
        QMap<QString, QJsonObject> runtime;
        for (const auto &value : definitions)
            runtime.insert(value.toObject()["name"].toString(), value.toObject());
        for (const auto &name : {"nova.vanilla.list", "nova.vanilla.texture", "nova.asset.relationships"}) {
            const auto tool = runtime.value(name);
            QVERIFY2(tool["annotations"].toObject()["readOnlyHint"].toBool(), name);
            QVERIFY(tool.contains("outputSchema"));
            QVERIFY(!tool["inputSchema"].toObject()["properties"].toObject().contains("file"));
        }
        // The last three definitions are the new ones; every earlier one keeps its position.
        QCOMPARE(definitions[36].toObject()["name"].toString(), "nova.vanilla.list");
        QCOMPARE(definitions[38].toObject()["name"].toString(), "nova.asset.relationships");
        // The published reference embeds the exact definitions of the new tools and of nova.pack.validate.
        const QStringList published{"nova.vanilla.list", "nova.vanilla.texture", "nova.asset.relationships",
                                    "nova.pack.validate"};
        if (const auto dump = qEnvironmentVariable("NOVA_ASSET_SCHEMA_DUMP"); !dump.isEmpty()) {
            // Regenerates the reference blocks: one indented definition per tool.
            QFile file(dump);
            QVERIFY(file.open(QIODevice::WriteOnly));
            for (const auto &name : published)
                file.write(QJsonDocument(runtime.value(name)).toJson(QJsonDocument::Indented) + "\n");
        }
        const auto doc = QString::fromUtf8(bytes(QString::fromUtf8(NOVA_ASSET_TOOLS_DOC)));
        const QRegularExpression block("```json\\s*\\n(\\{.*?\\n)```", QRegularExpression::DotMatchesEverythingOption);
        QStringList documented;
        for (auto it = block.globalMatch(doc); it.hasNext();) {
            const auto published = QJsonDocument::fromJson(it.next().captured(1).toUtf8()).object();
            if (!published.contains("inputSchema"))
                continue;
            const auto name = published["name"].toString();
            documented << name;
            QVERIFY2(QJsonDocument(runtime.value(name)).toJson(QJsonDocument::Compact) ==
                         QJsonDocument(published).toJson(QJsonDocument::Compact), qPrintable(name));
        }
        QCOMPARE(documented, published);
        // Errors are text-only: no structured content that a client could check against a success schema.
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        for (const auto &[tool, args] : QList<QPair<QString, QJsonObject>>{
                 {"nova.vanilla.list", {{"version", "1.21.11"}}},
                 {"nova.vanilla.texture", {{"version", "1.20.1"}, {"path", QString(T) + "block/stone.png"}}},
                 {"nova.asset.relationships", {{"path", "assets/nova/items/ruby.json"}, {"version", "26.3"}}}}) {
            const auto result = toolReply(1, run(s, tool, args))["result"].toObject();
            QVERIFY2(result["isError"].toBool() && !result.contains("structuredContent"), qPrintable(tool));
            QCOMPARE(result["content"].toArray().size(), 1);
        }
    }

    // Large results stay inside the response limit: rows are capped and flagged, never cut mid-way.
    void responseBudget() {
        // 300 models with a vanilla parent, six faces each, all binding one texture.
        for (int i = 0; i < 300; ++i)
            write(root + QString("/assets/nova/models/block/many_%1.json").arg(i, 3, 10, QChar('0')),
                  QString("{\"parent\":\"minecraft:block/cube_all\",\"textures\":{\"all\":\"nova:block/plain\","
                          "\"top\":\"nova:block/absent_%1\"},\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16],\"faces\":{"
                          "\"north\":{\"texture\":\"#all\",\"uv\":[0,0,16,16]},\"south\":{\"texture\":\"#all\"},"
                          "\"east\":{\"texture\":\"#all\"},\"west\":{\"texture\":\"#all\"},\"up\":{\"texture\":\"#top\"},"
                          "\"down\":{\"texture\":\"#all\"}}}]}").arg(i).toUtf8());
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        auto report = run(s, "nova.pack.validate", {{"limit", 200}});
        QVERIFY(!report.contains("error"));
        QCOMPARE(report["truncated"].toBool(), true);
        int total = 0;
        for (const auto &value : report["counts"].toObject())
            total += value.toInt();
        QVERIFY(total > report["diagnostics"].toArray().size()); // every finding counted, not all listed
        const auto line = QJsonDocument(toolReply(1, report)).toJson(QJsonDocument::Compact);
        QVERIFY2(line.size() < ResponseLimit, qPrintable(QString::number(line.size())));
        auto related = run(s, "nova.asset.relationships", {{"path", "assets/nova/textures/block/plain.png"}});
        QCOMPARE(related["truncated"].toBool(), true);
        QVERIFY(related["relationships"].toArray().size() <= RelationshipLimit);
        const auto relatedLine = QJsonDocument(toolReply(2, related)).toJson(QJsonDocument::Compact);
        QVERIFY2(relatedLine.size() < ResponseLimit, qPrintable(QString::number(relatedLine.size())));
        auto model = run(s, "nova.asset.relationships", {{"path", "assets/nova/models/block/crystal.json"}});
        QVERIFY(QJsonDocument(toolReply(3, model)).toJson(QJsonDocument::Compact).size() < ResponseLimit);
    }

    // An exceeded model catalog budget is reported; absent models become unknown, never errors.
    void catalogBudget() {
        const auto big = base + '/' + QTest::currentTestFunction() + "/bigstack";
        write(big + "/pack.mcmeta", "{\"pack\":{\"pack_format\":75,\"description\":\"big\"}}");
        for (int i = 0; i < 8300; ++i)
            write(big + QString("/assets/filler/models/block/f%1.json").arg(i), "{}");
        write(root + "/assets/zz/models/block/late.json", "{\"parent\":\"zz:block/nowhere\"}");
        auto s = snapshotOf(root, "1.21.11", cacheRoot);
        addStack(s, big);
        QCOMPARE(s.resourceStack.first().status, "loaded");
        auto report = run(s, "nova.pack.validate", {{"limit", 200}});
        QCOMPARE(report["sources"].toObject()["modelCatalogComplete"].toBool(), false);
        QCOMPARE(finding(report, "model_catalog_incomplete", "assets/zz/models/block/late.json")["severity"].toString(),
                 "Unsupported");
        for (const auto &value : report["diagnostics"].toArray())
            QVERIFY2(value.toObject()["rule"] != "unresolved_parent" && value.toObject()["rule"] != "unreferenced_texture",
                     QJsonDocument(value.toObject()).toJson().constData());
        bool noted = false;
        for (const auto &text : report["limitations"].toArray())
            noted |= text.toString().contains("catalog budget");
        QVERIFY(noted);
        auto related = run(s, "nova.asset.relationships", {{"path", "assets/zz/models/block/late.json"}});
        QCOMPARE(related["sources"].toObject()["modelCatalogComplete"].toBool(), false);
        QVERIFY(rows(related, "parent_chain").first().toObject()["reason"].toString().contains("read only in part"));
    }

    // The real window and bridge: provider data, authorization, revocation, project switch, official SDK.
    void endToEnd() {
        populate(cacheRoot, "1.21.11", cacheFiles());
        qputenv("NOVA_VANILLA_BASELINE_ROOT", QDir::toNativeSeparators(cacheRoot).toUtf8());
        // The user's resource stack for this project and version, as Models › Resource stack saves it.
        const auto key = "modelResources/" +
            QString::fromLatin1(QCryptographicHash::hash(QFileInfo(root).canonicalFilePath().toCaseFolded().toUtf8(),
                                                         QCryptographicHash::Sha256).toHex()) + "/1.21.11";
        {
            QSettings prefs(nova::Settings(qApp->property("settingsFile").toString()).fileName(), QSettings::IniFormat);
            prefs.setValue(key, QStringList{stack, fakeVanilla, base + "/missing-folder"});
        }
        const auto cacheBefore = fingerprint(cacheRoot);
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        nova::EditorTestAccess::open(window, root + "/assets/nova/textures/block/plain.png");
        auto doc = nova::EditorTestAccess::active(window);
        QVERIFY(doc);
        QVERIFY(window.documents().paint(doc, doc->activeLayer(), {1, 1}, {1, 1}, qRgba(0, 0, 255, 255), true));
        QProcess client;
        start(client, authorize(window));
        initialize(client);
        const auto info = projectInfo(client);
        QCOMPARE(info["minecraftVersion"].toString(), "1.21.11");
        const QJsonObject scope{{"generation", info["generation"]}, {"projectId", info["projectId"]}};
        auto with = [&](QJsonObject args) {
            for (auto it = scope.begin(); it != scope.end(); ++it)
                args[it.key()] = it.value();
            return args;
        };
        client.write(call(2, "nova.vanilla.list", with({{"version", "1.21.11"}, {"prefix", "block/"}})));
        auto list = payload(receive(client));
        QVERIFY2(!list.contains("error"), lastLine.constData());
        QCOMPARE(list["matched"].toInt(), 3);
        QVERIFY(pathFree(lastLine, cacheRoot, base));
        client.write(call(3, "nova.vanilla.texture", with({{"version", "1.21.11"}, {"path", QString(T) + "block/stone.png"}})));
        const auto fetched = receive(client);
        QCOMPARE(fetched["result"].toObject()["content"].toArray().last().toObject()["type"].toString(), "image");
        QVERIFY(pathFree(lastLine, cacheRoot, base));
        client.write(call(4, "nova.asset.relationships", with({{"path", "assets/nova/models/block/shared.json"}})));
        auto shared = payload(receive(client));
        QVERIFY2(!shared.contains("error"), lastLine.constData());
        const auto stackState = shared["sources"].toObject()["resourceStack"].toArray();
        QCOMPARE(shared["providers"].toArray().size(), 2);
        QCOMPARE(shared["providers"].toArray()[1].toObject()["label"].toString(), "stackpack");
        QCOMPARE(stackState.size(), 3);
        QCOMPARE(stackState[0].toObject()["status"].toString(), "loaded");
        QCOMPARE(stackState[1].toObject()["status"].toString(), "unavailable");
        QCOMPARE(stackState[2].toObject()["label"].toString(), "missing-folder");
        QVERIFY(pathFree(lastLine, stack, base));
        client.write(call(5, "nova.pack.validate", {{"generation", scope["generation"]}, {"limit", 200}}));
        auto report = payload(receive(client, 60000));
        QVERIFY2(!report.contains("error"), lastLine.constData());
        bool live = false;
        for (const auto &value : report["diagnostics"].toArray())
            live |= value.toObject()["code"] == "live_texture_checked" &&
                    value.toObject()["path"] == "assets/nova/textures/block/plain.png" &&
                    value.toObject()["source"] == "open_draft" && !value.toObject()["revision"].toString().isEmpty();
        QVERIFY(live);
        const QString generatedPath = "assets/nova/models/item/ruby.json";
        const auto preview = finding(report, "preview_limitation", generatedPath);
        QCOMPARE(preview["code"].toString(), "preview_limitation");
        QCOMPARE(preview["severity"].toString(), "Information");
        QCOMPARE(preview["source"].toString(), "disk");
        QVERIFY(preview["message"].toString().contains("cannot faithfully render"));
        client.write(call(6, "nova.workflow.validate_changes", with({
            {"assetIds", QJsonArray{preview["assetId"]}}, {"limit", 200}})));
        const auto checked = payload(receive(client, 60000));
        QVERIFY2(!checked.contains("error"), lastLine.constData());
        const auto checkedPreview = findings(checked, "preview_limitation");
        QCOMPARE(checkedPreview.size(), 1);
        const auto same = checkedPreview.first().toObject();
        QCOMPARE(same["severity"], preview["severity"]);
        QCOMPARE(same["message"], preview["message"]);
        QCOMPARE(same["code"], preview["rule"]);
        QCOMPARE(same["assetId"], preview["assetId"]);
        QCOMPARE(findings(report, "resource_stack_unavailable").size(), 2);
        QVERIFY(pathFree(lastLine, stack, base));
        QCOMPARE(fingerprint(cacheRoot), cacheBefore);

        // Revocation ends the connection.
        window.findChild<QDialog *>("mcpAccessDialog")->findChild<QPushButton *>("mcpRevoke")->click();
        QTRY_COMPARE(client.state(), QProcess::NotRunning);

        // The official SDK validates every structured result against the declared output schemas.
        if (const auto python = qEnvironmentVariable("NOVA_MCP_SDK_PYTHON"); !python.isEmpty()) {
            QProcess sdk;
            qputenv("NOVA_PHASE3_TEXTURE", "assets/nova/textures/block/crystal.png");
            start(sdk, authorize(window), python, {QString::fromUtf8(NOVA_MCP_TEST_SDK), bridge(), "phase3"});
            QTRY_COMPARE_WITH_TIMEOUT(sdk.state(), QProcess::NotRunning, 120000);
            const auto output = sdk.readAllStandardOutput();
            qInfo().noquote() << "SDK phase3" << output;
            QVERIFY2(sdk.exitCode() == 0, sdk.readAllStandardError().constData());
            const auto summary = QJsonDocument::fromJson(output.trimmed().split('\n').last()).object();
            QCOMPARE(summary["tools"].toInt(), 39);
        } else {
            qInfo() << "NOVA_MCP_SDK_PYTHON is not set: official SDK part skipped";
        }

        // Project switch: the old connection cannot read either project's cache view or relationships.
        QProcess switched;
        start(switched, authorize(window));
        initialize(switched);
        const auto current = projectInfo(switched);
        const QJsonObject old{{"generation", current["generation"]}, {"projectId", current["projectId"]}};
        QVERIFY(window.documents().dispatch(doc, nova::Command::Undo));
        QTimer discard;
        discard.setInterval(50);
        QObject::connect(&discard, &QTimer::timeout, &window, [] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || !box->text().startsWith("Save changes"))
                return;
            for (auto button : box->buttons())
                if (box->buttonRole(button) == QMessageBox::DestructiveRole)
                    return button->click();
        });
        discard.start();
        auto path = window.findChild<QLineEdit *>("projectPath");
        path->setText(other);
        QTest::keyClick(path, Qt::Key_Return);
        QTRY_COMPARE(window.findChild<Service *>()->status(), "Project changed");
        discard.stop();
        int id = 50;
        for (const auto &[tool, args] : QList<QPair<QString, QJsonObject>>{
                 {"nova.vanilla.list", {{"version", "1.21.11"}}},
                 {"nova.vanilla.texture", {{"version", "1.21.11"}, {"path", QString(T) + "block/stone.png"}}},
                 {"nova.asset.relationships", {{"path", "assets/nova/items/ruby.json"}}}}) {
            auto full = args;
            for (auto it = old.begin(); it != old.end(); ++it)
                full[it.key()] = it.value();
            switched.write(call(++id, tool, full));
            QCOMPARE(payload(receive(switched))["error"].toObject()["code"].toString(), "stale_project");
        }
        switched.closeWriteChannel();
        QTRY_COMPARE(switched.state(), QProcess::NotRunning);
        QCOMPARE(fingerprint(cacheRoot), cacheBefore);
        qunsetenv("NOVA_VANILLA_BASELINE_ROOT");
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    RelationshipsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "mcp_relationships_test.moc"
