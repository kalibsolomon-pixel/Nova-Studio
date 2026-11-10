// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include <QtConcurrent>
#include <QtWidgets>
namespace nova {
void Window::conflictDialog() {
    if (!current())
        return;
    auto doc = current()->document;
    auto disk = Revision::read(doc->path);
    QMessageBox box(this);
    box.setWindowTitle("External File Conflict");
    box.setObjectName("externalConflictDialog");
    box.setText("Nova Studio's " + QString(doc->modified() ? "unsaved" : "in-memory") +
                " version and the external disk version:\n" + doc->path);
    box.setInformativeText(QString("Last saved: %1 bytes, SHA-256 %2\nDisk now: %3, SHA-256 "
                                   "%4\nKeep preserves your pixels; Reload discards local edits.")
                               .arg(doc->revision().size)
                               .arg(QString::fromLatin1(doc->revision().hash.toHex().left(16)))
                               .arg(disk.exists ? QString::number(disk.size) + " bytes" : "missing")
                               .arg(QString::fromLatin1(disk.hash.toHex().left(16))));
    auto keep = box.addButton("Keep current", QMessageBox::RejectRole);
    auto reload = box.addButton("Reload disk…", QMessageBox::ActionRole);
    auto saveAs = box.addButton("Save local as…", QMessageBox::ActionRole);
    auto overwrite = box.addButton("Overwrite disk…", QMessageBox::DestructiveRole);
    box.setDefaultButton(keep);
    box.exec();
    QString error;
    if (box.clickedButton() == saveAs)
        action("saveAs");
    else if (box.clickedButton() == reload) {
        if (doc->modified() &&
            QMessageBox::question(
                this, "Discard unsaved pixels?",
                "Reload the external PNG and discard Nova Studio's unsaved version?",
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        if (!doc->reload(&error))
            QMessageBox::warning(this, "Reload failed", error);
        else
            recovery->remove(doc->id());
    } else if (box.clickedButton() == overwrite) {
        if (QMessageBox::question(this, "Overwrite external disk version?",
                                  "Replace the external version with Nova Studio's current pixels? "
                                  "The destination will be checked again before replacement.",
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        if (!doc->saveOver(disk, &error))
            QMessageBox::warning(this, "Save blocked", error);
        else
            recovery->remove(doc->id());
    }
    refresh();
}
void Window::backgroundSafety() {
    if (ioBusy || qaMode || exercise)
        return;
    struct Item {
        QUuid id;
        QString path;
        Revision saved;
        std::shared_ptr<Document> snapshotDocument;
        qint64 key;
        bool snapshot;
    };
    QSet<QUuid> live;
    for (auto &doc : session.all())
        live.insert(doc->id());
    for (auto id : recoveredKeys.keys())
        if (!live.contains(id))
            recoveredKeys.remove(id);
    for (auto id : externalRevisions.keys())
        if (!live.contains(id))
            externalRevisions.remove(id);
    QList<Item> items;
    for (const auto &doc : session.all()) {
        bool snapshot =
            doc->modified() && recoveredKeys.value(doc->id()) != qint64(doc->changeSerial());
        items.append({doc->id(), doc->path, doc->revision(),
                      snapshot ? doc->recoverySnapshot() : nullptr, qint64(doc->changeSerial()),
                      snapshot});
        if (!doc->modified())
            recovery->cleanupClean(doc->id());
    }
    if (items.isEmpty())
        return;
    ioBusy = true;
    auto store = *recovery;
    auto root = project;
    using Results = QList<std::tuple<QUuid, Revision, qint64, QString>>;
    auto watcher = new QFutureWatcher<Results>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        ioBusy = false;
        auto results = watcher->result();
        watcher->deleteLater();
        for (auto [id, disk, key, error] : results) {
            std::shared_ptr<Document> doc;
            for (auto &d : session.all())
                if (d->id() == id)
                    doc = d;
            if (!doc)
                continue;
            if (key && error.isEmpty())
                recoveredKeys[id] = key;
            if (!error.isEmpty()) {
                qWarning().noquote() << "Recovery snapshot failed:" << doc->path << error;
                statusBar()->showMessage("Recovery snapshot failed: " + error);
            }
            if (!doc->revision().sameContent(disk)) {
                externalRevisions[id] = disk;
                statusBar()->showMessage(
                    "External file change detected · File > Review External Change");
            } else
                externalRevisions.remove(id);
        }
    });
    watcher->setFuture(QtConcurrent::run(&ioPool, [items, store, root] {
        Results results;
        for (const auto &item : items) {
            auto disk = Revision::read(item.path);
            QString error;
            const bool written = item.snapshot && store.writeDocument(*item.snapshotDocument, root, &error);
            results.append({item.id, disk, written ? item.key : 0, error});
        }
        return results;
    }));
}
void Window::recoverSession() {
    auto store = *recovery;
    auto watcher = new QFutureWatcher<QList<RecoveryEntry>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        auto entries = watcher->result();
        watcher->deleteLater();
        for (auto entry : entries) {
            // Offer only this project's snapshots. Others stay protected on disk (startup
            // claims them, so they are never pruned unseen) until their project is opened.
            // Unreadable snapshots have no trustworthy project and are always offered.
            if (!project.isEmpty() && entry.error.isEmpty() && pathIdentity(entry.project) != pathIdentity(project))
                continue;
            QMessageBox box(this);
            box.setWindowTitle("Recover Unsaved Session");
            box.setObjectName("recoveryDialog");
            box.setText(entry.error.isEmpty() ? "Unsaved texture recovered: " + entry.path
                                              : "Unreadable recovery snapshot: " + entry.error);
            box.setInformativeText(
                "Original PNG files are never replaced by recovery.\nSnapshot: " +
                entry.timestamp.toLocalTime().toString(Qt::ISODate));
            auto restore = box.addButton("Restore", QMessageBox::AcceptRole);
            restore->setEnabled(entry.error.isEmpty());
            auto inspect = box.addButton("Inspect", QMessageBox::ActionRole);
            auto discard = box.addButton("Discard snapshot", QMessageBox::DestructiveRole);
            box.setEscapeButton(box.addButton("Later", QMessageBox::RejectRole));
            box.exec();
            if (box.clickedButton() == inspect || box.clickedButton() == restore) {
                QFutureWatcher<RecoveryEntry> loader;
                QEventLoop loop;
                QProgressDialog progress("Loading recovery image…", QString(), 0, 0, this);
                progress.setWindowModality(Qt::WindowModal);
                progress.setMinimumDuration(250);
                connect(&loader, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
                auto store = *recovery;
                auto id = entry.id;
                loader.setFuture(
                    QtConcurrent::run(&ioPool, [store, id] { return store.load(id); }));
                if (!loader.isFinished())
                    loop.exec();
                progress.close();
                entry = loader.result();
                if (!entry.error.isEmpty()) {
                    QMessageBox::warning(this, "Recovery unavailable", entry.error);
                    continue;
                }
            }
            if (box.clickedButton() == inspect) {
                QDialog view(this);
                view.setWindowTitle("Recovery Snapshot");
                QVBoxLayout layout(&view);
                QLabel details(entry.error.isEmpty() ? entry.path + "\nProject: " + entry.project +
                                                           "\nDisk changes will be checked on save."
                                                     : entry.error);
                details.setWordWrap(true);
                layout.addWidget(&details);
                QLabel preview;
                preview.setPixmap(
                    QPixmap::fromImage(entry.image)
                        .scaled(320, 320, Qt::KeepAspectRatio, Qt::FastTransformation));
                layout.addWidget(&preview);
                QDialogButtonBox done(QDialogButtonBox::Close);
                layout.addWidget(&done);
                connect(&done, &QDialogButtonBox::rejected, &view, &QDialog::reject);
                view.exec();
            } else if (box.clickedButton() == discard) {
                if (!recovery->discardEarlier(entry.id))
                    statusBar()->showMessage("Recovery snapshot kept: this session is using it");
            }
            else if (box.clickedButton() == restore) {
                std::shared_ptr<Document> doc;
                for (auto &d : session.all())
                    if (pathIdentity(d->path) == pathIdentity(entry.path))
                        doc = d;
                if (doc && doc->modified()) {
                    QMessageBox::warning(
                        this, "Recovery deferred",
                        "Close the already modified texture before restoring this snapshot");
                    continue;
                }
                auto restored = std::make_shared<Document>(entry.path, entry.image);
                if (!entry.nativeData.isEmpty()) {
                    if (!restored->decodeNative(entry.nativeData, "recovery.png", &entry.error)) {
                        QMessageBox::warning(this, "Recovery unavailable", entry.error);
                        continue;
                    }
                    restored->restoreEditableBinding(entry.nativeFile, entry.nativeRevision);
                } else
                    restored->restorePixels(entry.image, entry.original);
                if (!doc) {
                    doc = restored;
                    if (!session.adopt(doc)) {
                        QMessageBox::warning(this, "Recovery deferred",
                                             "Close another texture to restore this snapshot");
                        continue;
                    }
                    // An edited vanilla texture that was never saved stays baseline-backed (create-only).
                    if (!doc->revision().exists && baselineCandidate(doc->path))
                        baselineDocs.insert(doc->id());
                    if (!doc->revision().exists)
                        model->applyIndex(browserIndex(), {QDir(project).relativeFilePath(doc->path)});
                    showTextureDocument(doc);
                }
                if (!session.canRestoreDocument(doc, *restored)) {
                    QMessageBox::warning(
                        this, "Recovery deferred",
                        "Close another texture before restoring this image (512 MiB pixel budget)");
                    continue;
                }
                if (doc != restored) {
                    if (!entry.nativeData.isEmpty()) {
                        if (!doc->decodeNative(entry.nativeData, "recovery.png", &entry.error))
                            continue;
                        doc->restoreEditableBinding(entry.nativeFile, entry.nativeRevision);
                    } else
                        doc->restorePixels(entry.image, entry.original);
                }
                const bool fromBaseline = baselineDocs.remove(doc->id());
                doc->setRecoveryIdentity(entry.id);
                if (fromBaseline)
                    baselineDocs.insert(doc->id());
                recovery->restored(entry.id);
                doc->setHistoryLimits(qint64(preferences.historyMiB) * 1024 * 1024, 2048);
                refresh();
            }
        }
    });
    // Only an earlier session's snapshots: one this session wrote before the check (an edit in its
    // first moments) is its own live recovery copy, not a session to recover.
    watcher->setFuture(QtConcurrent::run(&ioPool, [store] {
        store.prune();
        return store.earlierEntries();
    }));
}
} // namespace nova
