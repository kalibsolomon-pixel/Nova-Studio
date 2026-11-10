// SPDX-License-Identifier: GPL-3.0-or-later
#include "installer.h"
#include "revision.h"
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QtConcurrent>
#include <QtCore/private/qzipreader_p.h>

namespace nova::vanilla {
namespace {
constexpr qint64 maxJarEntries = 100000;
constexpr qint64 maxExtractedBytes = 1024LL * 1024 * 1024;
constexpr qint64 spareBytes = 64LL * 1024 * 1024;
using Error = BaselineInstaller::Error;

struct Outcome {
    Error kind = Error::None;
    QString message;
    QMap<QString, BaselineFile> files;
};
Outcome failed(Error kind, const QString &message) { return {kind, message, {}}; }

const QRegularExpression &stagingRule() {
    static const QRegularExpression rule("^[0-9a-f]{16}$");
    return rule;
}
// Removes the regular files directly inside one staging folder (never links, never
// subfolders), then the folder itself if it is empty.
void clearFolder(const BaselineCache &cache, const QString &folder) {
    if (!cache.safePath(folder) || fs::isReparsePoint(folder) || !QFileInfo(folder).isDir())
        return;
    for (const auto &name : QDir(folder).entryList(QDir::Files | QDir::Hidden | QDir::System))
        fs::removeRegularFile(folder + '/' + name);
    QDir().rmdir(folder);
}
// Staging folders of earlier, interrupted runs (only token-named ones). Caller holds the lock.
void sweepStaging(const BaselineCache &cache) {
    const auto root = cache.stagingRoot();
    if (!cache.safePath(root) || !QFileInfo(root).isDir())
        return;
    for (const auto &name : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden))
        if (stagingRule().match(name).hasMatch())
            clearFolder(cache, root + '/' + name);
    QDir().rmdir(root);
}
QString megabytes(qint64 bytes) { return QString::number((bytes + 1048575) / 1048576); }

// Worker thread: verify the jar, then store every texture entry as an object.
Outcome extractClient(const BaselineCache &cache, const QString &jar, const Download &client,
                      const std::shared_ptr<std::atomic_bool> &cancel,
                      const std::function<void(qint64, qint64)> &progress) {
    if (QFileInfo(jar).size() != client.size)
        return failed(Error::Integrity, "The downloaded Minecraft client does not have the "
                                        "published size. It was deleted.");
    const auto sha1 = sha1File(jar, cancel);
    if (cancel->load())
        return failed(Error::Cancelled, "Cancelled");
    if (sha1 != client.sha1)
        return failed(Error::Integrity, "The downloaded Minecraft client does not match its "
                                        "published SHA-1. It was deleted.");
    Outcome out;
    QZipReader zip(jar);
    if (!zip.isReadable() || zip.status() != QZipReader::NoError)
        return failed(Error::Integrity, "The downloaded Minecraft client is not a readable jar");
    const auto entries = zip.fileInfoList();
    if (entries.size() > maxJarEntries)
        return failed(Error::Integrity, "The Minecraft client has too many entries");
    const QLatin1String prefix(texturePrefix);
    qint64 total = 0, done = 0;
    for (const auto &info : entries) {
        if (cancel->load())
            return failed(Error::Cancelled, "Cancelled");
        if (++done % 512 == 0)
            progress(done, entries.size());
        const auto name = info.filePath;
        const bool inside = QString(name).replace('\\', '/').startsWith(prefix, Qt::CaseInsensitive);
        if (info.isSymLink)
            return failed(Error::Integrity, "The Minecraft client contains a link entry");
        if (!inside || info.isDir)
            continue; // everything outside the textures folder is ignored, never stored
        QString why;
        if (!validBaselinePath(name, &why))
            return failed(Error::Integrity, "The Minecraft client contains unexpected data: " + why);
        if (out.files.contains(name))
            return failed(Error::Integrity, "The Minecraft client lists a texture twice: " + name);
        if (info.size < 0 || info.size > maxTextureBytes || (total += info.size) > maxExtractedBytes)
            return failed(Error::Integrity, "The Minecraft client contains an unexpectedly large "
                                            "texture: " + name);
        const auto bytes = zip.fileData(name);
        if (bytes.size() != info.size)
            return failed(Error::Integrity, "The Minecraft client's " + name +
                                                " could not be read completely");
        const auto sha256 = sha256Hex(bytes);
        QString error;
        if (!cache.publishObject(bytes, sha256, name, &error))
            return failed(Error::Io, error);
        out.files.insert(name, {sha256, bytes.size(), "jar"});
    }
    zip.close();
    if (out.files.isEmpty())
        return failed(Error::Integrity, "The Minecraft client contains no textures");
    return out;
}
} // namespace

BaselineInstaller::BaselineInstaller(Options options, QObject *parent)
    : QObject(parent), o(std::move(options)), cache(o.cacheRoot), network(o.network) {
    lockTimer.setSingleShot(true);
    connect(&lockTimer, &QTimer::timeout, this, &BaselineInstaller::tryLock);
}

BaselineInstaller::~BaselineInstaller() {
    if (!running)
        return;
    cancelFlag->store(true);
    if (current) {
        current->disconnect(this);
        delete current.data(); // aborts the reply and removes the .part file
    }
    worker.waitForFinished(); // workers check the flag between entries
    cleanStaging();
    lock.reset();
}

void BaselineInstaller::start(const QString &id) {
    if (running)
        return;
    running = true;
    reused = false;
    kind = Error::None;
    message.clear();
    result = {};
    ident.clear();
    files.clear();
    staging.clear();
    version = id;
    cancelFlag = std::make_shared<std::atomic_bool>(false);
    if (!validVersionId(version))
        return finish(Error::NotListed, QString("\"%1\" is not a Minecraft Java version id")
                                            .arg(version.left(64)));
    if (!network)
        network = new QNetworkAccessManager(this);
    lockWaited.start();
    tryLock();
}

void BaselineInstaller::cancel() {
    if (!running || cancelFlag->exchange(true))
        return;
    if (lockTimer.isActive())
        return finish(Error::Cancelled, "Cancelled");
    if (current)
        return current->cancel(); // its finished() reports the cancellation
    if (!worker.isRunning())
        finish(Error::Cancelled, "Cancelled");
    // else: the worker sees the flag and its completion reports the cancellation
}

void BaselineInstaller::tryLock() {
    if (!running)
        return;
    if (!lock) {
        if (!QDir().mkpath(cache.root()) || !cache.safePath(cache.root()))
            return finish(Error::Io, "The vanilla baseline cache folder could not be created");
        lock = std::make_shared<QLockFile>(cache.lockPath());
    }
    if (!lock->tryLock(0)) {
        if (lock->error() != QLockFile::LockFailedError)
            return finish(Error::Io, "The vanilla baseline cache could not be locked");
        if (lockWaited.elapsed() >= o.lockTimeoutMs)
            return finish(Error::Busy, "Another Nova window is still preparing the vanilla "
                                       "baseline. Try again when it has finished.");
        emit progress(stageLock, 0, -1);
        lockTimer.start(100); // never blocks the GUI thread
        return;
    }
    // Under the lock: sweep old staging, then reuse a complete entry if the caller already
    // knows the identity (no network access at all).
    auto c = cache;
    const auto expected = o.expectedIdentity;
    const auto check = o.repair ? BaselineCache::Check::Full : BaselineCache::Check::Sizes;
    const auto id = version;
    const auto flag = cancelFlag;
    work<std::optional<BaselineEntry>>(
        [c, expected, check, id, flag]() -> std::optional<BaselineEntry> {
            sweepStaging(c);
            if (!validSha256(expected))
                return std::nullopt;
            return c.load(id, expected, check, nullptr, flag);
        },
        [this](std::optional<BaselineEntry> existing) { prepared(std::move(existing)); });
}

void BaselineInstaller::prepared(std::optional<BaselineEntry> existing) {
    if (existing) {
        result = std::move(*existing);
        ident = result.identity;
        reused = true;
        return finish(Error::None, {});
    }
    fetch(stageMetadata, o.manifestUrl, 4LL * 1024 * 1024, {}, [this](const QByteArray &body) {
        QString error;
        auto parsed = parseVersionManifest(body, version, &error, o.policy);
        if (!parsed)
            return finish(error == notListedMessage(version) ? Error::NotListed : Error::Integrity,
                          error);
        ref = *parsed;
        fetchVersion();
    });
}

void BaselineInstaller::fetchVersion() {
    fetch(stageMetadata, ref.url, 4LL * 1024 * 1024, {}, [this](const QByteArray &body) {
        QString error;
        auto parsed = parseVersionJson(body, ref, &error, o.policy);
        if (!parsed)
            return finish(Error::Integrity, error);
        meta = *parsed;
        ident = identityKey(meta);
        if (!o.expectedIdentity.isEmpty() && o.expectedIdentity != ident)
            return finish(Error::MetadataChanged,
                          QString("Mojang's metadata for Minecraft Java %1 has changed since this "
                                  "project's baseline was recorded (client or asset index differs). "
                                  "Nothing was downloaded.")
                              .arg(version));
        auto c = cache;
        const auto check = o.repair ? BaselineCache::Check::Full : BaselineCache::Check::Sizes;
        const auto id = version, identity = ident;
        const auto flag = cancelFlag;
        work<std::optional<BaselineEntry>>(
            [c, check, id, identity, flag] { return c.load(id, identity, check, nullptr, flag); },
            [this](std::optional<BaselineEntry> existing) { checked(std::move(existing)); });
    });
}

void BaselineInstaller::checked(std::optional<BaselineEntry> existing) {
    if (existing) { // another window or an earlier run already finished this identity
        result = std::move(*existing);
        reused = true;
        return finish(Error::None, {});
    }
    fetchAssetIndex();
}

void BaselineInstaller::fetchAssetIndex() {
    fetch(stageMetadata, meta.assetIndex.url, meta.assetIndex.size, {},
          [this](const QByteArray &body) {
              if (body.size() != meta.assetIndex.size || sha1Hex(body) != meta.assetIndex.sha1)
                  return finish(Error::Integrity, "The asset index does not match its published "
                                                  "size and SHA-1");
              QString error;
              auto textures = parseAssetIndexTextures(body, &error);
              if (!textures)
                  return finish(Error::Integrity, error);
              assetTextures = *textures;
              downloadClient();
          });
}

void BaselineInstaller::downloadClient() {
    const qint64 need = meta.client.size * 2 + spareBytes;
    const qint64 available = o.freeSpace ? o.freeSpace(cache.root())
                                         : QStorageInfo(cache.root()).bytesAvailable();
    if (available >= 0 && available < need)
        return finish(Error::DiskSpace,
                      QString("Not enough free disk space for the vanilla baseline: %1 MB needed, "
                              "%2 MB available.")
                          .arg(megabytes(need), megabytes(available)));
    staging = cache.stagingRoot() + '/' +
              QString::number(QRandomGenerator::global()->generate64(), 16).rightJustified(16, '0');
    if (!cache.safePath(staging) || !QDir().mkpath(staging) || !cache.safePath(staging))
        return finish(Error::Io, "The vanilla baseline staging folder could not be created");
    fetch(stageClient, meta.client.url, meta.client.size, staging + "/client.jar",
          [this](const QByteArray &) { extracted(); });
}

void BaselineInstaller::extracted() {
    emit progress(stageExtract, 0, -1);
    auto c = cache;
    const auto jar = staging + "/client.jar";
    const auto client = meta.client;
    const auto flag = cancelFlag;
    QPointer<BaselineInstaller> self(this);
    auto report = [self](qint64 done, qint64 total) {
        QMetaObject::invokeMethod(self.data(), [self, done, total] {
            if (self)
                emit self->progress(stageExtract, done, total);
        }, Qt::QueuedConnection);
    };
    work<Outcome>(
        [c, jar, client, flag, report] {
            auto out = extractClient(c, jar, client, flag, report);
            fs::removeRegularFile(jar); // only textures are kept
            return out;
        },
        [this](Outcome out) {
            if (out.kind != Error::None)
                return finish(out.kind, out.message);
            files = std::move(out.files);
            pendingObjects.clear();
            for (const auto &t : assetTextures)
                if (!files.contains(t.path)) // the jar is the primary vanilla pack
                    pendingObjects.append(t);
            downloadNextObject();
        });
}

void BaselineInstaller::downloadNextObject() {
    for (qsizetype i = 0; i < pendingObjects.size(); ++i) {
        const auto &t = pendingObjects[i];
        const auto path = staging + '/' + t.sha1;
        if (QFileInfo::exists(path))
            continue;
        emit progress(stageAssets, i, pendingObjects.size());
        return fetch(stageAssets, assetObjectUrl(t.sha1, o.objectBaseUrl), t.size, path,
                     [this](const QByteArray &) { downloadNextObject(); });
    }
    publishObjects();
}

void BaselineInstaller::publishObjects() {
    emit progress(stagePublish, 0, -1);
    auto c = cache;
    const auto pending = pendingObjects;
    const auto folder = staging;
    const auto flag = cancelFlag;
    work<Outcome>(
        [c, pending, folder, flag] {
            Outcome out;
            for (const auto &t : pending) {
                if (flag->load())
                    return failed(Error::Cancelled, "Cancelled");
                const auto bytes = readFileBytes(folder + '/' + t.sha1, t.size);
                if (bytes.size() != t.size || sha1Hex(bytes) != t.sha1)
                    return failed(Error::Integrity, "The asset texture " + t.path +
                                                        " does not match its published size "
                                                        "and SHA-1");
                const auto sha256 = sha256Hex(bytes);
                QString error;
                if (!c.publishObject(bytes, sha256, t.path, &error))
                    return failed(Error::Io, error);
                out.files.insert(t.path, {sha256, bytes.size(), "asset-index"});
            }
            return out;
        },
        [this](Outcome out) {
            if (out.kind != Error::None)
                return finish(out.kind, out.message);
            files.insert(out.files);
            if (o.beforePublish && !o.beforePublish())
                return finish(Error::Io, "The baseline was interrupted before it was saved");
            writeManifest();
        });
}

void BaselineInstaller::writeManifest() {
    BaselineEntry entry;
    entry.root = cache.root();
    entry.version = version;
    entry.identity = ident;
    entry.created = QDateTime::currentDateTimeUtc();
    entry.files = files;
    entry.source = QJsonObject{
        {"manifest_url", o.manifestUrl.toString()},
        {"version_url", meta.versionUrl.toString()},
        {"version_sha1", meta.versionSha1},
        {"client", QJsonObject{{"url", meta.client.url.toString()},
                               {"sha1", meta.client.sha1},
                               {"size", meta.client.size}}},
        {"asset_index", QJsonObject{{"id", meta.assetIndexId},
                                    {"url", meta.assetIndex.url.toString()},
                                    {"sha1", meta.assetIndex.sha1},
                                    {"size", meta.assetIndex.size}}}};
    auto c = cache;
    work<Outcome>(
        [c, entry] {
            QString error;
            if (!c.writeManifest(entry, &error))
                return failed(Error::Io, "The baseline could not be saved: " + error);
            if (!c.load(entry.version, entry.identity, BaselineCache::Check::Sizes, &error))
                return failed(Error::Io, error);
            return Outcome{};
        },
        [this, entry](Outcome out) {
            if (out.kind != Error::None)
                return finish(out.kind, out.message);
            result = entry;
            finish(Error::None, {});
        },
        false); // published: a late cancel no longer applies
}

void BaselineInstaller::fetch(const QString &name, const QUrl &url, qint64 maxBytes,
                              const QString &destination,
                              std::function<void(const QByteArray &)> done) {
    stage = name;
    auto policy = o.policy;
    policy.maxBytes = std::min(policy.maxBytes, std::max<qint64>(maxBytes, 0));
    auto f = new testing::Fetch(network, policy, this);
    current = f;
    connect(f, &testing::Fetch::progress, this, [this](qint64 received, qint64 total) {
        emit progress(stage, received, total);
    });
    connect(f, &testing::Fetch::finished, this, [this, f, done](const QString &error) {
        const auto body = f->body();
        f->deleteLater();
        current = nullptr;
        if (!running)
            return;
        if (cancelFlag->load())
            return finish(Error::Cancelled, "Cancelled");
        if (!error.isEmpty()) {
            const auto kind = error.contains("larger than expected") ? Error::Integrity
                              : error.contains("written") || error.contains("finalized")
                                  ? Error::Io
                                  : Error::Network;
            return finish(kind, "Could not download the vanilla baseline: " + error);
        }
        done(body);
    });
    f->start(url, destination);
}

template <typename T>
void BaselineInstaller::work(std::function<T()> task, std::function<void(T)> done,
                             bool cancellable) {
    auto watcher = new QFutureWatcher<T>(this);
    const auto flag = cancelFlag;
    connect(watcher, &QFutureWatcher<T>::finished, this, [this, watcher, flag, done, cancellable] {
        watcher->deleteLater();
        if (!running || flag != cancelFlag)
            return; // stale result of an earlier run
        if (cancellable && flag->load())
            return finish(Error::Cancelled, "Cancelled");
        done(watcher->result());
    });
    auto future = QtConcurrent::run(std::move(task));
    worker = QFuture<void>(future);
    watcher->setFuture(future);
}

void BaselineInstaller::finish(Error error, const QString &text) {
    if (!running)
        return;
    running = false;
    lockTimer.stop();
    if (current) {
        current->disconnect(this);
        current->cancel();
        current->deleteLater();
        current = nullptr;
    }
    kind = error;
    message = error == Error::None ? QString() : text;
    if (!worker.isRunning())
        cleanStaging();
    lock.reset(); // unlock
    emit finished(message);
}

void BaselineInstaller::cleanStaging() {
    if (staging.isEmpty())
        return;
    clearFolder(cache, staging);
    QDir().rmdir(cache.stagingRoot());
    staging.clear();
}

} // namespace nova::vanilla
