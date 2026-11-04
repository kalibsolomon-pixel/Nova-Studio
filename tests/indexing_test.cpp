// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor.h"
#include "window.h"
#include <QAbstractItemModelTester>
#include <QComboBox>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
class IndexingTests : public QObject {
    Q_OBJECT
    void file(const QString &path, QRgb color = 0) {
        QImage im(16, 16, QImage::Format_RGBA8888);
        im.fill(QColor::fromRgba(color));
        // The model's thumbnail pool may still be reading the previous file. The production
        // writer must replace it without help from the fixture (Phase 4-F).
        QString error;
        const bool saved = nova::savePng(path, im, &error);
        QVERIFY2(saved, qPrintable(error));
    }
  private slots:
    void refreshedSubtreesMatchFullScan() {
        QTemporaryDir tmp;
        const auto folder = QString("assets/nova/textures/block");
        QVERIFY(QDir().mkpath(tmp.path() + '/' + folder));
        for (int i = 0; i < 80; ++i) file(tmp.path() + '/' + folder + QString("/t%1.png").arg(i));
        auto writeMeta = [&](const QString &name) {
            QFile f(tmp.path() + '/' + folder + '/' + name + ".mcmeta");
            QVERIFY(f.open(QIODevice::WriteOnly)); f.write("{}");
        };
        writeMeta("t0.png");
        auto index = nova::Index::scan(tmp.path());
        auto equalScan = [&] {
            const auto full = nova::Index::scan(tmp.path());
            QCOMPARE(index.error, QString());
            QCOMPARE(index.files, full.files);
            QCOMPARE(index.directories, full.directories);
            QCOMPARE(index.records.size(), full.records.size());
            for (auto it = full.records.cbegin(); it != full.records.cend(); ++it) {
                const auto actual = index.records.value(it.key());
                QCOMPARE(actual.metadataReference, it->metadataReference);
                QCOMPARE(actual.nameSpace, it->nameSpace);
                QCOMPARE(actual.category, it->category);
                QCOMPARE(actual.filesystemPath, it->filesystemPath);
                QCOMPARE(actual.size, it->size);
            }
        };
        index = nova::Index::refresh(tmp.path(), index, {folder + "/t0.png"}); equalScan();
        writeMeta("t1.png");
        index = nova::Index::refresh(tmp.path(), index, {folder + "/t1.png.mcmeta"}); equalScan();
        // Remove only a test-owned sidecar; every other source must remain intact.
        QVERIFY(QFile::remove(tmp.path() + '/' + folder + "/t0.png.mcmeta"));
        index = nova::Index::refresh(tmp.path(), index, {folder + "/t0.png.mcmeta"}); equalScan();
        index = nova::Index::refresh(tmp.path(), index, {"assets", folder}); equalScan();
        QVERIFY(QDir(tmp.path() + "/assets/nova/textures").rename("block", "renamed"));
        index = nova::Index::refresh(tmp.path(), index, {folder, "assets/nova/textures/renamed"}); equalScan();
    }
    void incrementalModel() {
        QTemporaryDir tmp;
        QDir().mkpath(tmp.path() + "/assets/a/textures/block/empty");
        file(tmp.path() + "/assets/a/textures/block/x.png");
        auto index = nova::Index::scan(tmp.path());
        nova::AssetModel model;
        model.setIndex(index, tmp.path());
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        file(tmp.path() + "/assets/a/textures/block/y.png", 1);
        auto next = nova::Index::refresh(tmp.path(), index, {"assets/a/textures/block/y.png"});
        model.applyIndex(next, {"assets/a/textures/block/y.png"});
        QVERIFY(model.indexFor("assets/a/textures/block/y.png").isValid());
        QCOMPARE(reset.count(), 0);
        QVERIFY(QDir(tmp.path() + "/assets/a/textures").rename("block", "renamed"));
        next = nova::Index::refresh(tmp.path(), next,
                                    {"assets/a/textures/block", "assets/a/textures/renamed"});
        model.applyIndex(next, {"assets/a/textures/block", "assets/a/textures/renamed"});
        QVERIFY(!model.indexFor("assets/a/textures/block/x.png").isValid());
        QVERIFY(model.indexFor("assets/a/textures/renamed/empty").isValid());
        QVERIFY(QFile::remove(tmp.path() + "/assets/a/textures/renamed/x.png"));
        next = nova::Index::refresh(tmp.path(), next, {"assets/a/textures/renamed/x.png"});
        model.applyIndex(next, {"assets/a/textures/renamed/x.png"});
        QVERIFY(!model.indexFor("assets/a/textures/renamed/x.png").isValid());
        QCOMPARE(reset.count(), 0);
        QVERIFY(QDir().rmdir(tmp.path() + "/assets/a/textures/renamed/empty"));
        QFile replacement(tmp.path() + "/assets/a/textures/renamed/empty");
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        replacement.write("asset");
        replacement.close();
        next = nova::Index::refresh(tmp.path(), next, {"assets/a/textures/renamed/empty"});
        model.applyIndex(next, {"assets/a/textures/renamed/empty"});
        QVERIFY(!model.isDirectory(model.indexFor("assets/a/textures/renamed/empty")));
        auto cancel = std::make_shared<std::atomic_bool>(true);
        QCOMPARE(nova::Index::scan(tmp.path(), cancel).error, QString("Cancelled"));
    }
    void nativeMonitor() {
        QTemporaryDir tmp;
        nova::DirectoryMonitor monitor;
        QVERIFY(monitor.watch(tmp.path()));
        QSignalSpy changed(&monitor, &nova::DirectoryMonitor::changed),
            uncertain(&monitor, &nova::DirectoryMonitor::uncertain);
        for (int i = 0; i < 100; ++i)
            file(tmp.path() + QString("/%1.png").arg(i));
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 5000);
        monitor.simulateOverflow();
        QCOMPARE(uncertain.count(), 1);
        monitor.stop();
    }
    void liveUpdatesAndSwitching() {
        QTemporaryDir tmp, other;
        QDir().mkpath(tmp.path() + "/assets/a/textures/block");
        file(tmp.path() + "/assets/a/textures/block/x.png");
        qApp->setProperty("settingsFile", other.path() + "/settings.ini");
        QElapsedTimer time;
        time.start();
        nova::Window window(tmp.path(), {}, time);
        window.show();
        auto model = window.findChild<nova::AssetModel *>();
        auto tree = window.findChild<QTreeView *>("assetTree");
        QTRY_VERIFY(model->indexFor("assets/a/textures/block/x.png").isValid());
        for (int i = 0; i < 20; ++i)
            file(tmp.path() + QString("/assets/a/textures/block/new%1.png").arg(i));
        QTRY_VERIFY_WITH_TIMEOUT(model->indexFor("assets/a/textures/block/new19.png").isValid(),
                                 10000);
        QVERIFY(QFile::remove(tmp.path() + "/assets/a/textures/block/new19.png"));
        QTRY_VERIFY(!model->indexFor("assets/a/textures/block/new19.png").isValid());
        auto category = window.findChild<QComboBox *>("assetCategory");
        category->setCurrentText("Blocks");
        QVERIFY(tree->model()->rowCount() > 0);
        category->setCurrentIndex(0);
        auto path = window.findChild<QLineEdit *>("projectPath");
        path->setText(other.path());
        emit path->returnPressed();
        path->setText(tmp.path());
        emit path->returnPressed();
        QTRY_VERIFY_WITH_TIMEOUT(model->indexFor("assets/a/textures/block/x.png").isValid(), 10000);
        window.findChild<nova::DirectoryMonitor *>()->simulateOverflow();
        QTest::qWait(800);
        QVERIFY(model->indexFor("assets/a/textures/block/x.png").isValid());
        QVERIFY(window.close());
    }
    void cacheLimits() {
        QTemporaryDir tmp;
        for (int i = 0; i < 400; ++i)
            file(tmp.path() + QString("/%1.png").arg(i), QRgb(i));
        nova::AssetModel model;
        model.setIndex(nova::Index::scan(tmp.path()), tmp.path());
        for (int pass = 0; pass < 5; ++pass) {
            for (int i = 0; i < 400; ++i)
                model.data(model.indexFor(QString("%1.png").arg(i)), Qt::DecorationRole);
            QTest::qWait(100);
            QVERIFY(model.cachedBytes() <= 4 * 1024 * 1024);
            QVERIFY(model.pendingCount() <= 128);
        }
        file(tmp.path() + "/0.png", 123);
        model.applyIndex(nova::Index::scan(tmp.path()), {"0.png"});
        QVERIFY(model.cachedBytes() <= 4 * 1024 * 1024);
        auto index = nova::Index::scan(tmp.path());
        for (int i = 0; i < 12; ++i) {
            file(tmp.path() + "/0.png", qRgba(i, 30, 60, 255));
            index = nova::Index::refresh(tmp.path(), index, {"0.png"});
            model.applyIndex(index, {"0.png"});
            model.data(model.indexFor("0.png"), Qt::DecorationRole);
        }
        auto expected = QColor(11, 30, 60, 255);
        QTRY_COMPARE_WITH_TIMEOUT(model.data(model.indexFor("0.png"), Qt::DecorationRole)
                                      .value<QIcon>()
                                      .pixmap(64, 64)
                                      .toImage()
                                      .pixelColor(0, 0),
                                  expected, 10000);
    }
    void contentUpdatesPreserveCategoryAndPersistentIndexes() {
        QTemporaryDir tmp;
        const auto path = QString("assets/nova/textures/block/t.png");
        QVERIFY(QDir().mkpath(QFileInfo(tmp.path() + '/' + path).absolutePath()));
        file(tmp.path() + '/' + path, qRgb(13, 81, 120));
        auto index = nova::Index::scan(tmp.path());
        nova::AssetModel model;
        model.setIndex(index, tmp.path());
        model.setCategory("Blocks");
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QPersistentModelIndex persistent(model.indexFor(path));
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset), updates(&model, &QAbstractItemModel::dataChanged);
        file(tmp.path() + '/' + path, qRgb(99, 47, 201));
        index = nova::Index::refresh(tmp.path(), index, {path});
        model.applyIndex(index, {path});
        QCOMPARE(resets.count(), 0);
        QVERIFY(persistent.isValid());
        QCOMPARE(model.pathFor(persistent), path);
        QVERIFY(!updates.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(model.data(persistent, Qt::DecorationRole).value<QIcon>()
            .pixmap(64, 64).toImage().pixelColor(0, 0), QColor(99, 47, 201), 10000);
        file(tmp.path() + "/assets/nova/textures/block/new.png", qRgb(1, 2, 3));
        index = nova::Index::refresh(tmp.path(), index, {"assets/nova/textures/block/new.png"});
        model.applyIndex(index, {"assets/nova/textures/block/new.png"});
        QCOMPARE(resets.count(), 1);
        QVERIFY(model.indexFor("assets/nova/textures/block/new.png").isValid());
    }
    void contentUpdatesInvalidateThumbnailsOutsideVisibleCategory() {
        QTemporaryDir tmp;
        const auto path = QString("assets/nova/textures/item/t.png");
        QVERIFY(QDir().mkpath(QFileInfo(tmp.path() + '/' + path).absolutePath()));
        QVERIFY(QDir().mkpath(tmp.path() + "/assets/nova/textures/block"));
        file(tmp.path() + '/' + path, qRgb(10, 20, 30));
        file(tmp.path() + "/assets/nova/textures/block/b.png", qRgb(80, 90, 100));
        auto index = nova::Index::scan(tmp.path());
        nova::AssetModel model; model.setIndex(index, tmp.path());
        auto color = [&] { return model.data(model.indexFor(path), Qt::DecorationRole)
            .value<QIcon>().pixmap(64, 64).toImage().pixelColor(0, 0); };
        QTRY_COMPARE_WITH_TIMEOUT(color(), QColor(10, 20, 30), 10000);
        model.setCategory("Blocks");
        QVERIFY(!model.indexFor(path).isValid());
        file(tmp.path() + '/' + path, qRgb(200, 70, 90));
        index = nova::Index::refresh(tmp.path(), index, {path});
        model.applyIndex(index, {path});
        model.setCategory("Items");
        QTRY_COMPARE_WITH_TIMEOUT(color(), QColor(200, 70, 90), 10000);
    }
};
QTEST_MAIN(IndexingTests)
#include "indexing_test.moc"
