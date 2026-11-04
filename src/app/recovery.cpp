// SPDX-License-Identifier: GPL-3.0-or-later
#include "recovery.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QMutex>
#include <QHash>
namespace nova {
namespace {
class HashWriter : public QIODevice {
    QIODevice &destination;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *bytes, qint64 length) override {
        const auto written = destination.write(bytes, length);
        if (written > 0)
            hash.addData(QByteArrayView(bytes, written));
        return written;
    }

  public:
    explicit HashWriter(QIODevice &file) : destination(file) {
        open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    }
    bool isSequential() const override { return true; }
    QByteArray checksum() const { return hash.result(); }
};
} // namespace
struct RecoveryStore::Lifecycle {
    enum class State { Pending, Restored };
    QMutex mutex;
    QHash<QUuid, State> protectedSnapshots;
};

RecoveryStore::RecoveryStore(QString root)
    : directory(std::move(root)), lifecycle(std::make_shared<Lifecycle>()) {
    // Claim existing snapshots before any timer, save or asynchronous discovery can run.
    const auto files = QDir(directory).entryInfoList({"*.nvr"}, QDir::Files | QDir::NoSymLinks);
    for (const auto &file : files)
        lifecycle->protectedSnapshots.insert(QUuid(file.baseName()), Lifecycle::State::Pending);
}
QString RecoveryStore::pathFor(QUuid id) const {
    return directory + '/' + id.toString(QUuid::WithoutBraces) + ".nvr";
}
bool RecoveryStore::writeDocument(const Document &doc, const QString &project,
                                  QString *error) const {
    if (!doc.nativeDocument())
        return write(doc.id(), doc.path, project, doc.revision(), doc.image, error);
    auto data = doc.encodeNative("recovery.png", error);
    if (data.isEmpty() || data.size() > 120LL * 1024 * 1024) {
        if (error && error->isEmpty())
            *error = "Layered recovery exceeds 120 MiB encoded budget";
        return false;
    }
    QMutexLocker lock(&lifecycle->mutex);
    const auto pending = lifecycle->protectedSnapshots.constFind(doc.id());
    if (pending != lifecycle->protectedSnapshots.cend() && *pending == Lifecycle::State::Pending)
        return false;
    const auto original = doc.revision(), native = doc.editableRevision();
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = "Recovery folder is inaccessible";
        return false;
    }
    auto ok = atomicWriteStream(
        pathFor(doc.id()), Revision::read(pathFor(doc.id())),
        [&](QIODevice &file) {
            HashWriter writer(file);
            QDataStream stream(&writer);
            stream.setVersion(QDataStream::Qt_6_0);
            stream << quint32(0x4e565232) << quint32(2) << doc.id() << doc.path << project
                   << QDateTime::currentDateTimeUtc() << original.exists << original.readable
                   << original.size << original.modified << original.hash << data
                   << doc.editablePath() << native.exists << native.readable << native.size
                   << native.modified << native.hash;
            return stream.status() == QDataStream::Ok && file.write(writer.checksum()) == 32;
        },
        error);
    if (ok) {
        lifecycle->protectedSnapshots.remove(doc.id());
        pruneLocked();
    }
    return ok;
}
bool RecoveryStore::write(QUuid id, const QString &path, const QString &project,
                          const Revision &original, const QImage &image, QString *error) const {
    if (image.isNull() || image.width() > 4096 || image.height() > 4096)
        return false;
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG") || png.size() > 64LL * 1024 * 1024) {
        if (error)
            *error = "Recovery image exceeds snapshot budget";
        return false;
    }
    QMutexLocker lock(&lifecycle->mutex);
    const auto pending = lifecycle->protectedSnapshots.constFind(id);
    if (pending != lifecycle->protectedSnapshots.cend() && *pending == Lifecycle::State::Pending)
        return false;
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = "Recovery folder is inaccessible";
        return false;
    }
    auto result = atomicWriteStream(
        pathFor(id), Revision::read(pathFor(id)),
        [&](QIODevice &file) {
            HashWriter writer(file);
            QDataStream stream(&writer);
            stream.setVersion(QDataStream::Qt_6_0);
            stream << quint32(0x4e565232) << quint32(1) << id << path << project
                   << QDateTime::currentDateTimeUtc() << original.exists << original.readable
                   << original.size << original.modified << original.hash << png;
            return stream.status() == QDataStream::Ok && file.write(writer.checksum()) == 32;
        },
        error);
    if (result) {
        lifecycle->protectedSnapshots.remove(id);
        pruneLocked();
    }
    return result;
}
static RecoveryEntry readEntry(const QFileInfo &file, bool decode) {
    RecoveryEntry e;
    e.file = file.absoluteFilePath();
    e.id = QUuid(file.baseName());
    QFile input(e.file);
    if (file.isSymLink() || file.size() > 121LL * 1024 * 1024 || !input.open(QIODevice::ReadOnly)) {
        e.error = "Unreadable or oversized snapshot";
        return e;
    }
    auto bytes = input.readAll();
    if (bytes.size() < 32 ||
        QCryptographicHash::hash(QByteArrayView(bytes.constData(), bytes.size() - 32),
                                 QCryptographicHash::Sha256) != bytes.last(32)) {
        e.error = "Recovery checksum failed";
        return e;
    }
    bytes.chop(32);
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic, schema;
    QByteArray png;
    QUuid embedded;
    stream >> magic >> schema >> embedded >> e.path >> e.project >> e.timestamp >>
        e.original.exists >> e.original.readable >> e.original.size >> e.original.modified >>
        e.original.hash >> png;
    if (schema == 2)
        stream >> e.nativeFile >> e.nativeRevision.exists >> e.nativeRevision.readable >>
            e.nativeRevision.size >> e.nativeRevision.modified >> e.nativeRevision.hash;
    if (magic != 0x4e565232 || (schema != 1 && schema != 2) || embedded != e.id || e.id.isNull() ||
        stream.status() != QDataStream::Ok || !stream.atEnd() ||
        png.size() > (schema == 1 ? 64LL : 120LL) * 1024 * 1024)
        e.error = "Unsupported or corrupt recovery data";
    else if (schema == 2) {
        if (decode) {
            Document doc(e.path, QImage{});
            if (!doc.decodeNative(png, "recovery.png", &e.error) || doc.id() != e.id) {
                if (e.error.isEmpty())
                    e.error = "Recovery document identity mismatch";
            } else {
                e.nativeData = png;
                e.image = doc.image;
            }
        }
    } else {
        QBuffer buffer(&png);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "PNG");
        auto size = reader.size();
        if (size.isEmpty() || size.width() > 4096 || size.height() > 4096)
            e.error = "Recovery image dimensions are invalid";
        else if (decode) {
            e.image = reader.read().convertToFormat(QImage::Format_RGBA8888);
            if (e.image.isNull())
                e.error = "Recovery PNG is corrupt";
        }
    }
    return e;
}
QList<RecoveryEntry> RecoveryStore::entries() const {
    QList<RecoveryEntry> result;
    auto files =
        QDir(directory).entryInfoList({"*.nvr"}, QDir::Files | QDir::NoSymLinks, QDir::Time);
    for (const auto &file : files)
        result.append(readEntry(file, false));
    return result;
}
RecoveryEntry RecoveryStore::load(QUuid id) const {
    return readEntry(QFileInfo(pathFor(id)), true);
}
void RecoveryStore::remove(QUuid id) const {
    QMutexLocker lock(&lifecycle->mutex);
    const auto pending = lifecycle->protectedSnapshots.constFind(id);
    if (pending != lifecycle->protectedSnapshots.cend() && *pending == Lifecycle::State::Pending)
        return;
    if (QFile::remove(pathFor(id)))
        lifecycle->protectedSnapshots.remove(id);
}

void RecoveryStore::cleanupClean(QUuid id) const {
    QMutexLocker lock(&lifecycle->mutex);
    if (!lifecycle->protectedSnapshots.contains(id))
        QFile::remove(pathFor(id));
}

void RecoveryStore::restored(QUuid id) const {
    QMutexLocker lock(&lifecycle->mutex);
    const auto entry = lifecycle->protectedSnapshots.find(id);
    if (entry != lifecycle->protectedSnapshots.end())
        *entry = Lifecycle::State::Restored;
}

void RecoveryStore::discard(QUuid id) const {
    QMutexLocker lock(&lifecycle->mutex);
    if (QFile::remove(pathFor(id)))
        lifecycle->protectedSnapshots.remove(id);
}

void RecoveryStore::prune() const {
    QMutexLocker lock(&lifecycle->mutex);
    pruneLocked();
}

void RecoveryStore::pruneLocked() const {
    auto files =
        QDir(directory).entryInfoList({"*.nvr"}, QDir::Files | QDir::NoSymLinks, QDir::Time);
    qint64 bytes = 0;
    int count = 0;
    for (const auto &file : files) {
        if (lifecycle->protectedSnapshots.contains(QUuid(file.baseName())))
            continue;
        bytes += file.size();
        ++count;
        if (count > 32 || bytes > 256LL * 1024 * 1024 ||
            file.lastModified().daysTo(QDateTime::currentDateTime()) > 30)
            QFile::remove(file.absoluteFilePath());
    }
}
} // namespace nova
