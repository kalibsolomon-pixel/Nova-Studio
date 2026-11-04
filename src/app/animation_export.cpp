// SPDX-License-Identifier: GPL-3.0-or-later
#include "animation_export.h"
#include "minecraft_animation.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QRegularExpression>
namespace nova {
namespace {
constexpr qint64 PngLimit = 128LL * 1024 * 1024, MetadataLimit = 512 * 1024;
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
Revision digest(const QByteArray &bytes) { return {true, true, bytes.size(), 0, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)}; }
QJsonObject encodeRevision(const Revision &r) {
    return {{"exists", r.exists}, {"size", double(r.size)}, {"sha256", QString::fromLatin1(r.hash.toHex())}};
}
Revision decodeRevision(const QJsonObject &o) {
    return {o["exists"].toBool(), true, o["size"].toInteger(-1), 0, QByteArray::fromHex(o["sha256"].toString().toLatin1())};
}
bool safePath(const QString &path) {
    auto info = QFileInfo(path);
    if (info.fileName().isEmpty() || info.fileName().endsWith('.') || info.fileName().endsWith(' ') ||
        info.fileName().contains(':')) return false;
    for (;;) {
        if (info.isSymLink() || info.isJunction()) return false;
        auto parent = QFileInfo(info.absolutePath());
        if (parent.absoluteFilePath() == info.absoluteFilePath()) break;
        info = parent;
    }
    return true;
}
bool textureFileName(const QString &name) {
    static const QRegularExpression invalid(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])"));
    static const QRegularExpression device(
        QStringLiteral(R"(^(CON|PRN|AUX|NUL|COM[0-9\x{00b9}\x{00b2}\x{00b3}]|LPT[0-9\x{00b9}\x{00b2}\x{00b3}])(?:\.|$))"),
        QRegularExpression::CaseInsensitiveOption);
    return name.size() > 4 && name.endsWith(".png", Qt::CaseInsensitive) &&
           !invalid.match(name).hasMatch() && !device.match(name).hasMatch();
}
bool readBytes(const QString &file, qint64 limit, QByteArray *bytes, QString *error) {
    QFile input(file);
    if (!safePath(file) || input.size() < 0 || input.size() > limit || !input.open(QIODevice::ReadOnly))
        return fail(error, "Missing, unsafe or oversized transaction payload: " + file);
    *bytes = input.read(limit + 1);
    return bytes->size() <= limit || fail(error, "Transaction payload grew while reading");
}
struct Transaction {
    QString journal, folder, png, meta, state;
    Revision beforePng, beforeMeta, afterPng, afterMeta, journalRevision;
    QJsonObject json;
};
bool load(const QString &journal, Transaction *tx, QString *error) {
    QByteArray bytes;
    const auto journalPath = QDir::fromNativeSeparators(QFileInfo(journal).absoluteFilePath());
    if (!safePath(journalPath))
        return fail(error, "Unsafe paired-export journal path");
    const auto observed = Revision::read(journalPath);
    if (!readBytes(journalPath, 64 * 1024, &bytes, error)) return false;
    QJsonParseError parse;
    const auto object = QJsonDocument::fromJson(bytes, &parse).object();
    const QFileInfo folder(QFileInfo(journalPath).absolutePath());
    const QFileInfo transactions(folder.absolutePath());
    const QFileInfo novaFolder(transactions.absolutePath());
    const auto schema = object["schema"].toInt();
    const auto target = object["png"].toString();
    const auto state = object["state"].toString();
    const bool terminal = state == "complete" || state == "rolled_back";
    QString name = target;
    if (schema == 1) {
        // Legacy paths are never opened. A relocated terminal record is
        // historical, while an interrupted legacy record must still match
        // the original location before it can authorize writes.
        const auto legacy = QDir::fromNativeSeparators(target);
        if (!QFileInfo(legacy).isAbsolute() || QDir::cleanPath(legacy) != legacy ||
            (!terminal && QFileInfo(legacy).absolutePath() != novaFolder.absolutePath()))
            return fail(error, "Invalid or relocated interrupted legacy journal");
        name = QFileInfo(legacy).fileName();
    }
    const auto png = QDir(novaFolder.absolutePath()).filePath(name);
    if (parse.error != QJsonParseError::NoError || (schema != 1 && schema != 2) ||
        transactions.fileName() != "animation-exports" || novaFolder.fileName() != ".nova" ||
        QUuid(folder.fileName()).isNull() || QFileInfo(journalPath).fileName() != "journal.json" ||
        !textureFileName(name) || !safePath(png) || !safePath(png + ".mcmeta") ||
        !QStringList{"prepared", "png_replaced", "complete", "rolling_back", "rolled_back"}.contains(state) ||
        !observed.sameContent(Revision::read(journalPath)))
        return fail(error, "Invalid or changed paired-export journal");
    tx->journal = journalPath; tx->folder = folder.absoluteFilePath(); tx->png = png; tx->meta = png + ".mcmeta";
    tx->state = state; tx->json = object; tx->journalRevision = observed;
    tx->beforePng = decodeRevision(object["png_before"].toObject()); tx->beforeMeta = decodeRevision(object["meta_before"].toObject());
    tx->afterPng = decodeRevision(object["png_after"].toObject()); tx->afterMeta = decodeRevision(object["meta_after"].toObject());
    for (const auto &r : {tx->beforePng, tx->beforeMeta, tx->afterPng, tx->afterMeta})
        if (r.size < 0 || r.size > PngLimit || (r.exists && r.hash.size() != 32) || (!r.exists && (!r.hash.isEmpty() || r.size)))
            return fail(error, "Invalid transaction revision");
    if (!tx->afterPng.exists || !tx->afterMeta.exists) return fail(error, "Missing intended pair revisions");
    return true;
}
bool record(Transaction &tx, const QString &state, QString *error) {
    tx.json["state"] = state;
    auto bytes = QJsonDocument(tx.json).toJson(QJsonDocument::Indented);
    if (!atomicWrite(tx.journal, bytes, tx.journalRevision, error)) return false;
    tx.journalRevision = digest(bytes); tx.state = state; return true;
}
bool payload(const Transaction &tx, const QString &name, const Revision &expected, qint64 limit,
             QByteArray *bytes, QString *error) {
    if (!expected.exists) { bytes->clear(); return true; }
    if (!readBytes(tx.folder + '/' + name, limit, bytes, error)) return false;
    return expected.sameContent(digest(*bytes)) || fail(error, "Transaction payload/backup checksum mismatch: " + name);
}
bool known(const QString &file, const Revision &before, const Revision &after) {
    const auto current = Revision::read(file); return before.sameContent(current) || after.sameContent(current);
}
}
QString stageAnimationPair(const QString &requested, const QByteArray &pixels, const QByteArray &metadata,
                           const Revision &expectedPng, const Revision &expectedMeta, QString *error, const PairWriter &stagingWriter) {
    const auto png = QDir::fromNativeSeparators(QFileInfo(requested).absoluteFilePath());
    auto reject = [&](const QString &message) { fail(error, message); return QString{}; };
    if (!textureFileName(QFileInfo(png).fileName()) || !safePath(png) || !safePath(png + ".mcmeta") ||
        !QFileInfo(QFileInfo(png).absolutePath()).isDir() || pixels.isEmpty() || pixels.size() > PngLimit ||
        metadata.isEmpty() || metadata.size() > MetadataLimit) return reject("Invalid paired-export destination or byte budget");
    QBuffer pngBuffer; pngBuffer.setData(pixels); pngBuffer.open(QIODevice::ReadOnly); QImageReader reader(&pngBuffer, "PNG");
    const auto dimensions = reader.size(); QJsonParseError parse; auto json = QJsonDocument::fromJson(metadata, &parse);
    if (!dimensions.isValid() || dimensions.width() > 32768 || dimensions.height() > 32768 ||
        qint64(dimensions.width()) * dimensions.height() * 4 > Document::PixelLimit ||
        parse.error != QJsonParseError::NoError || !json.isObject() || !json.object()["animation"].isObject())
        return reject("Both outputs must validate before staging");
    AnimationState checked;
    if (!parseAnimation(reader.read(), json.object(), "26.1", &checked, error)) return {};
    if (!expectedPng.sameContent(Revision::read(png)) || !expectedMeta.sameContent(Revision::read(png + ".mcmeta")))
        return reject("External PNG or metadata conflict; no destination was changed");
    if ((expectedPng.exists && expectedPng.size > PngLimit) || (expectedMeta.exists && expectedMeta.size > MetadataLimit))
        return reject("Original files exceed backup limits");
    const auto root = QFileInfo(png).absolutePath() + "/.nova/animation-exports";
    if (!safePath(root)) return reject("Unsafe transaction directory");
    qint64 retained = 0; int journals = 0;
    QDirIterator files(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (files.hasNext()) { files.next(); if (files.fileInfo().isSymLink()) return reject("Linked transaction storage"); retained += files.fileInfo().size(); if (files.fileName() == "journal.json") ++journals; }
    if (journals >= 32 || retained + pixels.size() + metadata.size() + expectedPng.size + expectedMeta.size > 512LL * 1024 * 1024)
        return reject("Retained export transactions reached 32 journals / 512 MiB; archive completed transactions first");
    const auto folder = root + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(folder) || !safePath(folder)) return reject("Cannot create transaction storage");
    auto write = [&](const QString &name, const QByteArray &bytes) {
        return stagingWriter ? stagingWriter(folder + '/' + name, bytes, {}, error) : atomicWrite(folder + '/' + name, bytes, {}, error);
    };
    QByteArray backup;
    if (expectedPng.exists) {
        if (!readBytes(png, PngLimit, &backup, error)) return {};
        if (!expectedPng.sameContent(digest(backup))) return reject("PNG changed during backup");
        if (!write("png.before", backup)) return {};
    }
    if (expectedMeta.exists) {
        if (!readBytes(png + ".mcmeta", MetadataLimit, &backup, error)) return {};
        if (!expectedMeta.sameContent(digest(backup))) return reject("Metadata changed during backup");
        if (!write("meta.before", backup)) return {};
    }
    if (!write("png.after", pixels) || !write("meta.after", metadata)) return {};
    QJsonObject journal{{"schema", 2}, {"png", QFileInfo(png).fileName()}, {"state", "prepared"},
                         {"png_before", encodeRevision(expectedPng)}, {"meta_before", encodeRevision(expectedMeta)},
                         {"png_after", encodeRevision(digest(pixels))}, {"meta_after", encodeRevision(digest(metadata))}};
    const auto file = folder + "/journal.json";
    if (!write("journal.json", QJsonDocument(journal).toJson(QJsonDocument::Indented))) return {};
    return file;
}
PairStatus inspectAnimationPair(const QString &journal) {
    Transaction tx; PairStatus status; status.journal = journal;
    status.valid = load(journal, &tx, &status.error); status.png = tx.png; status.state = tx.state;
    status.complete = status.valid && (tx.state == "complete" || tx.state == "rolled_back"); return status;
}
bool finishAnimationPair(const QString &journal, PairAction action, QString *error,
                         const std::function<bool()> &afterFirst) {
    Transaction tx; if (!load(journal, &tx, error)) return false;
    if (tx.json["schema"].toInt() == 1 &&
        QDir::fromNativeSeparators(tx.json["png"].toString()) != tx.png)
        return fail(error, "A relocated legacy journal is retained for inspection only");
    if (tx.state == "rolled_back") return action == PairAction::Rollback || fail(error, "This transaction was rolled back; create a fresh export");
    if (tx.state == "complete" && action == PairAction::Retry) {
        if (tx.afterPng.sameContent(Revision::read(tx.png)) && tx.afterMeta.sameContent(Revision::read(tx.meta))) return true;
        return fail(error, "Completed export changed externally; create a new reviewed export");
    }
    if (tx.state == "rolling_back" && action == PairAction::Retry) return fail(error, "Finish rollback before creating a new export");
    // Check both destinations before either mutation, and each again at its atomic commit.
    if (!known(tx.png, tx.beforePng, tx.afterPng) || !known(tx.meta, tx.beforeMeta, tx.afterMeta))
        return fail(error, "External PNG or metadata change blocks retry/rollback; preserve and review the transaction");
    QByteArray pngAfter, metaAfter, pngBefore, metaBefore;
    if (!payload(tx, "png.after", tx.afterPng, PngLimit, &pngAfter, error) ||
        !payload(tx, "meta.after", tx.afterMeta, MetadataLimit, &metaAfter, error) ||
        !payload(tx, "png.before", tx.beforePng, PngLimit, &pngBefore, error) ||
        !payload(tx, "meta.before", tx.beforeMeta, MetadataLimit, &metaBefore, error)) return false;
    if (action == PairAction::Retry) {
        if (!tx.afterPng.sameContent(Revision::read(tx.png)))
            if (!atomicWrite(tx.png, pngAfter, tx.beforePng, error)) return false;
        if (!record(tx, "png_replaced", error)) return false;
        if (afterFirst && !afterFirst()) return fail(error, "Interrupted after first replacement; journal and backups retained");
        if (!tx.afterPng.sameContent(Revision::read(tx.png))) return fail(error, "PNG changed before metadata replacement; journal retained");
        if (!tx.afterMeta.sameContent(Revision::read(tx.meta)))
            if (!atomicWrite(tx.meta, metaAfter, tx.beforeMeta, error)) return false;
        if (!tx.afterPng.sameContent(Revision::read(tx.png)) || !tx.afterMeta.sameContent(Revision::read(tx.meta)))
            return fail(error, "Pair changed during export; journal retained for review");
        return record(tx, "complete", error);
    }
    if (!record(tx, "rolling_back", error)) return false;
    auto restore = [&](const QString &file, const QString &retained, const Revision &before, const Revision &after, const QByteArray &backup) {
        if (before.sameContent(Revision::read(file))) return true;
        if (!after.sameContent(Revision::read(file))) return fail(error, "External change during rollback");
        if (before.exists) return atomicWrite(file, backup, after, error);
        // An absent original is restored by retaining the new file in the journal, never deleting it.
        if (!safePath(file) || QFileInfo::exists(tx.folder + '/' + retained) || !QFile::rename(file, tx.folder + '/' + retained))
            return fail(error, "Cannot retain newly created output while rolling back");
        return true;
    };
    if (!restore(tx.png, "png.rolled-forward", tx.beforePng, tx.afterPng, pngBefore) ||
        !restore(tx.meta, "meta.rolled-forward", tx.beforeMeta, tx.afterMeta, metaBefore)) return false;
    if (!tx.beforePng.sameContent(Revision::read(tx.png)) || !tx.beforeMeta.sameContent(Revision::read(tx.meta)))
        return fail(error, "External change while finalizing rollback");
    return record(tx, "rolled_back", error);
}
QStringList pendingAnimationPairs(const QString &png) {
    QStringList result;
    const auto root = QFileInfo(png).absolutePath() + "/.nova/animation-exports";
    for (const auto &dir : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (dir.isSymLink()) continue;
        const auto journal = dir.absoluteFilePath() + "/journal.json"; auto status = inspectAnimationPair(journal);
        if ((!status.valid && QFileInfo::exists(journal)) ||
            (status.valid && !status.complete && pathIdentity(status.png) == pathIdentity(png))) result.push_back(journal);
    }
    return result;
}
} // namespace nova
