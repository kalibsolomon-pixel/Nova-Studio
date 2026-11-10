// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include "project.h"
#include "recovery.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
class IntegrityTests : public QObject {
    Q_OBJECT
    QImage image(QRgb color = 0) {
        QImage im(32, 32, QImage::Format_RGBA8888);
        im.fill(color);
        return im;
    }
  private slots:
    void externalChanges() {
        QTemporaryDir tmp;
        auto path = tmp.path() + "/x.png";
        QVERIFY(nova::savePng(path, image()));
        auto doc = nova::Document::open(path);
        QVERIFY(doc);
        QVERIFY(!doc->externalChanged());
        QVERIFY(nova::savePng(path, image(qRgba(1, 2, 3, 4))));
        QVERIFY(doc->externalChanged());
        QVERIFY(!doc->modified());
        QVERIFY(!doc->save());
        QVERIFY(doc->reload());
        QVERIFY(!doc->externalChanged());
        doc->draw({0, 0}, {1, 1}, qRgba(4, 5, 6, 7));
        doc->finish();
        for (int i = 0; i < 20; ++i) {
            auto external = image(qRgba(i, 3, 4, 255));
            QVERIFY(nova::savePng(path, external));
            QVERIFY(!doc->save());
            QCOMPARE(nova::loadPng(path), external);
        }
        auto observed = nova::Revision::read(path);
        QVERIFY(nova::savePng(path, image(1)));
        QVERIFY(!doc->saveOver(observed));
        QVERIFY(QFile::remove(path));
        QVERIFY(doc->externalChanged());
        QVERIFY(!doc->save());
        QVERIFY(doc->modified());
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(nova::savePng(path, image(2)));
        QVERIFY(!doc->save());
        QVERIFY(doc->saveOver(nova::Revision::read(path)));
        QVERIFY(!doc->modified());
    }
    void atomicRaceAndFailure() {
        QTemporaryDir tmp;
        auto path = tmp.path() + "/data";
        QString error;
        QVERIFY(nova::atomicWrite(path, "original", {}, &error));
        auto expected = nova::Revision::read(path);
        QVERIFY(!nova::atomicWrite(path, "local", expected, &error, [&] {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("external");
        }));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("external"));
        file.close();
        QVERIFY(!nova::atomicWrite(tmp.path() + "/missing/x", "data", {}, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!nova::atomicWrite(tmp.path(), "data", nova::Revision::read(tmp.path()), &error));
    }
    void saveAsConflict() {
        QTemporaryDir tmp;
        auto path = tmp.path() + "/x.png", target = tmp.path() + "/target.png";
        QVERIFY(nova::savePng(path, image()));
        QVERIFY(nova::savePng(target, image(1)));
        auto doc = nova::Document::open(path);
        QVERIFY(!doc->saveAs(target));
        auto revision = nova::Revision::read(target);
        auto external = image(2);
        QVERIFY(nova::savePng(target, external));
        QVERIFY(!doc->saveAsRevision(target, revision));
        QCOMPARE(nova::loadPng(target), external);
    }
    void failedAtomicReplacement() {
#ifdef Q_OS_WIN
        QTemporaryDir tmp;
        auto path = tmp.path() + "/locked";
        QVERIFY(nova::atomicWrite(path, "original", {}));
        auto expected = nova::Revision::read(path);
        HANDLE locked = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked != INVALID_HANDLE_VALUE);
        QString error;
        bool saved = nova::atomicWrite(path, "replacement", expected, &error);
        CloseHandle(locked);
        QVERIFY(!saved);
        QVERIFY(!error.isEmpty());
        QVERIFY(expected.sameContent(nova::Revision::read(path)));
#else
        QSKIP("Windows replacement-sharing failure injection");
#endif
    }
    void metadataConflict() {
        QTemporaryDir tmp;
        QString error;
        auto root = tmp.path() + "/pack";
        QVERIFY(nova::Project::create(root, "p", nova::gameVersions()[0], "x", "minecraft", {},
                                      false, &error));
        auto p = nova::Project::inspect(root);
        auto bytes = p.metadata;
        bytes.replace("\"x\"", "\"external\"");
        QVERIFY(nova::atomicWrite(root + "/pack.mcmeta", bytes, p.revision));
        QVERIFY(!p.saveMetadata(p.metadata, &error));
        QCOMPARE(nova::Project::inspect(root).metadata, bytes);
    }
    void boundedHistory() {
        nova::Document doc({}, image());
        doc.setHistoryLimits(4096, 100);
        for (int i = 0; i < 6000; ++i) {
            doc.draw({0, 0}, {10, 0}, qRgba(i % 255, 4, 5, 255));
            doc.finish();
            QVERIFY(doc.historyBytes() <= 4096);
            QVERIFY(doc.historyOperations() <= 100);
        }
        auto pixels = doc.image;
        int undos = 0;
        while (doc.canUndo()) {
            doc.undo();
            ++undos;
        }
        QVERIFY(undos > 0);
        QVERIFY(doc.modified());
        while (doc.canRedo())
            doc.redo();
        QCOMPARE(doc.image, pixels);
        doc.undo();
        doc.draw({2, 2}, {2, 2}, 1);
        doc.finish();
        QVERIFY(!doc.canRedo());
        nova::Document second({}, image());
        second.setHistoryLimits(1024, 2);
        for (int i = 1; i < 10; ++i) {
            second.draw({0, 0}, {0, 0}, QRgb(i));
            second.finish();
        }
        QCOMPARE(second.historyOperations(), 2);
        QTemporaryDir tmp;
        QVERIFY(second.saveAs(tmp.path() + "/saved.png"));
        second.setHistoryLimits(128, 0);
        QVERIFY(!second.modified());
        second.draw({1, 1}, {1, 1}, 5);
        second.finish();
        QVERIFY(second.modified());
        QVERIFY(!second.canUndo());
        nova::Document noOp({}, image());
        noOp.draw({1, 1}, {1, 1}, 1);
        noOp.draw({1, 1}, {1, 1}, 0);
        noOp.finish();
        QCOMPARE(noOp.historyBytes(), 0);
        QVERIFY(!noOp.modified());
        QImage large(1024, 1024, QImage::Format_RGBA8888);
        large.fill(0);
        nova::Document big({}, large);
        big.setHistoryLimits(8192, 2048);
        for (int y = 0; y < 1024; ++y)
            big.draw({0, y}, {1023, y}, 1);
        big.finish();
        QVERIFY(big.historyBytes() <= 8192);
    }
    void recovery() {
        QTemporaryDir tmp;
        nova::RecoveryStore store(tmp.path() + "/recovery");
        auto id = QUuid::createUuid();
        auto path = tmp.path() + "/missing.png";
        QVERIFY(store.write(id, path, tmp.path(), {}, image(42)));
        auto entries = store.entries();
        QCOMPARE(entries.size(), 1);
        QVERIFY(entries[0].image.isNull());
        entries[0] = store.load(entries[0].id);
        QVERIFY(entries[0].error.isEmpty());
        QCOMPARE(entries[0].image, image(42));
        QVERIFY(!QFileInfo::exists(path));
        nova::Document doc(path, entries[0].image);
        doc.restorePixels(entries[0].image, entries[0].original);
        QVERIFY(doc.modified());
        QVERIFY(doc.save());
        QCOMPARE(nova::loadPng(path), image(42));
        auto external = image(99);
        QVERIFY(nova::savePng(path, external));
        doc.restorePixels(entries[0].image, entries[0].original);
        QVERIFY(!doc.save());
        QCOMPARE(nova::loadPng(path), external);
        QFile corrupt(store.pathFor(id));
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("bad data");
        corrupt.close();
        QVERIFY(!store.entries()[0].error.isEmpty());
        store.remove(id);
        QVERIFY(store.entries().isEmpty());
        auto blocked = tmp.path() + "/blocked";
        QFile blocker(blocked);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QString error;
        QVERIFY(!nova::RecoveryStore(blocked + "/r").write(id, path, {}, {}, image(), &error));
        for (int i = 0; i < 35; ++i)
            QVERIFY(store.write(QUuid::createUuid(), path, {}, {}, image()));
        QCOMPARE(store.entries().size(), 32);
    }
    void pendingRecoverySurvivesCleanupAndRestart() {
        QTemporaryDir tmp;
        const auto a = QUuid::createUuid(), b = QUuid::createUuid();
        const auto path = tmp.filePath("texture.png");
        {
            nova::RecoveryStore writer(tmp.path());
            QVERIFY(writer.write(a, path, {}, {}, image(42)));
            QVERIFY(writer.write(b, path, {}, {}, image(84)));
        }
        for (int restart = 0; restart < 3; ++restart) {
            nova::RecoveryStore store(tmp.path());
            auto worker = store;
            QCOMPARE(store.entries().size(), 2);
            // No acknowledgement on Later, failed restore, or shutdown.
            worker.cleanupClean(a);
            store.remove(b);
            worker.prune();
            QVERIFY(!worker.write(a, path, {}, {}, image(99)));
            QCOMPARE(store.load(a).image, image(42));
            QCOMPARE(store.load(b).image, image(84));
        }
        nova::RecoveryStore store(tmp.path());
        store.discard(b);
        store.cleanupClean(b);
        QVERIFY(!QFileInfo::exists(store.pathFor(b)));
        store.restored(a);
        store.cleanupClean(a);
        QVERIFY(!store.write(a, path, {}, {}, QImage{}));
        QCOMPARE(store.load(a).image, image(42));
        QVERIFY(store.write(a, path, {}, {}, image(99)));
        QCOMPARE(store.load(a).image, image(99));
        store.cleanupClean(a);
        QVERIFY(!QFileInfo::exists(store.pathFor(a)));
    }
    void forcedTermination() {
        QTemporaryDir tmp;
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--recovery-child", tmp.path()});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForReadyRead(10000));
        QCOMPARE(child.readAllStandardOutput().trimmed(), QByteArray("ready"));
        child.kill();
        QVERIFY(child.waitForFinished());
        auto recovered = nova::RecoveryStore(tmp.path()).entries();
        QCOMPARE(recovered.size(), 2);
        for (auto &e : recovered) {
            QVERIFY(e.error.isEmpty());
            auto pixels = nova::RecoveryStore(tmp.path()).load(e.id);
            QVERIFY(!pixels.image.isNull());
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--recovery-child")) {
        nova::RecoveryStore store(app.arguments().last());
        QImage im(16, 16, QImage::Format_RGBA8888);
        im.fill(123);
        for (int i = 0; i < 2; ++i)
            if (!store.write(QUuid::createUuid(), "missing" + QString::number(i) + ".png", {}, {},
                             im))
                return 2;
        fprintf(stdout, "ready\n");
        fflush(stdout);
        return app.exec();
    }
    IntegrityTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "integrity_test.moc"
