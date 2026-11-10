// SPDX-License-Identifier: GPL-3.0-or-later
// Vanilla texture baseline: metadata parsing, path rules, identity, the shared cache and
// its population, removal and overlay. Everything is offline: Mojang is a loopback mock
// server, the client jar is built here with QZipWriter and every texture is a synthetic
// PNG generated in-test. No game file from any local path is ever read.
#include "vanilla/cache.h"
#include "vanilla/installer.h"
#include "vanilla/metadata.h"
#include "vanilla/overlay.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSignalSpy>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtCore/private/qzipwriter_p.h>
#include <QtTest>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace nova;
using namespace nova::vanilla;
using Check = BaselineCache::Check;
using Error = BaselineInstaller::Error;

namespace {
// SYNTHETIC TEST-ONLY CERTIFICATE AND PRIVATE KEY. Self-signed for 127.0.0.1, generated for
// this suite with the toolchain's openssl (RSA 2048, PKCS#12, password "nova-test"). It is
// not trusted by anything and protects nothing; it exists only to prove that an untrusted
// server certificate is rejected.
constexpr const char *syntheticTestPkcs12 =
    "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";

QString sha1(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
}
QString sha256(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
// Synthetic texture: a deterministic gradient, never game art.
QByteArray png(int seed, int width = 16, int height = 16) {
    QImage image(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, qRgba((x * 16 + seed * 37) & 255, (y * 16 + seed * 11) & 255,
                                       (seed * 53) & 255, 255));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}
using Entries = QList<std::pair<QString, QByteArray>>;
QByteArray zip(const Entries &entries, const QStringList &links = {}) {
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QZipWriter writer(&buffer);
    for (const auto &[name, bytes] : entries)
        writer.addFile(name, bytes);
    for (const auto &link : links)
        writer.addSymLink(link, "target.png");
    writer.close();
    return buffer.data();
}
QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
bool readOnlyAttribute(const QString &path) {
#ifdef Q_OS_WIN
    const auto attributes =
        GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY);
#else
    return !QFileInfo(path).isWritable();
#endif
}
QStringList filesUnder(const QString &folder) {
    QStringList result;
    QDirIterator it(folder, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext())
        result << QDir(folder).relativeFilePath(it.next());
    result.sort();
    return result;
}
testing::DownloadPolicy loopback() { return {{}, true, 256LL * 1024 * 1024, "NovaStudio-Test"}; }

class MockServer : public QTcpServer {
  public:
    struct Reply {
        int status = 200;
        QByteArray body;
        qint64 contentLength = -1; // -1 = body size
        bool stall = false;        // send headers and half the body, then keep the socket open
    };
    QMap<QString, Reply> routes;
    QStringList requests;
    QList<QPointer<QTcpSocket>> stalled;
    MockServer() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto socket = nextPendingConnection())
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const auto path = QString::fromLatin1(socket->readAll().split(' ').value(1));
                    requests << path;
                    const auto reply = routes.value(path, {404, "missing"});
                    const auto length =
                        reply.contentLength >= 0 ? reply.contentLength : reply.body.size();
                    socket->write("HTTP/1.1 " + QByteArray::number(reply.status) +
                                  " X\r\nContent-Length: " + QByteArray::number(length) +
                                  "\r\nConnection: close\r\n\r\n");
                    if (reply.stall) {
                        socket->write(reply.body.left(reply.body.size() / 2));
                        stalled << socket;
                        return;
                    }
                    socket->write(reply.body);
                    socket->disconnectFromHost();
                });
        });
        listen(QHostAddress::LocalHost);
    }
    QString url(const QString &path) const {
        return QString("http://127.0.0.1:%1%2").arg(serverPort()).arg(path);
    }
    int count(const QString &prefix) const {
        return int(std::count_if(requests.cbegin(), requests.cend(),
                                 [&](const QString &r) { return r.startsWith(prefix); }));
    }
};

// A fake piston-meta / piston-data / resources host on loopback.
struct Mojang {
    MockServer server;
    QJsonArray versions;
    struct Published {
        QString identity, clientPath;
        QByteArray jar;
    };
    Published publish(const QString &id, const Entries &jarEntries,
                      const QMap<QString, QByteArray> &objects = {},
                      const std::function<void(QJsonObject &)> &tweak = {},
                      const QString &indexId = "29") {
        Published p;
        p.jar = zip(jarEntries);
        const auto jarSha1 = sha1(p.jar);
        p.clientPath = "/v1/objects/" + jarSha1 + "/client.jar";
        server.routes[p.clientPath] = {200, p.jar};
        QJsonObject indexObjects;
        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
            const auto h = sha1(it.value());
            indexObjects.insert(it.key(), QJsonObject{{"hash", h}, {"size", it.value().size()}});
            server.routes["/objects/" + h.left(2) + '/' + h] = {200, it.value()};
        }
        const auto index = QJsonDocument(QJsonObject{{"objects", indexObjects}}).toJson();
        const auto indexPath = "/v1/packages/" + sha1(index) + '/' + indexId + ".json";
        server.routes[indexPath] = {200, index};
        QJsonObject version{
            {"id", id},
            {"type", "release"},
            {"downloads",
             QJsonObject{{"client", QJsonObject{{"url", server.url(p.clientPath)},
                                                {"sha1", jarSha1},
                                                {"size", p.jar.size()}}}}},
            {"assetIndex", QJsonObject{{"id", indexId},
                                       {"url", server.url(indexPath)},
                                       {"sha1", sha1(index)},
                                       {"size", index.size()},
                                       {"totalSize", 1}}}};
        if (tweak)
            tweak(version);
        const auto bytes = QJsonDocument(version).toJson();
        const auto versionPath = "/v1/packages/" + sha1(bytes) + '/' + id + ".json";
        server.routes[versionPath] = {200, bytes};
        for (qsizetype i = versions.size() - 1; i >= 0; --i)
            if (versions[i].toObject().value("id").toString() == id)
                versions.removeAt(i);
        versions.prepend(QJsonObject{{"id", id},
                                     {"type", "release"},
                                     {"url", server.url(versionPath)},
                                     {"time", "2026-11-07T00:00:00+00:00"},
                                     {"sha1", sha1(bytes)},
                                     {"complianceLevel", 1}});
        server.routes["/mc/game/version_manifest_v2.json"] = {
            200, QJsonDocument(QJsonObject{{"latest", QJsonObject{{"release", id}}},
                                           {"versions", versions}})
                     .toJson()};
        const auto ref = parseVersionManifest(manifestBytes(), id, nullptr, loopback());
        if (ref)
            if (const auto meta = parseVersionJson(bytes, *ref, nullptr, loopback()))
                p.identity = identityKey(*meta);
        return p;
    }
    QByteArray manifestBytes() const {
        return server.routes.value("/mc/game/version_manifest_v2.json").body;
    }
};

// Typical client jar: textures (one animated), plus content that must never be stored.
Entries clientEntries(int variant = 0) {
    return {{"META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n"},
            {"net/minecraft/client/Main.class", QByteArray(64, 'c')},
            {"assets/minecraft/models/block/stone.json", "{\"parent\":\"block/cube_all\"}"},
            {"assets/minecraft/blockstates/stone.json", "{}"},
            {"assets/minecraft/texturesx/not_a_texture.png", png(90)},
            {"assets/realms/textures/realms.png", png(91)},
            {"data/minecraft/recipe/stone.json", "{}"},
            {"pack.png", png(92)},
            {"assets/minecraft/textures/block/stone.png", png(1)},
            {"assets/minecraft/textures/block/water_still.png", png(2, 16, 64)},
            {"assets/minecraft/textures/block/water_still.png.mcmeta",
             "{\"animation\":{\"frametime\":2}}"},
            {"assets/minecraft/textures/item/apple.png", png(10 + variant)},
            {"assets/minecraft/textures/gui/title/background/panorama_1.png", png(40)}};
}
QMap<QString, QByteArray> assetObjects() {
    return {{"minecraft/textures/gui/title/background/panorama_0.png", png(30, 32, 32)},
            {"minecraft/textures/gui/title/background/panorama_1.png", png(41)}, // jar wins
            {"minecraft/sounds/ambient/cave/cave1.ogg", QByteArray(100, 'o')},
            {"minecraft/lang/de_de.json", "{}"},
            {"icons/icon_16x16.png", png(50)}};
}
const QStringList expectedTextures{
    "assets/minecraft/textures/block/stone.png",
    "assets/minecraft/textures/block/water_still.png",
    "assets/minecraft/textures/block/water_still.png.mcmeta",
    "assets/minecraft/textures/gui/title/background/panorama_0.png",
    "assets/minecraft/textures/gui/title/background/panorama_1.png",
    "assets/minecraft/textures/item/apple.png"};
} // namespace

class BaselineCacheTest : public QObject {
    Q_OBJECT
    QNetworkAccessManager network;

    BaselineInstaller::Options options(const Mojang &mojang, const QString &root) {
        BaselineInstaller::Options o;
        o.cacheRoot = root;
        o.manifestUrl = QUrl(mojang.server.url("/mc/game/version_manifest_v2.json"));
        o.objectBaseUrl = QUrl(mojang.server.url("/objects/"));
        o.policy = loopback();
        o.network = &network;
        o.lockTimeoutMs = 60000;
        return o;
    }
    // Runs one population to completion; returns finished()'s error ("" = success).
    static QString run(BaselineInstaller &installer, const QString &version) {
        QSignalSpy spy(&installer, &BaselineInstaller::finished);
        installer.start(version);
        if (spy.isEmpty() && !spy.wait(60000))
            return "timeout";
        return spy.first().first().toString();
    }
    static bool noStaging(const QString &root) {
        return !QFileInfo::exists(root + "/staging") || filesUnder(root + "/staging").isEmpty();
    }
    static bool noManifest(const QString &root) {
        return !QFileInfo::exists(root + "/entries") ||
               !filesUnder(root + "/entries").filter("manifest.json").size();
    }

  private slots:
    void manifestParsing() {
        const QByteArray sha(40, 'a');
        auto manifest = [&](const QJsonArray &versions) {
            return QJsonDocument(QJsonObject{{"versions", versions}}).toJson();
        };
        auto entry = [&](const QString &id, const QString &url, const QString &hash) {
            return QJsonObject{{"id", id}, {"type", "release"}, {"url", url}, {"sha1", hash}};
        };
        const QString good = "https://piston-meta.mojang.com/v1/packages/" + sha + "/1.21.11.json";
        QString error;
        const auto ref = parseVersionManifest(
            manifest({entry("1.21.10", good, sha), entry("1.21.11", good, sha)}), "1.21.11", &error);
        QVERIFY2(ref, qPrintable(error));
        QCOMPARE(ref->id, QString("1.21.11"));
        QCOMPARE(ref->sha1, QString(sha));
        QCOMPARE(ref->url, QUrl(good));
        // Absent: exact message, never a nearby version.
        QVERIFY(!parseVersionManifest(manifest({entry("1.21.10", good, sha),
                                                entry("1.21.11-pre1", good, sha),
                                                entry("1.21.111", good, sha)}),
                                      "1.21.11", &error));
        QCOMPARE(error, QString("Minecraft Java 1.21.11 is not listed in Mojang's version manifest"));
        QCOMPARE(error, notListedMessage("1.21.11"));
        // Wrong or look-alike host, http, other role host, user info, port, bad SHA-1 format.
        for (const auto &url : {QString("https://evil.example/v1/x.json"),
                                QString("https://piston-meta.mojang.com.evil.example/x.json"),
                                QString("https://evilpiston-meta.mojang.com/x.json"),
                                QString("http://piston-meta.mojang.com/x.json"),
                                QString("https://piston-data.mojang.com/x.json"),
                                QString("https://user@piston-meta.mojang.com/x.json"),
                                QString("https://piston-meta.mojang.com:8443/x.json"),
                                QString("http://127.0.0.1/x.json")}) {
            QVERIFY2(!parseVersionManifest(manifest({entry("1.21.11", url, sha)}), "1.21.11",
                                           &error),
                     qPrintable(url));
            QVERIFY(error.contains("unexpected address"));
        }
        for (const auto &hash : {QString(39, 'a'), QString(40, 'A'), QString(64, 'a'),
                                 QString(39, 'a') + 'g', QString()})
            QVERIFY2(!parseVersionManifest(manifest({entry("1.21.11", good, hash)}), "1.21.11"),
                     qPrintable(hash));
        // Malformed JSON and unexpected types.
        QVERIFY(!parseVersionManifest("{not json", "1.21.11", &error));
        QVERIFY(error.contains("not valid JSON"));
        QVERIFY(!parseVersionManifest("[]", "1.21.11"));
        QVERIFY(!parseVersionManifest("{\"versions\":{}}", "1.21.11"));
        QVERIFY(!parseVersionManifest("{\"versions\":[1]}", "1.21.11"));
        QVERIFY(!parseVersionManifest("{\"versions\":[{\"id\":5}]}", "1.21.11"));
        QVERIFY(!parseVersionManifest(manifest({entry("1.21.11", good, sha),
                                                entry("1.21.11", good, sha)}),
                                      "1.21.11", &error));
        QVERIFY(error.contains("more than once"));
    }

    void versionJsonParsing() {
        const QString sha(40, 'b');
        auto json = [&](const std::function<void(QJsonObject &)> &tweak = {}) {
            QJsonObject v{
                {"id", "1.21.11"},
                {"downloads",
                 QJsonObject{{"client",
                              QJsonObject{{"url", "https://piston-data.mojang.com/v1/objects/" +
                                                      sha + "/client.jar"},
                                          {"sha1", sha},
                                          {"size", 30000000}}}}},
                {"assetIndex", QJsonObject{{"id", "29"},
                                           {"url", "https://piston-meta.mojang.com/v1/packages/" +
                                                       sha + "/29.json"},
                                           {"sha1", sha},
                                           {"size", 500000}}}};
            if (tweak)
                tweak(v);
            return QJsonDocument(v).toJson();
        };
        auto refFor = [&](const QByteArray &bytes) {
            return VersionRef{"1.21.11", "release",
                              QUrl("https://piston-meta.mojang.com/v1/packages/x/1.21.11.json"),
                              sha1(bytes)};
        };
        QString error;
        const auto bytes = json();
        const auto meta = parseVersionJson(bytes, refFor(bytes), &error);
        QVERIFY2(meta, qPrintable(error));
        QCOMPARE(meta->client.size, qint64(30000000));
        QCOMPARE(meta->assetIndexId, QString("29"));
        QCOMPARE(meta->assetIndex.size, qint64(500000));
        QCOMPARE(meta->versionSha1, sha1(bytes));
        // Body must match the manifest's SHA-1.
        auto wrong = refFor(bytes);
        wrong.sha1 = QString(40, 'c');
        QVERIFY(!parseVersionJson(bytes, wrong, &error));
        QVERIFY(error.contains("SHA-1"));
        auto set = [](const char *object, const char *key, const QJsonValue &value) {
            return [=](QJsonObject &v) {
                if (QString(object) == "client") {
                    auto downloads = v["downloads"].toObject();
                    auto client = downloads["client"].toObject();
                    client[key] = value;
                    downloads["client"] = client;
                    v["downloads"] = downloads;
                } else {
                    auto index = v[object].toObject();
                    index[key] = value;
                    v[object] = index;
                }
            };
        };
        const QList<std::function<void(QJsonObject &)>> bad{
            [](QJsonObject &v) { v["id"] = "1.21.10"; },
            [](QJsonObject &v) { v.remove("assetIndex"); },
            [](QJsonObject &v) { v.remove("downloads"); },
            set("client", "url", "https://piston-meta.mojang.com/client.jar"), // wrong role host
            set("client", "url", "http://piston-data.mojang.com/client.jar"),
            set("client", "url", "https://piston-data.mojang.com.evil.example/client.jar"),
            set("client", "sha1", QString(40, 'Z')),
            set("client", "size", 0),
            set("client", "size", -5),
            set("client", "size", 1.5),
            set("client", "size", "100"),
            set("client", "size", double(256LL * 1024 * 1024 + 1)),
            set("assetIndex", "size", double(4LL * 1024 * 1024 + 1)),
            set("assetIndex", "url", "https://piston-data.mojang.com/29.json"),
            set("assetIndex", "id", "../29"),
            set("assetIndex", "id", ""),
            set("assetIndex", "sha1", QString(41, 'a'))};
        for (qsizetype i = 0; i < bad.size(); ++i) {
            const auto b = json(bad[i]);
            QVERIFY2(!parseVersionJson(b, refFor(b)), qPrintable(QString::number(i)));
        }
        QVERIFY(!parseVersionJson("nope", refFor("nope")));
    }

    void assetIndexParsing() {
        auto object = [](const QString &hash, const QJsonValue &size) {
            return QJsonObject{{"hash", hash}, {"size", size}};
        };
        const QString a(40, 'a'), b(40, 'b');
        QJsonObject objects{
            {"minecraft/textures/gui/title/background/panorama_0.png", object(a, 100)},
            {"minecraft/textures/block/x.png.mcmeta", object(b, 20)},
            {"minecraft/sounds/ambient/cave/cave1.ogg", object(a, 9000000)},
            {"minecraft/lang/en_us.json", object(b, 0)},
            {"minecraft/textures/notes.txt", object(b, 5)},
            {"icons/minecraft.icns", object(a, 7)}};
        QString error;
        auto parsed = parseAssetIndexTextures(QJsonDocument(QJsonObject{{"objects", objects}}).toJson(),
                                              &error);
        QVERIFY2(parsed, qPrintable(error));
        QCOMPARE(parsed->size(), 2);
        QCOMPARE(parsed->at(0).path, QString("assets/minecraft/textures/block/x.png.mcmeta"));
        QCOMPARE(parsed->at(1).path,
                 QString("assets/minecraft/textures/gui/title/background/panorama_0.png"));
        QCOMPARE(parsed->at(1).sha1, a);
        QCOMPARE(parsed->at(1).size, qint64(100));
        const QList<std::pair<QString, QJsonValue>> bad{
            {"minecraft/textures/a.png", object("ABC", 1)},
            {"minecraft/textures/a.png", object(a, -1)},
            {"minecraft/textures/a.png", object(a, 1.5)},
            {"minecraft/textures/a.png", object(a, "1")},
            {"minecraft/sounds/a.ogg", object(a.left(39), 1)}, // every entry is validated
            {"minecraft/textures/a.png", QJsonValue(5)},
            {"minecraft/textures/../a.png", object(a, 1)},
            {"minecraft/textures/Block/a.png", object(a, 1)},
            {"minecraft/textures/a.png", object(a, 0)}};
        for (const auto &[key, value] : bad) {
            auto o = objects;
            o.insert(key, value);
            QVERIFY2(!parseAssetIndexTextures(QJsonDocument(QJsonObject{{"objects", o}}).toJson()),
                     qPrintable(key));
        }
        QVERIFY(!parseAssetIndexTextures("{"));
        QVERIFY(!parseAssetIndexTextures("{\"objects\":[]}"));
        QVERIFY(!parseAssetIndexTextures("[]"));
    }

    void identityDependsOnClientAndAssetIndexOnly() {
        VersionMetadata m;
        m.id = "1.21.11";
        m.client = {QUrl("https://piston-data.mojang.com/c.jar"), QString(40, '1'), 30000000};
        m.assetIndexId = "29";
        m.assetIndex = {QUrl("https://piston-meta.mojang.com/29.json"), QString(40, '2'), 500000};
        m.versionSha1 = QString(40, '3');
        const auto base = identityKey(m);
        QVERIFY(validSha256(base));
        QCOMPARE(base, sha256(QString("nova-vanilla-baseline/1|java|1.21.11|%1|30000000|29|%2")
                                  .arg(QString(40, '1'), QString(40, '2'))
                                  .toUtf8()));
        auto changed = [&](const std::function<void(VersionMetadata &)> &change) {
            auto copy = m;
            change(copy);
            return identityKey(copy);
        };
        QVERIFY(changed([](VersionMetadata &c) { c.client.sha1 = QString(40, '4'); }) != base);
        QVERIFY(changed([](VersionMetadata &c) { c.client.size += 1; }) != base);
        QVERIFY(changed([](VersionMetadata &c) { c.assetIndex.sha1 = QString(40, '5'); }) != base);
        QVERIFY(changed([](VersionMetadata &c) { c.assetIndexId = "30"; }) != base);
        QVERIFY(changed([](VersionMetadata &c) { c.id = "26.1"; }) != base);
        // A republished version JSON (new SHA-1 / URL) with the same client keeps the identity.
        QCOMPARE(changed([](VersionMetadata &c) {
                     c.versionSha1 = QString(40, '6');
                     c.versionUrl = QUrl("https://piston-meta.mojang.com/other.json");
                     c.client.url = QUrl("https://piston-data.mojang.com/mirror.jar");
                 }),
                 base);
    }

    void pathRules_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("valid");
        const QString p = "assets/minecraft/textures/";
        QTest::newRow("block") << p + "block/stone.png" << true;
        QTest::newRow("mcmeta") << p + "block/water_still.png.mcmeta" << true;
        QTest::newRow("chars") << p + "entity/a-b_c.d/x9.png" << true;
        QTest::newRow("33 segments") << p + QString("a/").repeated(29) + "x.png" << true; // 3 prefix + 29 + file
        QTest::newRow("255 chars") << p + QString(251, 'a') + ".png" << true;
        QTest::newRow("console prefix ok") << p + "block/console.png" << true;
        QTest::newRow("com10 ok") << p + "block/com10.png" << true;
        QTest::newRow("34 segments") << p + QString("a/").repeated(30) + "x.png" << false;
        QTest::newRow("256 chars") << p + QString(252, 'a') + ".png" << false;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("traversal") << p + "../x.png" << false;
        QTest::newRow("traversal deep") << p + "block/../../../../x.png" << false;
        QTest::newRow("dot segment") << p + "./x.png" << false;
        QTest::newRow("empty segment") << p + "block//x.png" << false;
        QTest::newRow("leading slash") << "/" + p + "x.png" << false;
        QTest::newRow("backslash") << p + "block\\x.png" << false;
        QTest::newRow("colon") << p + "block/x.png:stream" << false;
        QTest::newRow("drive") << "C:/" + p + "x.png" << false;
        QTest::newRow("nul") << p + "block/x" + QChar(0) + ".png" << false;
        QTest::newRow("uppercase") << p + "block/Stone.png" << false;
        QTest::newRow("uppercase prefix") << "Assets/minecraft/textures/block/x.png" << false;
        QTest::newRow("space") << p + "block/a b.png" << false;
        QTest::newRow("unicode") << p + QString::fromUtf8("block/st\xc3\xb6ne.png") << false;
        QTest::newRow("con") << p + "block/con.png" << false;
        QTest::newRow("nul device") << p + "nul/x.png" << false;
        QTest::newRow("aux ext") << p + "block/aux.tar.png" << false;
        QTest::newRow("com1") << p + "block/com1.png" << false;
        QTest::newRow("lpt9") << p + "lpt9/x.png" << false;
        QTest::newRow("prn") << p + "prn.png" << false;
        QTest::newRow("trailing dot") << p + "block./x.png" << false;
        QTest::newRow("other namespace") << "assets/realms/textures/x.png" << false;
        QTest::newRow("models") << "assets/minecraft/models/block/x.png" << false;
        QTest::newRow("prefix lookalike") << "assets/minecraft/texturesx/x.png" << false;
        QTest::newRow("json") << p + "block/x.json" << false;
        QTest::newRow("mcmeta alone") << p + "block/x.mcmeta" << false;
        QTest::newRow("png dir only") << p + "block/" << false;
    }
    void pathRules() {
        QFETCH(QString, path);
        QFETCH(bool, valid);
        QString error;
        QCOMPARE(validBaselinePath(path, &error), valid);
        QCOMPARE(error.isEmpty(), valid);
    }

    void downloadPolicyIsPinned() {
        const auto p = vanillaPolicy();
        QCOMPARE(p.hosts, QStringList({"piston-meta.mojang.com", "piston-data.mojang.com",
                                       "resources.download.minecraft.net"}));
        QVERIFY(!p.allowLoopbackHttp);
        QCOMPARE(p.maxBytes, 256LL * 1024 * 1024);
        QCOMPARE(QUrl(defaultManifestUrl),
                 QUrl("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"));
        QVERIFY(testing::urlAllowed(QUrl(defaultManifestUrl), p));
        QVERIFY(testing::urlAllowed(QUrl("https://piston-data.mojang.com/v1/objects/x/client.jar"), p));
        QVERIFY(testing::urlAllowed(assetObjectUrl(QString(40, 'a')), p));
        QCOMPARE(assetObjectUrl(QString(40, 'a')),
                 QUrl("https://resources.download.minecraft.net/aa/" + QString(40, 'a')));
        for (const auto &url :
             {"http://piston-meta.mojang.com/mc/game/version_manifest_v2.json",
              "http://127.0.0.1/x", "https://127.0.0.1/x", "http://localhost/x",
              "https://piston-meta.mojang.com.evil.example/x", "https://evil.example/piston-meta.mojang.com",
              "https://piston-meta.mojang.co/x", "https://xpiston-data.mojang.com/x",
              "https://user:pass@piston-meta.mojang.com/x", "https://user@resources.download.minecraft.net/x",
              "https://piston-meta.mojang.com:444/x", "https://piston-meta.mojang.com/x#frag",
              "ftp://piston-data.mojang.com/x", "https://launcher.mojang.com/x"})
            QVERIFY2(!testing::urlAllowed(QUrl(url), p), url);
        // The launcher policies keep their own user agent and hosts.
        QCOMPARE(testing::officialLauncherPolicy().userAgent, QByteArray("NovaStudio-LauncherSetup"));
        QCOMPARE(p.userAgent, QByteArray("NovaStudio-VanillaBaseline"));
    }

    void defaultRootUsesEnvironment() {
        const auto saved = qgetenv("NOVA_VANILLA_BASELINE_ROOT");
        auto restore = qScopeGuard([&] {
            if (saved.isEmpty())
                qunsetenv("NOVA_VANILLA_BASELINE_ROOT");
            else
                qputenv("NOVA_VANILLA_BASELINE_ROOT", saved);
        });
        QTemporaryDir temp;
        qputenv("NOVA_VANILLA_BASELINE_ROOT", QDir::toNativeSeparators(temp.path() + "/vb").toUtf8());
        QCOMPARE(BaselineCache::defaultRoot(), QDir::cleanPath(temp.path() + "/vb"));
        QCOMPARE(BaselineInstaller::Options().cacheRoot, QDir::cleanPath(temp.path() + "/vb"));
        qunsetenv("NOVA_VANILLA_BASELINE_ROOT");
        QCOMPARE(BaselineCache::defaultRoot(),
                 QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                     "/vanilla-baseline");
    }

    void populateFromMockServer() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        Mojang mojang;
        const auto published = mojang.publish("1.21.11", clientEntries(), assetObjects());
        QVERIFY(validSha256(published.identity));
        BaselineInstaller installer(options(mojang, root));
        QStringList stages;
        connect(&installer, &BaselineInstaller::progress, this,
                [&](const QString &stage) { if (!stages.contains(stage)) stages << stage; });
        QSignalSpy finished(&installer, &BaselineInstaller::finished);
        QCOMPARE(run(installer, "1.21.11"), QString());
        QTest::qWait(20);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(installer.error(), Error::None);
        QVERIFY(!installer.reusedExisting());
        QCOMPARE(installer.identity(), published.identity);
        QVERIFY(stages.contains(stageClient) && stages.contains(stageExtract));
        // Requests: manifest, version JSON, asset index, jar, one asset object (panorama_0;
        // panorama_1 comes from the jar; sounds, languages and icons are never fetched).
        QCOMPARE(mojang.server.count("/v1/objects/"), 1);
        QCOMPARE(mojang.server.count("/objects/"), 1);
        QCOMPARE(mojang.server.requests.size(), 5);

        const auto entry = installer.entry();
        QCOMPARE(entry.version, QString("1.21.11"));
        QCOMPARE(entry.identity, published.identity);
        QCOMPARE(QStringList(entry.files.keys()), expectedTextures);
        QCOMPARE(entry.files.value("assets/minecraft/textures/gui/title/background/panorama_0.png").origin,
                 QString("asset-index"));
        QCOMPARE(entry.files.value("assets/minecraft/textures/gui/title/background/panorama_1.png").origin,
                 QString("jar"));
        BaselineCache cache(root);
        QString error;
        const auto loaded = cache.load("1.21.11", published.identity, Check::Full, &error);
        QVERIFY2(loaded, qPrintable(error));
        QCOMPARE(loaded->files.keys(), entry.files.keys());
        QCOMPARE(loaded->source.value("client").toObject().value("sha1").toString(),
                 sha1(published.jar));
        QByteArray bytes;
        QVERIFY(cache.readVerified(*loaded, "assets/minecraft/textures/block/stone.png", &bytes));
        QCOMPARE(bytes, png(1));
        QVERIFY(cache.readVerified(*loaded, "assets/minecraft/textures/block/water_still.png.mcmeta",
                                   &bytes));
        QCOMPARE(bytes, QByteArray("{\"animation\":{\"frametime\":2}}"));
        QVERIFY(cache.readVerified(*loaded,
                                   "assets/minecraft/textures/gui/title/background/panorama_0.png",
                                   &bytes));
        QCOMPARE(bytes, png(30, 32, 32));
        QVERIFY(!cache.readVerified(*loaded, "assets/minecraft/models/block/stone.json", &bytes));
        QVERIFY(cache.verify(*loaded).isEmpty());
        // Layout: read-only content-addressed objects, one manifest, no jar, no staging.
        for (const auto &rel : expectedTextures) {
            const auto object = loaded->objectPath(rel);
            QVERIFY(object.startsWith(root + "/objects/"));
            QVERIFY2(readOnlyAttribute(object), qPrintable(rel));
        }
        const auto all = filesUnder(root);
        QVERIFY(all.contains("entries/1.21.11/" + published.identity.left(32) + "/manifest.json"));
        QVERIFY(all.filter(".jar").isEmpty());
        QVERIFY(all.filter(".part").isEmpty());
        QVERIFY(all.filter(".tmp").isEmpty());
        QCOMPARE(all.filter("objects/").size(), expectedTextures.size());
        QVERIFY(noStaging(root));
        // Nothing but texture bytes is stored: the class file and models never appear.
        for (const auto &file : all.filter("objects/"))
            QVERIFY(file.endsWith(".png") || file.endsWith(".mcmeta"));
        const auto summaries = cache.list();
        QCOMPARE(summaries.size(), 1);
        QCOMPARE(summaries[0].status, BaselineSummary::Status::Complete);
        QCOMPARE(summaries[0].identity, published.identity);
        QCOMPARE(summaries[0].files, int(expectedTextures.size()));
        QCOMPARE(summaries[0].sharedBytes, qint64(0));
        QCOMPARE(summaries[0].uniqueBytes, summaries[0].bytes);
        QVERIFY(cache.totalBytes() > summaries[0].bytes);
        auto newest = cache.latest("1.21.11");
        QVERIFY(newest);
        QCOMPARE(newest->identity, published.identity);

        // Second populate of the same identity: zero jar or object requests.
        mojang.server.requests.clear();
        BaselineInstaller again(options(mojang, root));
        QCOMPARE(run(again, "1.21.11"), QString());
        QVERIFY(again.reusedExisting());
        QCOMPARE(again.entry().identity, published.identity);
        QCOMPARE(mojang.server.count("/v1/objects/"), 0);
        QCOMPARE(mojang.server.count("/objects/"), 0);
        // With the identity known (a project's record), no request at all.
        mojang.server.requests.clear();
        auto o = options(mojang, root);
        o.expectedIdentity = published.identity;
        BaselineInstaller known(o);
        QCOMPARE(run(known, "1.21.11"), QString());
        QVERIFY(known.reusedExisting());
        QVERIFY(mojang.server.requests.isEmpty());

        // Offline: the server is gone, the entry still loads and still populates "for free".
        mojang.server.close();
        QVERIFY(cache.load("1.21.11", published.identity, Check::Full));
        BaselineInstaller offline(o);
        QCOMPARE(run(offline, "1.21.11"), QString());
        QVERIFY(offline.reusedExisting());
    }

    void integrityFailuresLeaveNothing_data() {
        QTest::addColumn<int>("scenario");
        QTest::newRow("client sha1 mismatch") << 0;
        QTest::newRow("client size mismatch") << 1;
        QTest::newRow("content-length longer than body") << 2;
        QTest::newRow("short body") << 3;
        QTest::newRow("asset object mismatch") << 4;
        QTest::newRow("asset index mismatch") << 5;
        QTest::newRow("jar not a zip") << 6;
    }
    void integrityFailuresLeaveNothing() {
        QFETCH(int, scenario);
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        Mojang mojang;
        std::function<void(QJsonObject &)> tweak;
        auto client = [](QJsonObject &v, const char *key, const QJsonValue &value) {
            auto downloads = v["downloads"].toObject();
            auto c = downloads["client"].toObject();
            c[key] = value;
            downloads["client"] = c;
            v["downloads"] = downloads;
        };
        if (scenario == 0)
            tweak = [&](QJsonObject &v) { client(v, "sha1", sha1("other")); };
        if (scenario == 1)
            tweak = [&](QJsonObject &v) {
                const auto size = v.value("downloads").toObject().value("client").toObject().value("size");
                client(v, "size", size.toInt() + 10);
            };
        const QByteArray junk(4096, 'j'); // correctly hashed, but not a jar
        if (scenario == 6)
            tweak = [&](QJsonObject &v) {
                client(v, "sha1", sha1(junk));
                client(v, "size", junk.size());
            };
        if (scenario == 5)
            tweak = [&](QJsonObject &v) {
                auto index = v["assetIndex"].toObject();
                index["sha1"] = sha1("other");
                v["assetIndex"] = index;
            };
        const auto published = mojang.publish("1.21.11", clientEntries(), assetObjects(), tweak);
        auto &route = mojang.server.routes[published.clientPath];
        if (scenario == 2)
            route.contentLength = route.body.size() + 100;
        if (scenario == 3)
            route.body.chop(100), route.contentLength = route.body.size();
        if (scenario == 4) {
            const auto h = sha1(assetObjects().value("minecraft/textures/gui/title/background/panorama_0.png"));
            mojang.server.routes["/objects/" + h.left(2) + '/' + h].body = png(77, 32, 32);
        }
        if (scenario == 6)
            route = {200, junk};
        BaselineInstaller installer(options(mojang, root));
        const auto error = run(installer, "1.21.11");
        QVERIFY(!error.isEmpty());
        if (scenario == 2)
            QVERIFY2(installer.error() == Error::Integrity || installer.error() == Error::Network,
                     qPrintable(error));
        else
            QVERIFY2(installer.error() == Error::Integrity, qPrintable(error));
        QVERIFY(!error.contains(temp.path())); // no local paths in messages
        QVERIFY(noManifest(root));
        QVERIFY(noStaging(root));
        QVERIFY(filesUnder(root).filter(".jar").isEmpty());
        QVERIFY(BaselineCache(root).list().isEmpty());
    }

    void maliciousJarEntriesFail_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("link");
        const QString p = "assets/minecraft/textures/";
        QTest::newRow("traversal") << p + "../x.png" << false;
        QTest::newRow("deep traversal") << p + "block/../../../../../evil.png" << false;
        QTest::newRow("uppercase") << p + "block/Stone.png" << false;
        QTest::newRow("uppercase prefix") << "Assets/Minecraft/textures/block/x.png" << false;
        QTest::newRow("device") << p + "block/con.png" << false;
        QTest::newRow("space") << p + "block/a b.png" << false;
        QTest::newRow("other suffix") << p + "block/readme.txt" << false;
        QTest::newRow("link") << p + "block/link.png" << true;
    }
    void maliciousJarEntriesFail() {
        QFETCH(QString, name);
        QFETCH(bool, link);
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        Mojang mojang;
        auto entries = clientEntries();
        Mojang::Published published;
        if (link) {
            // QZipWriter writes the link entry; serve that jar under the published client URL.
            const auto jar = zip(entries, {name});
            published = mojang.publish("1.21.11", entries, assetObjects(), [&](QJsonObject &v) {
                auto downloads = v["downloads"].toObject();
                auto c = downloads["client"].toObject();
                c["sha1"] = sha1(jar);
                c["size"] = jar.size();
                downloads["client"] = c;
                v["downloads"] = downloads;
            });
            mojang.server.routes[published.clientPath] = {200, jar};
        } else {
            entries.append({name, png(5)});
            mojang.publish("1.21.11", entries, assetObjects());
        }
        BaselineInstaller installer(options(mojang, root));
        const auto error = run(installer, "1.21.11");
        QVERIFY2(installer.error() == Error::Integrity, qPrintable(error));
        QVERIFY(error.contains("unexpected data") || error.contains("link"));
        QVERIFY(noManifest(root));
        QVERIFY(noStaging(root));
        // Nothing was written outside the cache, wherever the entry pointed.
        QCOMPARE(QDir(temp.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
                 QStringList({"cache"}));
    }

    void notListedNeverSubstitutes() {
        QTemporaryDir temp;
        Mojang mojang;
        mojang.publish("1.21.10", clientEntries());
        mojang.server.requests.clear();
        BaselineInstaller installer(options(mojang, temp.path() + "/cache"));
        const auto error = run(installer, "1.21.11");
        QCOMPARE(installer.error(), Error::NotListed);
        QCOMPARE(error, notListedMessage("1.21.11"));
        QCOMPARE(mojang.server.requests, QStringList({"/mc/game/version_manifest_v2.json"}));
        BaselineInstaller invalid(options(mojang, temp.path() + "/cache"));
        QVERIFY(!run(invalid, "../1.21").isEmpty());
        QCOMPARE(invalid.error(), Error::NotListed);
    }

    void interruptionLeavesNoEntryThenRetrySucceeds() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        Mojang mojang;
        const auto published = mojang.publish("1.21.11", clientEntries(), assetObjects());
        auto o = options(mojang, root);
        int calls = 0;
        o.beforePublish = [&] {
            ++calls;
            return false; // as if the process stopped just before the manifest rename
        };
        BaselineInstaller interrupted(o);
        QVERIFY(!run(interrupted, "1.21.11").isEmpty());
        QCOMPARE(calls, 1);
        QCOMPARE(interrupted.error(), Error::Io);
        QVERIFY(noManifest(root));
        QVERIFY(noStaging(root));
        QVERIFY(!BaselineCache(root).load("1.21.11", published.identity, Check::Manifest));
        QVERIFY(!BaselineCache(root).latest("1.21.11"));
        // Orphan objects are harmless: the retry re-verifies and reuses them.
        BaselineInstaller retry(options(mojang, root));
        QCOMPARE(run(retry, "1.21.11"), QString());
        QVERIFY(!retry.reusedExisting());
        QVERIFY(BaselineCache(root).load("1.21.11", published.identity, Check::Full));
        QCOMPARE(mojang.server.count(published.clientPath), 2);
    }

    void corruptObjectIsDetectedAndRepaired() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        Mojang mojang;
        const auto a = mojang.publish("1.21.11", clientEntries(0), assetObjects());
        BaselineInstaller first(options(mojang, root));
        QCOMPARE(run(first, "1.21.11"), QString());
        const auto b = mojang.publish("26.1", clientEntries(1), assetObjects());
        BaselineInstaller second(options(mojang, root));
        QCOMPARE(run(second, "26.1"), QString());
        BaselineCache cache(root);
        const auto otherManifest = cache.manifestPath("26.1", b.identity);
        const auto otherBytes = readAll(otherManifest);
        const auto otherModified = QFileInfo(otherManifest).lastModified();
        QVERIFY(!otherBytes.isEmpty());

        // 1. Different size: load(Sizes) refuses the entry.
        const QString apple = "assets/minecraft/textures/item/apple.png";
        auto entry = cache.load("1.21.11", a.identity, Check::Manifest);
        QVERIFY(entry);
        const auto object = entry->objectPath(apple);
        QVERIFY(fs::setReadOnly(object, false));
        QVERIFY(writeFile(object, "damaged"));
        QString error;
        QVERIFY(!cache.load("1.21.11", a.identity, Check::Sizes, &error));
        QVERIFY2(error.contains(apple), qPrintable(error));
        QVERIFY(!error.contains(temp.path()));
        QCOMPARE(cache.verify(*entry), QStringList({apple}));
        QByteArray bytes;
        QVERIFY(!cache.readVerified(*entry, apple, &bytes));
        QCOMPARE(cache.list()[0].status, BaselineSummary::Status::Corrupt);
        BaselineInstaller repair(options(mojang, root));
        QCOMPARE(run(repair, "1.21.11"), QString());
        QVERIFY(!repair.reusedExisting());
        QVERIFY(cache.load("1.21.11", a.identity, Check::Full));
        QVERIFY(readOnlyAttribute(object));

        // 2. Same size, different bytes: only a full check sees it; repair re-hashes.
        auto damaged = readAll(object);
        damaged[damaged.size() - 1] = char(damaged.back() ^ 0x55);
        QVERIFY(fs::setReadOnly(object, false));
        QVERIFY(writeFile(object, damaged));
        QVERIFY(cache.load("1.21.11", a.identity, Check::Sizes));
        QVERIFY(!cache.load("1.21.11", a.identity, Check::Full));
        QCOMPARE(cache.verify(*entry), QStringList({apple}));
        auto o = options(mojang, root);
        o.repair = true;
        BaselineInstaller full(o);
        QCOMPARE(run(full, "1.21.11"), QString());
        QVERIFY(cache.verify(*entry).isEmpty());
        QVERIFY(cache.load("1.21.11", a.identity, Check::Full));

        // The other version's entry was never touched.
        QCOMPARE(readAll(otherManifest), otherBytes);
        QCOMPARE(QFileInfo(otherManifest).lastModified(), otherModified);
        QVERIFY(cache.load("26.1", b.identity, Check::Full));
    }

    void damagedManifestIsNotAnEntry() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        Mojang mojang;
        const auto a = mojang.publish("1.21.11", clientEntries(), assetObjects());
        BaselineInstaller installer(options(mojang, root));
        QCOMPARE(run(installer, "1.21.11"), QString());
        BaselineCache cache(root);
        const auto path = cache.manifestPath("1.21.11", a.identity);
        const auto good = readAll(path);
        // Any edit breaks the digest; a recomputed digest with a forged file breaks the paths.
        auto edited = good;
        edited.replace("block/stone.png", "block/stonx.png");
        QVERIFY(writeFile(path, edited));
        QString error;
        QVERIFY(!cache.load("1.21.11", a.identity, Check::Manifest, &error));
        QVERIFY(error.contains("damaged"));
        auto forged = QJsonDocument::fromJson(good).object();
        forged.remove("digest");
        auto files = forged["files"].toObject();
        files["assets/minecraft/textures/../../x.png"] = files.begin().value();
        forged["files"] = files;
        forged["digest"] = sha256(QJsonDocument(forged).toJson(QJsonDocument::Compact));
        QVERIFY(writeFile(path, QJsonDocument(forged).toJson(QJsonDocument::Compact)));
        QVERIFY(!cache.load("1.21.11", a.identity, Check::Manifest));
        QCOMPARE(cache.list().size(), 1);
        QCOMPARE(cache.list()[0].status, BaselineSummary::Status::Corrupt);
        QCOMPARE(cache.list()[0].identity, a.identity.left(32));
        // A repair population rewrites it.
        BaselineInstaller repair(options(mojang, root));
        QCOMPARE(run(repair, "1.21.11"), QString());
        QVERIFY(cache.load("1.21.11", a.identity, Check::Full));
    }

    void otherIdentityManifestLossLeavesEntryLoadable() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        Mojang mojang;
        const auto a = mojang.publish("1.21.11", clientEntries(0), assetObjects());
        BaselineInstaller first(options(mojang, root));
        QCOMPARE(run(first, "1.21.11"), QString());
        // Mojang ships a new client for the same version id: a new identity and entry.
        const auto b = mojang.publish("1.21.11", clientEntries(3), assetObjects());
        QVERIFY(a.identity != b.identity);
        BaselineInstaller second(options(mojang, root));
        QCOMPARE(run(second, "1.21.11"), QString());
        QVERIFY(!second.reusedExisting());
        BaselineCache cache(root);
        QVERIFY(cache.load("1.21.11", a.identity, Check::Full));
        QVERIFY(cache.load("1.21.11", b.identity, Check::Full));
        QCOMPARE(cache.list().size(), 2);
        QVERIFY(QFile::remove(cache.manifestPath("1.21.11", b.identity)));
        QVERIFY(cache.load("1.21.11", a.identity, Check::Full));
        QVERIFY(!cache.load("1.21.11", b.identity));
        QCOMPARE(cache.latest("1.21.11")->identity, a.identity);
        QCOMPARE(cache.list().size(), 1);
    }

    void concurrentInstallersDownloadOnce() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        Mojang mojang;
        const auto a = mojang.publish("1.21.11", clientEntries(), assetObjects());
        QNetworkAccessManager otherNetwork;
        auto o2 = options(mojang, root);
        o2.network = &otherNetwork;
        BaselineInstaller one(options(mojang, root)), two(o2);
        QSignalSpy doneOne(&one, &BaselineInstaller::finished), doneTwo(&two, &BaselineInstaller::finished);
        one.start("1.21.11");
        two.start("1.21.11");
        QTRY_VERIFY_WITH_TIMEOUT(doneOne.size() == 1 && doneTwo.size() == 1, 60000);
        QCOMPARE(doneOne.first().first().toString(), QString());
        QCOMPARE(doneTwo.first().first().toString(), QString());
        QCOMPARE(mojang.server.count(a.clientPath), 1);
        QCOMPARE(int(one.reusedExisting()) + int(two.reusedExisting()), 1);
        QCOMPARE(one.entry().identity, a.identity);
        QCOMPARE(two.entry().identity, a.identity);
        QVERIFY(BaselineCache(root).load("1.21.11", a.identity, Check::Full));
    }

    void busyLockIsReported() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        QVERIFY(QDir().mkpath(root));
        Mojang mojang;
        const auto a = mojang.publish("1.21.11", clientEntries(), assetObjects());
        QLockFile held(root + "/cache.lock"); // another process populating
        QVERIFY(held.tryLock(0));
        auto o = options(mojang, root);
        o.lockTimeoutMs = 300;
        BaselineInstaller installer(o);
        QElapsedTimer clock;
        clock.start();
        QVERIFY(!run(installer, "1.21.11").isEmpty());
        QCOMPARE(installer.error(), Error::Busy);
        QVERIFY(clock.elapsed() >= 250);
        QVERIFY(mojang.server.requests.isEmpty());
        // The wait was cancellable too.
        o.lockTimeoutMs = 60000;
        BaselineInstaller waiting(o);
        QSignalSpy spy(&waiting, &BaselineInstaller::finished);
        waiting.start("1.21.11");
        QTest::qWait(150);
        waiting.cancel();
        QCOMPARE(spy.size(), 1);
        QCOMPARE(waiting.error(), Error::Cancelled);
        held.unlock();
        BaselineInstaller later(options(mojang, root));
        QCOMPARE(run(later, "1.21.11"), QString());
        // remove() takes the same lock.
        QVERIFY(held.tryLock(0));
        QString error;
        QVERIFY(!BaselineCache(root).remove("1.21.11", a.identity, &error));
        QVERIFY(error.contains("Another Nova window"));
        QVERIFY(BaselineCache(root).load("1.21.11", a.identity));
    }

    void removeKeepsSharedObjectsAndEverythingOutside() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        // Sentinels: a project folder beside the cache and a file outside it.
        const auto projectFile = temp.path() + "/project/assets/minecraft/textures/block/stone.png";
        QVERIFY(writeFile(projectFile, png(1)));
        QVERIFY(writeFile(temp.path() + "/outside.txt", "keep"));
        Mojang mojang;
        const auto a = mojang.publish("1.21.11", clientEntries(0), assetObjects());
        BaselineInstaller first(options(mojang, root));
        QCOMPARE(run(first, "1.21.11"), QString());
        const auto b = mojang.publish("26.1", clientEntries(1), assetObjects());
        BaselineInstaller second(options(mojang, root));
        QCOMPARE(run(second, "26.1"), QString());
        BaselineCache cache(root);
        const auto entryA = *cache.load("1.21.11", a.identity);
        const auto entryB = *cache.load("26.1", b.identity);
        const QString apple = "assets/minecraft/textures/item/apple.png";
        const auto uniqueA = entryA.objectPath(apple);
        QVERIFY(uniqueA != entryB.objectPath(apple));
        // A file in the cache with an unknown name is never deleted.
        const auto stray = QFileInfo(uniqueA).path() + "/notes.txt";
        QVERIFY(writeFile(stray, "user note"));
        const auto summaries = cache.list();
        QCOMPARE(summaries.size(), 2);
        for (const auto &s : summaries) {
            QCOMPARE(s.status, BaselineSummary::Status::Complete);
            QVERIFY(s.sharedBytes > 0);
            QCOMPARE(s.bytes, s.uniqueBytes + s.sharedBytes);
        }
        const auto summaryA = std::find_if(summaries.cbegin(), summaries.cend(),
                                           [](const BaselineSummary &s) { return s.version == "1.21.11"; });
        QCOMPARE(summaryA->uniqueBytes, entryA.files.value(apple).size);

        RemoveReport report;
        QString error;
        QVERIFY2(cache.remove("1.21.11", a.identity, &error, &report), qPrintable(error));
        QVERIFY(!report.collectionSkipped);
        QCOMPARE(report.objectsRemoved, 1);
        QCOMPARE(report.bytesRemoved, entryA.files.value(apple).size);
        QVERIFY(!QFileInfo::exists(uniqueA));
        QVERIFY(!QFileInfo::exists(root + "/entries/1.21.11"));
        QVERIFY(!cache.load("1.21.11", a.identity));
        QVERIFY(cache.load("26.1", b.identity, Check::Full)); // shared objects kept
        QVERIFY(cache.verify(entryB).isEmpty());
        QCOMPARE(readAll(stray), QByteArray("user note"));
        QCOMPARE(readAll(projectFile), png(1));
        QCOMPARE(readAll(temp.path() + "/outside.txt"), QByteArray("keep"));
        QCOMPARE(cache.list().size(), 1);
        QVERIFY(!cache.remove("1.21.11", a.identity, &error)); // already gone
        QVERIFY(!cache.remove("../x", a.identity, &error));

        // An unreadable remaining manifest stops object collection entirely.
        const auto c = mojang.publish("26.3", clientEntries(2), assetObjects());
        BaselineInstaller third(options(mojang, root));
        QCOMPARE(run(third, "26.3"), QString());
        const auto entryC = *cache.load("26.3", c.identity);
        QVERIFY(writeFile(cache.manifestPath("26.1", b.identity), "{broken"));
        QVERIFY(cache.remove("26.3", c.identity, &error, &report));
        QVERIFY(report.collectionSkipped);
        QCOMPARE(report.objectsRemoved, 0);
        QVERIFY(QFileInfo::exists(entryC.objectPath(apple)));
        // The corrupt entry itself can be removed by its folder name.
        const auto corrupt = cache.list();
        QCOMPARE(corrupt.size(), 1);
        QCOMPARE(corrupt[0].status, BaselineSummary::Status::Corrupt);
        QVERIFY(cache.remove("26.1", corrupt[0].identity, &error, &report));
        QVERIFY(!report.collectionSkipped);
        QVERIFY(cache.list().isEmpty());
        QVERIFY(filesUnder(root + "/objects").filter(QRegularExpression("\\.(png|mcmeta)$")).isEmpty());
        QCOMPARE(readAll(stray), QByteArray("user note"));
        QCOMPARE(readAll(projectFile), png(1));
    }

#ifdef Q_OS_WIN
    void reparsePointsAreRefused() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        const auto outside = temp.path() + "/outside";
        QVERIFY(QDir().mkpath(root + "/entries"));
        QVERIFY(writeFile(outside + "/keep.png", "keep"));
        // A junction inside the cache (no privilege needed) pointing outside it.
        const auto link = root + "/objects";
        QProcess mk;
        mk.start("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(link),
                             QDir::toNativeSeparators(outside)});
        QVERIFY(mk.waitForFinished(10000));
        if (!QFileInfo(link).exists())
            QSKIP("Could not create a junction in the test folder");
        BaselineCache cache(root);
        QVERIFY(fs::isReparsePoint(link));
        QVERIFY(!cache.safePath(link + "/keep.png"));
        QString error;
        QVERIFY(!cache.publishObject("x", sha256("x"), "assets/minecraft/textures/a.png", &error));
        QVERIFY(!fs::removeRegularFile(link));
        QDir().rmdir(link); // removes the junction only
        QCOMPARE(readAll(outside + "/keep.png"), QByteArray("keep"));
    }
#endif

    void expectedIdentityMismatchDownloadsNothing() {
        QTemporaryDir temp;
        Mojang mojang;
        const auto a = mojang.publish("1.21.11", clientEntries(), assetObjects());
        auto o = options(mojang, temp.path() + "/cache");
        o.expectedIdentity = QString(64, 'a');
        BaselineInstaller installer(o);
        const auto error = run(installer, "1.21.11");
        QCOMPARE(installer.error(), Error::MetadataChanged);
        QVERIFY(error.contains("changed"));
        QCOMPARE(installer.identity(), a.identity);
        QCOMPARE(mojang.server.count(a.clientPath), 0);
        QCOMPARE(mojang.server.count("/objects/"), 0);
        QCOMPARE(mojang.server.requests.size(), 2); // manifest + version JSON only
    }

    void diskSpaceIsCheckedBeforeTheClient() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        Mojang mojang;
        const auto a = mojang.publish("1.21.11", clientEntries(), assetObjects());
        auto o = options(mojang, root);
        QString queried;
        o.freeSpace = [&](const QString &path) {
            queried = path;
            return qint64(1024 * 1024);
        };
        BaselineInstaller installer(o);
        const auto error = run(installer, "1.21.11");
        QCOMPARE(installer.error(), Error::DiskSpace);
        QVERIFY2(error.contains("65 MB needed") && error.contains("1 MB available"), qPrintable(error));
        QCOMPARE(queried, QDir::cleanPath(root));
        QCOMPARE(mojang.server.count(a.clientPath), 0);
        QVERIFY(noStaging(root));
        QVERIFY(noManifest(root));
    }

    void cancelDuringClientDownload() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        Mojang mojang;
        const auto a = mojang.publish("1.21.11", clientEntries(), assetObjects());
        mojang.server.routes[a.clientPath].stall = true;
        BaselineInstaller installer(options(mojang, root));
        QSignalSpy finished(&installer, &BaselineInstaller::finished);
        bool cancelled = false;
        connect(&installer, &BaselineInstaller::progress, this,
                [&](const QString &stage, qint64 done) {
                    if (stage == stageClient && done > 0 && !cancelled) {
                        cancelled = true;
                        QVERIFY(QFileInfo::exists(root + "/staging"));
                        installer.cancel();
                    }
                });
        installer.start("1.21.11");
        QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 30000);
        QVERIFY(cancelled);
        QCOMPARE(installer.error(), Error::Cancelled);
        QTest::qWait(50);
        QCOMPARE(finished.size(), 1);
        QVERIFY(noStaging(root));
        QVERIFY(!QFileInfo::exists(root + "/staging"));
        QVERIFY(noManifest(root));
        QVERIFY(filesUnder(root).filter(QRegularExpression("\\.(jar|part)$")).isEmpty());
        // The lock was released: a new run succeeds.
        mojang.server.routes[a.clientPath].stall = false;
        BaselineInstaller retry(options(mojang, root));
        QCOMPARE(run(retry, "1.21.11"), QString());
    }

    void destroyingARunningInstallerCleansUp() {
        QTemporaryDir temp;
        const auto root = temp.path() + "/cache";
        Mojang mojang;
        const auto a = mojang.publish("1.21.11", clientEntries(), assetObjects());
        mojang.server.routes[a.clientPath].stall = true;
        {
            BaselineInstaller installer(options(mojang, root));
            bool downloading = false;
            connect(&installer, &BaselineInstaller::progress, this,
                    [&](const QString &stage, qint64 done) { downloading |= stage == stageClient && done > 0; });
            installer.start("1.21.11");
            QTRY_VERIFY_WITH_TIMEOUT(downloading, 30000);
        }
        QVERIFY(noStaging(root));
        QLockFile lock(root + "/cache.lock");
        QVERIFY(lock.tryLock(0));
    }

    void overlayMergesProjectOverBaseline() {
        QTemporaryDir temp;
        const auto project = temp.path() + "/project";
        const QString p = "assets/minecraft/textures/";
        QVERIFY(writeFile(project + "/pack.mcmeta", "{}"));
        QVERIFY(writeFile(project + '/' + p + "block/stone.png", png(100)));  // override
        QVERIFY(writeFile(project + '/' + p + "block/custom.png", png(101))); // new asset
        QVERIFY(writeFile(project + '/' + p + "misc", "a file where vanilla has a folder"));
        BaselineEntry entry;
        entry.root = temp.path() + "/cache";
        entry.version = "1.21.11";
        entry.identity = QString(64, 'e');
        entry.created = QDateTime::currentDateTimeUtc();
        auto add = [&](const QString &rel, int seed) {
            const auto bytes = png(seed);
            entry.files.insert(rel, {sha256(bytes), bytes.size(), "jar"});
        };
        add(p + "block/stone.png", 1);
        add(p + "block/water_still.png", 2);
        add(p + "block/water_still.png.mcmeta", 3);
        add(p + "item/apple.png", 4);
        add(p + "misc/hidden.png", 5);
        const auto baseline = baselineIndex(entry);
        QCOMPARE(baseline.files, QStringList(entry.files.keys()));
        QCOMPARE(baseline.directories.value(""), QStringList({"assets"}));
        QCOMPARE(baseline.directories.value("assets/minecraft/textures"),
                 QStringList({p + "block", p + "item", p + "misc"}));
        const auto water = baseline.records.value(p + "block/water_still.png");
        QCOMPARE(water.origin, AssetIdentity::Origin::Baseline);
        QCOMPARE(water.metadataReference, p + "block/water_still.png.mcmeta");
        QCOMPARE(water.projectRoot, QDir::cleanPath(entry.root));
        QCOMPARE(water.filesystemPath, entry.objectPath(p + "block/water_still.png"));
        QCOMPARE(water.nameSpace, QString("minecraft"));
        QCOMPARE(water.category, QString("Blocks"));
        QCOMPARE(baseline.records.value(p + "block/water_still.png.mcmeta").category,
                 QString("Animations"));
        QCOMPARE(baseline.records.value(p + "item/apple.png").category, QString("Items"));

        const auto projectIndex = Index::scan(project);
        QVERIFY(projectIndex.error.isEmpty());
        const auto merged = mergeBaseline(projectIndex, baseline);
        const auto stone = merged.records.value(p + "block/stone.png");
        QCOMPARE(stone.origin, AssetIdentity::Origin::Project);
        QVERIFY(stone.shadowsBaseline);
        QCOMPARE(stone.filesystemPath, projectIndex.records.value(p + "block/stone.png").filesystemPath);
        const auto custom = merged.records.value(p + "block/custom.png");
        QCOMPARE(custom.origin, AssetIdentity::Origin::Project);
        QVERIFY(!custom.shadowsBaseline);
        QCOMPARE(merged.records.value(p + "item/apple.png").origin, AssetIdentity::Origin::Baseline);
        QVERIFY(!merged.records.contains(p + "misc/hidden.png")); // a project file owns "misc"
        QVERIFY(!merged.directories.contains(p + "misc"));
        QStringList sortedFiles = merged.files;
        sortedFiles.sort();
        QCOMPARE(merged.files, sortedFiles);
        QCOMPARE(merged.files.size(), merged.records.size());
        QCOMPARE(merged.directories.value(""), QStringList({"assets", "pack.mcmeta"}));
        QCOMPARE(merged.directories.value(p + "block"),
                 QStringList({p + "block/custom.png", p + "block/stone.png",
                              p + "block/water_still.png", p + "block/water_still.png.mcmeta"}));
        QCOMPARE(merged.directories.value(p + "item"), QStringList({p + "item/apple.png"}));
        QCOMPARE(merged.directories.value("assets/minecraft/textures"),
                 QStringList({p + "block", p + "item", p + "misc"}));
        for (auto it = merged.directories.cbegin(); it != merged.directories.cend(); ++it) {
            auto children = it.value();
            children.removeDuplicates();
            QCOMPARE(children.size(), it.value().size());
            for (const auto &child : it.value())
                QVERIFY2(merged.directories.contains(child) || merged.records.contains(child),
                         qPrintable(child));
        }
        // Without a baseline the project index is returned unchanged.
        const auto plain = mergeBaseline(projectIndex, Index{});
        QCOMPARE(plain.files, projectIndex.files);
        QCOMPARE(plain.directories, projectIndex.directories);
    }

    void overlayIsFastForAFullVersion() {
        // ~11k records, the size of a real 26.x texture set (synthetic names and hashes).
        BaselineEntry entry;
        entry.root = QDir::tempPath() + "/vb-timing";
        entry.version = "26.3";
        entry.identity = QString(64, 'f');
        entry.created = QDateTime::currentDateTimeUtc();
        const QStringList folders{"block", "item", "entity/cow", "entity/villager/profession",
                                  "gui/sprites/hud", "particle", "environment", "painting",
                                  "mob_effect", "font", "misc", "trims/items"};
        for (int i = 0; i < 11000; ++i) {
            const auto rel = QString("assets/minecraft/textures/%1/t%2.png")
                                 .arg(folders[i % folders.size()])
                                 .arg(i, 5, 10, QChar('0'));
            entry.files.insert(rel, {sha256(QByteArray::number(i)), 100 + i, "jar"});
            if (i % 50 == 0)
                entry.files.insert(rel + ".mcmeta", {sha256(QByteArray::number(-i)), 30, "jar"});
        }
        Index project;
        project.directories.insert("", {});
        for (int i = 0; i < 300; ++i) {
            const auto rel = QString("assets/minecraft/textures/block/t%1.png").arg(i * 12, 5, 10, QChar('0'));
            AssetIdentity a;
            a.relativePath = rel;
            project.records.insert(rel, a);
            project.files.append(rel);
        }
        project.files.sort();
        QElapsedTimer clock;
        clock.start();
        const auto baseline = baselineIndex(entry);
        const auto built = clock.nsecsElapsed();
        clock.restart();
        const auto merged = mergeBaseline(project, baseline);
        const auto mergedTime = clock.nsecsElapsed();
        qInfo("baselineIndex: %lld records in %.1f ms; mergeBaseline with %lld project files: %.1f ms",
              qint64(baseline.records.size()), built / 1e6, qint64(project.files.size()),
              mergedTime / 1e6);
        QCOMPARE(baseline.records.size(), entry.files.size());
        QCOMPARE(merged.records.size(), entry.files.size());
        int shadowed = 0;
        for (const auto &r : merged.records)
            shadowed += r.shadowsBaseline;
        QCOMPARE(shadowed, 300);
        QVERIFY(std::is_sorted(merged.files.cbegin(), merged.files.cend()));
    }

    void untrustedServerCertificateIsRejected() {
        if (!QSslSocket::supportsSsl())
            QSKIP("No TLS backend is available to this test process");
        auto der = QByteArray::fromBase64(syntheticTestPkcs12);
        QBuffer buffer(&der);
        buffer.open(QIODevice::ReadOnly);
        QSslKey key;
        QSslCertificate certificate;
        QList<QSslCertificate> chain;
        if (!QSslCertificate::importPkcs12(&buffer, &key, &certificate, &chain, "nova-test"))
            QSKIP(qPrintable("The TLS backend (" + QSslSocket::activeBackend() +
                             ") cannot import the synthetic PKCS#12 test certificate"));
        auto serve = [&](QSslServer &server, quint16 port) {
            auto config = QSslConfiguration::defaultConfiguration();
            config.setLocalCertificate(certificate);
            config.setPrivateKey(key);
            config.setPeerVerifyMode(QSslSocket::VerifyNone);
            server.setSslConfiguration(config);
            connect(&server, &QSslServer::pendingConnectionAvailable, &server, [&server] {
                while (auto socket = server.nextPendingConnection())
                    connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                        socket->readAll();
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
                        socket->disconnectFromHost();
                    });
            });
            return server.listen(QHostAddress::LocalHost, port);
        };
        QSslServer server;
        QStringList serverErrors;
        connect(&server, &QSslServer::errorOccurred, this,
                [&](QSslSocket *, QAbstractSocket::SocketError e) { serverErrors << QString::number(e); });
        QVERIFY(serve(server, 0));
        // Qt's default configuration, exactly as Fetch uses it: the handshake must fail.
        QNetworkRequest request(QUrl(QString("https://127.0.0.1:%1/x").arg(server.serverPort())));
        request.setTransferTimeout(15000);
        QList<QSslError> sslErrors;
        auto reply = network.get(request);
        connect(reply, &QNetworkReply::sslErrors, this,
                [&](const QList<QSslError> &errors) { sslErrors = errors; });
        QSignalSpy done(reply, &QNetworkReply::finished);
        QVERIFY(done.wait(20000));
        const auto failure = reply->error();
        reply->deleteLater();
        if (failure != QNetworkReply::SslHandshakeFailedError && sslErrors.isEmpty())
            QSKIP(qPrintable(QString("The %1 backend could not serve the synthetic certificate "
                                     "(client error %2, server errors %3)")
                                 .arg(QSslSocket::activeBackend())
                                 .arg(int(failure))
                                 .arg(serverErrors.join(','))));
        QCOMPARE(failure, QNetworkReply::SslHandshakeFailedError);
        QVERIFY(!sslErrors.isEmpty());
        const bool trustError = std::any_of(sslErrors.cbegin(), sslErrors.cend(), [](const QSslError &e) {
            return e.error() == QSslError::SelfSignedCertificate ||
                   e.error() == QSslError::CertificateUntrusted ||
                   e.error() == QSslError::UnableToGetLocalIssuerCertificate ||
                   e.error() == QSslError::UnableToVerifyFirstCertificate;
        });
        QVERIFY2(trustError, qPrintable(sslErrors.first().errorString()));
        // Through Fetch itself when the default https port is free on loopback (policy URLs
        // must use port 443).
        QSslServer standard;
        if (!serve(standard, 443)) {
            qInfo("Port 443 on 127.0.0.1 is unavailable; the Fetch-level check was skipped");
            return;
        }
        testing::DownloadPolicy policy{{"127.0.0.1"}, false, 1024, "NovaStudio-Test"};
        testing::Fetch fetch(&network, policy);
        QSignalSpy fetched(&fetch, &testing::Fetch::finished);
        fetch.start(QUrl("https://127.0.0.1/x"));
        QVERIFY(fetched.wait(20000));
        const auto error = fetched.first().first().toString();
        QVERIFY2(error.startsWith("Network error"), qPrintable(error));
        QVERIFY(fetch.body().isEmpty());
    }
};

QTEST_GUILESS_MAIN(BaselineCacheTest)
#include "baseline_cache_test.moc"
