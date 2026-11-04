// SPDX-License-Identifier: GPL-3.0-or-later
#include "index.h"
#include "project.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
class ProjectTests : public QObject {
    Q_OBJECT
  private slots:
    void lifecycle() {
        QTemporaryDir tmp;
        QString error;
        auto root = tmp.path() + "/pack";
        QVERIFY(nova::Project::create(root, "Pack", nova::gameVersions()[0], "Description",
                                      "minecraft", {}, true, &error));
        auto p = nova::Project::inspect(root);
        QCOMPARE(p.kind, nova::Project::Kind::ResourcePack);
        QCOMPARE(p.namespaces, QStringList{"minecraft"});
        auto original = p.metadata;
        auto revision = p.revision;
        auto reopened = nova::Project::inspect(root);
        QCOMPARE(reopened.metadata, original);
        QVERIFY(revision.sameContent(reopened.revision));
        QVERIFY(!nova::Project::create(root, "Pack", nova::gameVersions()[0], "x", "minecraft", {},
                                       true, &error));
        QCOMPARE(nova::Project::inspect(root).metadata, original);
        QCOMPARE(nova::Project::inspect(tmp.path()).kind, nova::Project::Kind::Folder);
        QCOMPARE(nova::Project::inspect(tmp.path() + "/missing").kind,
                 nova::Project::Kind::Invalid);
    }
    void metadata() {
        QTemporaryDir tmp;
        QString error;
        QVERIFY(nova::Project::create(tmp.path() + "/pack", "Pack", nova::gameVersions()[1], "test",
                                      "example", {}, false, &error));
        auto p = nova::Project::inspect(tmp.path() + "/pack");
        QByteArray bytes =
            R"({ "unknown": {"test": 1}, "pack": {"description": {"text":"hi"},"min_format":[75,0],"max_format":[75,0], "future":true} })";
        QVERIFY(p.saveMetadata(bytes, &error));
        QCOMPARE(nova::Project::inspect(p.root).metadata, bytes);
        QVERIFY(!p.saveMetadata("broken", &error));
        QCOMPARE(nova::Project::inspect(p.root).metadata, bytes);
        QVERIFY(!nova::Project::validateMetadata(R"({"pack":{"description":"x","pack_format":75}})")
                     .isEmpty());
        QVERIFY(!nova::PackFormat::parse(97.1));
        auto format = nova::PackFormat::parse(QJsonArray{97, 1});
        QVERIFY(format);
        QCOMPARE(format->minor, 1);
    }
    void pathsAndEmptyFolders() {
        QTemporaryDir tmp;
        QDir().mkpath(tmp.path() + "/assets/example/textures/block/empty");
        QFile file(tmp.path() + "/assets/example/textures/block/Stone.png");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("png");
        file.close();
        auto index = nova::Index::scan(tmp.path());
        QVERIFY(index.directories.contains("assets/example/textures/block/empty"));
        auto record = index.records.first();
        QCOMPARE(record.nameSpace, QString("example"));
        QCOMPARE(record.category, QString("Blocks"));
        QVERIFY(record.relativePath.endsWith("Stone.png"));
        QCOMPARE(nova::pathIdentity(tmp.path() + "/a/../X"), nova::pathIdentity(tmp.path() + "/x"));
    }
    void oversizedMetadata() {
        QTemporaryDir tmp;
        QFile metadata(tmp.path() + "/pack.mcmeta");
        QVERIFY(metadata.open(QIODevice::WriteOnly));
        metadata.write(QByteArray(1024 * 1024 + 1, ' '));
        metadata.close();
        auto project = nova::Project::inspect(tmp.path());
        QCOMPARE(project.kind, nova::Project::Kind::NeedsRepair);
        QVERIFY(!project.revision.readable);
        QVERIFY(!nova::Revision::read(metadata.fileName(), 4).readable);
    }
};
QTEST_GUILESS_MAIN(ProjectTests)
#include "project_test.moc"
