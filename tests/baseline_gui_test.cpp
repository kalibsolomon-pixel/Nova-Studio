// SPDX-License-Identifier: GPL-3.0-or-later
// Vanilla texture baseline in the workspace: merged asset browser, virtual baseline documents,
// override creation, Reset to Vanilla / Remove from Project with undo, MCP isolation, legacy
// projects, the new-project choice (cached and mock-downloaded) and sparse export.
// Every texture is a synthetic PNG generated here, the cache lives in a temporary folder and the
// only "Mojang" is a loopback mock server. No game file and no real network is ever used.
#include "baseline_ui.h"
#include "project.h"
#include "project_config.h"
#include "recovery.h"
#include "theme.h"
#include "window.h"
#include "vanilla/cache.h"
#include "vanilla/overlay.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtCore/private/qzipwriter_p.h>
#include <QtTest>
#include <QtWidgets>

namespace nova {
class EditorTestAccess {
  public:
    static QString project(Window &w) { return w.project; }
    static bool busy(Window &w) { return w.indexBusy; }
    static bool loaded(Window &w) { return bool(w.baseline.entry); }
    static bool loading(Window &w) { return w.baseline.loading; }
    static bool hasRef(Window &w) { return bool(w.baseline.ref); }
    static Index workspace(Window &w, QSet<QString> *provisional = nullptr) { return w.workspaceIndex(provisional); }
    static const Index &collection(Window &w) { return w.collection; }
    static void open(Window &w, const QString &relative) { w.openTexture(w.project + '/' + relative); }
    static void action(Window &w, const QString &name) { w.action(name); }
    static bool backed(Window &w, const Document &doc) { return w.baselineBacked(doc); }
    static void reset(Window &w, const QStringList &assets, ResetKind kind) { w.baselineReset(assets, kind); }
    static int history(Window &w) { return int(w.resetHistory.size()); }
    static int cursor(Window &w) { return w.resetCursor; }
    static void safety(Window &w) { w.backgroundSafety(); }
    static bool external(Window &w, QUuid id) { return w.externalRevisions.contains(id); }
    static QAction *context(Window &w, QMenu &menu, const QString &relative) {
        return w.addBaselineContextAction(menu, relative);
    }
    static void select(Window &w, const QString &relative) { w.nav.selected = relative; }
    static QTreeView *tree(Window &w) { return w.tree; }
    static QString origin(Window &w) {
        w.updateInspector();
        return w.inspectorValues.value("origin")->isVisibleTo(w.inspectorPages) ? w.inspectorValues.value("origin")->text()
                                                                               : QString();
    }
    static void actions(Window &w) { w.updateBaselineActions(); }
};
} // namespace nova
using namespace nova;
using namespace nova::vanilla;

namespace {
constexpr auto P = "assets/minecraft/textures/";
QColor seedColor(int seed) { return QColor((seed * 37) & 255, (seed * 11 + 40) & 255, (seed * 53 + 7) & 255); }
QByteArray png(int seed, int w = 16, int h = 16) {
    QImage image(w, h, QImage::Format_RGBA8888);
    image.fill(seedColor(seed));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}
QString sha(const QByteArray &bytes, QCryptographicHash::Algorithm a = QCryptographicHash::Sha256) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, a).toHex());
}
QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}
bool write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
// The complete recursive tree (folders end in '/'), as relative paths and content hashes.
QMap<QString, QString> tree(const QString &root) {
    QMap<QString, QString> out;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const auto relative = QDir(root).relativeFilePath(it.filePath());
        out[it.fileInfo().isDir() ? relative + '/' : relative] = it.fileInfo().isDir() ? QString() : sha(read(it.filePath()));
    }
    return out;
}
QStringList files(const QString &root) {
    QStringList out;
    for (const auto &k : tree(root).keys())
        if (!k.endsWith('/'))
            out << k;
    return out;
}
QStringList exported(const QString &root, const QString &destination) {
    QString error;
    if (!Project::exportMinecraft(root, destination, &error))
        return {"<export failed: " + error + ">"};
    return files(destination);
}
QColor centre(const QIcon &icon) { return icon.pixmap(64, 64).toImage().pixelColor(8, 8); }

// Synthetic cache entry: `count` filler textures, three named ones and an animated one.
BaselineEntry populate(const QString &root, const QString &version, int count) {
    VersionMetadata m;
    m.id = version;
    m.client = {QUrl("https://piston-data.mojang.com/v1/objects/x/client.jar"), QString(40, '1'), 4096};
    m.assetIndexId = "29";
    m.assetIndex = {QUrl("https://piston-meta.mojang.com/v1/packages/x/29.json"), QString(40, '2'), 512};
    BaselineEntry entry;
    entry.root = root;
    entry.version = version;
    entry.identity = identityKey(m);
    entry.created = QDateTime::currentDateTimeUtc();
    entry.source = QJsonObject{
        {"manifest_url", QString(defaultManifestUrl)},
        {"version_url", "https://piston-meta.mojang.com/v1/packages/x/" + version + ".json"},
        {"version_sha1", QString(40, '3')},
        {"client", QJsonObject{{"url", m.client.url.toString()}, {"sha1", m.client.sha1}, {"size", m.client.size}}},
        {"asset_index", QJsonObject{{"id", m.assetIndexId}, {"url", m.assetIndex.url.toString()},
                                    {"sha1", m.assetIndex.sha1}, {"size", m.assetIndex.size}}}};
    const BaselineCache cache(root);
    auto add = [&](const QString &relative, const QByteArray &bytes) {
        QString error;
        if (!cache.publishObject(bytes, sha(bytes), relative, &error))
            qWarning() << error;
        entry.files.insert(relative, {sha(bytes), bytes.size(), "jar"});
    };
    add(QString(P) + "block/stone.png", png(1));
    add(QString(P) + "item/apple.png", png(2));
    add(QString(P) + "gui/sprites/hud/heart.png", png(3));
    add(QString(P) + "block/water_still.png", png(4, 16, 64));
    add(QString(P) + "block/water_still.png.mcmeta", R"({"animation":{"frametime":2}})");
    for (int i = 0; i < count; ++i)
        add(QString(P) + QString("block/b%1.png").arg(i, 3, 10, QChar('0')), png(10 + i));
    QString error;
    if (!cache.writeManifest(entry, &error))
        qWarning() << error;
    return entry;
}
GameVersion game(const QString &id) {
    for (const auto &v : gameVersions())
        if (v.id == id)
            return v;
    return {};
}
QString createProject(const QString &root, const BaselineEntry *entry, const QString &version = "1.21.11") {
    QString error;
    const auto ref = entry ? std::optional(ui::baselineRef(*entry)) : std::nullopt;
    if (!Project::create(root, "Pack", game(version), "A synthetic pack", "minecraft", {}, true, &error,
                         ref ? &*ref : nullptr))
        qWarning() << error;
    return QDir(root).absolutePath();
}
// Clicks a button (by standard button or text) on the next message boxes; records their text.
class Answer : public QTimer {
  public:
    QStringList seen;
    Answer(const QString &button, int times = 1) {
        setInterval(15);
        connect(this, &QTimer::timeout, this, [this, button, times] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || seen.size() >= times)
                return;
            for (auto b : box->buttons())
                if (b->text().remove('&') == button) {
                    seen << box->text();
                    b->click();
                    return;
                }
        });
        start();
    }
};
QString recordText(const QAbstractItemModel *model, const QModelIndex &index) {
    return model->data(index, Qt::ToolTipRole).toString();
}
QStringList modelPaths(const AssetModel *model, const QModelIndex &parent = {}) {
    QStringList out;
    for (int r = 0; r < model->rowCount(parent); ++r) {
        const auto i = model->index(r, 0, parent);
        out << model->pathFor(i);
        out += modelPaths(model, i);
    }
    return out;
}
testing::DownloadPolicy loopback() { return {{}, true, 256LL * 1024 * 1024, "NovaStudio-Test"}; }

// A loopback stand-in for piston-meta / piston-data / resources.download.minecraft.net.
class MockMojang : public QTcpServer {
  public:
    QMap<QString, QByteArray> routes;
    QStringList requests;
    QString identity;
    MockMojang() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto socket = nextPendingConnection())
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const auto path = QString::fromLatin1(socket->readAll().split(' ').value(1));
                    requests << path;
                    const bool found = routes.contains(path);
                    const auto body = found ? routes.value(path) : QByteArray("missing");
                    socket->write("HTTP/1.1 " + QByteArray(found ? "200" : "404") + " X\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
        });
        listen(QHostAddress::LocalHost);
    }
    QString url(const QString &path) const { return QString("http://127.0.0.1:%1%2").arg(serverPort()).arg(path); }
    void publish(const QString &id) {
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        {
            QZipWriter zip(&buffer);
            zip.addFile("net/minecraft/client/Main.class", QByteArray(64, 'c'));
            zip.addFile("assets/minecraft/models/block/stone.json", "{}");
            zip.addFile(QString(P) + "block/stone.png", png(60));
            zip.addFile(QString(P) + "block/dirt.png", png(61));
            zip.addFile(QString(P) + "block/lava_still.png", png(62, 16, 32));
            zip.addFile(QString(P) + "block/lava_still.png.mcmeta", R"({"animation":{}})");
            zip.close();
        }
        const auto jar = buffer.data();
        const auto clientPath = "/v1/objects/" + sha(jar, QCryptographicHash::Sha1) + "/client.jar";
        routes[clientPath] = jar;
        const auto index = QJsonDocument(QJsonObject{{"objects", QJsonObject{}}}).toJson();
        const auto indexPath = "/v1/packages/" + sha(index, QCryptographicHash::Sha1) + "/29.json";
        routes[indexPath] = index;
        const auto version = QJsonDocument(QJsonObject{
            {"id", id}, {"type", "release"},
            {"downloads", QJsonObject{{"client", QJsonObject{{"url", url(clientPath)},
                                                             {"sha1", sha(jar, QCryptographicHash::Sha1)},
                                                             {"size", jar.size()}}}}},
            {"assetIndex", QJsonObject{{"id", "29"}, {"url", url(indexPath)},
                                       {"sha1", sha(index, QCryptographicHash::Sha1)},
                                       {"size", index.size()}, {"totalSize", 1}}}}).toJson();
        const auto versionPath = "/v1/packages/" + sha(version, QCryptographicHash::Sha1) + '/' + id + ".json";
        routes[versionPath] = version;
        routes["/mc/game/version_manifest_v2.json"] = QJsonDocument(QJsonObject{
            {"latest", QJsonObject{{"release", id}}},
            {"versions", QJsonArray{QJsonObject{{"id", id}, {"type", "release"}, {"url", url(versionPath)},
                                                {"time", "2026-11-07T00:00:00+00:00"},
                                                {"sha1", sha(version, QCryptographicHash::Sha1)}}}}}).toJson();
        if (const auto ref = parseVersionManifest(routes["/mc/game/version_manifest_v2.json"], id, nullptr, loopback()))
            if (const auto meta = parseVersionJson(version, *ref, nullptr, loopback()))
                identity = identityKey(*meta);
    }
};
} // namespace

class BaselineGuiTests : public QObject {
    Q_OBJECT
    QTemporaryDir temp;
    QString cacheRoot;
    BaselineEntry entry;
    std::unique_ptr<Window> window;
    int settings = 0;

    std::unique_ptr<Window> open(const QString &root) {
        qApp->setProperty("settingsFile", temp.filePath(QString("settings-%1/studio.ini").arg(++settings)));
        QElapsedTimer time;
        time.start();
        auto w = std::make_unique<Window>(root, QString(), time);
        // Optional native evidence runs: 900x700 minimum and 1100x720 common test geometries.
        const auto requested = qEnvironmentVariable("NOVA_BASELINE_WINDOW_SIZE").split('x');
        if (requested.size() == 2) {
            const QSize size(requested[0].toInt(), requested[1].toInt());
            if (size.isValid())
                w->resize(size);
        }
        w->show();
        return w;
    }
    bool capture(QWidget &widget, const QString &name) {
        const auto output = qEnvironmentVariable("NOVA_BASELINE_CAPTURE_DIR");
        if (output.isEmpty())
            return true; // normal regression runs do not write screenshots
        if (!QDir().mkpath(output))
            return false;
        widget.activateWindow();
        QTest::qWait(250);
        const auto prefix = qEnvironmentVariable("NOVA_BASELINE_CAPTURE_LABEL");
        const auto base = output + '/' + (prefix.isEmpty() ? name : prefix + '-' + name);
        if (!widget.grab().save(base + ".png"))
            return false;
        QJsonObject metadata{{"window_on_screen", widget.screen() ? widget.screen()->name() : QString()},
                             {"device_pixel_ratio", widget.devicePixelRatioF()},
                             {"logical_dpi", widget.screen() ? widget.screen()->logicalDotsPerInch() : 0.0},
                             {"width", widget.width()}, {"height", widget.height()},
                             {"synthetic_fixtures", true}};
        QFile file(base + ".json");
        return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(metadata).toJson()) > 0;
    }
    void ready(Window &w, bool baseline = true) {
        QTRY_VERIFY_WITH_TIMEOUT(!EditorTestAccess::busy(w), 15000);
        if (baseline)
            QTRY_VERIFY_WITH_TIMEOUT(!EditorTestAccess::loading(w), 15000);
    }
    AssetModel *model(Window &w) { return w.findChild<AssetModel *>(); }
    int origin(Window &w, const QString &relative) {
        const auto i = model(w)->indexFor(relative);
        return i.isValid() ? model(w)->data(i, AssetModel::OriginRole).toInt() : -1;
    }
    std::shared_ptr<Document> openAndWait(Window &w, const QString &relative) {
        EditorTestAccess::open(w, relative);
        const auto path = EditorTestAccess::project(w) + '/' + relative;
        [&] { QTRY_VERIFY_WITH_TIMEOUT(w.activeCanvas() && w.activeCanvas()->document->path == path, 10000); }();
        return w.activeCanvas() ? w.activeCanvas()->document : nullptr;
    }
    void closeWindow(std::unique_ptr<Window> &w) {
        if (!w)
            return;
        Answer discard("Discard", 32);
        w->close();
        w.reset();
    }

  private slots:
    void initTestCase() {
        QVERIFY(temp.isValid());
        cacheRoot = temp.filePath("cache");
        qputenv("NOVA_VANILLA_BASELINE_ROOT", QDir::toNativeSeparators(cacheRoot).toUtf8());
        QCOMPARE(BaselineCache::defaultRoot(), QDir::cleanPath(cacheRoot));
        entry = populate(cacheRoot, "1.21.11", 400);
        QVERIFY(BaselineCache().load(entry.version, entry.identity));
        // Tests must never reach the network through the installer.
        ui::baselineInstallerHook = [](InstallerOptions &o) {
            o.manifestUrl = QUrl("http://127.0.0.1:9/never");
            o.policy = loopback();
            QFAIL("the installer must not run in this test");
        };
    }
    void cleanup() { closeWindow(window); }

    // Baseline-only rows appear with the Baseline origin and open without creating any file.
    void browserShowsBaselineAndOpensWithoutWriting() {
        const auto root = createProject(temp.filePath("browse"), &entry);
        QVERIFY(write(root + '/' + P + "block/b001.png", png(101)));  // override
        QVERIFY(write(root + '/' + P + "block/custom.png", png(102))); // new asset
        const auto before = tree(root);
        window = open(root);
        ready(*window);
        QVERIFY(EditorTestAccess::loaded(*window));
        auto m = model(*window);
        const auto stone = QString(P) + "block/stone.png";
        QTRY_VERIFY(m->indexFor(stone).isValid());
        QCOMPARE(origin(*window, stone), int(AssetModel::BaselineRow));
        QCOMPARE(origin(*window, QString(P) + "block/b001.png"), int(AssetModel::OverrideRow));
        QCOMPARE(origin(*window, QString(P) + "block/custom.png"), int(AssetModel::ProjectRow));
        QVERIFY(recordText(m, m->indexFor(stone)).contains("Vanilla Java 1.21.11 · not in project (read-only baseline)"));
        QVERIFY(recordText(m, m->indexFor(QString(P) + "block/b001.png")).contains("Project override of vanilla Java 1.21.11"));
        QCOMPARE(recordText(m, m->indexFor(QString(P) + "block/custom.png")), QString(P) + "block/custom.png");
        QCOMPARE(m->data(m->indexFor(stone), Qt::ForegroundRole).value<QColor>(), ui::current().muted);
        QCOMPARE(m->data(m->indexFor(QString(P) + "block/b001.png"), Qt::ForegroundRole).value<QColor>(), ui::current().accent);
        QVERIFY(!m->data(m->indexFor(QString(P) + "block/custom.png"), Qt::ForegroundRole).isValid());
        // Theme switch: the foreground follows the live tokens.
        window->findChild<QAction *>("darkThemeCommand")->toggle();
        QCOMPARE(m->data(m->indexFor(stone), Qt::ForegroundRole).value<QColor>(), ui::current().muted);
        window->findChild<QAction *>("darkThemeCommand")->toggle();
        // Every baseline file is listed (400 filler + 5), project files once.
        const auto all = modelPaths(m);
        QCOMPARE(int(std::count_if(all.begin(), all.end(), [](const QString &p) { return p.endsWith(".png"); })), 400 + 4 + 1);
        // Thumbnails: the vanilla row from the cache, the override from the project.
        QTRY_COMPARE(centre(m->data(m->indexFor(stone), Qt::DecorationRole).value<QIcon>()), seedColor(1));
        QTRY_COMPARE(centre(m->data(m->indexFor(QString(P) + "block/b001.png"), Qt::DecorationRole).value<QIcon>()),
                     seedColor(101));
        auto label = window->findChild<QLabel *>("baselineStatus");
        QVERIFY(label && label->isVisibleTo(window.get()));
        QCOMPARE(label->text(), QString("Baseline: Vanilla Java 1.21.11"));
        // Opening a vanilla texture writes nothing.
        auto doc = openAndWait(*window, stone);
        QVERIFY(doc);
        QVERIFY(!doc->revision().exists);
        QVERIFY(EditorTestAccess::backed(*window, *doc));
        QVERIFY(!doc->modified());
        QCOMPARE(doc->image.pixelColor(3, 3), seedColor(1));
        QCOMPARE(EditorTestAccess::origin(*window), QString("Vanilla Java 1.21.11 (not in project)"));
        auto water = openAndWait(*window, QString(P) + "block/water_still.png");
        QVERIFY(water && water->animated());
        QVERIFY(!water->modified());
        QTest::qWait(300);
        QCOMPARE(tree(root), before);
        // MCP sees project files only: no baseline row, no provisional entry for open vanilla documents.
        QSet<QString> provisional;
        const auto mcp = EditorTestAccess::workspace(*window, &provisional);
        QVERIFY(provisional.isEmpty());
        QCOMPARE(mcp.records.keys(), EditorTestAccess::collection(*window).records.keys());
        QVERIFY(!mcp.records.contains(stone));
        QCOMPARE(mcp.files, Index::scan(root).files);
        for (const auto &record : mcp.records)
            QVERIFY(record.origin == AssetIdentity::Origin::Project);
        // An override takes precedence: its document shows the project pixels.
        auto override = openAndWait(*window, QString(P) + "block/b001.png");
        QVERIFY(override && override->revision().exists);
        QCOMPARE(override->image.pixelColor(0, 0), seedColor(101));
        QCOMPARE(EditorTestAccess::origin(*window), QString("Override of vanilla Java 1.21.11"));
        openAndWait(*window, QString(P) + "block/custom.png");
        QCOMPARE(EditorTestAccess::origin(*window), QString("Project asset"));
    }

    // First edit + save creates the override (with its folders); undo/redo, conflict detection.
    void saveCreatesOverride() {
        const auto root = createProject(temp.filePath("save"), &entry);
        window = open(root);
        ready(*window);
        const auto heart = QString(P) + "gui/sprites/hud/heart.png";
        QVERIFY(!QFileInfo::exists(root + '/' + P + "gui"));
        auto doc = openAndWait(*window, heart);
        QVERIFY(doc);
        window->activeCanvas()->setFocus();
        doc->draw({2, 2}, {2, 2}, qRgba(250, 1, 2, 255));
        doc->finish();
        window->documents().notify(doc);
        QVERIFY(doc->modified());
        EditorTestAccess::action(*window, "undo");
        QCOMPARE(doc->image.pixelColor(2, 2), seedColor(3));
        EditorTestAccess::action(*window, "redo");
        QCOMPARE(doc->image.pixelColor(2, 2), QColor(250, 1, 2));
        QVERIFY(!QFileInfo::exists(root + '/' + heart));
        EditorTestAccess::action(*window, "save");
        QVERIFY(QFileInfo::exists(root + '/' + heart));
        QVERIFY(doc->revision().exists && !doc->modified());
        QCOMPARE(loadPng(root + '/' + heart).pixelColor(2, 2), QColor(250, 1, 2));
        QTRY_COMPARE_WITH_TIMEOUT(origin(*window, heart), int(AssetModel::OverrideRow), 10000);
        QVERIFY(EditorTestAccess::collection(*window).records.contains(heart));
        QCOMPARE(EditorTestAccess::origin(*window), QString("Override of vanilla Java 1.21.11"));
        QTRY_COMPARE(centre(model(*window)->data(model(*window)->indexFor(heart), Qt::DecorationRole).value<QIcon>()),
                     QColor(250, 1, 2)); // the thumbnail now comes from the project override
        // External modification of the saved override uses the existing conflict path.
        QImage changed(16, 16, QImage::Format_RGBA8888);
        changed.fill(Qt::blue);
        QVERIFY(savePng(root + '/' + heart, changed));
        EditorTestAccess::safety(*window);
        QTRY_VERIFY_WITH_TIMEOUT(EditorTestAccess::external(*window, doc->id()), 10000);
        QVERIFY(doc->externalChanged());
    }

    // An edited, never-saved vanilla texture survives a crash and is restored create-only.
    void recoveryRestoresCreateOnly() {
        const auto root = createProject(temp.filePath("recover"), &entry);
        const auto apple = QString(P) + "item/apple.png";
        qApp->setProperty("settingsFile", temp.filePath("recovery-settings/studio.ini"));
        {
            QByteArray bytes;
            QVERIFY(BaselineCache().readVerified(entry, apple, &bytes));
            auto edited = Document::fromBaseline(root + '/' + apple, bytes, {}, "1.21.11");
            QVERIFY(edited);
            edited->draw({1, 1}, {1, 1}, qRgba(9, 200, 9, 255));
            edited->finish();
            RecoveryStore store(temp.filePath("recovery-settings/recovery"));
            QString error;
            QVERIFY2(store.writeDocument(*edited, root, &error), qPrintable(error));
        }
        QElapsedTimer time;
        time.start();
        Answer restore("Restore");
        window = std::make_unique<Window>(root, QString(), time);
        window->show();
        ready(*window);
        QTRY_COMPARE_WITH_TIMEOUT(restore.seen.size(), 1, 10000);
        QTRY_VERIFY(window->activeCanvas() && window->activeCanvas()->document->path == root + '/' + apple);
        auto doc = window->activeCanvas()->document;
        QVERIFY(!doc->revision().exists);
        QVERIFY(doc->modified());
        QVERIFY(EditorTestAccess::backed(*window, *doc));
        QCOMPARE(doc->image.pixelColor(1, 1), QColor(9, 200, 9));
        QSet<QString> provisional;
        QVERIFY(!EditorTestAccess::workspace(*window, &provisional).records.contains(apple));
        // Create-only: a file that appeared meanwhile is never overwritten.
        QVERIFY(write(root + '/' + apple, png(77)));
        {
            Answer keep("Keep current");
            EditorTestAccess::action(*window, "save");
            QTRY_COMPARE(keep.seen.size(), 1);
        }
        QCOMPARE(read(root + '/' + apple), png(77));
        QVERIFY(QFile::remove(root + '/' + apple)); // the test's own file
        EditorTestAccess::action(*window, "save");
        QCOMPARE(loadPng(root + '/' + apple).pixelColor(1, 1), QColor(9, 200, 9));
        QTRY_COMPARE_WITH_TIMEOUT(origin(*window, apple), int(AssetModel::OverrideRow), 10000);
    }

    // Reset to Vanilla, its undo/redo (commands and Ctrl+Z in the browser), refusal on a damaged
    // cache, Remove from Project and Reset All; export afterwards lacks the reset textures.
    void resetRemoveUndoAndExport() {
        const auto root = createProject(temp.filePath("reset"), &entry);
        const auto b1 = QString(P) + "block/b001.png", b2 = QString(P) + "block/b002.png",
                   b3 = QString(P) + "block/b003.png", custom = QString(P) + "block/custom.png";
        QVERIFY(write(root + '/' + b1, png(201)));
        QVERIFY(write(root + '/' + b2, png(202)));
        QVERIFY(write(root + '/' + b3, png(203)));
        QVERIFY(write(root + '/' + custom, png(204)));
        window = open(root);
        ready(*window);
        QTRY_COMPARE(origin(*window, b1), int(AssetModel::OverrideRow));
        // The context menu offers Reset for overrides and Remove for new textures, never mixed up.
        {
            QMenu menu;
            QCOMPARE(EditorTestAccess::context(*window, menu, b1)->text(), QString("Reset to Vanilla"));
            QCOMPARE(EditorTestAccess::context(*window, menu, custom)->text(), QString("Remove from Project…"));
            QVERIFY(!EditorTestAccess::context(*window, menu, QString(P) + "block/stone.png"));
        }
        auto tree = EditorTestAccess::tree(*window);
        window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window.get()));
        EditorTestAccess::select(*window, b1);
        tree->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(QApplication::focusWidget() == tree);
        EditorTestAccess::actions(*window);
        auto resetAction = window->findChild<QAction *>("resetToVanillaCommand");
        QVERIFY(resetAction->isEnabled());
        QVERIFY(!window->findChild<QAction *>("removeFromProjectCommand")->isEnabled());
        resetAction->trigger();
        QVERIFY(!QFileInfo::exists(root + '/' + b1));
        QCOMPARE(read(root + '/' + b2), png(202));
        QCOMPARE(read(root + '/' + custom), png(204));
        QCOMPARE(EditorTestAccess::history(*window), 1);
        QVERIFY(window->statusBar()->currentMessage().contains("Undo with Ctrl+Z in the asset browser"));
        QTRY_COMPARE_WITH_TIMEOUT(origin(*window, b1), int(AssetModel::BaselineRow), 10000);
        QTRY_COMPARE(centre(model(*window)->data(model(*window)->indexFor(b1), Qt::DecorationRole).value<QIcon>()),
                     seedColor(11));
        const auto afterReset = exported(root, temp.filePath("reset-export-1"));
        QVERIFY(!afterReset.contains(b1));
        QVERIFY(afterReset.contains(b2) && afterReset.contains(custom));
        // Explicit Undo / Redo Reset commands.
        window->findChild<QAction *>("undoBaselineResetCommand")->trigger();
        QCOMPARE(read(root + '/' + b1), png(201));
        QTRY_COMPARE_WITH_TIMEOUT(origin(*window, b1), int(AssetModel::OverrideRow), 10000);
        window->findChild<QAction *>("redoBaselineResetCommand")->trigger();
        QVERIFY(!QFileInfo::exists(root + '/' + b1));
        // Ctrl+Z / Ctrl+Y with the asset browser focused.
        tree->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(QApplication::focusWidget() == tree);
        QTest::keyClick(tree, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(read(root + '/' + b1), png(201));
        QCOMPARE(EditorTestAccess::cursor(*window), 0);
        QTest::qWait(300);
        tree->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(QApplication::focusWidget() == tree);
        QTest::keyClick(tree, Qt::Key_Y, Qt::ControlModifier);
        QTRY_VERIFY(!QFileInfo::exists(root + '/' + b1));
        QCOMPARE(EditorTestAccess::cursor(*window), 1);
        // A damaged or missing cache object refuses the reset; the override is preserved.
        const auto damaged = entry.objectPath(b2);
        const auto original = read(damaged);
        QVERIFY(fs::setReadOnly(damaged, false));
        QVERIFY(write(damaged, "not the vanilla texture"));
        {
            Answer ok("OK");
            EditorTestAccess::reset(*window, {b2}, ResetKind::ResetToVanilla);
            QCOMPARE(ok.seen.size(), 1);
            QVERIFY(ok.seen.first().contains("preserved"));
        }
        QCOMPARE(read(root + '/' + b2), png(202));
        QVERIFY(write(damaged, original));
        QVERIFY(fs::setReadOnly(damaged, true));
        const auto missing = entry.objectPath(b3);
        const auto missingBytes = read(missing);
        QVERIFY(fs::removeRegularFile(missing));
        {
            Answer ok("OK");
            EditorTestAccess::reset(*window, {b3}, ResetKind::ResetToVanilla);
            QCOMPARE(ok.seen.size(), 1);
        }
        QCOMPARE(read(root + '/' + b3), png(203));
        QString error;
        QVERIFY2(BaselineCache().publishObject(missingBytes, sha(missingBytes), b3, &error), qPrintable(error));
        QCOMPARE(EditorTestAccess::history(*window), 1);
        // Remove from Project: a new texture, undoable, labelled as removal.
        {
            Answer yes("Yes");
            EditorTestAccess::select(*window, custom);
            tree->setFocus(Qt::OtherFocusReason);
            EditorTestAccess::actions(*window);
            auto remove = window->findChild<QAction *>("removeFromProjectCommand");
            QVERIFY(remove->isEnabled());
            QVERIFY(!resetAction->isEnabled());
            QCOMPARE(remove->text(), QString("Remove from Project…"));
            remove->trigger();
            QCOMPARE(yes.seen.size(), 1);
            QVERIFY(!yes.seen.first().contains("Reset"));
        }
        QVERIFY(!QFileInfo::exists(root + '/' + custom));
        QTRY_VERIFY_WITH_TIMEOUT(!model(*window)->indexFor(custom).isValid(), 10000);
        window->findChild<QAction *>("undoBaselineResetCommand")->trigger();
        QCOMPARE(read(root + '/' + custom), png(204));
        QTRY_COMPARE_WITH_TIMEOUT(origin(*window, custom), int(AssetModel::ProjectRow), 10000);
        // Reset All: confirmation with the count, one undo step.
        EditorTestAccess::actions(*window);
        {
            Answer cancel("Cancel");
            window->findChild<QAction *>("resetAllToVanillaCommand")->trigger();
            QCOMPARE(cancel.seen.size(), 1);
            QVERIFY(cancel.seen.first().contains("Reset 2 overrides"));
        }
        QVERIFY(QFileInfo::exists(root + '/' + b2) && QFileInfo::exists(root + '/' + b3));
        const int steps = EditorTestAccess::cursor(*window); // the undone removal is discarded as redo history
        {
            Answer yes("Yes");
            window->findChild<QAction *>("resetAllToVanillaCommand")->trigger();
            QCOMPARE(yes.seen.size(), 1);
        }
        QVERIFY(!QFileInfo::exists(root + '/' + b2) && !QFileInfo::exists(root + '/' + b3));
        QCOMPARE(read(root + '/' + custom), png(204));
        QCOMPARE(EditorTestAccess::history(*window), steps + 1);
        QCOMPARE(EditorTestAccess::cursor(*window), steps + 1);
        window->findChild<QAction *>("undoBaselineResetCommand")->trigger();
        QCOMPARE(read(root + '/' + b2), png(202));
        QCOMPARE(read(root + '/' + b3), png(203));
        // Undo never overwrites a file that appeared at the destination.
        window->findChild<QAction *>("redoBaselineResetCommand")->trigger();
        QVERIFY(write(root + '/' + b2, png(250)));
        {
            Answer ok("OK");
            window->findChild<QAction *>("undoBaselineResetCommand")->trigger();
            QCOMPARE(ok.seen.size(), 1);
            QVERIFY(ok.seen.first().contains("Nothing was overwritten"));
        }
        QCOMPARE(read(root + '/' + b2), png(250));
        QVERIFY(!QFileInfo::exists(root + '/' + b3));
    }

    // Legacy projects (no .nova/project.json): no baseline rows, no enabled commands, same model.
    void legacyProjectUnchanged() {
        const auto root = createProject(temp.filePath("legacy"), nullptr);
        QVERIFY(write(root + '/' + P + "block/stone.png", png(5)));
        QVERIFY(!QFileInfo::exists(root + "/.nova/project.json"));
        const auto before = tree(root);
        window = open(root);
        ready(*window);
        auto m = model(*window);
        QTRY_VERIFY(m->indexFor(QString(P) + "block/stone.png").isValid());
        QVERIFY(!EditorTestAccess::hasRef(*window));
        auto paths = modelPaths(m);
        paths.sort();
        const auto scan = Index::scan(root);
        QStringList expected = scan.files + scan.directories.keys();
        expected.removeAll("");
        expected.sort();
        QCOMPARE(paths, expected);
        for (const auto &path : paths) {
            const auto i = m->indexFor(path);
            QCOMPARE(m->data(i, AssetModel::OriginRole).toInt(), int(AssetModel::ProjectRow));
            QCOMPARE(m->data(i, Qt::ToolTipRole).toString(), path);
            QVERIFY(!m->data(i, Qt::ForegroundRole).isValid());
        }
        EditorTestAccess::actions(*window);
        for (const auto *name : {"resetToVanillaCommand", "removeFromProjectCommand", "resetAllToVanillaCommand",
                                 "undoBaselineResetCommand", "redoBaselineResetCommand", "repairBaselineCommand"})
            QVERIFY2(!window->findChild<QAction *>(name)->isEnabled(), name);
        QVERIFY(window->findChild<QAction *>("useVanillaBaselineCommand")->isEnabled());
        QVERIFY(!window->findChild<QLabel *>("baselineStatus")->isVisibleTo(window.get()));
        QMenu menu;
        QVERIFY(!EditorTestAccess::context(*window, menu, QString(P) + "block/stone.png"));
        QVERIFY(EditorTestAccess::origin(*window).isEmpty());
        QTest::qWait(200);
        QCOMPARE(tree(root), before);
        // Project Properties names the (absent) baseline.
        QString info;
        QTimer::singleShot(100, [&] {
            auto dialog = QApplication::activeModalWidget();
            info = dialog->findChild<QLabel *>("projectBaselineInfo")->text();
            qobject_cast<QDialog *>(dialog)->reject();
        });
        window->findChild<QAction *>("projectPropertiesCommand")->trigger();
        QCOMPARE(info, QString("None (blank pack)"));
        // Opting in later: the version matching pack.mcmeta is preselected, never assigned silently.
        QTimer::singleShot(100, [&] {
            auto dialog = QApplication::activeModalWidget();
            QVERIFY(dialog && dialog->objectName() == "useVanillaBaselineDialog");
            QCOMPARE(dialog->findChild<QComboBox *>("baselineVersion")->currentData().toString(), QString("1.21.11"));
            QTRY_VERIFY(dialog->findChild<QLabel *>("baselineVersionStatus")->text().contains("already in your local cache"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        window->findChild<QAction *>("useVanillaBaselineCommand")->trigger();
        QCOMPARE(*ProjectConfig::load(root).baseline, ui::baselineRef(entry));
        QTRY_VERIFY_WITH_TIMEOUT(EditorTestAccess::loaded(*window), 10000);
        QTRY_COMPARE(origin(*window, QString(P) + "block/stone.png"), int(AssetModel::OverrideRow));
        QTRY_COMPARE(origin(*window, QString(P) + "item/apple.png"), int(AssetModel::BaselineRow));
        QTimer::singleShot(100, [&] {
            auto dialog = QApplication::activeModalWidget();
            info = dialog->findChild<QLabel *>("projectBaselineInfo")->text();
            qobject_cast<QDialog *>(dialog)->reject();
        });
        window->findChild<QAction *>("projectPropertiesCommand")->trigger();
        QCOMPARE(info, QString("Java 1.21.11 (identity %1)").arg(entry.identity.left(12)));
        QCOMPARE(window->findChild<QLabel *>("baselineStatus")->text(), QString("Baseline: Vanilla Java 1.21.11"));
    }

    // The cache entry is missing: project files only, a permanent explanation, reset disabled.
    void baselineUnavailable() {
        auto missing = entry;
        VersionMetadata m;
        m.id = "1.21.11";
        m.client = {{}, QString(40, '9'), 4096};
        m.assetIndexId = "29";
        m.assetIndex = {{}, QString(40, '8'), 512};
        missing.identity = identityKey(m);
        missing.source["client"] = QJsonObject{{"url", "u"}, {"sha1", QString(40, '9')}, {"size", 4096}};
        const auto root = createProject(temp.filePath("unavailable"), &missing);
        QVERIFY(write(root + '/' + P + "block/b001.png", png(101)));
        window = open(root);
        ready(*window);
        QVERIFY(!EditorTestAccess::loaded(*window));
        auto label = window->findChild<QLabel *>("baselineStatus");
        QVERIFY(label->isVisibleTo(window.get()));
        QCOMPARE(label->text(), QString("Vanilla Java 1.21.11 baseline not available · Resources › Vanilla Baseline › Download…"));
        auto paths = modelPaths(model(*window)).filter(".png");
        QCOMPARE(paths, QStringList{QString(P) + "block/b001.png"});
        EditorTestAccess::actions(*window);
        EditorTestAccess::select(*window, QString(P) + "block/b001.png");
        QVERIFY(!window->findChild<QAction *>("resetToVanillaCommand")->isEnabled());
        QVERIFY(!window->findChild<QAction *>("resetAllToVanillaCommand")->isEnabled());
        QVERIFY(window->findChild<QAction *>("repairBaselineCommand")->isEnabled());
        QCOMPARE(read(root + '/' + P + "block/b001.png"), png(101));
    }

    // New Resource Pack: blank exactly as before, baseline from the cache without any download,
    // and a cancelled baseline download that creates nothing. Prints the export file sets.
    void newProjectDialogAndExportSets() {
        window = open({});
        QTest::qWait(200);
        auto create = [&](const QString &destination, bool baseline) {
            QTimer::singleShot(100, [&, destination, baseline] {
                auto dialog = QApplication::activeModalWidget();
                QVERIFY(dialog && dialog->objectName() == "newProjectDialog");
                QVERIFY(dialog->findChild<QRadioButton *>("projectBlank")->isChecked());
                dialog->findChild<QLineEdit *>("projectDestination")->setText(destination);
                auto version = dialog->findChild<QComboBox *>();
                version->setCurrentIndex(version->findText("Java 1.21.11", Qt::MatchStartsWith));
                if (baseline) {
                    dialog->findChild<QRadioButton *>("projectBaseline")->setChecked(true);
                    auto hint = dialog->findChild<QLabel *>("projectBaselineHint");
                    QTRY_VERIFY(hint->text().contains("already in your local cache"));
                }
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            });
            window->findChild<QAction *>("newProjectCommand")->trigger();
        };
        // Blank: identical to Project::create without a baseline.
        const auto blank = temp.filePath("dialog-blank");
        create(blank, false);
        QVERIFY(QFileInfo::exists(blank + "/pack.mcmeta"));
        QVERIFY(!QFileInfo::exists(blank + "/.nova/project.json"));
        QVERIFY(QDir().mkpath(temp.filePath("reference")));
        const auto reference = temp.filePath("reference/dialog-blank");
        QString error; // the dialog's defaults, through the API that existed before baselines
        QVERIFY2(Project::create(reference, "My Resource Pack", game("1.21.11"), "A Nova Studio resource pack",
                                 "minecraft", {}, true, &error), qPrintable(error));
        QCOMPARE(tree(blank).keys(), tree(reference).keys());
        QCOMPARE(read(blank + "/pack.mcmeta"), read(reference + "/pack.mcmeta"));
        QTRY_COMPARE(EditorTestAccess::project(*window), QDir(blank).canonicalPath());
        ready(*window, false);
        // Baseline from the cache: zero network (the installer hook fails the test if reached).
        const auto based = temp.filePath("dialog-baseline");
        create(based, true);
        const auto config = ProjectConfig::load(based);
        QVERIFY(config.baseline);
        QCOMPARE(*config.baseline, ui::baselineRef(entry));
        auto expectedTree = QStringList(tree(reference).keys()) << ".nova/" << ".nova/project.json";
        expectedTree.sort();
        QCOMPARE(tree(based).keys(), expectedTree);
        QTRY_COMPARE(EditorTestAccess::project(*window), QDir(based).canonicalPath());
        ready(*window);
        QVERIFY(EditorTestAccess::loaded(*window));
        // Export evidence for the GUI-created project.
        const auto root = EditorTestAccess::project(*window);
        auto report = [&](const QString &name, const QString &source) {
            const auto list = exported(source, temp.filePath("export-" + name));
            qInfo().noquote() << "EXPORT" << name << QString("(%1 files):").arg(list.size()) << list.join(", ");
            return list;
        };
        const auto blankSet = report("blank", blank);
        QCOMPARE(blankSet, QStringList{"pack.mcmeta"});
        QCOMPARE(report("baseline-only", root), QStringList{"pack.mcmeta"});
        const auto stone = QString(P) + "block/stone.png";
        auto doc = openAndWait(*window, stone);
        QVERIFY(doc);
        doc->draw({2, 2}, {2, 2}, qRgba(1, 2, 3, 255));
        doc->finish();
        window->documents().notify(doc);
        EditorTestAccess::action(*window, "save");
        QCOMPARE(report("edited", root), (QStringList{QString(P) + "block/stone.png", "pack.mcmeta"}));
        QTRY_COMPARE_WITH_TIMEOUT(origin(*window, stone), int(AssetModel::OverrideRow), 10000);
        EditorTestAccess::reset(*window, {stone}, ResetKind::ResetToVanilla);
        QVERIFY(!QFileInfo::exists(root + '/' + stone));
        QCOMPARE(report("reset", root), QStringList{"pack.mcmeta"});
        QVERIFY(write(root + '/' + P + "item/ruby.png", png(300)));
        QCOMPARE(report("new-asset", root), (QStringList{QString(P) + "item/ruby.png", "pack.mcmeta"}));
        QTRY_COMPARE(window->activeCanvas() ? window->activeCanvas()->document->path : QString(), root + '/' + stone);
        // A declined download creates nothing; the user returns to the dialog.
        const auto declined = temp.filePath("dialog-declined");
        QTimer::singleShot(100, [&] {
            auto dialog = QApplication::activeModalWidget();
            QVERIFY(dialog && dialog->objectName() == "newProjectDialog");
            dialog->findChild<QLineEdit *>("projectDestination")->setText(declined);
            auto version = dialog->findChild<QComboBox *>();
            version->setCurrentIndex(version->findText("Java 26.1", Qt::MatchStartsWith));
            dialog->findChild<QRadioButton *>("projectBaseline")->setChecked(true);
            auto hint = dialog->findChild<QLabel *>("projectBaselineHint");
            QTRY_VERIFY(hint->text().contains("Nova will download the official Minecraft Java 26.1 client"));
            Answer no("Cancel"), back("Back");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            QCOMPARE(no.seen.size(), 1);
            QCOMPARE(back.seen.size(), 1);
            QVERIFY(!QFileInfo::exists(declined));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
        });
        window->findChild<QAction *>("newProjectCommand")->trigger();
        QVERIFY(!QFileInfo::exists(declined));
    }

    // The download path end to end against a loopback mock of Mojang's servers.
    // Native visual evidence is opt-in through NOVA_BASELINE_CAPTURE_DIR. All pixels are synthetic.
    void visualEvidence() {
        const auto root = createProject(temp.filePath("visual"), &entry);
        const auto overridden = QString(P) + "block/b001.png";
        QVERIFY(write(root + '/' + overridden, png(201)));
        QVERIFY(write(root + '/' + P + "block/custom.png", png(202)));
        window = open(root);
        ready(*window);
        QVERIFY(EditorTestAccess::loaded(*window));
        QCOMPARE(origin(*window, overridden), int(AssetModel::OverrideRow));
        auto theme = window->findChild<QAction *>("darkThemeCommand");
        QVERIFY(theme);
        theme->setChecked(false);
        QVERIFY(capture(*window, "assets-light"));
        theme->setChecked(true);
        QVERIFY(capture(*window, "assets-dark"));
        EditorTestAccess::select(*window, overridden);
        EditorTestAccess::actions(*window);
        auto reset = window->findChild<QAction *>("resetToVanillaCommand");
        auto menu = window->findChild<QMenu *>("menu-VanillaBaseline");
        QVERIFY(reset && reset->isEnabled() && menu);
        menu->popup(window->mapToGlobal(QPoint(20, 40)));
        QTest::qWait(150);
        QVERIFY(capture(*menu, "reset-menu-dark"));
        menu->close();
        QVERIFY(openAndWait(*window, QString(P) + "block/stone.png"));
        QVERIFY(capture(*window, "editor-dark"));
        theme->setChecked(false);
        QVERIFY(capture(*window, "editor-light"));
    }

    // One repeatable, local-only performance ledger. A path is supplied only for evidence runs.
    void performanceEvidence() {
        QJsonObject result{{"fixture", "405 synthetic PNG/mcmeta records plus project overrides"},
                           {"baseline_records", entry.files.size()}};
        QElapsedTimer timer;
        timer.start();
        const auto loaded = BaselineCache().load(entry.version, entry.identity, BaselineCache::Check::Sizes);
        QVERIFY(loaded);
        result["cache_load_sizes_ms"] = timer.nsecsElapsed() / 1.0e6;
        const auto root = createProject(temp.filePath("performance"), &entry);
        window = open(root);
        ready(*window);
        timer.start();
        const auto scanned = Index::scan(root);
        result["project_index_ms"] = timer.nsecsElapsed() / 1.0e6;
        result["project_files_before_edit"] = scanned.files.size();
        const auto heart = QString(P) + "gui/sprites/hud/heart.png";
        timer.restart();
        auto doc = openAndWait(*window, heart);
        QVERIFY(doc);
        result["open_baseline_texture_ms"] = timer.nsecsElapsed() / 1.0e6;
        timer.restart();
        doc->draw({2, 2}, {2, 2}, qRgba(250, 1, 2, 255));
        doc->finish();
        window->documents().notify(doc);
        result["first_edit_in_memory_ms"] = timer.nsecsElapsed() / 1.0e6;
        timer.restart();
        EditorTestAccess::action(*window, "save");
        QVERIFY(doc->revision().exists && !doc->modified());
        result["first_save_create_override_ms"] = timer.nsecsElapsed() / 1.0e6;
        timer.restart();
        doc->draw({3, 3}, {3, 3}, qRgba(1, 2, 250, 255));
        doc->finish();
        window->documents().notify(doc);
        EditorTestAccess::action(*window, "save");
        QVERIFY(!doc->modified());
        result["subsequent_edit_and_save_ms"] = timer.nsecsElapsed() / 1.0e6;
        QVERIFY(write(root + '/' + P + "item/new_project_texture.png", png(204)));
        timer.restart();
        const auto out = exported(root, temp.filePath("performance-export"));
        result["sparse_export_ms"] = timer.nsecsElapsed() / 1.0e6;
        result["export_files"] = QJsonArray::fromStringList(out);
        QVERIFY(out.contains(heart));
        QVERIFY(out.contains(QString(P) + "item/new_project_texture.png"));
        QVERIFY(!out.contains(QString(P) + "block/stone.png"));
        const auto encoded = QJsonDocument(result).toJson(QJsonDocument::Compact);
        qInfo().noquote() << "BASELINE PERFORMANCE" << encoded;
        if (const auto path = qEnvironmentVariable("NOVA_BASELINE_PERFORMANCE_JSON"); !path.isEmpty()) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write(QJsonDocument(result).toJson(QJsonDocument::Indented)) > 0);
        }
    }

    void newProjectDownloadsFromMock() {
        MockMojang mojang;
        mojang.publish("26.1");
        QVERIFY(!mojang.identity.isEmpty());
        bool used = false;
        ui::baselineInstallerHook = [&](InstallerOptions &o) {
            used = true;
            o.manifestUrl = QUrl(mojang.url("/mc/game/version_manifest_v2.json"));
            o.objectBaseUrl = QUrl(mojang.url("/objects/"));
            o.policy = loopback();
            o.lockTimeoutMs = 60000;
        };
        window = open({});
        QTest::qWait(200);
        const auto destination = temp.filePath("dialog-download");
        QTimer::singleShot(100, [&] {
            auto dialog = QApplication::activeModalWidget();
            QVERIFY(dialog && dialog->objectName() == "newProjectDialog");
            dialog->findChild<QLineEdit *>("projectDestination")->setText(destination);
            auto version = dialog->findChild<QComboBox *>();
            version->setCurrentIndex(version->findText("Java 26.1", Qt::MatchStartsWith));
            dialog->findChild<QRadioButton *>("projectBaseline")->setChecked(true);
            Answer yes("Yes");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            QCOMPARE(yes.seen.size(), 1);
            QVERIFY(yes.seen.first().contains("about 30 MB") || yes.seen.first().contains("30 MB"));
        });
        window->findChild<QAction *>("newProjectCommand")->trigger();
        QVERIFY(used);
        QVERIFY(mojang.requests.contains("/mc/game/version_manifest_v2.json"));
        const auto config = ProjectConfig::load(destination);
        QVERIFY(config.baseline);
        QCOMPARE(config.baseline->version, QString("26.1"));
        QCOMPARE(config.baseline->identity, mojang.identity);
        const auto downloaded = BaselineCache().load("26.1", mojang.identity);
        QVERIFY(downloaded);
        QCOMPARE(QStringList(downloaded->files.keys()),
                 (QStringList{QString(P) + "block/dirt.png", QString(P) + "block/lava_still.png",
                              QString(P) + "block/lava_still.png.mcmeta", QString(P) + "block/stone.png"}));
        QTRY_COMPARE(EditorTestAccess::project(*window), QDir(destination).canonicalPath());
        ready(*window);
        QTRY_COMPARE(origin(*window, QString(P) + "block/dirt.png"), int(AssetModel::BaselineRow));
        ui::baselineInstallerHook = [](InstallerOptions &) { QFAIL("the installer must not run in this test"); };
    }
};
QTEST_MAIN(BaselineGuiTests)
#include "baseline_gui_test.moc"
