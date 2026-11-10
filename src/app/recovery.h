// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include <QDateTime>
#include <memory>
namespace nova {
struct RecoveryEntry {
    QString file, path, project, error;
    QUuid id;
    Revision original;
    QImage image;
    QByteArray nativeData;
    QString nativeFile;
    Revision nativeRevision;
    QDateTime timestamp;
};
class RecoveryStore {
    struct Lifecycle;
    QString directory;
    std::shared_ptr<Lifecycle> lifecycle;

    void pruneLocked() const;

  public:
    explicit RecoveryStore(QString root);
    QString root() const { return directory; }
    QString pathFor(QUuid id) const;
    bool write(QUuid id, const QString &path, const QString &project, const Revision &original,
               const QImage &image, QString *error = nullptr) const;
    bool writeDocument(const Document &document, const QString &project,
                       QString *error = nullptr) const;
    QList<RecoveryEntry> entries() const;
    // Snapshots an earlier session left: on disk when this store started and not yet restored or
    // discarded. Snapshots written after start-up (this session's own) are never included.
    QList<RecoveryEntry> earlierEntries() const;
    RecoveryEntry load(QUuid id) const;
    void remove(QUuid id) const;
    void cleanupClean(QUuid id) const;
    void restored(QUuid id) const;
    void discard(QUuid id) const;
    // Deletes one of earlierEntries(); refuses any other snapshot, so this session's own recovery
    // copies cannot be discarded from the recovery prompt.
    bool discardEarlier(QUuid id) const;
    void prune() const;
};
} // namespace nova
