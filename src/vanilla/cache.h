// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Shared per-user vanilla texture cache (DESIGN.md section 3):
//   objects/<aa>/<sha256>.png|.mcmeta     content-addressed, read-only, shared by all versions
//   entries/<version>/<identity[0:32]>/manifest.json   the only "complete" marker
//   staging/<token>/                      temporary downloads, swept under the lock
//   cache.lock                            QLockFile held for every mutation
// Readers never lock: objects are immutable and manifests appear atomically. Every
// mutation (BaselineInstaller, remove) holds cache.lock.
#include "metadata.h"
#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>
#include <optional>

namespace nova::vanilla {

struct BaselineFile {
    QString sha256; // 64 lowercase hex
    qint64 size = 0;
    QString origin; // "jar" | "asset-index"
};

struct BaselineEntry {
    QString root;     // cache root the objects live in
    QString version;  // Minecraft Java version id
    QString identity; // 64 hex, identityKey() of the source metadata
    QDateTime created;
    QJsonObject source; // manifest_url, version_url, version_sha1, client{}, asset_index{}
    QMap<QString, BaselineFile> files; // assets/minecraft/textures/... -> object
    bool isValid() const { return !identity.isEmpty(); }
    bool contains(const QString &relative) const { return files.contains(relative); }
    // Absolute path of the object holding `relative` ("" if not in the entry).
    QString objectPath(const QString &relative) const;
};

struct BaselineSummary {
    enum class Status { Complete, Corrupt };
    Status status = Status::Corrupt;
    QString version;
    QString identity; // 64 hex when the manifest is readable, else the 32-hex folder name
    QDateTime created;
    int files = 0;
    qint64 bytes = 0;       // all objects the entry references (each object counted once)
    qint64 uniqueBytes = 0; // referenced by no other valid entry
    qint64 sharedBytes = 0; // also referenced by another valid entry
    QString error;          // why the entry is Corrupt
};

struct RemoveReport {
    int objectsRemoved = 0;
    qint64 bytesRemoved = 0;
    bool collectionSkipped = false; // a remaining manifest was unreadable
};

class BaselineCache {
  public:
    // How much load() checks: Manifest = parse, digest, paths and fields; Sizes = also that
    // every object exists as a regular file with the recorded size; Full = also SHA-256.
    enum class Check { Manifest, Sizes, Full };

    explicit BaselineCache(QString root = defaultRoot());
    // NOVA_VANILLA_BASELINE_ROOT, else AppLocalDataLocation + "/vanilla-baseline".
    static QString defaultRoot();
    QString root() const { return base; }

    QList<BaselineSummary> list() const;
    std::optional<BaselineEntry> load(const QString &version, const QString &identity,
                                      Check check = Check::Sizes, QString *error = nullptr,
                                      const std::shared_ptr<std::atomic_bool> &cancel = {}) const;
    // Newest (by `created`) entry for `version` passing `check`.
    std::optional<BaselineEntry> latest(const QString &version, Check check = Check::Sizes,
                                        QString *error = nullptr) const;
    // Reads one file of an entry, verifying size and SHA-256 against the manifest.
    bool readVerified(const BaselineEntry &entry, const QString &relative, QByteArray *bytes,
                      QString *error = nullptr) const;
    // Full re-hash. Returns the entry paths whose object is missing, unsafe or wrong.
    QStringList verify(const BaselineEntry &entry,
                       const std::shared_ptr<std::atomic_bool> &cancel = {}) const;
    // Takes cache.lock (waiting at most lockTimeoutMs; run off the GUI thread), deletes the
    // entry's manifest and folder, then the objects no remaining valid manifest references.
    // `identity` may be the 64-hex identity or the 32-hex folder name of a corrupt entry.
    bool remove(const QString &version, const QString &identity, QString *error = nullptr,
                RemoveReport *report = nullptr, int lockTimeoutMs = 0) const;
    // Bytes of all regular files under the root.
    qint64 totalBytes() const;

    // Layout helpers (used by BaselineInstaller, which holds the lock).
    QString lockPath() const { return base + "/cache.lock"; }
    QString stagingRoot() const { return base + "/staging"; }
    QString entryFolder(const QString &version, const QString &identity) const;
    QString manifestPath(const QString &version, const QString &identity) const;
    QString objectPath(const QString &sha256, const QString &relative) const;
    // True if `path` is inside the root and neither it nor any existing component between
    // the root and it (root included) is a reparse point / symlink.
    bool safePath(const QString &path) const;
    // Publishes verified bytes as an object (create-only rename; an existing object that
    // re-hashes correctly is kept, a bad one is replaced). Sets the read-only attribute.
    bool publishObject(const QByteArray &bytes, const QString &sha256, const QString &relative,
                       QString *error = nullptr) const;
    // Writes manifest.json (with digest) atomically. Caller holds the lock.
    bool writeManifest(const BaselineEntry &entry, QString *error = nullptr) const;
    // Parses and validates a manifest (digest, fields, paths, identity) without touching objects.
    static std::optional<BaselineEntry> parseManifest(const QByteArray &bytes, const QString &root,
                                                      QString *error = nullptr);
    static QByteArray manifestBytes(const BaselineEntry &entry);

  private:
    QString base;
};

// File-system helpers restricted to what the cache needs.
namespace fs {
bool isReparsePoint(const QString &path); // also true for symlinks
bool setReadOnly(const QString &path, bool readOnly);
// Deletes one regular file (clearing read-only first). Never follows or deletes links.
bool removeRegularFile(const QString &path);
} // namespace fs

} // namespace nova::vanilla
