// SPDX-License-Identifier: GPL-3.0-or-later
#include "baseline_reset.h"
#include "animation_export.h"
#include "core.h"
#include "project_config.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
namespace nova {
namespace {
constexpr qint64 JournalLimit = 4 * 1024 * 1024, FileLimit = 256LL * 1024 * 1024;
constexpr int AssetLimit = 16384;
bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
Revision digest(const QByteArray &bytes) {
    return {true, true, bytes.size(), 0, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
}
bool occupied(const QString &path) {
    const QFileInfo f(path);
    return f.exists() || f.isSymLink();
}
bool regular(const QString &path) { return QFileInfo(path).isFile() && !reparsePoint(path); }
bool validAssetPath(const QString &asset) {
    if (!validProjectPath(asset) || !asset.startsWith("assets/") ||
        !asset.endsWith(".png", Qt::CaseInsensitive) || asset.count('/') < 2)
        return false;
    for (const auto &part : asset.split('/'))
        if (part.compare(".nova", Qt::CaseInsensitive) == 0)
            return false;
    return true;
}
// Existing ancestors must be real folders; the file itself must not be a link.
bool safeAncestors(const QString &base, const QString &relative) {
    auto path = base;
    const auto parts = relative.split('/');
    for (int i = 0; i + 1 < parts.size(); ++i) {
        path += '/' + parts[i];
        if (reparsePoint(path) || (occupied(path) && !QFileInfo(path).isDir()))
            return false;
    }
    return !reparsePoint(base + '/' + relative);
}
// The project files that make up one texture: PNG, .png.mcmeta and the editable sidecar.
QStringList candidates(const QString &base, const QString &asset) {
    QStringList result{asset, asset + ".mcmeta"};
    const auto sidecar = nativeDocumentPath(base, base + '/' + asset);
    if (!sidecar.isEmpty())
        result << QDir(base).relativeFilePath(sidecar);
    return result;
}
bool moveNoReplace(const QString &from, const QString &to, QString *error) {
#ifdef Q_OS_WIN
    // Same volume (both inside the project), never replacing an existing destination.
    const auto a = QDir::toNativeSeparators(from), b = QDir::toNativeSeparators(to);
    if (MoveFileExW(reinterpret_cast<const wchar_t *>(a.utf16()),
                    reinterpret_cast<const wchar_t *>(b.utf16()), MOVEFILE_WRITE_THROUGH))
        return true;
    return fail(error, QString("Could not move %1 (Windows error %2)").arg(a).arg(GetLastError()));
#else
    if (occupied(to) || !QFile::rename(from, to))
        return fail(error, "Could not move " + from);
    return true;
#endif
}
// An incomplete paired animation export beside the texture blocks any reset there.
bool pendingPairs(const QString &folder) {
    const auto transactions = folder + "/.nova/animation-exports";
    if (reparsePoint(folder + "/.nova") || reparsePoint(transactions))
        return true;
    for (const auto &dir : QDir(transactions).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot |
                                                            QDir::Hidden | QDir::System)) {
        if (reparsePoint(dir.absoluteFilePath()))
            return true;
        const auto journal = dir.absoluteFilePath() + "/journal.json";
        if (!QFileInfo::exists(journal))
            continue;
        const auto status = inspectAnimationPair(journal);
        if (!status.valid || !status.complete)
            return true;
    }
    return false;
}
bool matches(const QString &file, const QJsonObject &f) {
    const auto r = Revision::read(file, FileLimit);
    return r.exists && r.readable && r.size == f["size"].toInteger(-1) &&
           QString::fromLatin1(r.hash.toHex()) == f["sha256"].toString();
}
// Records what is actually there now (a concurrent external change is kept, not discarded).
void observe(QJsonArray &files, int i, const QString &file) {
    auto f = files[i].toObject();
    const auto r = Revision::read(file, FileLimit);
    if (r.exists && r.readable) {
        f["sha256"] = QString::fromLatin1(r.hash.toHex());
        f["size"] = double(r.size);
        files[i] = f;
    }
}
struct Tx {
    QString base, folder, file;
    QJsonObject json;
    Revision revision; // Revision{} until journal.json exists: the first write is create-only
    QString state() const { return json["state"].toString(); }
    QStringList assets() const { return json["assets"].toVariant().toStringList(); }
};
bool record(Tx &tx, const QString &state, const QJsonArray &files, QString *error) {
    auto json = tx.json;
    json["state"] = state;
    json["updated"] = now();
    json["files"] = files;
    const auto bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);
    if (!atomicWrite(tx.file, bytes, tx.revision, error))
        return false;
    tx.json = json;
    tx.revision = digest(bytes);
    return true;
}
bool load(const QString &base, const QString &id, Tx *tx, QString *error) {
    const QUuid uuid(id);
    if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != id)
        return fail(error, "Invalid reset journal id");
    tx->base = base;
    tx->folder = resetFolder(base) + '/' + id;
    tx->file = tx->folder + "/journal.json";
    if (reparsePoint(base + "/.nova") || reparsePoint(resetFolder(base)) ||
        reparsePoint(tx->folder) || !regular(tx->file))
        return fail(error, "Reset journal is missing or linked: " + id);
    QString issue;
    const auto bytes = readFileBytes(tx->file, JournalLimit, &issue);
    if (!issue.isEmpty())
        return fail(error, issue);
    auto invalid = [&](const QString &why) {
        return fail(error, "Invalid reset journal " + id + ": " + why);
    };
    QJsonParseError parse;
    const auto o = QJsonDocument::fromJson(bytes, &parse).object();
    static const QStringList states{"prepared", "applied", "undoing", "undone", "redoing", "rolled_back"};
    if (parse.error != QJsonParseError::NoError || o["schema"].toInt() != 1 || o["id"].toString() != id)
        return invalid("unknown schema or id");
    if (!states.contains(o["state"].toString()) ||
        (o["kind"].toString() != "reset" && o["kind"].toString() != "remove"))
        return invalid("unknown state or kind");
    // Only the files of the journal's own textures may ever be moved by it.
    QSet<QString> allowed, seen, retained;
    const auto assets = o["assets"].toArray();
    if (assets.isEmpty() || assets.size() > AssetLimit)
        return invalid("asset list");
    for (const auto &value : assets) {
        if (!validAssetPath(value.toString()))
            return invalid("asset path");
        for (const auto &c : candidates(base, value.toString()))
            allowed.insert(c.toCaseFolded());
    }
    static const QRegularExpression name("^[0-9]{1,6}\\.retained$"), sha("^[0-9a-f]{64}$");
    const auto files = o["files"].toArray();
    if (files.isEmpty())
        return invalid("file list");
    for (const auto &value : files) {
        const auto f = value.toObject();
        const auto path = f["path"].toString().toCaseFolded(), keep = f["retained"].toString();
        if (!allowed.contains(path) || seen.contains(path) || !name.match(keep).hasMatch() ||
            retained.contains(keep) || !sha.match(f["sha256"].toString()).hasMatch() ||
            f["size"].toInteger(-1) < 0)
            return invalid("file record");
        seen.insert(path);
        retained.insert(keep);
    }
    tx->json = o;
    tx->revision = digest(bytes);
    return true;
}
bool ensureParent(const QString &base, const QString &relative, QString *error) {
    return relative.startsWith(".nova/") ? ensureProjectDirectory(base, relative, error)
                                         : ensureAssetDirectory(base, relative, error);
}
// Project -> journal for every file not retained yet. requirePresent: a vanished file fails.
bool moveIn(Tx &tx, QJsonArray &files, bool requirePresent, QString *error) {
    for (int i = 0; i < files.size(); ++i) {
        const auto f = files[i].toObject();
        const auto path = f["path"].toString();
        const auto source = tx.base + '/' + path, keep = tx.folder + '/' + f["retained"].toString();
        if (occupied(keep))
            continue;
        if (!occupied(source)) {
            if (requirePresent)
                return fail(error, path + " disappeared while resetting");
            continue;
        }
        if (!regular(source) || !safeAncestors(tx.base, path))
            return fail(error, "Not a regular project file: " + path);
        if (!moveNoReplace(source, keep, error))
            return false;
        observe(files, i, keep);
    }
    return true;
}
// Journal -> project for every retained file; never replaces anything at the destination.
bool moveOut(Tx &tx, QJsonArray &files, QString *error) {
    for (int i = 0; i < files.size(); ++i) {
        const auto f = files[i].toObject();
        const auto path = f["path"].toString();
        const auto target = tx.base + '/' + path, keep = tx.folder + '/' + f["retained"].toString();
        if (!occupied(keep))
            continue;
        if (occupied(target))
            return fail(error, "Restoring would overwrite " + path + ", which now exists in the project");
        if (!regular(keep) || !ensureParent(tx.base, path, error) || !moveNoReplace(keep, target, error))
            return false;
        observe(files, i, target);
    }
    return true;
}
// Non-recursive rmdir of emptied folders, up to (not including) assets/<namespace>.
void pruneFolders(const QString &base, const QStringList &assets) {
    for (const auto &asset : assets)
        for (auto relative = QFileInfo(asset).path(); relative.count('/') >= 2;
             relative = QFileInfo(relative).path()) {
            const auto path = base + '/' + relative;
            if (reparsePoint(path) || !QFileInfo(path).isDir() || !QDir().rmdir(path))
                break;
        }
}
bool noInterrupted(const QString &base, const QString &except, QString *error) {
    for (const auto &j : resetJournals(base))
        if (j.id != except && j.interrupted())
            return fail(error, "Recover the interrupted vanilla reset " + j.id + " first");
    return true;
}
bool canonical(const QString &root, QString *base, QString *error) {
    *base = QFileInfo(root).canonicalFilePath();
    return !base->isEmpty() || fail(error, "The project folder is missing or inaccessible");
}
// Serialises reset mutations across Nova processes sharing the project.
struct Guard {
    std::unique_ptr<QLockFile> lock;
    bool acquire(const QString &base, QString *error) {
        if (!ensureProjectDirectory(base, ".nova/baseline-resets/.lock", error))
            return false;
        lock = std::make_unique<QLockFile>(resetFolder(base) + "/.lock");
        return lock->tryLock(5000) || fail(error, "Another Nova process is changing vanilla resets");
    }
};
} // namespace
QString resetFolder(const QString &root) { return root + "/.nova/baseline-resets"; }
bool applyReset(const QString &root, const QStringList &requested, ResetKind kind,
                const BaselineCheck &baselineAvailable, QString *journalId, QString *error,
                const std::function<void()> &beforeMoving) {
    QString base;
    if (!canonical(root, &base, error))
        return false;
    QStringList assets;
    QSet<QString> seen;
    for (auto asset : requested) {
        if (!validAssetPath(asset))
            return fail(error, "Not a project texture path: " + asset);
        // Journal the on-disk spelling so undo restores the exact original name.
        const QFileInfo existing(base + '/' + asset);
        const auto actual = existing.exists() && !reparsePoint(existing.filePath())
                                ? existing.canonicalFilePath() : QString();
        if (actual.startsWith(base + '/', Qt::CaseInsensitive) && validAssetPath(actual.mid(base.size() + 1)))
            asset = actual.mid(base.size() + 1);
        if (!seen.contains(asset.toCaseFolded())) {
            seen.insert(asset.toCaseFolded());
            assets << asset;
        }
    }
    if (assets.isEmpty() || assets.size() > AssetLimit)
        return fail(error, "Select between 1 and 16384 textures");
    // Every baseline is verified first: one unavailable baseline changes nothing at all.
    if (kind == ResetKind::ResetToVanilla)
        for (const auto &asset : assets) {
            QString issue;
            if (!baselineAvailable || !baselineAvailable(asset, &issue))
                return fail(error, "The vanilla baseline is unavailable for " + asset +
                                       (issue.isEmpty() ? QString() : " (" + issue + ")") +
                                       "; the override was not changed");
        }
    // Preflight writes nothing (a refused reset leaves no trace); the lock is taken just before
    // the journal is created and the interrupted check repeated under it.
    if (!noInterrupted(base, {}, error))
        return false;
    QJsonArray files;
    for (const auto &asset : assets) {
        if (!safeAncestors(base, asset))
            return fail(error, "Linked folders cannot be reset safely: " + asset);
        if (pendingPairs(QFileInfo(base + '/' + asset).absolutePath()))
            return fail(error, "Recover the interrupted animation export beside " + asset +
                                   " first; nothing was changed");
        int found = 0;
        for (const auto &relative : candidates(base, asset)) {
            const auto file = base + '/' + relative;
            if (!occupied(file))
                continue;
            if (!regular(file) || !safeAncestors(base, relative))
                return fail(error, "Not a regular project file: " + relative);
            const auto r = Revision::read(file, FileLimit);
            if (!r.readable)
                return fail(error, "Cannot read " + relative);
            const auto hash = QString::fromLatin1(r.hash.toHex());
            files.append(QJsonObject{{"path", relative},
                                     {"retained", QString("%1.retained").arg(files.size())},
                                     {"sha256", hash},
                                     {"before_sha256", hash},
                                     {"size", double(r.size)}});
            ++found;
        }
        if (!found)
            return fail(error, "The project has no files for " + asset + "; nothing was changed");
    }
    Guard guard;
    if (!guard.acquire(base, error) || !noInterrupted(base, {}, error))
        return false;
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Tx tx;
    tx.base = base;
    tx.folder = resetFolder(base) + '/' + id;
    tx.file = tx.folder + "/journal.json";
    if (!ensureProjectDirectory(base, ".nova/baseline-resets/" + id + "/journal.json", error))
        return false;
    tx.json = {{"schema", 1},
               {"id", id},
               {"kind", kind == ResetKind::ResetToVanilla ? "reset" : "remove"},
               {"created", now()},
               {"assets", QJsonArray::fromStringList(assets)}};
    // The journal names every file before the first one moves.
    if (!record(tx, "prepared", files, error))
        return false;
    if (journalId)
        *journalId = id;
    if (beforeMoving)
        beforeMoving();
    QString issue;
    if (!moveIn(tx, files, true, &issue)) {
        QString rollback;
        if (moveOut(tx, files, &rollback) && record(tx, "rolled_back", files, &rollback))
            return fail(error, issue + "; nothing was changed");
        return fail(error, issue + "; " + rollback + "; the interrupted reset is kept for recovery");
    }
    pruneFolders(base, assets);
    return record(tx, "applied", files, error);
}
bool undoReset(const QString &root, const QString &id, QString *error) {
    QString base;
    Guard guard;
    Tx tx;
    if (!canonical(root, &base, error) || !guard.acquire(base, error) || !load(base, id, &tx, error))
        return false;
    if (tx.state() != "applied")
        return fail(error, "Only an applied reset can be undone (state: " + tx.state() + ")");
    if (!noInterrupted(base, id, error))
        return false;
    auto files = tx.json["files"].toArray();
    // Preflight everything; an occupied destination is never overwritten.
    for (const auto &value : files) {
        const auto f = value.toObject();
        const auto path = f["path"].toString();
        if (occupied(base + '/' + path))
            return fail(error, "Undo would overwrite " + path +
                                   ", which now exists in the project; nothing was changed");
        const auto keep = tx.folder + '/' + f["retained"].toString();
        if (!regular(keep) || !matches(keep, f))
            return fail(error, "The retained copy of " + path + " is missing or changed; nothing was changed");
        if (!safeAncestors(base, path))
            return fail(error, "Linked folders block restoring " + path);
    }
    for (const auto &value : files)
        if (!ensureParent(base, value.toObject()["path"].toString(), error))
            return false;
    if (!record(tx, "undoing", files, error))
        return false;
    QString issue;
    if (!moveOut(tx, files, &issue)) {
        QString rollback;
        if (moveIn(tx, files, false, &rollback) && record(tx, "applied", files, &rollback)) {
            pruneFolders(base, tx.assets());
            return fail(error, issue + "; the reset stays applied");
        }
        return fail(error, issue + "; " + rollback + "; the interrupted undo is kept for recovery");
    }
    return record(tx, "undone", files, error);
}
bool redoReset(const QString &root, const QString &id, QString *error,
               const BaselineCheck &baselineAvailable) {
    QString base;
    Guard guard;
    Tx tx;
    if (!canonical(root, &base, error) || !guard.acquire(base, error) || !load(base, id, &tx, error))
        return false;
    if (tx.state() != "undone")
        return fail(error, "Only an undone reset can be redone (state: " + tx.state() + ")");
    if (!noInterrupted(base, id, error))
        return false;
    const auto assets = tx.assets();
    if (tx.json["kind"].toString() == "reset" && baselineAvailable)
        for (const auto &asset : assets) {
            QString issue;
            if (!baselineAvailable(asset, &issue))
                return fail(error, "The vanilla baseline is unavailable for " + asset +
                                       "; the override was not changed");
        }
    auto files = tx.json["files"].toArray();
    QSet<QString> recorded;
    for (const auto &value : files) {
        const auto f = value.toObject();
        const auto path = f["path"].toString();
        recorded.insert(path.toCaseFolded());
        if (!regular(base + '/' + path) || !safeAncestors(base, path) || !matches(base + '/' + path, f))
            return fail(error, path + " changed since the undo; redo was refused and nothing was changed");
        if (occupied(tx.folder + '/' + f["retained"].toString()))
            return fail(error, "The reset journal already holds " + path);
    }
    for (const auto &asset : assets) {
        for (const auto &c : candidates(base, asset))
            if (!recorded.contains(c.toCaseFolded()) && occupied(base + '/' + c))
                return fail(error, "New project file " + c + " belongs to this texture; redo was refused");
        if (pendingPairs(QFileInfo(base + '/' + asset).absolutePath()))
            return fail(error, "Recover the interrupted animation export beside " + asset + " first");
    }
    if (!record(tx, "redoing", files, error))
        return false;
    QString issue;
    if (!moveIn(tx, files, true, &issue)) {
        QString rollback;
        if (moveOut(tx, files, &rollback) && record(tx, "undone", files, &rollback))
            return fail(error, issue + "; nothing was changed");
        return fail(error, issue + "; " + rollback + "; the interrupted redo is kept for recovery");
    }
    pruneFolders(base, assets);
    return record(tx, "applied", files, error);
}
QVector<ResetJournal> resetJournals(const QString &root, QString *error) {
    QVector<ResetJournal> result;
    QString base;
    if (!canonical(root, &base, error))
        return result;
    const auto folder = resetFolder(base);
    if (reparsePoint(base + "/.nova") || reparsePoint(folder)) {
        fail(error, "Linked reset storage is not read");
        return result;
    }
    for (const auto &entry : QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot |
                                                        QDir::Hidden | QDir::System, QDir::Name)) {
        const QUuid uuid(entry.fileName());
        if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != entry.fileName())
            continue;
        ResetJournal j;
        j.id = entry.fileName();
        Tx tx;
        if (!load(base, j.id, &tx, &j.error)) {
            j.state = "invalid";
            result << j;
            continue;
        }
        j.state = tx.state();
        j.kind = tx.json["kind"].toString() == "remove" ? ResetKind::RemoveFromProject
                                                         : ResetKind::ResetToVanilla;
        j.assets = tx.assets();
        j.files = tx.json["files"].toArray();
        j.created = QDateTime::fromString(tx.json["created"].toString(), Qt::ISODateWithMs);
        j.updated = QDateTime::fromString(tx.json["updated"].toString(), Qt::ISODateWithMs);
        result << j;
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const ResetJournal &a, const ResetJournal &b) { return a.created > b.created; });
    return result;
}
bool recoverReset(const QString &root, const QString &id, QString *error) {
    QString base;
    Guard guard;
    Tx tx;
    if (!canonical(root, &base, error) || !guard.acquire(base, error) || !load(base, id, &tx, error))
        return false;
    const auto state = tx.state();
    if (state != "prepared" && state != "undoing" && state != "redoing")
        return true;
    auto files = tx.json["files"].toArray();
    if (state == "undoing") {
        // Restored files go back to their (vacant) journal slots: the reset stays applied.
        if (!moveIn(tx, files, false, error))
            return false;
        pruneFolders(base, tx.assets());
        return record(tx, "applied", files, error);
    }
    // Moving into the journal was interrupted: roll back unless a file appeared at a destination.
    bool conflict = false;
    for (const auto &value : files) {
        const auto f = value.toObject();
        conflict = conflict || (occupied(tx.folder + '/' + f["retained"].toString()) &&
                                occupied(base + '/' + f["path"].toString()));
    }
    if (!conflict) {
        if (!moveOut(tx, files, error))
            return false;
        return record(tx, state == "prepared" ? "rolled_back" : "undone", files, error);
    }
    if (!moveIn(tx, files, false, error))
        return false;
    pruneFolders(base, tx.assets());
    return record(tx, "applied", files, error);
}
void pruneResets(const QString &root, int keep, int maxAgeDays) {
    const auto base = QFileInfo(root).canonicalFilePath();
    const auto folder = resetFolder(base);
    if (base.isEmpty() || !QFileInfo(folder).isDir() || reparsePoint(base + "/.nova") ||
        reparsePoint(folder))
        return;
    QLockFile lock(folder + "/.lock");
    if (!lock.tryLock(2000))
        return;
    const auto journals = resetJournals(base);
    const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-std::max(0, maxAgeDays));
    for (int i = 0; i < journals.size(); ++i) {
        const auto &j = journals[i];
        if (j.state != "applied" && j.state != "undone" && j.state != "rolled_back")
            continue;
        const auto stamp = j.updated.isValid() ? j.updated : j.created;
        if (i < std::max(0, keep) && (!stamp.isValid() || stamp >= cutoff))
            continue;
        const auto dir = folder + '/' + j.id;
        if (reparsePoint(dir))
            continue;
        const auto entries = QDir(dir).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                                     QDir::NoDotAndDotDot);
        bool plain = true;
        for (const auto &e : entries)
            plain = plain && e.isFile() && !reparsePoint(e.absoluteFilePath());
        if (!plain)
            continue;
        // journal.json goes last: an interrupted prune stays a prunable (stable) journal.
        for (const auto &e : entries)
            if (e.fileName() != "journal.json")
                QFile::remove(e.absoluteFilePath());
        QFile::remove(dir + "/journal.json");
        QDir().rmdir(dir);
    }
}
} // namespace nova
