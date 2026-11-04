// SPDX-License-Identifier: GPL-3.0-or-later
#include "animation_export.h"
#include "minecraft_animation.h"
#include "project.h"
#include <QCoreApplication>
#include <QFile>
#include <QDirIterator>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <cstdlib>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace nova;
namespace {
std::shared_ptr<Document> document(const QString &path) {
    QImage pixels(8, 4, QImage::Format_RGBA8888); pixels.fill(QColor(10, 20, 30, 40));
    auto doc = std::make_shared<Document>(path, pixels); doc->convertToAnimation(); doc->editFrame(FrameCommand::Duplicate);
    doc->draw({1, 1}, {1, 1}, qRgb(200, 10, 30)); doc->finish();
    auto state = doc->animationState(); state.defaultTicks = 3; state.entries.push_back({state.frames[0].id, 7}); state.interpolate = true;
    state.metadata = {{"unknown", QJsonObject{{"retain", "this"}}}, {"texture", QJsonObject{{"blur", false}}}};
    doc->replaceAnimation(state, "timing"); return doc;
}
QByteArray read(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
void write(const QString &path, const QByteArray &bytes) { QFile file(path); if (file.open(QIODevice::WriteOnly)) file.write(bytes); }
}
class AnimationExportTest : public QObject {
    Q_OBJECT
  private slots:
    void exactPairRoundTripAndBaseline() {
        QTemporaryDir temp; const auto path = temp.filePath("texture.png"); auto doc = document(path);
        QString error; auto planned = planAnimationExport(*doc, 2, "26.3", &error); QVERIFY2(planned.ok(), qPrintable(error));
        auto journal = stageAnimationPair(path, planned.png, planned.mcmeta, {}, {}, &error); QVERIFY2(!journal.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(path)); QVERIFY(finishAnimationPair(journal, PairAction::Retry, &error));
        QVERIFY(inspectAnimationPair(journal).complete); QCOMPARE(read(path), planned.png); QCOMPARE(read(path + ".mcmeta"), planned.mcmeta);
        auto imported = openMinecraftAnimation(path, "26.3", &error); QVERIFY2(imported, qPrintable(error));
        auto a = doc->animationState(), b = imported->animationState(); QCOMPARE(a.frames.size(), b.frames.size());
        for (size_t i = 0; i < a.frames.size(); ++i) QCOMPARE(doc->frameComposite(a.frames[i].id), imported->frameComposite(b.frames[i].id));
        QCOMPARE(b.entries.size(), size_t(3)); QCOMPARE(b.entries[2].ticks, 7); QVERIFY(b.interpolate); QCOMPARE(b.metadata["unknown"], a.metadata["unknown"]);
        doc->acceptAnimationExport(Revision::read(path), Revision::read(path + ".mcmeta")); QVERIFY(!doc->exportModified());
        QVERIFY(doc->saveNative(temp.path(), &error)); Document loaded(path, QImage{}); QVERIFY(loaded.loadNative(temp.path(), &error));
        QVERIFY(!loaded.exportModified()); QVERIFY(loaded.mcmetaRevision().sameContent(Revision::read(path + ".mcmeta")));
    }
    void preflightExternalChanges_data() { QTest::addColumn<bool>("metadata"); QTest::newRow("PNG") << false; QTest::newRow("metadata") << true; }
    void preflightExternalChanges() {
        QFETCH(bool, metadata); QTemporaryDir temp; auto path = temp.filePath("texture.png"); auto doc = document(path); auto plan = planAnimationExport(*doc);
        write(path, plan.png); write(path + ".mcmeta", plan.mcmeta); auto png = Revision::read(path), meta = Revision::read(path + ".mcmeta");
        const auto changed = path + (metadata ? ".mcmeta" : ""); write(changed, "external edit");
        QString error; QVERIFY(stageAnimationPair(path, plan.png, plan.mcmeta, png, meta, &error).isEmpty()); QVERIFY(error.contains("conflict")); QCOMPARE(read(changed), QByteArray("external edit"));
    }
    void partialRetryAndRollback() {
        QTemporaryDir temp; auto path = temp.filePath("texture.png"); auto plan = planAnimationExport(*document(path));
        write(path, "old PNG bytes"); write(path + ".mcmeta", "{\"texture\":{\"clamp\":true}}");
        auto png = Revision::read(path), meta = Revision::read(path + ".mcmeta"); QString error;
        auto journal = stageAnimationPair(path, plan.png, plan.mcmeta, png, meta, &error); QVERIFY2(!journal.isEmpty(), qPrintable(error));
        QVERIFY(!finishAnimationPair(journal, PairAction::Retry, &error, [] { return false; }));
        QCOMPARE(read(path), plan.png); QVERIFY(meta.sameContent(Revision::read(path + ".mcmeta")));
        QCOMPARE(pendingAnimationPairs(path).size(), 1);
        QVERIFY2(finishAnimationPair(journal, PairAction::Rollback, &error), qPrintable(error));
        QVERIFY(png.sameContent(Revision::read(path))); QVERIFY(meta.sameContent(Revision::read(path + ".mcmeta")));
        QVERIFY(QFileInfo::exists(QFileInfo(journal).absolutePath() + "/png.before"));
        auto fresh = stageAnimationPair(path, plan.png, plan.mcmeta, png, meta, &error); QVERIFY(!fresh.isEmpty());
        QVERIFY(!finishAnimationPair(fresh, PairAction::Retry, &error, [] { return false; }));
        QVERIFY(finishAnimationPair(fresh, PairAction::Retry, &error)); QCOMPARE(read(path + ".mcmeta"), plan.mcmeta);
    }
    void concurrentChangeBeforeSecondCommit_data() { QTest::addColumn<bool>("metadata"); QTest::newRow("PNG") << false; QTest::newRow("metadata") << true; }
    void concurrentChangeBeforeSecondCommit() {
        QFETCH(bool, metadata); QTemporaryDir temp; auto path = temp.filePath("texture.png"); auto plan = planAnimationExport(*document(path)); QString error;
        auto journal = stageAnimationPair(path, plan.png, plan.mcmeta, {}, {}, &error); QVERIFY(!journal.isEmpty());
        const auto changed = path + (metadata ? ".mcmeta" : "");
        QVERIFY(!finishAnimationPair(journal, PairAction::Retry, &error, [&] { write(changed, "external"); return true; }));
        QCOMPARE(read(changed), QByteArray("external"));
        QVERIFY(!finishAnimationPair(journal, PairAction::Retry, &error)); QVERIFY(!finishAnimationPair(journal, PairAction::Rollback, &error));
        QCOMPARE(read(changed), QByteArray("external"));
    }
    void damagedBackupAndInjectedDiskFull() {
        QTemporaryDir temp; auto path = temp.filePath("texture.png"); auto plan = planAnimationExport(*document(path));
        write(path, "original"); const auto original = Revision::read(path); QString error;
        auto writer = [](const QString &file, const QByteArray &bytes, const Revision &expected, QString *error) {
            if (file.endsWith("meta.after")) { if (error) *error = "Injected insufficient disk space"; return false; }
            return atomicWrite(file, bytes, expected, error);
        };
        QVERIFY(stageAnimationPair(path, plan.png, plan.mcmeta, original, {}, &error, writer).isEmpty());
        QVERIFY(error.contains("disk space")); QVERIFY(original.sameContent(Revision::read(path))); QVERIFY(!QFileInfo::exists(path + ".mcmeta"));
        auto journal = stageAnimationPair(path, plan.png, plan.mcmeta, original, {}, &error); QVERIFY(!journal.isEmpty());
        QVERIFY(!finishAnimationPair(journal, PairAction::Retry, &error, [] { return false; }));
        write(QFileInfo(journal).absolutePath() + "/png.before", "damaged");
        const auto before = Revision::read(path); QVERIFY(!finishAnimationPair(journal, PairAction::Rollback, &error));
        QVERIFY(error.contains("checksum")); QVERIFY(before.sameContent(Revision::read(path)));
    }
    void absentOriginalRollbackRetainsNewBytes() {
        QTemporaryDir temp; auto path = temp.filePath("texture.png"); auto plan = planAnimationExport(*document(path)); QString error;
        auto journal = stageAnimationPair(path, plan.png, plan.mcmeta, {}, {}, &error); QVERIFY(!journal.isEmpty());
        QVERIFY(!finishAnimationPair(journal, PairAction::Retry, &error, [] { return false; }));
        QVERIFY(finishAnimationPair(journal, PairAction::Rollback, &error)); QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(read(QFileInfo(journal).absolutePath() + "/png.rolled-forward"), plan.png);
    }
    void nativeWindowsLockedMetadata() {
#ifdef Q_OS_WIN
        QTemporaryDir temp; auto path = temp.filePath("texture.png"); auto plan = planAnimationExport(*document(path));
        write(path + ".mcmeta", "{}"); const auto before = Revision::read(path + ".mcmeta"); QString error;
        auto journal = stageAnimationPair(path, plan.png, plan.mcmeta, {}, before, &error); QVERIFY(!journal.isEmpty());
        HANDLE lock = CreateFileW(reinterpret_cast<LPCWSTR>((path + ".mcmeta").utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        const bool committed = finishAnimationPair(journal, PairAction::Retry, &error); CloseHandle(lock);
        QVERIFY(!committed); QVERIFY(before.sameContent(Revision::read(path + ".mcmeta"))); QCOMPARE(read(path), plan.png);
        QVERIFY2(finishAnimationPair(journal, PairAction::Retry, &error), qPrintable(error));
#endif
    }
    void abruptExitBetweenReplacements() {
        QTemporaryDir temp; auto path = temp.filePath("texture.png"); QProcess process;
        process.start(QCoreApplication::applicationFilePath(), {"--crash-export", path}); QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 88);
        auto pending = pendingAnimationPairs(path); QCOMPARE(pending.size(), 1); QVERIFY(QFileInfo::exists(path)); QVERIFY(!QFileInfo::exists(path + ".mcmeta"));
        QString error; QVERIFY2(finishAnimationPair(pending[0], PairAction::Retry, &error), qPrintable(error));
        QVERIFY(openMinecraftAnimation(path, "26.1", &error)); QVERIFY(pendingAnimationPairs(path).isEmpty());
    }
    void relocatedAnimationJournal_data() {
        QTest::addColumn<bool>("legacy");
        QTest::addColumn<bool>("move");
        QTest::newRow("copy-v2") << false << false;
        QTest::newRow("move-v2") << false << true;
        QTest::newRow("copy-v1") << true << false;
        QTest::newRow("move-v1") << true << true;
    }
    void relocatedAnimationJournal() {
        QFETCH(bool, legacy);
        QFETCH(bool, move);
        QTemporaryDir temp;
        const auto original = temp.filePath("original");
        const auto relocated = temp.filePath("relocated");
        const QString asset = "assets/minecraft/textures/block/animated.png";
        const auto path = original + '/' + asset;
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        auto doc = document(path);
        const auto plan = planAnimationExport(*doc);
        QString error;
        const auto journal = stageAnimationPair(path, plan.png, plan.mcmeta, {}, {}, &error);
        QVERIFY2(!journal.isEmpty(), qPrintable(error));
        QVERIFY2(finishAnimationPair(journal, PairAction::Retry, &error), qPrintable(error));
        doc->acceptAnimationExport(Revision::read(path), Revision::read(path + ".mcmeta"));
        QVERIFY2(doc->saveNative(original, &error), qPrintable(error));
        if (legacy) {
            auto record = QJsonDocument::fromJson(read(journal)).object();
            record["schema"] = 1;
            record["png"] = path;
            write(journal, QJsonDocument(record).toJson());
        }
        QVERIFY(inspectAnimationPair(journal).complete);
        if (move) {
            QVERIFY(QDir().rename(original, relocated));
        } else {
            QDirIterator files(original, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
            while (files.hasNext()) {
                const auto source = files.next();
                const auto target = relocated + '/' + QDir(original).relativeFilePath(source);
                QVERIFY(QDir().mkpath(QFileInfo(target).absolutePath()));
                QVERIFY(QFile::copy(source, target));
            }
        }
        const auto copiedPath = relocated + '/' + asset;
        const auto copiedJournal = relocated + '/' + QDir(original).relativeFilePath(journal);
        const auto status = inspectAnimationPair(copiedJournal);
        QVERIFY2(status.complete, qPrintable(status.error));
        QCOMPARE(status.png, copiedPath);
        QVERIFY(pendingAnimationPairs(copiedPath).isEmpty());
        QVERIFY2(Project::exportMinecraft(relocated, temp.filePath("exported"), &error),
                 qPrintable(error));
        QCOMPARE(read(temp.filePath("exported/" + asset + ".mcmeta")), plan.mcmeta);

        const auto interruptedPath = QFileInfo(copiedPath).absolutePath() + "/interrupted.png";
        const auto interrupted = stageAnimationPair(interruptedPath, plan.png, plan.mcmeta, {}, {});
        QVERIFY(!interrupted.isEmpty());
        QVERIFY(!finishAnimationPair(interrupted, PairAction::Retry, &error, [] { return false; }));
        QCOMPARE(pendingAnimationPairs(interruptedPath), QStringList{interrupted});
        QVERIFY(!Project::exportMinecraft(relocated, temp.filePath("blocked"), &error));
        QVERIFY(error.contains("interrupted"));
        QVERIFY2(finishAnimationPair(interrupted, PairAction::Retry, &error), qPrintable(error));
    }
    void malformedAndEscapingJournalTargets() {
        QTemporaryDir temp;
        const auto png = temp.filePath("texture.png");
        const auto plan = planAnimationExport(*document(png));
        const auto journal = stageAnimationPair(png, plan.png, plan.mcmeta, {}, {});
        QVERIFY(!journal.isEmpty());
        auto record = QJsonDocument::fromJson(read(journal)).object();
        for (const QString target : {"", "../texture.png", "dir/texture.png",
                                     "dir\\texture.png", "C:/outside.png", "a:stream.png",
                                     "NUL.png"}) {
            record["png"] = target;
            write(journal, QJsonDocument(record).toJson());
            QVERIFY(!inspectAnimationPair(journal).valid);
            QVERIFY(!finishAnimationPair(journal, PairAction::Retry));
        }
        write(journal, "{");
        QVERIFY(!inspectAnimationPair(journal).valid);
#ifdef Q_OS_WIN
        const auto target = temp.filePath("outside");
        QVERIFY(QDir().mkdir(target));
        const auto junction = temp.filePath("escape");
        QCOMPARE(QProcess::execute("cmd.exe", {"/c", "mklink", "/J",
                    QDir::toNativeSeparators(junction), QDir::toNativeSeparators(target)}), 0);
        QVERIFY(QFileInfo(junction).isJunction());
        const auto outsidePng = target + "/texture.png";
        const auto outsideJournal = stageAnimationPair(outsidePng, plan.png, plan.mcmeta, {}, {});
        QVERIFY(!outsideJournal.isEmpty());
        const auto throughJunction = junction + '/' + QDir(target).relativeFilePath(outsideJournal);
        QVERIFY(!inspectAnimationPair(throughJunction).valid);
        QVERIFY(!finishAnimationPair(throughJunction, PairAction::Retry));
        QVERIFY(!QFileInfo::exists(outsidePng));
#endif
    }
    void wholePackExportChecksClosedAnimationPair() {
        QTemporaryDir temp;
        const auto root = temp.filePath("pack"), path = root + "/assets/minecraft/textures/block/animated.png";
        QDir().mkpath(QFileInfo(path).absolutePath());
        auto doc = document(path); auto plan = planAnimationExport(*doc); QString error;
        auto journal = stageAnimationPair(path, plan.png, plan.mcmeta, {}, {}, &error); QVERIFY(!journal.isEmpty());
        QVERIFY(finishAnimationPair(journal, PairAction::Retry, &error));
        doc->acceptAnimationExport(Revision::read(path), Revision::read(path + ".mcmeta")); QVERIFY(doc->saveNative(root, &error));
        QVERIFY2(Project::exportMinecraft(root, temp.filePath("exported"), &error), qPrintable(error));
        QCOMPARE(read(temp.filePath("exported/assets/minecraft/textures/block/animated.png.mcmeta")), plan.mcmeta);
        QVERIFY(!QFileInfo::exists(temp.filePath("exported/.nova")));
        write(path + ".mcmeta", "{\"animation\":{\"frametime\":90}}"); QVERIFY(doc->externalChanged());
        QVERIFY(!Project::exportMinecraft(root, temp.filePath("blocked"), &error));
        QVERIFY(error.contains("metadata")); QVERIFY(!QFileInfo::exists(temp.filePath("blocked")));
        write(path + ".mcmeta", plan.mcmeta);
        auto unbound = QFileInfo(path).absolutePath() + "/unbound.png";
        auto pending = stageAnimationPair(unbound, plan.png, plan.mcmeta, {}, {}, &error); QVERIFY(!pending.isEmpty());
        QVERIFY(!finishAnimationPair(pending, PairAction::Retry, &error, [] { return false; }));
        QVERIFY(!Project::exportMinecraft(root, temp.filePath("partial-blocked"), &error)); QVERIFY(error.contains("interrupted"));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); const auto args = app.arguments();
    if (args.size() == 3 && args[1] == "--crash-export") {
        auto path = args[2]; auto plan = planAnimationExport(*document(path)); auto journal = stageAnimationPair(path, plan.png, plan.mcmeta, {}, {});
        if (journal.isEmpty()) return 4;
        finishAnimationPair(journal, PairAction::Retry, nullptr, []() -> bool { std::_Exit(88); }); return 5;
    }
    AnimationExportTest test; return QTest::qExec(&test, argc, argv);
}
#include "animation_export_test.moc"
