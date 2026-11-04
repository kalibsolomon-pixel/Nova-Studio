// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include "index.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
class CoreTests : public QObject {
    Q_OBJECT
  private slots:
    void coordinates() {
        using nova::coordinate;
        QCOMPARE(coordinate({9.9, 10}, {10, 10}, 8, {16, 16}), std::optional<QPoint>{});
        QCOMPARE(coordinate({137.9, 137.9}, {10, 10}, 8, {16, 16}),
                 std::optional<QPoint>(QPoint(15, 15)));
        QVERIFY(!coordinate({138, 10}, {10, 10}, 8, {16, 16}));
        QVERIFY(!coordinate({0, 0}, {0, 0}, 0, {16, 16}));
        QCOMPARE(coordinate({11, 11}, {10, 10}, .5, {16, 16}), std::optional<QPoint>(QPoint(2, 2)));
    }
    void zoom() {
        QVERIFY(std::abs(nova::zoomBy(nova::zoomBy(8, 100), -100) - 8) < .001);
        QCOMPARE(nova::zoomBy(64, 1000), 64.0);
    }
    void brushEraseUndo() {
        QTemporaryDir tmp;
        QImage im(16, 16, QImage::Format_RGBA8888);
        im.fill(Qt::transparent);
        nova::Document d(tmp.path() + "/t.png", im);
        auto color = qRgba(20, 30, 40, 255);
        d.draw({0, 0}, {15, 15}, color);
        d.finish();
        QCOMPARE(d.image.pixel(8, 8), color);
        QVERIFY(d.modified());
        QVERIFY(d.save());
        QVERIFY(!d.modified());
        d.draw({8, 8}, {8, 8}, 0);
        d.finish();
        QCOMPARE(d.image.pixel(8, 8), QRgb(0));
        d.undo();
        QCOMPARE(d.image.pixel(8, 8), color);
        QVERIFY(!d.modified());
        d.redo();
        QCOMPARE(d.image.pixel(8, 8), QRgb(0));
        d.undo();
        d.undo();
        d.draw({0, 0}, {1, 0}, qRgba(1, 1, 1, 1));
        d.finish();
        QVERIFY(d.modified());
        d.redo();
        QCOMPARE(d.image.pixel(8, 8), QRgb(0));
    }
    void repeatedPixel() {
        QImage im(16, 16, QImage::Format_RGBA8888);
        im.fill(Qt::transparent);
        nova::Document d("", im);
        d.draw({1, 1}, {1, 1}, qRgba(1, 2, 3, 4));
        d.draw({1, 1}, {1, 1}, qRgba(5, 6, 7, 8));
        d.finish();
        d.undo();
        QCOMPARE(d.image.pixel(1, 1), QRgb(0));
        d.redo();
        QCOMPARE(d.image.pixel(1, 1), qRgba(5, 6, 7, 8));
    }
    void pngRoundtrip() {
        QTemporaryDir tmp;
        for (int size : {16, 32, 64, 128, 256}) {
            QImage im(size, size, QImage::Format_RGBA8888);
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    im.setPixel(x, y, qRgba(x % 256, y % 256, 91, x % 2 == 0 ? 0 : 123));
            QString p = tmp.path() + QString("/%1.png").arg(size);
            QVERIFY(nova::savePng(p, im));
            QCOMPARE(nova::loadPng(p), im);
            QVERIFY(nova::savePng(p, im));
        }
        QVERIFY(nova::loadPng(tmp.path() + "/missing.png").isNull());
        QVERIFY(!nova::savePng(tmp.path() + "/missing/file.png",
                               QImage(16, 16, QImage::Format_RGBA8888)));
        QFile bad(tmp.path() + "/bad.png");
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not png");
        bad.close();
        QVERIFY(nova::loadPng(bad.fileName()).isNull());
    }
    void indexAndNavigation() {
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath("assets/a");
        for (auto p : {"assets/a/stone.png", "assets/a/Glass.PNG", "assets/a/no.txt"}) {
            QFile f(tmp.path() + '/' + p);
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        auto index = nova::Index::scan(tmp.path());
        QCOMPARE(index.files.size(), 3); // Phase 2-B indexes non-PNG assets too.
        QCOMPARE(index.directories.value(""), QStringList{"assets"});
        nova::Navigation n;
        n.directory = "assets/a";
        n.selected = "assets/a/stone.png";
        n.query = "STONE";
        QCOMPARE(n.entries(index), QStringList{"assets/a/stone.png"});
        auto original = n;
        n.grid = true;
        QCOMPARE(n.directory, original.directory);
        QCOMPARE(n.selected, original.selected);
        QCOMPARE(n.query, original.query);
        QVERIFY(!nova::Index::scan(tmp.path() + "/missing").error.isEmpty());
    }
};
QTEST_GUILESS_MAIN(CoreTests)
#include "core_test.moc"
