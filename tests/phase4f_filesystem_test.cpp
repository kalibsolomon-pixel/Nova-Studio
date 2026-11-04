// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-F: Windows write contention, retry safety and reader handle lifetime.
// Critical cases hold real Win32 handles with the share modes that readers,
// viewers and scanners use, rather than simulating failures.
#include "assets.h"
#include "core.h"
#include "index.h"
#include "recovery.h"
#include <QDirIterator>
#include <QElapsedTimer>
#include <QImageReader>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <thread>
#include <qt_windows.h>

using namespace nova;
namespace {
std::wstring native(const QString &path) { return QDir::toNativeSeparators(path).toStdWString(); }
QByteArray bytesOf(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QImage pattern(int seed, int size = 64) {
    QImage im(size, size, QImage::Format_RGBA8888);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            im.setPixel(x, y, qRgba((x * 7 + seed) & 255, (y * 5 + seed) & 255, seed & 255, 255));
    return im;
}
QByteArray png(const QImage &im) {
    QByteArray out;
    QBuffer b(&out);
    b.open(QIODevice::WriteOnly);
    im.save(&b, "PNG");
    return out;
}
// Files beside `path` other than the destination itself (leftover temporary files).
QStringList strays(const QString &folder, const QStringList &expected) {
    QStringList out;
    for (const auto &name : QDir(folder).entryList(QDir::Files | QDir::Hidden | QDir::System))
        if (!expected.contains(name))
            out << name;
    return out;
}
// Holds a real handle with the given share mode for `ms` milliseconds on another thread.
struct HeldFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::thread releaser;
    HeldFile(const QString &path, DWORD share, int ms = -1) {
        handle = CreateFileW(native(path).c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (ms >= 0)
            releaser = std::thread([this, ms] {
                Sleep(DWORD(ms));
                release();
            });
    }
    void release() {
        auto h = std::exchange(handle, INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    ~HeldFile() {
        if (releaser.joinable())
            releaser.join();
        release();
    }
};
} // namespace

class Phase4fFilesystemTests : public QObject {
    Q_OBJECT
  private slots:
    // Root cause: the rename QSaveFile used is refused while any handle without delete
    // sharing is open on the destination, and sharing delete does not change that.
    void windowsRefusesReplaceOntoOpenFile() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("a.png"), next = tmp.filePath("b.tmp");
        for (DWORD share : {DWORD(FILE_SHARE_READ | FILE_SHARE_WRITE),
                            DWORD(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)}) {
            QFile::remove(dest);
            QVERIFY(savePng(dest, pattern(1)));
            QFile f(next);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("replacement");
            f.close();
            HeldFile reader(dest, share);
            QVERIFY(reader.handle != INVALID_HANDLE_VALUE);
            QVERIFY(!MoveFileExW(native(next).c_str(), native(dest).c_str(), MOVEFILE_REPLACE_EXISTING));
            QCOMPARE(GetLastError(), DWORD(ERROR_ACCESS_DENIED));
            reader.release();
            QVERIFY(MoveFileExW(native(next).c_str(), native(dest).c_str(), MOVEFILE_REPLACE_EXISTING));
        }
    }
    // Nova's own readers share delete, so a replacing save publishes on the first
    // attempt while they are open (POSIX-semantics rename on NTFS).
    void sharedReadersDoNotBlockSaves() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("e.png");
        QVERIFY(savePng(dest, pattern(1)));
        QFile shared;
        QVERIFY(openSharedRead(dest, shared));
        const auto old = shared.read(8);
        QString error;
        QVERIFY2(atomicWrite(dest, png(pattern(3)), Revision::read(dest), &error), qPrintable(error));
        const auto d = lastWriteDiagnostics();
        QVERIFY(d.posixRename);
        QCOMPARE(d.publishAttempts, 1);
        QCOMPARE(d.sharingRetries, 0);
        // The open reader keeps reading the file it opened; new opens see the new bytes.
        QCOMPARE(old, png(pattern(1)).left(8));
        QCOMPARE(shared.readAll(), png(pattern(1)).mid(8));
        QCOMPARE(bytesOf(dest), png(pattern(3)));
    }
    // A reader that closes within the retry window no longer fails the save, and the
    // writer and beforeCommit callbacks still run exactly once.
    void transientReaderIsRetriedWithoutRerunningCallbacks() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("t.png");
        QVERIFY(savePng(dest, pattern(2)));
        const auto replacement = png(pattern(3));
        int writes = 0, commits = 0;
        QString error;
        {
            HeldFile reader(dest, FILE_SHARE_READ | FILE_SHARE_WRITE, 120);
            QVERIFY(reader.handle != INVALID_HANDLE_VALUE);
            QVERIFY2(atomicWriteStream(
                         dest, Revision::read(dest),
                         [&](QIODevice &file) {
                             ++writes;
                             return file.write(replacement) == replacement.size();
                         },
                         &error, [&] { ++commits; }),
                     qPrintable(error));
        }
        const auto d = lastWriteDiagnostics();
        QCOMPARE(writes, 1);
        QCOMPARE(commits, 1);
        QVERIFY(d.sharingRetries > 0);
        QVERIFY(d.publishAttempts > 1);
        QCOMPARE(bytesOf(dest), replacement);
        QCOMPARE(strays(tmp.path(), {"t.png"}), QStringList());
    }
    // A handle held longer than the bounded window produces a clear failure; the
    // existing bytes and the folder are left exactly as they were.
    void persistentLockFailsWithoutDamage() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("locked.png");
        QVERIFY(savePng(dest, pattern(4)));
        const auto before = bytesOf(dest);
        HeldFile viewer(dest, FILE_SHARE_READ);
        QVERIFY(viewer.handle != INVALID_HANDLE_VALUE);
        int writes = 0;
        QString error;
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!atomicWriteStream(
            dest, Revision::read(dest),
            [&](QIODevice &file) {
                ++writes;
                return file.write(png(pattern(5))) > 0;
            },
            &error));
        const auto elapsed = timer.elapsed();
        const auto d = lastWriteDiagnostics();
        QCOMPARE(writes, 1);
        QVERIFY(d.retriesExhausted);
        QVERIFY(d.temporaryRemoved);
        QVERIFY2(error.contains("in use by another program"), qPrintable(error));
        QVERIFY2(elapsed < 2500, qPrintable(QString::number(elapsed)));
        viewer.release();
        QCOMPARE(bytesOf(dest), before);
        QCOMPARE(strays(tmp.path(), {"locked.png"}), QStringList());
        // Recovery after the failure: the next explicit save succeeds normally.
        QVERIFY2(atomicWrite(dest, png(pattern(6)), Revision::read(dest), &error), qPrintable(error));
        QCOMPARE(lastWriteDiagnostics().sharingRetries, 0);
        QCOMPARE(bytesOf(dest), png(pattern(6)));
    }
    // An exclusive lock (no read sharing) makes the destination unreadable: that is
    // reported as an inaccessible destination, never written over.
    void exclusiveLockIsNotOverwritten() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("x.png");
        QVERIFY(savePng(dest, pattern(7)));
        const auto expected = Revision::read(dest);
        const auto before = bytesOf(dest);
        QString error;
        {
            HeldFile exclusive(dest, 0);
            QVERIFY(exclusive.handle != INVALID_HANDLE_VALUE);
            QVERIFY(!atomicWrite(dest, "new", expected, &error));
            QVERIFY2(error.startsWith("External file conflict"), qPrintable(error));
        }
        QCOMPARE(bytesOf(dest), before);
        QCOMPARE(strays(tmp.path(), {"x.png"}), QStringList());
    }
    // Read-only files are refused at once: no temporary file, no rename, no retry.
    void readOnlyDestinationFailsImmediately() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("ro.png");
        QVERIFY(savePng(dest, pattern(8)));
        const auto before = bytesOf(dest);
        QVERIFY(SetFileAttributesW(native(dest).c_str(), FILE_ATTRIBUTE_READONLY));
        QString error;
        int writes = 0;
        QVERIFY(!atomicWriteStream(
            dest, Revision::read(dest), [&](QIODevice &) { return ++writes, true; }, &error));
        QCOMPARE(writes, 0);
        QCOMPARE(lastWriteDiagnostics().publishAttempts, 0);
        QVERIFY2(error.contains("not writable"), qPrintable(error));
        QVERIFY(!savePng(dest, pattern(9), &error));
        QVERIFY(SetFileAttributesW(native(dest).c_str(), FILE_ATTRIBUTE_NORMAL));
        QCOMPARE(bytesOf(dest), before);
        QCOMPARE(strays(tmp.path(), {"ro.png"}), QStringList());
    }
    // A permission refusal on the rename is not a sharing violation and is not retried.
    void deniedDeleteIsNotRetried() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("acl.txt");
        QVERIFY(atomicWrite(dest, "original", {}));
        // Deny DELETE to the current user on this test-owned file only.
        // Delete on a file is also granted through the folder's delete-child right, so
        // deny both, on this test-owned folder and file only.
        const auto user = qEnvironmentVariable("USERNAME");
        auto icacls = [](const QStringList &args) {
            QProcess p;
            p.start("icacls", args);
            return p.waitForFinished(10000) && p.exitCode() == 0;
        };
        QVERIFY(icacls({QDir::toNativeSeparators(dest), "/deny", user + ":(DE)"}));
        QVERIFY(icacls({QDir::toNativeSeparators(tmp.path()), "/deny", user + ":(DC)"}));
        QString error;
        QElapsedTimer timer;
        timer.start();
        const bool ok = atomicWrite(dest, "replacement", Revision::read(dest), &error);
        const auto d = lastWriteDiagnostics();
        QVERIFY(icacls({QDir::toNativeSeparators(tmp.path()), "/remove:d", user}));
        QVERIFY(icacls({QDir::toNativeSeparators(dest), "/remove:d", user}));
        QVERIFY(!ok);
        QCOMPARE(d.systemError, 5ul);
        QVERIFY(!d.retriesExhausted);
        QVERIFY2(d.sharingRetries <= 2, qPrintable(QString::number(d.sharingRetries)));
        QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(bytesOf(dest), QByteArray("original"));
        QCOMPARE(strays(tmp.path(), {"acl.txt"}), QStringList());
    }
    void invalidDestinations() {
        QTemporaryDir tmp;
        QString error;
        QVERIFY(!atomicWrite(tmp.filePath("missing/dir/a.png"), "x", {}, &error));
        QVERIFY(error.contains("folder does not exist"));
        QVERIFY(QDir(tmp.path()).mkdir("folder.png"));
        QVERIFY(!atomicWrite(tmp.filePath("folder.png"), "x", Revision::read(tmp.filePath("folder.png")), &error));
        QVERIFY(!atomicWrite(tmp.filePath("bad<name>.png"), "x", {}, &error));
        QVERIFY(!savePng(tmp.filePath("not-png.txt"), pattern(1), &error));
        QCOMPARE(strays(tmp.path(), {}), QStringList());
    }
    // A writer that fails midway leaves the original intact and removes its partial file.
    void interruptedWriteKeepsOriginal() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("i.bin");
        QVERIFY(atomicWrite(dest, QByteArray(4096, 'a'), {}));
        QString error;
        QVERIFY(!atomicWriteStream(
            dest, Revision::read(dest),
            [](QIODevice &file) {
                file.write(QByteArray(1024, 'b'));
                return false;
            },
            &error));
        QCOMPARE(bytesOf(dest), QByteArray(4096, 'a'));
        QCOMPARE(strays(tmp.path(), {"i.bin"}), QStringList());
    }
    // External changes are detected before publication, including during a retry wait.
    void conflictsAreNeverOverwritten() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("c.png");
        QVERIFY(atomicWrite(dest, "base", {}));
        const auto baseline = Revision::read(dest);
        QString error;
        int commits = 0;
        QVERIFY(!atomicWrite(dest, "ours", baseline, &error, [&] {
            ++commits;
            QFile f(dest);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("theirs");
        }));
        QCOMPARE(commits, 1);
        QVERIFY(error.startsWith("External file conflict"));
        QCOMPARE(bytesOf(dest), QByteArray("theirs"));
        // Expected-absent destination that appears before the rename is not replaced.
        const auto fresh = tmp.filePath("new.png");
        QVERIFY(!atomicWrite(fresh, "ours", Revision::read(fresh), &error, [&] {
            QFile f(fresh);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("theirs");
        }));
        QCOMPARE(bytesOf(fresh), QByteArray("theirs"));
        // A change made while the save waits for a reader is a conflict, not a replace.
        const auto waiting = tmp.filePath("w.png");
        QVERIFY(atomicWrite(waiting, "base", {}));
        const auto waitingBase = Revision::read(waiting);
        std::atomic_bool changed = false;
        {
            HeldFile reader(waiting, FILE_SHARE_READ | FILE_SHARE_WRITE);
            std::thread external([&] {
                Sleep(60);
                HANDLE h = CreateFileW(native(waiting).c_str(), GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                DWORD written = 0;
                WriteFile(h, "external", 8, &written, nullptr);
                CloseHandle(h);
                changed = true;
                Sleep(20);
                reader.release();
            });
            QVERIFY(!atomicWrite(waiting, "ours", waitingBase, &error));
            external.join();
        }
        QVERIFY(changed);
        QVERIFY2(error.startsWith("External file conflict"), qPrintable(error));
        QCOMPARE(bytesOf(waiting), QByteArray("external"));
        QCOMPARE(strays(tmp.path(), {"c.png", "new.png", "w.png"}), QStringList());
    }
    // Hundreds of replacements against a pool of decoding readers, the workload that
    // exposed the failure in the Phase 4-E indexing regression.
    void concurrentThumbnailReadsAndWrites() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("hot.png");
        QVERIFY(savePng(dest, pattern(0, 256)));
        std::atomic_bool stop = false;
        std::atomic_int decoded = 0, bad = 0;
        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i)
            readers.emplace_back([&] {
                while (!stop) {
                    const auto im = loadPng(dest);
                    if (im.isNull())
                        continue; // the file may be mid-replace on another reader's open
                    if (im.size() != QSize(256, 256))
                        ++bad;
                    ++decoded;
                }
            });
        // An external program that keeps the path open while it decodes, with a pause
        // between reads like a viewer refresh or a scanner pass.
        readers.emplace_back([&] {
            while (!stop) {
                {
                    QImageReader reader(dest, "png");
                    reader.read();
                }
                Sleep(10);
            }
        });
        int retried = 0;
        QString error;
        bool allSaved = true;
        for (int i = 1; i <= 300 && allSaved; ++i) {
            allSaved = savePng(dest, pattern(i, 256), &error);
            retried += lastWriteDiagnostics().sharingRetries > 0;
        }
        stop = true;
        for (auto &t : readers)
            t.join();
        QVERIFY2(allSaved, qPrintable(error));
        QCOMPARE(bad.load(), 0);
        QVERIFY(decoded > 0);
        QCOMPARE(loadPng(dest), pattern(300, 256).convertToFormat(QImage::Format_RGBA8888));
        QCOMPARE(strays(tmp.path(), {"hot.png"}), QStringList());
        qInfo("decoded %d, saves that waited for a reader %d of 300", decoded.load(), retried);
    }
    void repeatedAtomicReplacement() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("r.json");
        Revision expected;
        for (int i = 0; i < 500; ++i) {
            const auto bytes = QByteArray::number(i).repeated(32);
            QString error;
            QVERIFY2(atomicWrite(dest, bytes, expected, &error), qPrintable(error));
            expected = Revision::read(dest);
            QCOMPARE(bytesOf(dest), bytes);
        }
        QCOMPARE(strays(tmp.path(), {"r.json"}), QStringList());
    }
    // loadPng must not keep the source open while it decodes. An exclusive-open probe
    // on another thread succeeds whenever no reader holds the file, so the success ratio
    // approximates the share of decode time the file is free. Decoding straight from the
    // path (the previous behaviour) holds it for nearly the whole decode.
    void readersReleaseHandlesBeforeDecoding() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("h.png");
        QImage noisy(1024, 1024, QImage::Format_RGBA8888);
        for (int y = 0; y < noisy.height(); ++y)
            for (int x = 0; x < noisy.width(); ++x)
                noisy.setPixel(x, y, qRgba((x * y) & 255, (x ^ y) & 255, (x + y) & 255, 255));
        QVERIFY(savePng(dest, noisy));
        auto measure = [&](const std::function<void()> &read) {
            std::atomic_bool stop = false;
            std::atomic_int attempts = 0, free = 0;
            std::thread probe([&] {
                while (!stop) {
                    HANDLE h = CreateFileW(native(dest).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                    ++attempts;
                    if (h != INVALID_HANDLE_VALUE) {
                        ++free;
                        CloseHandle(h);
                    }
                    Sleep(0);
                }
            });
            for (int i = 0; i < 20; ++i)
                read();
            stop = true;
            probe.join();
            return attempts ? double(free) / attempts : 0.0;
        };
        const double buffered = measure([&] { loadPng(dest); });
        const double direct = measure([&] { QImageReader(dest, "png").read(); });
        qInfo("exclusive-open success while decoding: buffered %.2f, direct %.2f", buffered, direct);
        QVERIFY2(buffered > 0.5, qPrintable(QString::number(buffered)));
        QVERIFY(buffered > direct);
        QString error;
        QVERIFY2(savePng(dest, pattern(2, 64), &error), qPrintable(error));
        QCOMPARE(lastWriteDiagnostics().sharingRetries, 0);
    }
    // Document saves keep history consistent across a failed and a retried save.
    void documentSaveFailureKeepsHistory() {
        QTemporaryDir tmp;
        const auto path = tmp.filePath("doc.png");
        QVERIFY(savePng(path, pattern(1, 16)));
        auto doc = Document::open(path);
        QVERIFY(doc);
        QVERIFY(doc->paint(doc->activeLayer(), {1, 1}, {4, 4}, qRgb(200, 10, 10)));
        QVERIFY(doc->modified());
        const auto before = bytesOf(path);
        QString error;
        {
            HeldFile viewer(path, FILE_SHARE_READ);
            QVERIFY(!doc->save(&error));
        }
        QVERIFY(doc->modified());
        QCOMPARE(bytesOf(path), before);
        QVERIFY2(doc->save(&error), qPrintable(error));
        QVERIFY(!doc->modified());
        doc->undo();
        QVERIFY(doc->modified());
        QCOMPARE(strays(tmp.path(), {"doc.png"}), QStringList());
    }
    // Explicit export through savePng (the pack icon writer) under contention.
    void explicitExportUnderContention() {
        QTemporaryDir tmp;
        const auto dest = tmp.filePath("pack.png");
        QVERIFY(savePng(dest, pattern(1)));
        QString error;
        {
            HeldFile scanner(dest, FILE_SHARE_READ | FILE_SHARE_WRITE, 80);
            QVERIFY2(savePng(dest, pattern(2), &error), qPrintable(error));
        }
        QCOMPARE(loadPng(dest), pattern(2).convertToFormat(QImage::Format_RGBA8888));
    }
    // Recovery snapshots use the same publisher; a snapshot written while the previous
    // one is open for reading still lands, and restore reads it back intact.
    void recoverySnapshotUnderContention() {
        QTemporaryDir tmp;
        RecoveryStore store(tmp.filePath("recovery"));
        const auto id = QUuid::createUuid();
        QString error;
        QVERIFY2(store.write(id, tmp.filePath("a.png"), tmp.path(), {}, pattern(1, 16), &error),
                 qPrintable(error));
        {
            HeldFile reader(store.pathFor(id), FILE_SHARE_READ | FILE_SHARE_WRITE, 80);
            QVERIFY2(store.write(id, tmp.filePath("a.png"), tmp.path(), {}, pattern(2, 16), &error),
                     qPrintable(error));
        }
        const auto entry = store.load(id);
        QVERIFY2(entry.error.isEmpty(), qPrintable(entry.error));
        QCOMPARE(entry.image.convertToFormat(QImage::Format_RGBA8888),
                 pattern(2, 16).convertToFormat(QImage::Format_RGBA8888));
        store.discard(id);
        QVERIFY(!QFileInfo::exists(store.pathFor(id)));
    }
    // Switching projects while thumbnails are still decoding: stale results are
    // dropped, the old project's files are released, and both remain writable.
    void projectSwitchDuringPendingThumbnails() {
        QTemporaryDir a, b;
        for (int i = 0; i < 120; ++i) {
            QVERIFY(savePng(a.filePath(QString("%1.png").arg(i)), pattern(i, 128)));
            QVERIFY(savePng(b.filePath(QString("%1.png").arg(i)), pattern(i + 1, 16)));
        }
        AssetModel model;
        model.setIndex(Index::scan(a.path()), a.path());
        for (int i = 0; i < 120; ++i)
            model.data(model.indexFor(QString("%1.png").arg(i)), Qt::DecorationRole);
        QVERIFY(model.pendingCount() > 0);
        model.setIndex(Index::scan(b.path()), b.path());
        for (int i = 0; i < 120; ++i) {
            QString error;
            QVERIFY2(savePng(a.filePath(QString("%1.png").arg(i)), pattern(i + 2, 16), &error),
                     qPrintable(error));
        }
        QTRY_COMPARE_WITH_TIMEOUT(model.pendingCount(), 0, 10000);
        // Icons now cached come from project B's sources, never project A's late results.
        auto corner = [&] {
            return model.data(model.indexFor("5.png"), Qt::DecorationRole)
                .value<QIcon>()
                .pixmap(64, 64)
                .toImage()
                .pixelColor(0, 0);
        };
        QTRY_COMPARE_WITH_TIMEOUT(corner(), QColor(6, 6, 6, 255), 10000);
        QVERIFY(model.cachedBytes() <= 4 * 1024 * 1024);
    }
};
QTEST_MAIN(Phase4fFilesystemTests)
#include "phase4f_filesystem_test.moc"
