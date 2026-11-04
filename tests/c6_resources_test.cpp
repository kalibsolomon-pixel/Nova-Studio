// SPDX-License-Identifier: GPL-3.0-or-later
#include "models.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace nova;
class ResourceTest : public QObject {
    Q_OBJECT
    static void put(const QString &root, const QString &path, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(root + '/' + path).absolutePath());
        QFile f(root + '/' + path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    }
  private slots:
    void priorityAndInvalidation() {
        QTemporaryDir editable, high, low;
        const QString path = "assets/minecraft/models/block/base.json";
        put(low.path(), path, R"({"textures":{"all":"minecraft:block/low"},"elements":[]})");
        put(high.path(), path, R"({"textures":{"all":"minecraft:block/high"},"elements":[]})");
        put(editable.path(), "assets/nova/models/block/child.json",
            R"({"parent":"minecraft:block/base","credit":"preserve"})");
        QString error;
        auto a = mc::ResourceSource::open(high.path(), "26.1", &error),
             b = mc::ResourceSource::open(low.path(), "26.1", &error);
        QVERIFY(error.isEmpty());
        mc::Library lib;
        lib.update(editable.path(), Index::scan(editable.path()));
        QVERIFY(!lib.model("nova:block/child").valid);
        lib.setFallbacks({a, b});
        QVERIFY(lib.model("nova:block/child").valid);
        QCOMPARE(lib.model("nova:block/child").textures["all"],
                 QString("assets/minecraft/textures/block/high.png"));
        QCOMPARE(lib.providers(path).size(), 2);
        QVERIFY(!lib.editable(path));
        put(editable.path(), path, R"({"elements":[],"credit":"local"})");
        lib.update(editable.path(), Index::scan(editable.path()), {path});
        QVERIFY(lib.editable(path));
        QCOMPARE(lib.providers(path).size(), 3);
        QCOMPARE(lib.assetPath(path, 1), QFileInfo(high.path() + '/' + path).canonicalFilePath());
        QCOMPARE(lib.assetPath(path, 2), QFileInfo(low.path() + '/' + path).canonicalFilePath());
        QVERIFY(lib.assetPath(path, -1).isEmpty());
        QVERIFY(lib.assetPath(path, 3).isEmpty());
        QVERIFY(lib.dependents(path).contains("assets/nova/models/block/child.json"));
        lib.setVersion("26.3");
        QVERIFY(lib.editable(path));
        QCOMPARE(lib.providers(path).size(), 1);
        put(editable.path(), path, R"({"parent":"nova:block/child"})");
        lib.update(editable.path(), Index::scan(editable.path()), {path});
        QVERIFY(!lib.model("nova:block/child").valid);
    }
    void verifiedVersionGate() {
        QTemporaryDir folder;
        QString error;
        put(folder.path(), "nova-vanilla.json",
            R"({"version":"1.21.11","client_sha1":"ba2df812c2d12e0219c489c4cd9a5e1f0760f5bd","files":{"assets/minecraft/models/block/a.json":"bad"}})");
        QVERIFY(mc::ResourceSource::open(folder.path(), "26.1", &error).root.isEmpty());
        QVERIFY(error.contains("target"));
        error.clear();
        QVERIFY(mc::ResourceSource::open(folder.path(), "1.21.11", &error).root.isEmpty());
        QVERIFY(error.contains("manifest"));
    }
    void shadowedHashGuard() {
        QTemporaryDir editable, fallback;
        const QString path = "assets/nova/models/base.json";
        put(editable.path(), path, "{\"credit\":\"editable\"}");
        const QByteArray original = "{\"credit\":\"verified fallback\"}";
        put(fallback.path(), path, original);
        QString error;
        auto source = mc::ResourceSource::open(fallback.path(), "26.1", &error);
        source.hashes[path] = QString::fromLatin1(
            QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex());
        mc::ResourceStack stack;
        stack.build(editable.path(), Index::scan(editable.path()), {source}, "26.1");
        QVERIFY(!stack.path(path, 1).isEmpty());
        put(fallback.path(), path, "{\"credit\":\"tampered\"}");
        QVERIFY(stack.path(path, 1).isEmpty());
        QVERIFY(!stack.path(path).isEmpty());
    }
    void localVanilla() {
        const auto path = qEnvironmentVariable("NOVA_VANILLA_CACHE");
        if (path.isEmpty())
            QSKIP("No authorized local vanilla cache configured");
        QString error;
        auto vanilla = mc::ResourceSource::open(path, "26.1", &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(vanilla.vanilla);
        QTemporaryDir pack;
        put(pack.path(), "assets/nova/models/block/vanilla.json",
            R"({"parent":"minecraft:block/cube_all","textures":{"all":"minecraft:block/stone"}})");
        mc::Library lib;
        lib.update(pack.path(), Index::scan(pack.path()));
        lib.setFallbacks({vanilla});
        auto model = lib.model("nova:block/vanilla");
        QVERIFY(model.valid);
        QCOMPARE(mc::mesh(model).faces.size(), 6);
        QVERIFY(!lib.assetPath("assets/minecraft/textures/block/stone.png").isEmpty());
        QVERIFY(!lib.editable("assets/minecraft/models/block/cube_all.json"));
    }
};
QTEST_GUILESS_MAIN(ResourceTest)
#include "c6_resources_test.moc"
