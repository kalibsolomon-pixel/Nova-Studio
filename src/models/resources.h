// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "index.h"
#include <QByteArray>
#include <QJsonObject>
#include <memory>
namespace nova::vanilla {
struct BaselineEntry;
}
namespace nova::mc {
struct ResourceSource {
    QString root, label, version;
    bool vanilla = false;
    Index index;
    QJsonObject hashes;
    static ResourceSource open(const QString &folder, const QString &target, QString *error);
    // Nova's verified baseline cache as a read-only lowest-priority source: records point
    // straight at the cache's content-addressed objects; `hashes` carries the manifest's
    // per-file SHA-256 so every read is integrity-checked like a verified vanilla folder.
    static ResourceSource fromBaseline(const vanilla::BaselineEntry &entry);
};
struct ResourceCandidate {
    QString path, label;
    bool editable = false;
    QString root;
    QByteArray sha256;
};
// Uses the existing Index snapshots. No independent filesystem crawler/watcher.
class ResourceStack {
    // The fallback sources change far less often than the editable pack, so their
    // candidates/records are built once and shared (copy-on-write, snapshot-friendly) by
    // every build() that is handed the same sources again.
    struct FallbackLayer {
        QVector<ResourceSource> sources; // pins the buffers `matches` compares by identity
        QString version;
        QMap<QString, QVector<ResourceCandidate>> candidates;
        Index combined;
        QStringList diagnostics;
        bool matches(const QVector<ResourceSource> &other, const QString &target) const;
    };
    QMap<QString, QVector<ResourceCandidate>> candidates; // editable pack only
    std::shared_ptr<const FallbackLayer> fallbackLayer;
    Index combined;
    const ResourceCandidate *candidate(const QString &asset, int provider) const;

  public:
    QStringList diagnostics;
    void build(const QString &editableRoot, const Index &editable,
               const QVector<ResourceSource> &fallbacks, const QString &version);
    const Index &index() const { return combined; }
    QString path(const QString &asset, int provider = 0) const;
    QVector<ResourceCandidate> providers(const QString &asset) const;
    // Presence only: no filesystem access and no integrity check (see path()).
    bool hasAsset(const QString &asset) const;
    bool editable(const QString &asset) const;
};
} // namespace nova::mc
