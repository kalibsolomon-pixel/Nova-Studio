// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Populates the shared baseline cache for one Minecraft Java version from Mojang's
// official metadata (DESIGN.md "Population"): lock -> re-check -> metadata -> free space
// -> client jar -> verify -> extract textures -> asset-index textures -> manifest.
// Runs on the GUI thread's event loop; hashing and extraction run on QtConcurrent.
// Nothing is published as an entry until the final atomic manifest write.
#include "cache.h"
#include <QFuture>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

class QLockFile;
class QNetworkAccessManager;
namespace nova::testing {
class Fetch;
}
namespace nova::vanilla {

inline constexpr auto stageLock = "Waiting for the baseline cache";
inline constexpr auto stageMetadata = "Reading Mojang's version metadata";
inline constexpr auto stageClient = "Downloading the Minecraft client";
inline constexpr auto stageExtract = "Extracting textures";
inline constexpr auto stageAssets = "Downloading asset textures";
inline constexpr auto stagePublish = "Saving the baseline";

struct InstallerOptions {
    QString cacheRoot = BaselineCache::defaultRoot();
    QUrl manifestUrl = QUrl(defaultManifestUrl);
    QUrl objectBaseUrl = QUrl(defaultObjectBaseUrl);
    testing::DownloadPolicy policy = vanillaPolicy();
    QNetworkAccessManager *network = nullptr; // created on demand when null
    // When set, a different identity from the official metadata fails with
    // MetadataChanged before anything large is downloaded; a complete entry with this
    // identity is reused without any network access.
    QString expectedIdentity;
    int lockTimeoutMs = 10 * 60 * 1000;
    // Re-verify an existing entry with full SHA-256 instead of sizes (explicit repair).
    bool repair = false;
    // Tests: bytes available at a path (default QStorageInfo).
    std::function<qint64(const QString &)> freeSpace;
    // Tests: called on the GUI thread after every object is stored, just before the
    // manifest is written. Returning false simulates an interruption (no entry).
    std::function<bool()> beforePublish;
};

class BaselineInstaller : public QObject {
    Q_OBJECT
  public:
    enum class Error {
        None,
        Network,         // connection, HTTP status, redirect to an unexpected address
        Integrity,       // size/SHA-1 mismatch, malformed metadata or jar, unexpected data
        DiskSpace,       // not enough free space before the client download
        Cancelled,
        Busy,            // another process held cache.lock for longer than lockTimeoutMs
        NotListed,       // the version is not in Mojang's manifest
        MetadataChanged, // the official metadata no longer yields expectedIdentity
        Io               // the cache could not be written (or a test interruption)
    };
    using Options = InstallerOptions;

    explicit BaselineInstaller(Options options = {}, QObject *parent = nullptr);
    ~BaselineInstaller() override;
    void start(const QString &version);
    void cancel();
    bool isRunning() const { return running; }
    Error error() const { return kind; }
    QString errorString() const { return message; }
    // The complete entry after finished(""); identity known once the metadata was read.
    const BaselineEntry &entry() const { return result; }
    QString identity() const { return ident; }
    bool reusedExisting() const { return reused; } // finished with an entry already cached

  signals:
    void progress(const QString &stage, qint64 done, qint64 total);
    void finished(const QString &error); // empty on success, emitted exactly once per start()

  private:
    void tryLock();
    void prepared(std::optional<BaselineEntry> existing);
    void fetchVersion();
    void checked(std::optional<BaselineEntry> existing);
    void fetchAssetIndex();
    void downloadClient();
    void extracted();
    void downloadNextObject();
    void publishObjects();
    void writeManifest();
    void fetch(const QString &stage, const QUrl &url, qint64 maxBytes, const QString &destination,
               std::function<void(const QByteArray &)> done);
    template <typename T>
    void work(std::function<T()> task, std::function<void(T)> done, bool cancellable = true);
    void finish(Error error, const QString &text);
    void cleanStaging();

    Options o;
    BaselineCache cache;
    QNetworkAccessManager *network = nullptr;
    QString version, ident, staging, stage;
    std::shared_ptr<QLockFile> lock;
    std::shared_ptr<std::atomic_bool> cancelFlag;
    QTimer lockTimer;
    QElapsedTimer lockWaited;
    QPointer<testing::Fetch> current;
    QFuture<void> worker;
    VersionRef ref;
    VersionMetadata meta;
    QList<AssetTexture> assetTextures, pendingObjects;
    QMap<QString, BaselineFile> files;
    BaselineEntry result;
    Error kind = Error::None;
    QString message;
    bool running = false, reused = false;
};

} // namespace nova::vanilla
