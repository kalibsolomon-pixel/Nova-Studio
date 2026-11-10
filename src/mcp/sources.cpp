// SPDX-License-Identifier: GPL-3.0-or-later
#include "sources.h"
#include "inspection.h"
#include "project.h"
#include "scope.h"
#include "vanilla/cache.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

namespace nova::mcp {
namespace {
QString pathFree(const QString &text) {
    static const QRegularExpression absolute(R"(([A-Za-z]:[\\/]|\\\\|(?:^|[\s"'(])/[^\s/]))");
    return text.contains(absolute) ? QString("The cache reported a problem with a local file.") : text.left(300);
}
} // namespace

QJsonObject Provider::json() const {
    QJsonObject o{{"source", source}};
    if (stackIndex >= 0)
        o["stackIndex"] = stackIndex;
    if (!label.isEmpty())
        o["label"] = label;
    if (!revision.isEmpty())
        o["revision"] = revision;
    if (source == "resource_stack" || source == "vanilla_texture_cache")
        o["verified"] = verified;
    if (unsavedDraft) {
        o["unsavedDraft"] = true;
        o["note"] = "An unsaved draft is open; this information comes from the saved file.";
    }
    return o;
}

bool supportedVersion(const QString &version) {
    for (const auto &v : gameVersions())
        if (v.id == version)
            return true;
    return false;
}

CacheSelection selectTextureCache(const QString &cacheRoot, const QString &version, const QString &identity,
                                  const QString &projectVersion, const QString &projectIdentity) {
    CacheSelection result;
    if (!supportedVersion(version)) {
        result.status = "unsupported_version";
        result.reason = QString("Nova does not support Minecraft Java %1.").arg(version.left(16));
        return result;
    }
    const vanilla::BaselineCache cache(cacheRoot);
    QList<vanilla::BaselineSummary> complete;
    QString damage, requestedDamage;
    for (const auto &summary : cache.list()) {
        if (summary.version != version)
            continue;
        ++result.entries;
        if (summary.status == vanilla::BaselineSummary::Status::Complete)
            complete.append(summary);
        else {
            ++result.damaged;
            if (damage.isEmpty())
                damage = pathFree(summary.error);
            if (!identity.isEmpty() && identity.startsWith(summary.identity))
                requestedDamage = pathFree(summary.error);
        }
    }
    QString chosen;
    if (!identity.isEmpty()) {
        result.selection = "requested_identity";
        for (const auto &summary : complete)
            if (summary.identity == identity)
                chosen = identity;
        if (chosen.isEmpty() && !requestedDamage.isEmpty()) {
            result.status = "invalid";
            result.reason = QString("The cached Java %1 entry %2 is incomplete or failed verification: %3")
                                .arg(version, identity.left(12), requestedDamage);
            return result;
        }
        if (chosen.isEmpty()) {
            result.status = "identity_not_cached";
            result.reason = QString("No complete verified entry with identity %1 exists for Java %2.")
                                .arg(identity.left(12), version);
            return result;
        }
    } else {
        // The project's own baseline identity first; otherwise the newest complete entry.
        if (projectVersion == version && !projectIdentity.isEmpty())
            for (const auto &summary : complete)
                if (summary.identity == projectIdentity) {
                    chosen = projectIdentity;
                    result.selection = "project_baseline";
                }
        if (chosen.isEmpty() && !complete.isEmpty()) {
            chosen = complete.first().identity; // list() sorts each version newest first
            result.selection = "latest_verified";
        }
    }
    if (chosen.isEmpty()) {
        result.status = result.entries ? "invalid" : "not_cached";
        result.reason = result.entries
            ? QString("Every cached Java %1 entry is incomplete or failed verification: %2").arg(version, damage)
            : QString("Nova's vanilla texture cache has no verified entry for Java %1.").arg(version);
        return result;
    }
    QString why;
    auto entry = cache.load(version, chosen, vanilla::BaselineCache::Check::Sizes, &why);
    if (!entry) {
        result.status = "invalid";
        result.reason = pathFree(why);
        return result;
    }
    result.status = "verified";
    result.entry = std::make_shared<const vanilla::BaselineEntry>(std::move(*entry));
    return result;
}

SourceSet::SourceSet(const Snapshot &snapshot, Scope &projectScope, const std::shared_ptr<std::atomic_bool> &cancel)
    : s(snapshot), scope(projectScope), catalog(snapshot.version) {
    for (qsizetype i = 0; i < s.resourceStack.size(); ++i)
        if (s.resourceStack[i].status == "loaded" && !s.resourceStack[i].root.isEmpty())
            stackRoots.insert(s.resourceStack[i].root, int(i));
    QVector<mc::ResourceSource> fallbacks;
    for (const auto &source : s.resourceSources) {
        if (source.version != s.version || !stackRoots.contains(source.root))
            continue;
        fallbacks.append(source);
        vanillaModels |= source.vanilla;
    }
    catalog.setFallbacks(fallbacks, s.version);
    // Project files as indexed; never-saved documents only through their live model source, and only
    // while nothing occupies their path on disk (the same rule as nova.pack.validate).
    Index editable;
    for (const auto &path : s.index.files) {
        if (cancel->load()) {
            stopped = true;
            return;
        }
        if (!logicalAsset(path) ||
            (s.provisional.contains(path) && (!s.models.contains(path) || !scope.vacant(path))))
            continue;
        editable.files.append(path);
        editable.records.insert(path, s.index.records.value(path));
    }
    mc::Library::AssetReader reader = [this](const QString &relative) -> std::unique_ptr<QIODevice> {
        const auto list = catalog.providers(relative);
        if (list.isEmpty())
            return {};
        if (list.first().editable) {
            if (s.provisional.contains(relative)) {
                auto buffer = std::make_unique<QBuffer>();
                buffer->setData(QJsonDocument(s.models.value(relative)).toJson(QJsonDocument::Compact));
                buffer->open(QIODevice::ReadOnly);
                return buffer;
            }
            return scope.open(relative);
        }
        // A resource-stack file: ResourceStack::path() checks containment (and a verified source's
        // hash); the bytes actually read are hashed again, so a file swapped in between is refused.
        const auto file = catalog.assetPath(relative);
        QFile input(file);
        if (file.isEmpty() || !input.open(QIODevice::ReadOnly) || input.size() > 2 * 1024 * 1024)
            return {};
        const auto bytes = input.readAll();
        const auto expected = list.first().sha256;
        if (!expected.isEmpty() && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != expected)
            return {};
        auto buffer = std::make_unique<QBuffer>();
        buffer->setData(bytes);
        buffer->open(QIODevice::ReadOnly);
        return buffer;
    };
    catalog.update(s.root, editable, {}, cancel, reader);
    if (cancel->load()) {
        stopped = true;
        return;
    }
    for (const auto &message : catalog.diagnostics)
        complete &= !message.startsWith("Model catalog budget exceeded");
    // Open model documents replace their saved source, as in Nova's own preview and validation.
    for (auto it = s.models.cbegin(); it != s.models.cend(); ++it) {
        if (!editable.records.contains(it.key()))
            continue;
        if (s.provisional.contains(it.key()))
            catalog.setProvisionalSource(it.key(), it.value());
        else
            catalog.previewSource(it.key(), it.value());
    }
}
SourceSet::~SourceSet() = default;

QString SourceSet::version() const { return s.version; }

QList<Provider> SourceSet::providers(const QString &path) const {
    QList<Provider> list;
    for (const auto &candidate : catalog.providers(path)) {
        Provider p;
        if (candidate.editable) {
            // Only model drafts are read live; an open texture or animation draft is noted, not used.
            const auto draft = s.drafts.constFind(path);
            const bool live = (draft != s.drafts.cend() && draft->type == "model") || s.provisional.contains(path);
            p.source = live ? "open_draft" : "project";
            p.unsavedDraft = draft != s.drafts.cend() && !live;
            if (draft != s.drafts.cend())
                p.revision = draft->revision;
        } else {
            p.source = "resource_stack";
            p.stackIndex = stackRoots.value(candidate.root, -1);
            if (p.stackIndex >= 0) {
                p.label = s.resourceStack[p.stackIndex].label;
                p.verified = s.resourceStack[p.stackIndex].vanilla;
            }
        }
        list.append(p);
    }
    if (list.isEmpty() && s.provisional.contains(path) && s.models.contains(path))
        list.append({"open_draft", {}, s.drafts.value(path).revision});
    return list;
}
Provider SourceSet::provider(const QString &path) const {
    const auto list = providers(path);
    return list.isEmpty() ? Provider{"none"} : list.first();
}
bool SourceSet::usable(const QString &path) const {
    const auto list = catalog.providers(path);
    if (list.isEmpty())
        return s.provisional.contains(path) && s.models.contains(path);
    return list.first().editable || !catalog.assetPath(path).isEmpty();
}

QJsonArray SourceSet::stack() const {
    QJsonArray entries;
    for (qsizetype i = 0; i < s.resourceStack.size(); ++i) {
        const auto &e = s.resourceStack[i];
        QJsonObject row{{"stackIndex", qint64(i)}, {"label", e.label}, {"status", e.status},
                        {"kind", e.vanilla ? "verified_vanilla_resources" : "resource_pack"}};
        if (!e.reason.isEmpty())
            row["reason"] = e.reason;
        entries.append(row);
    }
    return entries;
}
bool SourceSet::stackComplete() const {
    for (const auto &e : s.resourceStack)
        if (e.status != "loaded")
            return false;
    return true;
}

const CacheSelection &SourceSet::textureCache() {
    if (!cache)
        cache = selectTextureCache(s.vanillaCacheRoot, s.version, {}, s.baselineVersion, s.baselineIdentity);
    return *cache;
}
bool SourceSet::vanillaTexture(const QString &path) {
    const auto &selected = textureCache();
    return selected.entry && selected.entry->contains(path);
}

QByteArray SourceSet::read(const QString &path, qint64 limit, QString *why, bool prefix) {
    auto fail = [&](const QString &message) {
        if (why)
            *why = message;
        return QByteArray();
    };
    const auto list = catalog.providers(path);
    std::unique_ptr<QIODevice> device;
    if (!list.isEmpty() && list.first().editable) {
        if (s.provisional.contains(path))
            return fail("The document has not been saved yet.");
        device = scope.open(path);
    } else if (!list.isEmpty()) {
        const auto file = catalog.assetPath(path);
        if (!file.isEmpty()) {
            auto f = std::make_unique<QFile>(file);
            if (f->open(QIODevice::ReadOnly))
                device = std::move(f);
        }
    } else if (vanillaTexture(path)) {
        QByteArray bytes;
        QString error;
        const auto &entry = *textureCache().entry;
        if (!prefix && entry.files.value(path).size > limit)
            return fail("The file exceeds the inspection limit.");
        if (!vanilla::BaselineCache(entry.root).readVerified(entry, path, &bytes, &error))
            return fail("The vanilla texture cache copy failed its integrity check.");
        return prefix ? bytes.left(limit) : bytes;
    } else {
        return fail("No source provides this file.");
    }
    if (!device)
        return fail("The file cannot be opened safely.");
    if (prefix)
        return device->read(limit);
    if (device->size() > limit)
        return fail("The file exceeds the inspection limit.");
    return device->read(limit + 1);
}

QJsonObject SourceSet::summary() {
    const auto &selected = textureCache();
    QJsonObject cacheState{{"status", selected.status}, {"providesModels", false}};
    if (selected.entry) {
        cacheState["identity"] = selected.entry->identity;
        cacheState["selection"] = selected.selection;
    } else if (!selected.reason.isEmpty()) {
        cacheState["reason"] = selected.reason;
    }
    return {{"precedence", QJsonArray{"open_draft", "project", "resource_stack", "vanilla_texture_cache"}},
            {"resourceStack", stack()}, {"resourceStackComplete", stackComplete()},
            {"vanillaModelSource", vanillaModels}, {"modelCatalogComplete", complete},
            {"vanillaTextureCache", cacheState}};
}
} // namespace nova::mcp
