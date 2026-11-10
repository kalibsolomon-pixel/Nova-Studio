// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// MCP Phase 3: the sources a read-only relationship or validation request may use, in Nova's
// precedence order: the authorized project's files (open drafts first), then the resource stack
// the user configured for the project and version (Models › Resource stack), then Nova's verified
// vanilla *texture* cache. The cache holds textures only and is never a model source. Nothing here
// accepts a path from an agent, crawls the disk or downloads anything.
#include "models.h"
#include <QJsonArray>
#include <QJsonObject>
#include <atomic>
#include <memory>
#include <optional>

namespace nova::vanilla {
struct BaselineEntry;
}
namespace nova::mcp {
struct Snapshot;
class Scope;

// Where one resource path is provided. "source" is open_draft (a model read from its live source),
// project, resource_stack, vanilla_texture_cache or none.
struct Provider {
    QString source, label, revision;
    int stackIndex = -1;   // 0-based position in the configured resource stack
    bool verified = false; // a verified vanilla resource source, or the integrity-checked cache
    // A texture or animation open with unsaved changes: relationship data still comes from the saved file.
    bool unsavedDraft = false;
    QJsonObject json() const;
};

// Selection of a verified vanilla texture-cache entry for one version (shared with nova.vanilla.*).
struct CacheSelection {
    QString status;    // verified, not_cached, invalid, unsupported_version, identity_not_cached
    QString selection; // requested_identity, project_baseline, latest_verified
    QString reason;    // path-free explanation when status is not verified
    int entries = 0, damaged = 0;
    std::shared_ptr<const vanilla::BaselineEntry> entry;
};
CacheSelection selectTextureCache(const QString &cacheRoot, const QString &version, const QString &identity,
                                  const QString &projectVersion, const QString &projectIdentity);
bool supportedVersion(const QString &version);

class SourceSet {
  public:
    SourceSet(const Snapshot &snapshot, Scope &scope, const std::shared_ptr<std::atomic_bool> &cancel);
    ~SourceSet();
    bool canceled() const { return stopped; }
    mc::Library &library() { return catalog; }
    // Every provider of `path`, first = the one Minecraft would use. Empty when no source has it.
    QList<Provider> providers(const QString &path) const;
    Provider provider(const QString &path) const;
    bool provided(const QString &path) const { return !providers(path).isEmpty(); }
    // Provided and readable as Nova's resolver reads it (a verified stack file must still match its hash).
    bool usable(const QString &path) const;
    // Whether a verified vanilla resource source (with models, item definitions, blockstates) is loaded.
    bool vanillaModelSource() const { return vanillaModels; }
    // False when the model catalog budget (8192 JSON files / 32 MiB) cut the sources short: model data
    // is then incomplete and absent models cannot be called missing.
    bool catalogComplete() const { return complete; }
    Scope &projectScope() { return scope; }
    // Configured resource-stack sources, loaded or not (index, kind, status, label, reason).
    QJsonArray stack() const;
    bool stackComplete() const;
    // The verified vanilla texture cache for the project's version (lazily selected).
    const CacheSelection &textureCache();
    bool vanillaTexture(const QString &path);
    QJsonObject summary(); // sources block of a result
    QString version() const;
    // The bytes of `path` from its first provider (cache content SHA-256 verified); empty with `why`
    // when unavailable or larger than `limit`. `prefix` returns only the first `limit` bytes of any size.
    QByteArray read(const QString &path, qint64 limit, QString *why = nullptr, bool prefix = false);

  private:
    const Snapshot &s;
    Scope &scope;
    mc::Library catalog;
    QMap<QString, int> stackRoots; // canonical root -> stack index
    bool vanillaModels = false, stopped = false, complete = true;
    std::optional<CacheSelection> cache;
};
} // namespace nova::mcp
