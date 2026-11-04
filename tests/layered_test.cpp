// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include "index.h"
#include "project.h"
#include "recovery.h"
#include "session.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif
class LayeredTests : public QObject {
    Q_OBJECT
    QImage pixels(int size = 16, QRgb color = 0) {
        QImage im(size, size, QImage::Format_RGBA8888);
        im.fill(QColor::fromRgba(color));
        return im;
    }
    void add(nova::Document &doc, const QString &name = "Ink") {
        QVERIFY(doc.editLayer({nova::LayerCommand::Add, {}, name}));
    }
    QByteArray mutate(QByteArray bytes, const std::function<void(QJsonObject &)> &fn) {
        auto n = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + 8));
        auto m = QJsonDocument::fromJson(bytes.mid(12, n)).object();
        fn(m);
        auto json = QJsonDocument(m).toJson(QJsonDocument::Compact);
        QByteArray out("NVLD0001", 8);
        char length[4];
        qToLittleEndian(quint32(json.size()), reinterpret_cast<uchar *>(length));
        out.append(length, 4);
        out.append(json);
        out.append(bytes.mid(12 + n));
        return out;
    }
  private slots:
    void compositeMatchesOriginalIntegerArithmetic() {
        auto reference = [](const auto &layers, QSize size) {
            QImage out(size, QImage::Format_RGBA8888);
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x) {
                    int r = 0, g = 0, b = 0, alpha = 0;
                    for (const auto &layer : layers) {
                        if (!layer.visible || !layer.opacity) continue;
                        auto p = layer.pixels.constScanLine(y) + x * 4;
                        const int a = (int(p[3]) * layer.opacity + 127) / 255;
                        if (!a) continue;
                        const int weight = alpha * (255 - a), denominator = a * 255 + weight;
                        r = (int(p[0]) * a * 255 + r * weight + denominator / 2) / denominator;
                        g = (int(p[1]) * a * 255 + g * weight + denominator / 2) / denominator;
                        b = (int(p[2]) * a * 255 + b * weight + denominator / 2) / denominator;
                        alpha = (denominator + 127) / 255;
                    }
                    auto p = out.scanLine(y) + x * 4;
                    p[0] = uchar(r); p[1] = uchar(g); p[2] = uchar(b); p[3] = uchar(alpha);
                }
            return out;
        };
        QImage image(17, 19, QImage::Format_RGBA8888);
        quint32 random = 0x12345678;
        nova::Document doc({}, pixels(17));
        for (int k = 1; k < 64; ++k) {
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    random = random * 1664525 + 1013904223;
                    image.setPixelColor(x, y, QColor((random >> 24) & 255, (random >> 16) & 255,
                                                     (random >> 8) & 255, (x + y * 17 + k) % 256));
                }
            // Document dimensions stay square; exercise nonzero region offsets below.
            QVERIFY(doc.addLayerWithPixels("Random", image.copy(0, 0, 17, 17)));
            for (int opacity : {0, 1, 63, 127, 128, 254, 255}) {
                QVERIFY(doc.editLayer({nova::LayerCommand::Opacity, doc.activeLayer(), {}, opacity}));
                QCOMPARE(doc.image, reference(doc.layers(), doc.image.size()));
            }
            QVERIFY(doc.editLayer({nova::LayerCommand::Visibility, doc.activeLayer(), {}, k % 2}));
            QCOMPARE(doc.image, reference(doc.layers(), doc.image.size()));
        }
        const auto retained = doc.image;
        const QByteArray retainedBytes(reinterpret_cast<const char *>(retained.constBits()), retained.sizeInBytes());
        QVERIFY(doc.editLayer({nova::LayerCommand::Visibility, doc.activeLayer(), {}, 1}));
        doc.draw({3, 4}, {11, 12}, qRgba(27, 133, 219, 153)); doc.finish();
        QCOMPARE(doc.image, reference(doc.layers(), doc.image.size()));
        doc.undo(); QCOMPARE(doc.image, reference(doc.layers(), doc.image.size()));
        doc.redo(); QCOMPARE(doc.image, reference(doc.layers(), doc.image.size()));
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(retained.constBits()), retained.sizeInBytes()), retainedBytes);
    }
    void dimensions_data() {
        QTest::addColumn<int>("size");
        for (int n : {16, 32, 64, 128, 256})
            QTest::newRow(qPrintable(QString::number(n))) << n;
    }
    void dimensions() {
        QFETCH(int, size);
        nova::Document doc({}, pixels(size));
        add(doc);
        QCOMPARE(doc.image.size(), QSize(size, size));
        QCOMPARE(doc.layers()[0].pixels.size(), doc.layers()[1].pixels.size());
        doc.draw({size - 1, size - 1}, {size - 1, size - 1}, qRgba(200, 80, 30, 128));
        doc.finish();
        QTemporaryDir tmp;
        auto target = tmp.path() + "/x.png";
        QVERIFY(doc.saveAs(target));
        QCOMPARE(nova::loadPng(target), doc.image);
    }
    void limits() {
        nova::Document invalid({}, QImage(4097, 1, QImage::Format_RGBA8888));
        QVERIFY(!invalid.editLayer({nova::LayerCommand::Add, {}, "No"}));
        nova::Document doc({}, pixels());
        for (int i = 1; i < 64; ++i)
            add(doc, QString::number(i));
        QVERIFY(!doc.editLayer({nova::LayerCommand::Add, {}, "Too many"}));
        QVERIFY(!doc.editLayer({nova::LayerCommand::Opacity, doc.activeLayer(), {}, 256}));
        QVERIFY(!doc.editLayer({nova::LayerCommand::Opacity, doc.activeLayer(), {}, -1}));
        QVERIFY(!doc.editLayer({nova::LayerCommand::Rename, doc.activeLayer(), " "}));
        QVERIFY(!doc.editLayer({nova::LayerCommand::Move, doc.activeLayer(), {}, 64}));
        QVERIFY(!doc.editLayer({nova::LayerCommand::Delete, QUuid::createUuid()}));
        nova::Document large({}, pixels(4096));
        QVERIFY(!large.editLayer({nova::LayerCommand::Add, {}, "Beyond decoded budget"}));
    }
    void singleSurfaceWriteSharing() {
        nova::Document doc("x.png", pixels(1024));
        auto originalBuffer = doc.image.constBits();
        doc.draw({0, 0}, {0, 0}, qRgba(10, 20, 30, 255));
        doc.finish();
        QCOMPARE(doc.image.constBits(), originalBuffer);
        doc.undo();
        QCOMPARE(doc.image.constBits(), originalBuffer);
        doc.redo();
        QCOMPARE(doc.image.constBits(), originalBuffer);
        auto snapshot = doc.recoverySnapshot();
        auto shared = snapshot->image;
        doc.draw({1, 1}, {1, 1}, qRgba(90, 80, 70, 255));
        doc.finish();
        QVERIFY(doc.image.constBits() != originalBuffer);
        QCOMPARE(snapshot->image, shared);
        QCOMPARE(snapshot->layers()[0].pixels.pixel(1, 1), QRgb(0));
    }
    void identitiesAndOperations() {
        nova::Document doc({}, pixels());
        auto base = doc.activeLayer();
        QVERIFY(!doc.editLayer({nova::LayerCommand::Delete, base}));
        add(doc);
        auto ink = doc.activeLayer();
        QVERIFY(ink != base);
        doc.draw({0, 0}, {2, 0}, qRgba(250, 30, 40, 255));
        doc.finish();
        QVERIFY(doc.editLayer({nova::LayerCommand::Rename, ink, "Details"}));
        QVERIFY(doc.editLayer({nova::LayerCommand::Duplicate, ink}));
        auto copy = doc.activeLayer();
        QVERIFY(copy != ink);
        QCOMPARE(doc.layers()[1].pixels, doc.layers()[2].pixels);
        doc.draw({0, 0}, {0, 0}, 0);
        doc.finish();
        QCOMPARE(doc.layers()[1].pixels.pixel(0, 0), qRgba(250, 30, 40, 255));
        QCOMPARE(doc.layers()[2].pixels.pixel(0, 0), QRgb(0));
        QVERIFY(doc.editLayer({nova::LayerCommand::Move, copy, {}, 0}));
        QCOMPARE(doc.layers()[0].id, copy);
        QCOMPARE(doc.activeLayer(), copy);
        QVERIFY(doc.editLayer({nova::LayerCommand::Delete, ink}));
        doc.undo();
        QCOMPARE(doc.layers()[2].id, ink);
        QCOMPARE(doc.layers()[2].name, QString("Details"));
        doc.undo();
        QCOMPARE(doc.layers()[2].id, copy);
        doc.redo();
        QCOMPARE(doc.layers()[0].id, copy);
    }
    void alphaAndOrdering() {
        nova::Document doc({}, pixels(16, qRgba(0, 0, 255, 255)));
        add(doc);
        auto ink = doc.activeLayer();
        doc.draw({0, 0}, {0, 0}, qRgba(255, 0, 0, 128));
        doc.finish();
        QCOMPARE(doc.image.pixel(0, 0), qRgba(128, 0, 127, 255));
        QVERIFY(doc.editLayer({nova::LayerCommand::Opacity, ink, {}, 128}));
        QCOMPARE(doc.image.pixel(0, 0), qRgba(64, 0, 191, 255));
        QCOMPARE(doc.layers()[1].pixels.pixel(0, 0), qRgba(255, 0, 0, 128));
        QVERIFY(doc.editLayer({nova::LayerCommand::Visibility, ink, {}, 0}));
        QCOMPARE(doc.image.pixel(0, 0), qRgba(0, 0, 255, 255));
        QVERIFY(!doc.paint(ink, {0, 0}, {0, 0}, 0));
        doc.undo();
        QCOMPARE(doc.image.pixel(0, 0), qRgba(64, 0, 191, 255));
        doc.undo();
        QCOMPARE(doc.image.pixel(0, 0), qRgba(128, 0, 127, 255));
        QVERIFY(doc.editLayer({nova::LayerCommand::Move, ink, {}, 0}));
        QCOMPARE(doc.image.pixel(0, 0), qRgba(0, 0, 255, 255));
        for (int i = 0; i < 100; ++i) {
            doc.undo();
            QCOMPARE(doc.image.pixel(0, 0), qRgba(128, 0, 127, 255));
            doc.redo();
        }
        nova::Document clear({}, pixels(16, qRgba(100, 90, 80, 0)));
        add(clear);
        QCOMPARE(clear.image.pixel(0, 0), QRgb(0));
        clear.draw({0, 0}, {0, 0}, qRgba(255, 0, 0, 128));
        clear.finish();
        QCOMPARE(clear.image.pixel(0, 0), qRgba(255, 0, 0, 128));
        add(clear);
        clear.draw({0, 0}, {0, 0}, qRgba(0, 255, 0, 128));
        clear.finish();
        QCOMPARE(clear.image.pixel(0, 0), qRgba(85, 170, 0, 192));
    }
    void incrementalAndErase() {
        nova::Document doc({}, pixels(16, qRgba(20, 30, 40, 255)));
        add(doc);
        auto ink = doc.activeLayer();
        doc.dirty = {};
        doc.draw({5, 5}, {5, 5}, qRgba(240, 90, 60, 255));
        doc.finish();
        QCOMPARE(doc.dirty, QRect(5, 5, 1, 1));
        QCOMPARE(doc.image.pixel(4, 5), qRgba(20, 30, 40, 255));
        doc.draw({5, 5}, {5, 5}, 0);
        doc.finish();
        QCOMPARE(doc.image.pixel(5, 5), qRgba(20, 30, 40, 255));
        QCOMPARE(doc.layers()[0].pixels.pixel(5, 5), qRgba(20, 30, 40, 255));
        QVERIFY(!doc.paint(QUuid::createUuid(), {0, 0}, {0, 0}, 0));
        QVERIFY(!doc.paint(ink, {-1, 0}, {999999, 0}, 0));
    }
    void mixedHistoryAndBudgets() {
        QTemporaryDir tmp;
        nova::Document doc(tmp.path() + "/x.png", pixels());
        add(doc);
        auto ink = doc.activeLayer();
        doc.draw({0, 0}, {0, 0}, qRgba(10, 20, 30, 255));
        doc.finish();
        QVERIFY(doc.editLayer({nova::LayerCommand::Opacity, ink, {}, 90}));
        QVERIFY(doc.saveNative(tmp.path()));
        QVERIFY(!doc.modified());
        auto saved = doc.image;
        QVERIFY(doc.editLayer({nova::LayerCommand::Delete, ink}));
        doc.undo();
        QCOMPARE(doc.image, saved);
        QVERIFY(!doc.modified());
        doc.undo();
        QCOMPARE(doc.layers()[1].opacity, 255);
        doc.undo();
        QCOMPARE(doc.layers()[1].pixels.pixel(0, 0), QRgb(0));
        doc.redo();
        doc.redo();
        QVERIFY(!doc.modified());
        doc.undo();
        QVERIFY(doc.editLayer({nova::LayerCommand::Rename, ink, "branch"}));
        QVERIFY(!doc.canRedo());
        doc.setHistoryLimits(4096, 12);
        for (int i = 0; i < 3000; ++i) {
            QVERIFY(doc.editLayer({nova::LayerCommand::Opacity, ink, {}, i % 256}));
            QVERIFY(doc.historyBytes() <= 4096);
            QVERIFY(doc.historyOperations() <= 12);
        }
        QVERIFY(doc.modified());
        while (doc.canUndo())
            doc.undo();
        QVERIFY(doc.modified());
        doc.setHistoryLimits(128, 0);
        QCOMPARE(doc.historyBytes(), 0);
        nova::Document other({}, pixels());
        QVERIFY(!other.modified());
        QVERIFY(!other.canUndo());
    }
    void storageAndExport() {
        QTemporaryDir tmp;
        auto path = tmp.path() + "/assets/a/textures/x.png";
        QDir().mkpath(QFileInfo(path).absolutePath());
        auto original = pixels(16, qRgba(10, 20, 30, 255));
        QVERIFY(nova::savePng(path, original));
        nova::DocumentSession session;
        session.setProject(tmp.path());
        auto doc = session.open(path);
        QVERIFY(doc);
        QVERIFY(!QFileInfo::exists(tmp.path() + "/.nova"));
        add(*doc);
        auto ink = doc->activeLayer();
        doc->draw({1, 1}, {4, 4}, qRgba(200, 50, 100, 128));
        doc->finish();
        QVERIFY(doc->editLayer({nova::LayerCommand::Opacity, ink, {}, 125}));
        QVERIFY(doc->editLayer({nova::LayerCommand::Rename, ink, "Accent"}));
        QVERIFY(doc->editLayer({nova::LayerCommand::Duplicate, ink}));
        auto hidden = doc->activeLayer();
        QVERIFY(doc->editLayer({nova::LayerCommand::Visibility, hidden, {}, 0}));
        QVERIFY(doc->editLayer({nova::LayerCommand::Move, hidden, {}, 0}));
        QVERIFY(session.dispatch(doc, nova::Command::Save));
        QVERIFY(!doc->modified());
        QVERIFY(doc->exportModified());
        QCOMPARE(nova::loadPng(path), original);
        auto encoded = doc->encodeNative("assets/a/textures/x.png");
        nova::Document restored(path, QImage{});
        QVERIFY(restored.loadNative(tmp.path()));
        QCOMPARE(restored.encodeNative("assets/a/textures/x.png"), encoded);
        QCOMPARE(restored.image, doc->image);
        QVERIFY(session.dispatch(doc, nova::Command::ExportPng));
        QCOMPARE(nova::loadPng(path), doc->image);
        QVERIFY(!doc->exportModified());
        QVERIFY(doc->modified());
        QVERIFY(session.dispatch(doc, nova::Command::Save));
        auto target = tmp.path() + "/copy.png";
        QVERIFY(session.dispatch(doc, nova::Command::SaveAs, target));
        QCOMPARE(doc->path, path);
        QCOMPARE(nova::loadPng(target), doc->image);
        QVERIFY(!doc->modified());
        session.close(doc);
        auto reopened = session.open(path);
        QVERIFY(reopened);
        QCOMPARE(reopened->layers().size(), size_t(3));
        QVERIFY(QFile::remove(path));
        session.close(reopened);
        reopened = session.open(path);
        QVERIFY(reopened);
        QVERIFY(reopened->externalChanged());
    }
    void nativeConflictsAndFailure() {
        QTemporaryDir tmp;
        auto path = tmp.path() + "/x.png";
        QVERIFY(nova::savePng(path, pixels()));
        auto doc = nova::Document::open(path);
        add(*doc);
        QVERIFY(doc->saveNative(tmp.path()));
        auto nativePath = doc->editablePath();
        QFile file(nativePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("external");
        file.close();
        QVERIFY(!doc->saveNative(tmp.path()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("external"));
        file.close();
        auto external = pixels(16, qRgba(9, 8, 7, 255));
        QVERIFY(nova::savePng(path, external));
        QVERIFY(!doc->saveAsRevision(path, doc->revision()));
        QCOMPARE(nova::loadPng(path), external);
        auto stale = nova::Revision::read(path);
        QVERIFY(nova::savePng(path, pixels(16, 1)));
        QVERIFY(!doc->saveOver(stale));
        QVERIFY(doc->saveAs(tmp.path() + "/alternative.png"));
        QCOMPARE(doc->layers().size(), size_t(2));
        QVERIFY(!doc->saveAs(tmp.path() + "/missing/a.png"));
        nova::Document blocked(tmp.path() + "/other.png", pixels());
        add(blocked);
        QFile blocker(tmp.path() + "/.nova/documents/blocker");
        QVERIFY(!blocked.saveNative(tmp.path() + "/missing"));
    }
    void corruptNative() {
        nova::Document doc("x.png", pixels());
        add(doc);
        auto encoded = doc.encodeNative("x.png");
        QVERIFY(!encoded.isEmpty());
        const auto initial = doc.image;
        QList<QByteArray> invalid{QByteArray("bad"), encoded.first(encoded.size() - 4),
                                  encoded + "trailing"};
        invalid.append(mutate(encoded, [](QJsonObject &m) { m["schema"] = 99; }));
        invalid.append(mutate(encoded, [](QJsonObject &m) { m["width"] = 5000; }));
        invalid.append(
            mutate(encoded, [](QJsonObject &m) { m["active"] = QUuid::createUuid().toString(); }));
        invalid.append(mutate(encoded, [](QJsonObject &m) {
            auto a = m["layers"].toArray();
            auto x = a[1].toObject();
            x["id"] = a[0].toObject()["id"];
            a[1] = x;
            m["layers"] = a;
        }));
        invalid.append(mutate(encoded, [](QJsonObject &m) {
            auto a = m["layers"].toArray();
            auto x = a[1].toObject();
            x["opacity"] = 999;
            a[1] = x;
            m["layers"] = a;
        }));
        auto corrupt = encoded;
        corrupt[corrupt.size() - 10] = char(corrupt.back() ^ 127);
        invalid.append(corrupt);
        for (const auto &bytes : invalid) {
            QString error;
            QVERIFY(!doc.decodeNative(bytes, "x.png", &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(doc.image, initial);
        }
        QVERIFY(!doc.decodeNative(encoded, "X.png"));
        QVERIFY(!doc.decodeNative(encoded, "../x.png"));
        auto extensions = mutate(
            encoded, [](QJsonObject &m) { m["extensions"] = QJsonObject{{"future", "preserve"}}; });
        QVERIFY(doc.decodeNative(extensions, "x.png"));
        QVERIFY(doc.encodeNative("x.png").contains("preserve"));
    }
    void nativeAtomicReplacementFailure() {
#ifdef Q_OS_WIN
        QTemporaryDir tmp;
        auto path = tmp.path() + "/x.png";
        QVERIFY(nova::savePng(path, pixels()));
        auto doc = nova::Document::open(path);
        add(*doc);
        QVERIFY(doc->saveNative(tmp.path()));
        auto file = doc->editablePath();
        auto revision = nova::Revision::read(file);
        doc->draw({0, 0}, {0, 0}, qRgba(1, 2, 3, 255));
        doc->finish();
        HANDLE locked = CreateFileW(reinterpret_cast<LPCWSTR>(file.utf16()), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked != INVALID_HANDLE_VALUE);
        QString error;
        const bool saved = doc->saveNative(tmp.path(), &error);
        CloseHandle(locked);
        QVERIFY(!saved);
        QVERIFY(!error.isEmpty());
        QVERIFY(revision.sameContent(nova::Revision::read(file)));
        QVERIFY(doc->modified());
#else
        QSKIP("Windows native replacement-sharing failure injection");
#endif
    }
    void pathSafety() {
        QTemporaryDir tmp;
        auto root = tmp.path() + "/pack";
        QDir().mkpath(root);
        for (const auto &path : {tmp.path() + "/outside.png", root + "/../outside.png",
                                 root + "/.nova/x.png", root + "/assets/a.png:stream"})
            QVERIFY(nova::nativeDocumentPath(root, path).isEmpty());
        auto a = nova::nativeDocumentPath(root, root + "/assets/A.png"),
             b = nova::nativeDocumentPath(root, root + "/assets/a.png");
        QCOMPARE(a, b);
        QVERIFY(a.startsWith(root + "/.nova/documents/"));
        QFile blocker(root + "/.nova");
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QVERIFY(nova::nativeDocumentPath(root, root + "/x.png").isEmpty());
    }
    void commandNotifications() {
        nova::DocumentSession session;
        auto doc = std::make_shared<nova::Document>("x.png", pixels());
        QVERIFY(session.adopt(doc));
        QSignalSpy changed(&session, &nova::DocumentSession::changed);
        auto duplicate = std::make_shared<nova::Document>("duplicate.png", pixels());
        duplicate->setRecoveryIdentity(doc->id());
        QVERIFY(!session.adopt(duplicate));
        QVERIFY(session.edit(doc, {nova::LayerCommand::Add, {}, "Ink"}));
        auto ink = doc->activeLayer();
        QVERIFY(session.paint(doc, ink, {0, 0}, {1, 1}, qRgba(3, 4, 5, 255), true));
        QVERIFY(session.dispatch(doc, nova::Command::Undo));
        QCOMPARE(changed.size(), 3);
        QVERIFY(!session.edit(std::make_shared<nova::Document>("other.png", pixels()),
                              {nova::LayerCommand::Add, {}, "No"}));
        session.close(doc);
        QVERIFY(!session.paint(doc, ink, {0, 0}, {0, 0}, 0));
    }
    void packExportExcludesMetadata() {
        QTemporaryDir tmp;
        auto root = tmp.path() + "/pack";
        QDir().mkpath(root + "/.nova/documents");
        QDir().mkpath(root + "/assets/a");
        for (auto name :
             {QString("pack.mcmeta"), QString(".nova/private.json"), QString("assets/a/x.png")}) {
            QFile file(root + '/' + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("data");
        }
        QVERIFY(nova::savePng(root + "/assets/a/x.png", pixels()));
        auto doc = nova::Document::open(root + "/assets/a/x.png");
        add(*doc);
        QVERIFY(doc->saveNative(root));
        auto destination = tmp.path() + "/minecraft";
        QVERIFY(nova::Project::exportMinecraft(root, destination));
        QVERIFY(QFileInfo::exists(destination + "/assets/a/x.png"));
        QVERIFY(QFileInfo::exists(destination + "/pack.mcmeta"));
        QVERIFY(!QFileInfo::exists(destination + "/.nova"));
        QVERIFY(!nova::Project::exportMinecraft(root, destination));
        QVERIFY(!nova::Project::exportMinecraft(root, root + "/nested"));
        doc->draw({0, 0}, {0, 0}, qRgba(9, 8, 7, 255));
        doc->finish();
        QVERIFY(doc->saveNative(root));
        QVERIFY(!nova::Project::exportMinecraft(root, tmp.path() + "/stale-export"));
        QVERIFY(!QFileInfo::exists(tmp.path() + "/stale-export"));
        auto index = nova::Index::scan(root);
        QVERIFY(!index.directories.contains(".nova"));
    }
    void compositeBenchmarks() {
        QJsonArray results;
        for (int size : {16, 64, 256})
            for (int layers : {4, 16, 64}) {
                nova::Document doc("benchmark.png", pixels(size));
                for (int n = 1; n < layers; ++n) {
                    add(doc, QString::number(n));
                    doc.draw({0, 0}, {size - 1, size - 1}, qRgba(80, 120, n * 3, 100));
                    doc.finish();
                }
                QList<double> partial, full;
                for (int i = 0; i < 100; ++i) {
                    QElapsedTimer timer;
                    timer.start();
                    doc.draw({1, 1}, {1, 1}, qRgba(i, 80, 120, 255));
                    doc.finish();
                    partial.append(timer.nsecsElapsed() / 1e6);
                }
                for (int i = 0; i < 6; ++i) {
                    QElapsedTimer timer;
                    timer.start();
                    QVERIFY(doc.editLayer(
                        {nova::LayerCommand::Opacity, doc.activeLayer(), {}, i % 2 ? 128 : 255}));
                    full.append(timer.nsecsElapsed() / 1e6);
                }
                std::sort(partial.begin(), partial.end());
                std::sort(full.begin(), full.end());
                QJsonObject row{{"size", size},
                                {"layers", layers},
                                {"partial_p50_ms", partial[50]},
                                {"partial_p95_ms", partial[94]},
                                {"full_p50_ms", (full[2] + full[3]) / 2},
                                {"full_p95_ms", full[5]},
                                {"pixel_bytes", double(doc.pixelBytes())},
                                {"history_bytes", double(doc.historyBytes())}};
                results.append(row);
            }
        const auto out = qEnvironmentVariable("NOVA_LAYER_BENCHMARK");
        if (!out.isEmpty()) {
            QFile file(out);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(results).toJson());
        }
    }
    void layeredRecovery() {
        QTemporaryDir tmp;
        nova::RecoveryStore store(tmp.path() + "/r");
        nova::Document doc(tmp.path() + "/missing.png", pixels());
        add(doc);
        auto ink = doc.activeLayer();
        doc.draw({1, 1}, {6, 6}, qRgba(120, 80, 40, 200));
        doc.finish();
        QVERIFY(doc.editLayer({nova::LayerCommand::Opacity, ink, {}, 77}));
        QVERIFY(doc.editLayer({nova::LayerCommand::Duplicate, ink}));
        auto hidden = doc.activeLayer();
        QVERIFY(doc.editLayer({nova::LayerCommand::Visibility, hidden, {}, 0}));
        QVERIFY(doc.editLayer({nova::LayerCommand::Move, hidden, {}, 0}));
        auto snapshot = doc.recoverySnapshot();
        QVERIFY(store.writeDocument(*snapshot, tmp.path()));
        doc.draw({0, 0}, {0, 0},
                 1); // hidden target is rejected; snapshot remains detached from future edits.
        auto e = store.load(doc.id());
        QVERIFY2(e.error.isEmpty(), qPrintable(e.error));
        QVERIFY(!e.nativeData.isEmpty());
        nova::Document restored(e.path, QImage{});
        QVERIFY(restored.decodeNative(e.nativeData, "recovery.png"));
        restored.markRecovered();
        QCOMPARE(restored.encodeNative("recovery.png"), snapshot->encodeNative("recovery.png"));
        QCOMPARE(restored.activeLayer(), hidden);
        QCOMPARE(restored.layers()[0].visible, false);
        QVERIFY(restored.modified());
        QVERIFY(!QFileInfo::exists(e.path));
        QVERIFY(nova::savePng(e.path, pixels(16, 7)));
        QVERIFY(restored.externalChanged());
        QVERIFY(!restored.saveAsRevision(e.path, restored.revision()));
        auto legacy = QUuid::createUuid();
        QVERIFY(store.write(legacy, "old.png", {}, {}, pixels()));
        QVERIFY(!store.load(legacy).image.isNull());
    }
    void forcedLayeredTermination() {
        QTemporaryDir tmp;
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--layered-child", tmp.path()});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForReadyRead(15000));
        QCOMPARE(child.readAllStandardOutput().trimmed(), QByteArray("ready"));
        child.kill();
        QVERIFY(child.waitForFinished());
        nova::RecoveryStore store(tmp.path());
        auto entries = store.entries();
        QCOMPARE(entries.size(), 1);
        auto e = store.load(entries[0].id);
        QVERIFY(e.error.isEmpty());
        nova::Document doc(e.path, QImage{});
        QVERIFY(doc.decodeNative(e.nativeData, "recovery.png"));
        QCOMPARE(doc.layers().size(), size_t(3));
        QCOMPARE(doc.layers()[0].visible, false);
        QCOMPARE(doc.layers()[0].opacity, 100);
        QCOMPARE(doc.layers()[2].pixels.pixel(2, 2), qRgba(9, 8, 7, 255));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--layered-child")) {
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(0);
        nova::Document doc("missing.png", image);
        doc.editLayer({nova::LayerCommand::Add, {}, "One"});
        doc.draw({2, 2}, {2, 2}, qRgba(9, 8, 7, 255));
        doc.finish();
        doc.editLayer({nova::LayerCommand::Add, {}, "Two"});
        doc.draw({1, 1}, {1, 1}, qRgba(30, 50, 70, 255));
        doc.finish();
        auto id = doc.activeLayer();
        doc.editLayer({nova::LayerCommand::Opacity, id, {}, 100});
        doc.editLayer({nova::LayerCommand::Visibility, id, {}, 0});
        doc.editLayer({nova::LayerCommand::Move, id, {}, 0});
        if (!nova::RecoveryStore(app.arguments().last()).writeDocument(doc, {}))
            return 2;
        fprintf(stdout, "ready\n");
        fflush(stdout);
        return app.exec();
    }
    LayeredTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "layered_test.moc"
