// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// MCP Phase 3: how a resource-pack asset is selected and used (nova.asset.relationships), and the
// relationship-based checks nova.pack.validate adds. Every relationship carries its evidence and the
// source that provided it; "confirmed" means established from resolved data, "possible" is a naming
// suggestion, and anything Nova cannot establish from the available sources is reported as such.
#include <QJsonArray>
#include <QJsonObject>
#include <atomic>
#include <memory>

namespace nova::mcp {
struct Snapshot;
class Scope;
class SourceSet;
constexpr int RelationshipLimit = 200, RelationshipFaceLimit = 64, RowFaceLimit = 32, ParentChainLimit = 64;
// Serialized bytes of relationship rows (or validation findings) per result. A result travels twice in
// one line (text block and structured content) under the 512 KiB response limit; the model detail
// (at most 64 faces and 64 variables) and the sources block stay well under the remaining margin.
constexpr qsizetype RelationshipBytes = 160 * 1024;
QJsonObject relationships(const Snapshot &, Scope &, const QJsonObject &arguments,
                          const std::shared_ptr<std::atomic_bool> &cancel);
// Findings for nova.pack.validate beyond the per-file engine: resource stack availability, draft/disk
// conflicts, stale index entries, unreferenced textures, UV ranges and companion suggestions.
// `only` restricts per-asset checks to one path. Limitations explain what could not be checked.
QJsonArray relationshipFindings(const Snapshot &, SourceSet &, const QString &only,
                                const std::shared_ptr<std::atomic_bool> &cancel, QJsonArray &limitations);
// The resource kind of a path: texture, animation_metadata, model, item_definition, blockstate,
// equipment, atlas, pack_metadata or other.
QString resourceKind(const QString &path);
} // namespace nova::mcp
