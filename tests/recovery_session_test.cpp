// SPDX-License-Identifier: GPL-3.0-or-later
// Recovery prompt: a window offers only snapshots that an earlier session left behind, never the ones it
// wrote itself after start-up, and discarding from the prompt never deletes this session's own recovery
// copy. Synthetic packs and an isolated settings folder only.
#include "window.h"
#include "recovery.h"
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>

namespace nova {
class EditorTestAccess {
  public:
    static void open(Window &window, const QString &file) { window.openTexture(file); }
    static void checkpoint(Window &window) { window.backgroundSafety(); }
    static bool checkpointBusy(Window &window) { return window.ioBusy; }
    // Wait for worker completion without delivering the GUI thread's recovery timer.
    static bool finishCheckpoint(Window &window) { return window.ioPool.waitForDone(5000); }
    static std::shared_ptr<Document> active(Window &window) {
        return window.current() ? window.current()->document : nullptr;
    }
};
} // namespace nova

namespace {
const QString plain = "assets/nova/textures/block/plain.png";
QImage png(QColor color) {
    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(color);
    return image;
}
void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}
} // namespace

class RecoverySessionTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString root, folder;
    // Every recovery prompt that appears is recorded and answered "Discard snapshot".
    QTimer answer;
    QStringList offered;

    QString snapshot(QUuid id) const { return folder + '/' + id.toString(QUuid::WithoutBraces) + ".nvr"; }
    // A snapshot left by an earlier session of this project, an hour old.
    void earlierSnapshot(const QString &relative, QUuid *id) {
        *id = QUuid::createUuid();
        QString error;
        QVERIFY2(nova::RecoveryStore(folder).write(*id, root + '/' + relative, root, {}, png(Qt::blue), &error),
                 qPrintable(error));
        QFile file(snapshot(*id));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
    }

  private slots:
    void initTestCase() {
        QVERIFY(dir.isValid());
        answer.setInterval(10);
        connect(&answer, &QTimer::timeout, this, [this] {
            auto modal = QApplication::activeModalWidget();
            if (!modal)
                return;
            auto box = qobject_cast<QMessageBox *>(modal);
            offered << (box ? box->text() : modal->objectName() + ": " + modal->windowTitle());
            if (box && box->objectName() == "recoveryDialog")
                for (auto button : box->buttons())
                    if (button->text() == "Discard snapshot")
                        return button->click();
            // Unexpected dialogs belong to the display-safety observer; never dismiss them here.
        });
    }
    void init() {
        const QString tag = QTest::currentDataTag();
        const auto base = dir.path() + '/' + QTest::currentTestFunction() +
                          (tag.isEmpty() ? QString() : '/' + tag);
        root = base + "/Synthetic Pack";
        folder = base + "/settings/recovery";
        QVERIFY(QDir().mkpath(root + "/assets/nova/textures/block"));
        QFile meta(root + "/pack.mcmeta");
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write("{\"pack\":{\"min_format\":[75,0],\"max_format\":[75,0],\"description\":\"Recovery\"}}");
        meta.close();
        QVERIFY(png(Qt::gray).save(root + '/' + plain));
        qApp->setProperty("settingsFile", base + "/settings/studio.ini");
        offered.clear();
        answer.start();
    }
    void cleanup() { answer.stop(); }

    void ownSnapshotIsNeverOffered_data() {
        QTest::addColumn<int>("setupDelayMs");
        QTest::newRow("immediate") << 0;
        QTest::newRow("overdue-startup-timer") << 900;
    }

    // Complete a real checkpoint before allowing GUI events to deliver startup discovery. A snapshot
    // this session writes before that check (an edit in its first moments) is its own live recovery
    // copy: it must not be offered, and "Discard snapshot" must never delete it. The earlier session's
    // snapshot is offered (and discarded) as before.
    void ownSnapshotIsNeverOffered() {
        QFETCH(int, setupDelayMs);
        QUuid earlier;
        earlierSnapshot("assets/nova/textures/block/older.png", &earlier);
        if (QTest::currentTestFailed())
            return;
        QElapsedTimer opened;
        opened.start();
        nova::Window window(root, {}, opened);
        // Even an overdue timer cannot run until this thread dispatches events. Do not show the
        // window or pump events until the checkpoint file exists; constructor speed is irrelevant.
        QTest::qSleep(setupDelayMs);
        nova::EditorTestAccess::open(window, root + '/' + plain);
        auto doc = nova::EditorTestAccess::active(window);
        QVERIFY(doc);
        QVERIFY(window.documents().paint(doc, doc->activeLayer(), {1, 1}, {1, 1}, QColor(Qt::red).rgba(), true));
        nova::EditorTestAccess::checkpoint(window);
        QVERIFY(nova::EditorTestAccess::checkpointBusy(window));
        QVERIFY2(nova::EditorTestAccess::finishCheckpoint(window), "checkpoint worker did not finish");
        const auto own = snapshot(doc->id());
        QVERIFY(QFileInfo::exists(own));
        QVERIFY2(offered.isEmpty(), "recovery was delivered before the checkpoint precondition");

        window.show();
        QTRY_VERIFY(!nova::EditorTestAccess::checkpointBusy(window));

        const auto older = "Unsaved texture recovered: " + root + "/assets/nova/textures/block/older.png";
        QTRY_VERIFY2_WITH_TIMEOUT(offered.contains(older), qPrintable(offered.join(" | ")), 10000);
        QTest::qWait(1000); // any further prompt would appear by now
        QVERIFY2(QFileInfo::exists(own), qPrintable("this session's recovery copy was deleted; offered: " + offered.join(" | ")));
        QCOMPARE(offered, QStringList{older});
        QVERIFY(!QFileInfo::exists(snapshot(earlier)));
    }

    // The store offers what was on disk when it started (readable or not), not what appears later.
    void storeOffersOnlyStartupSnapshots() {
        QVERIFY(QDir().mkpath(folder));
        const auto earlier = QUuid::createUuid(), broken = QUuid::createUuid(), mine = QUuid::createUuid(),
                   later = QUuid::createUuid(), brokenLater = QUuid::createUuid();
        QVERIFY(nova::RecoveryStore(folder).write(earlier, root + '/' + plain, root, {}, png(Qt::blue)));
        writeFile(snapshot(broken), "not a snapshot");
        nova::RecoveryStore store(folder); // this session starts
        QVERIFY(store.write(mine, root + '/' + plain, root, {}, png(Qt::red)));
        QVERIFY(nova::RecoveryStore(folder).write(later, root + '/' + plain, root, {}, png(Qt::green)));
        writeFile(snapshot(brokenLater), "not a snapshot");
        QSet<QUuid> all, startup;
        for (const auto &entry : store.entries())
            all << entry.id;
        for (const auto &entry : store.earlierEntries())
            startup << entry.id;
        QCOMPARE(all, (QSet<QUuid>{earlier, broken, mine, later, brokenLater}));
        QCOMPARE(startup, (QSet<QUuid>{earlier, broken}));
    }

    // Discarding from the prompt deletes only an earlier session's snapshot: never one this session
    // wrote, nor one it restored (from then on that file is this session's recovery copy).
    void promptDiscardKeepsThisSessionsCopies() {
        QVERIFY(QDir().mkpath(folder));
        const auto earlier = QUuid::createUuid(), restored = QUuid::createUuid(), mine = QUuid::createUuid();
        for (const auto id : {earlier, restored})
            QVERIFY(nova::RecoveryStore(folder).write(id, root + '/' + plain, root, {}, png(Qt::blue)));
        nova::RecoveryStore store(folder);
        QVERIFY(store.write(mine, root + '/' + plain, root, {}, png(Qt::red)));
        QVERIFY(!store.discardEarlier(mine));
        QVERIFY(QFileInfo::exists(snapshot(mine)));
        store.restored(restored);
        QVERIFY(!store.discardEarlier(restored));
        QVERIFY(store.write(restored, root + '/' + plain, root, {}, png(Qt::green)));
        QVERIFY(!store.discardEarlier(restored));
        QVERIFY(QFileInfo::exists(snapshot(restored)));
        QVERIFY(store.discardEarlier(earlier));
        QVERIFY(!QFileInfo::exists(snapshot(earlier)));
        QVERIFY(!store.discardEarlier(earlier));
        QVERIFY(store.earlierEntries().isEmpty());
    }
};

QTEST_MAIN(RecoverySessionTest)
#include "recovery_session_test.moc"
