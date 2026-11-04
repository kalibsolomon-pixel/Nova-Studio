// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include "recovery.h"
#include "session.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <cstdlib>
#include <algorithm>
using namespace nova;
namespace {
std::shared_ptr<Document> makeDocument(const QString &path) {
    QImage pixels(16, 16, QImage::Format_RGBA8888); pixels.fill(Qt::transparent);
    auto doc = std::make_shared<Document>(path, pixels); doc->convertToAnimation();
    doc->editFrame(FrameCommand::Duplicate); doc->editFrame(FrameCommand::Duplicate); return doc;
}
}
class AnimationStorageTest : public QObject {
    Q_OBJECT
  private slots:
    void legacyNativeUnchangedSemantics() {
        QFile fixture(QString(QT_TESTCASE_SOURCEDIR) + "/fixtures/c2-legacy.nvdoc"); QVERIFY(fixture.open(QIODevice::ReadOnly));
        auto bytes = fixture.readAll(); const auto count = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + 8));
        auto asset = QJsonDocument::fromJson(bytes.mid(12, count)).object()["asset"].toString();
        Document doc("legacy.png", QImage{}); QVERIFY(doc.decodeNative(bytes, asset)); QVERIFY(!doc.animated());
        const auto layers = doc.layers(); auto rewritten = doc.encodeNative(asset); QVERIFY(rewritten.startsWith("NVLD0001"));
        Document reread("legacy.png", QImage{}); QVERIFY(reread.decodeNative(rewritten, asset));
        QCOMPARE(reread.layers().size(), layers.size());
        for (size_t i = 0; i < layers.size(); ++i) { QCOMPARE(reread.layers()[i].pixels, layers[i].pixels); QCOMPARE(reread.layers()[i].id, layers[i].id); }
    }
    void boundedCorruptionAndPreservedState() {
        auto doc = makeDocument("texture.png"); const auto original = doc->animationState(); auto bytes = doc->encodeNative("texture.png");
        for (int length : {0, 8, 11, 20, int(bytes.size() - 1)}) {
            QVERIFY(!doc->decodeNative(bytes.first(length), "texture.png")); QCOMPARE(doc->activeFrame(), original.active);
        }
        QVERIFY(!doc->decodeNative(bytes + 'x', "texture.png"));
        auto limits = doc->frameLimits(); limits.sourceFrames = 2;
        Document limited("texture.png", QImage(1, 1, QImage::Format_RGBA8888)); QVERIFY(limited.setAnimationLimits(limits));
        QVERIFY(!limited.decodeNative(bytes, "texture.png")); QVERIFY(!limited.animated());
        auto snapshot = doc->recoverySnapshot(); doc->draw({0, 0}, {0, 0}, qRgb(10, 20, 30)); doc->finish();
        QCOMPARE(snapshot->image.pixelColor(0, 0), QColor(Qt::transparent));
    }
    void processTerminationRecovery_data() {
        QTest::addColumn<QString>("scenario");
        for (auto name : {"edit", "duplicate", "reorder", "timing", "native-save", "delete", "multi-frame"}) QTest::newRow(name) << QString(name);
    }
    void processTerminationRecovery() {
        QFETCH(QString, scenario); QTemporaryDir tmp;
        QProcess process; process.start(QCoreApplication::applicationFilePath(), {"--crash-animation", scenario, tmp.path()});
        QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 87);
        RecoveryStore store(tmp.filePath("recovery")); auto entries = store.entries(); QCOMPARE(entries.size(), 1);
        QString error; auto entry = store.load(entries[0].id); QVERIFY2(entry.error.isEmpty(), qPrintable(entry.error));
        Document recovered(entry.path, QImage{}); QVERIFY2(recovered.decodeNative(entry.nativeData, "recovery.png", &error), qPrintable(error));
        QVERIFY(recovered.animated()); QVERIFY(recovered.validateAnimation(recovered.animationState(), &error));
        QCOMPARE(recovered.animationState().frames.size(), scenario == "duplicate" ? size_t(4) : scenario == "delete" ? size_t(2) : size_t(3));
        QVERIFY(!QFileInfo::exists(entry.path)); // recovering native work never exports a PNG
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--crash-animation") {
        const auto scenario = args[2], root = args[3]; auto doc = makeDocument(root + "/texture.png");
        if (scenario == "edit") { doc->draw({2, 2}, {5, 5}, qRgb(220, 50, 100)); doc->finish(); }
        if (scenario == "duplicate") doc->editFrame(FrameCommand::Duplicate);
        if (scenario == "delete") doc->editFrame(FrameCommand::Delete);
        if (scenario == "reorder" || scenario == "timing") {
            auto state = doc->animationState(); if (scenario == "reorder") std::reverse(state.entries.begin(), state.entries.end()); else state.entries[0].ticks = 12;
            doc->replaceAnimation(state, scenario);
        }
        if (scenario == "multi-frame") {
            std::vector<QUuid> ids; for (const auto &frame : doc->animationState().frames) ids.push_back(frame.id);
            doc->applyFrames(ids, {"invert", {}});
        }
        if (scenario == "native-save" && !doc->saveNative(root)) return 4;
        RecoveryStore store(root + "/recovery"); if (!store.writeDocument(*doc->recoverySnapshot(), root)) return 5;
        std::_Exit(87);
    }
    AnimationStorageTest test; return QTest::qExec(&test, argc, argv);
}
#include "animation_storage_test.moc"
