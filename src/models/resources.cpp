// SPDX-License-Identifier: GPL-3.0-or-later
#include "resources.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
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
void ResourceStack::build(const QString &root, const Index &editable,
                          const QVector<ResourceSource> &fallbacks, const QString &version) {
    candidates.clear();
    combined = {};
    diagnostics.clear();
    auto add = [&](const QString &base, const QString &label, const Index &index, bool writable,
                   const QJsonObject &hashes) {
        for (auto i = index.records.cbegin(); i != index.records.cend(); ++i) {
            if (!i.key().startsWith("assets/"))
                continue;
            const auto full = base + '/' + i.key();
            if (QDir::cleanPath(full) != full || !safe(base, full))
                continue;
            if (!hashes.isEmpty() && !hashes.contains(i.key()))
                continue;
            candidates[i.key()].append(
                {full, label, writable, base,
                 hashes.isEmpty() ? QByteArray{}
                                  : QByteArray::fromHex(hashes[i.key()].toString().toLatin1())});
            if (!combined.records.contains(i.key())) {
                auto record = i.value();
                record.filesystemPath = full;
                record.projectRoot = base;
                combined.records[i.key()] = record;
                combined.files << i.key();
            }
        }
    };
    add(QFileInfo(root).canonicalFilePath(), "Editable pack", editable, true, {});
    for (const auto &s : fallbacks) {
        if (!s.version.isEmpty() && s.version != version) {
            diagnostics << "Skipped resource source for different target: " + s.label;
            continue;
        }
        add(s.root, s.label, s.index, false, s.hashes);
    }
}
QString ResourceStack::path(const QString &asset, int provider) const {
    const auto list = candidates.value(asset);
    if (provider < 0 || provider >= list.size())
        return {};
    const auto &candidate = list[provider];
    const auto canonical = QFileInfo(candidate.path).canonicalFilePath();
    if (!safe(candidate.root, canonical))
        return {};
    if (!candidate.sha256.isEmpty()) {
        QFile f(canonical);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&f) || hash.result() != candidate.sha256)
            return {};
    }
    return canonical;
}
bool ResourceStack::editable(const QString &asset) const {
    const auto list = candidates.value(asset);
    return !list.isEmpty() && list[0].editable;
}
} // namespace nova::mc
