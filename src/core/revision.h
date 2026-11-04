// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QString>
#include <functional>
namespace nova {
QString pathIdentity(const QString &path);
struct Revision {
    bool exists = false, readable = true;
    qint64 size = 0, modified = 0;
    QByteArray hash;
    static Revision read(const QString &path, qint64 byteLimit = 128LL * 1024 * 1024);
    bool sameContent(const Revision &other) const;
};
// Writes to a temporary file beside `path`, then publishes it with one rename.
// The writer and beforeCommit callbacks run at most once. Only the final rename is
// retried, and only while Windows reports that another handle shares the file.
bool atomicWrite(const QString &path, const QByteArray &bytes, const Revision &expected,
                 QString *error = nullptr, const std::function<void()> &beforeCommit = {});
bool atomicWriteStream(const QString &path, const Revision &expected,
                       const std::function<bool(QIODevice &)> &writer, QString *error = nullptr,
                       const std::function<void()> &beforeCommit = {});
// Same publication without a revision check, for files with no prior baseline
// (staging folders, exported reports). Existing files are still replaced atomically.
bool atomicReplace(const QString &path, const std::function<bool(QIODevice &)> &writer,
                   QString *error = nullptr);

// What the last atomic write on the calling thread did. Used by tests and diagnostics.
struct WriteDiagnostics {
    int publishAttempts = 0;      // rename attempts, including the first
    int sharingRetries = 0;       // attempts repeated because another handle held a file
    unsigned long systemError = 0; // last Win32 error from the rename, 0 on success
    bool retriesExhausted = false;
    bool temporaryRemoved = true; // false only if a failed write could not delete its temp file
    bool posixRename = false;      // published with FileRenameInfoEx POSIX semantics
};
WriteDiagnostics lastWriteDiagnostics();

// Opens for reading while sharing read, write and delete, so Nova's own readers never
// block a replacing save. `file` must be a default-constructed QFile.
bool openSharedRead(const QString &path, QFile &file);
// Reads a whole file into memory and closes it before returning, so callers that decode
// afterwards never hold the source open. Fails above `byteLimit`.
QByteArray readFileBytes(const QString &path, qint64 byteLimit, QString *error = nullptr);
} // namespace nova
