// SPDX-License-Identifier: GPL-3.0-or-later
#include "resources.h"
#include "vanilla/cache.h"
#include "vanilla/overlay.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
namespace nova::mc {
namespace {
QString officialHash(const QString &version) {
    if (version == "1.21.11")
        return "ba2df812c2d12e0219c489c4cd9a5e1f0760f5bd";
    if (version == "26.1")
        return "191771837687b766537a8c4607cb6fad79c533a1";
    if (version == "26.3")
        return "e877b6a07acd633fb3bb475002175cec036e7b87";
    return {};
}
QString officialResourceManifest(const QString &version) {
    if (version == "1.21.11")
        return "2beb80b1f4c19fc8d356c2e4a22df2565a8e97d90ea98efc99745d69c86fdbcd";
    if (version == "26.1")
        return "cce571fa9ae42990e32b03bd93e4d9728e90d11da2a008f2275231c8d82f018a";
    if (version == "26.3")
        return "4b9a7b0a65da56dd79b5cf629f93776197cae85b986fda2304704e6b434e65c6";
    return {};
}
bool safe(const QString &root, const QString &path) {
    return !root.isEmpty() && path.startsWith(root + '/', Qt::CaseInsensitive);
}
} // namespace
ResourceSource ResourceSource::open(const QString &folder, const QString &target, QString *error) {
    ResourceSource s;
    s.root = QFileInfo(folder).canonicalFilePath();
    s.version = target;
    auto fail = [&](QString message) {
        if (error)
            *error = message;
        return ResourceSource{};
    };
    if (s.root.isEmpty() || !QFileInfo(s.root).isDir())
        return fail("Resource folder is unavailable");
    s.label = QFileInfo(s.root).fileName();
    QFile manifest(s.root + "/nova-vanilla.json");
    if (manifest.exists()) {
        if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 8 * 1024 * 1024)
            return fail("Invalid vanilla manifest");
        const auto o = QJsonDocument::fromJson(manifest.readAll()).object();
        if (o["version"].toString() != target ||
            o["client_sha1"].toString() != officialHash(target) || officialHash(target).isEmpty())
            return fail("Vanilla cache does not match the selected official target version");
        s.hashes = o["files"].toObject();
        if (s.hashes.isEmpty() ||
            QCryptographicHash::hash(QJsonDocument(s.hashes).toJson(QJsonDocument::Compact),
                                     QCryptographicHash::Sha256)
                    .toHex() != officialResourceManifest(target).toLatin1())
            return fail("Vanilla resource manifest does not match the verified official client");
        s.vanilla = true;
        s.label = "Vanilla Java " + target;
    }
    s.index = Index::scan(s.root);
    if (!s.index.error.isEmpty())
        return fail(s.index.error);
    return s;
}
ResourceSource ResourceSource::fromBaseline(const vanilla::BaselineEntry &entry) {
    ResourceSource s;
    s.root = entry.root; // the objects live under it; containment and hashes keep it read-only
    s.version = entry.version;
    s.vanilla = true;
    s.label = "Vanilla Java " + entry.version;
    s.index = vanilla::baselineIndex(entry, true);
    for (auto it = entry.files.cbegin(); it != entry.files.cend(); ++it)
        s.hashes.insert(it.key(), it->sha256);
    return s;
}
bool ResourceStack::FallbackLayer::matches(const QVector<ResourceSource> &other,
                                           const QString &target) const {
    if (version != target || sources.size() != other.size())
        return false;
    // Same buffers (the layer pins ours) means the Library handed the same snapshots again.
    for (qsizetype i = 0; i < sources.size(); ++i) {
        const auto &a = sources[i], &b = other[i];
        if (a.root != b.root || a.label != b.label || a.version != b.version ||
            a.vanilla != b.vanilla || a.index.files.constData() != b.index.files.constData() ||
            a.index.files.size() != b.index.files.size() ||
            a.index.records.size() != b.index.records.size() || a.hashes.size() != b.hashes.size())
            return false;
    }
    return true;
}
void ResourceStack::build(const QString &root, const Index &editable,
                          const QVector<ResourceSource> &fallbacks, const QString &version) {
    candidates.clear();
    combined = {};
    diagnostics.clear();
    auto add = [](QMap<QString, QVector<ResourceCandidate>> &into, Index &out, const QString &base,
                  const QString &label, const Index &index, bool writable,
                  const QJsonObject &hashes) {
        for (auto i = index.records.cbegin(); i != index.records.cend(); ++i) {
            if (!i.key().startsWith("assets/"))
                continue;
            // A source may file its records away from base/relative (the cache's objects).
            const auto stored = i.value().filesystemPath;
            const auto full = stored.startsWith(base + '/', Qt::CaseInsensitive) ? stored
                                                                                 : base + '/' + i.key();
            if (QDir::cleanPath(full) != full || !safe(base, full))
                continue;
            if (!hashes.isEmpty() && !hashes.contains(i.key()))
                continue;
            into[i.key()].append(
                {full, label, writable, base,
                 hashes.isEmpty() ? QByteArray{}
                                  : QByteArray::fromHex(hashes[i.key()].toString().toLatin1())});
            if (!out.records.contains(i.key())) {
                auto record = i.value();
                record.filesystemPath = full;
                record.projectRoot = base;
                out.records[i.key()] = record;
                out.files << i.key();
            }
        }
    };
    if (fallbacks.isEmpty())
        fallbackLayer.reset();
    else if (!fallbackLayer || !fallbackLayer->matches(fallbacks, version)) {
        auto layer = std::make_shared<FallbackLayer>();
        layer->sources = fallbacks;
        layer->version = version;
        for (const auto &s : fallbacks) {
            if (!s.version.isEmpty() && s.version != version) {
                layer->diagnostics << "Skipped resource source for different target: " + s.label;
                continue;
            }
            add(layer->candidates, layer->combined, s.root, s.label, s.index, false, s.hashes);
        }
        fallbackLayer = std::move(layer);
    }
    // Editable records are first in priority; fallback records fill in the keys they miss.
    Index local;
    add(candidates, local, QFileInfo(root).canonicalFilePath(), "Editable pack", editable, true, {});
    if (fallbackLayer) {
        diagnostics = fallbackLayer->diagnostics;
        const auto &lower = fallbackLayer->combined;
        combined = lower; // shared until the editable records below need merging in
        if (!local.records.isEmpty()) {
            for (auto i = local.records.cbegin(); i != local.records.cend(); ++i)
                combined.records[i.key()] = i.value();
            combined.files = local.files;
            for (const auto &file : lower.files)
                if (!local.records.contains(file))
                    combined.files << file;
        }
    } else
        combined = std::move(local);
}
const ResourceCandidate *ResourceStack::candidate(const QString &asset, int provider) const {
    if (provider < 0)
        return nullptr;
    const auto editableList = candidates.constFind(asset);
    const qsizetype own = editableList == candidates.cend() ? 0 : editableList->size();
    if (provider < own)
        return &(*editableList)[provider];
    if (!fallbackLayer)
        return nullptr;
    const auto lower = fallbackLayer->candidates.constFind(asset);
    if (lower == fallbackLayer->candidates.cend() || provider - own >= lower->size())
        return nullptr;
    return &(*lower)[provider - own];
}
QVector<ResourceCandidate> ResourceStack::providers(const QString &asset) const {
    auto list = candidates.value(asset);
    if (fallbackLayer)
        list += fallbackLayer->candidates.value(asset);
    return list;
}
bool ResourceStack::hasAsset(const QString &asset) const {
    return candidates.contains(asset) ||
           (fallbackLayer && fallbackLayer->candidates.contains(asset));
}
namespace {
// Files already hashed against their expected SHA-256, keyed by canonical path. An entry is
// only trusted while size, modification time and digest all match what was verified, so a
// changed, replaced or tampered file is hashed (and rejected) again.
struct VerifiedFile {
    qint64 size = 0, modified = 0;
    QByteArray sha256;
};
constexpr qsizetype MaxVerifiedFiles = 32768;
QMutex verifiedLock;
QHash<QString, VerifiedFile> &verifiedFiles() {
    static QHash<QString, VerifiedFile> files;
    return files;
}
bool verified(const QString &canonical, const QByteArray &expected) {
    const QFileInfo info(canonical);
    if (!info.isFile())
        return false;
    const qint64 size = info.size(), modified = info.lastModified().toMSecsSinceEpoch();
    {
        QMutexLocker lock(&verifiedLock);
        const auto hit = verifiedFiles().constFind(canonical);
        if (hit != verifiedFiles().cend() && hit->size == size && hit->modified == modified &&
            hit->sha256 == expected)
            return true;
    }
    QFile f(canonical);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f) || hash.result() != expected) {
        QMutexLocker lock(&verifiedLock);
        verifiedFiles().remove(canonical);
        return false;
    }
    QMutexLocker lock(&verifiedLock);
    auto &files = verifiedFiles();
    if (files.size() >= MaxVerifiedFiles)
        files.clear();
    files.insert(canonical, VerifiedFile{size, modified, expected});
    return true;
}
} // namespace
QString ResourceStack::path(const QString &asset, int provider) const {
    const auto *found = candidate(asset, provider);
    if (!found)
        return {};
    const auto canonical = QFileInfo(found->path).canonicalFilePath();
    if (!safe(found->root, canonical))
        return {};
    if (!found->sha256.isEmpty() && !verified(canonical, found->sha256))
        return {};
    return canonical;
}
bool ResourceStack::editable(const QString &asset) const {
    return candidates.contains(asset);
}
} // namespace nova::mc
