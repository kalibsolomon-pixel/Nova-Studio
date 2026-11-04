// SPDX-License-Identifier: GPL-3.0-or-later
#include "inspection.h"
#include "scope.h"
#include <QBuffer>
#include "protocol.h"
#include "validation.h"
#include "minecraft_animation.h"
#include "project.h"
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <algorithm>

namespace nova::mcp {
QString assetIdentifier(const Snapshot &s, const QString &path) {
    return QString::fromLatin1(QMessageAuthenticationCode::hash(
        path.toUtf8(), s.idKey, QCryptographicHash::Sha256).toHex());
}
namespace {
constexpr int AssetLimit = 60000;
constexpr int JsonLimit = 2 * 1024 * 1024;
QString resolve(const Snapshot &s, const QString &assetId, const std::atomic_bool &cancel) {
    for (const auto &path : s.index.files) {
        if (cancel.load())
            break;
        if (logicalAsset(path) && assetIdentifier(s, path) == assetId)
            return path;
    }
    return {};
}
QString kind(const QString &path) {
    if (path.endsWith(".png"))
        return "texture";
    if (path.endsWith(".png.mcmeta"))
        return "animation";
    if (!mc::Location::fromAsset(path, "models").isEmpty())
        return "model";
    return "resource";
}
QJsonObject entry(const Snapshot &s, const QString &path) {
    const auto &a = s.index.records.value(path);
    return {{"assetId", assetIdentifier(s, path)}, {"path", path}, {"type", kind(path)},
            {"format", QFileInfo(path).suffix().left(24)}, {"category", a.category},
            {"namespace", a.nameSpace}, {"bytes", a.size}};
}
QJsonObject json(QIODevice *file) {
    if (!file || file->size() > JsonLimit)
        return {};
    const auto doc = QJsonDocument::fromJson(file->read(JsonLimit + 1));
    return doc.isObject() ? doc.object() : QJsonObject{};
}
QJsonObject canceled() { return toolError("operation_canceled", "The operation was canceled."); }
bool provisionalAvailable(const Snapshot &s, Scope &scope, const QString &path) {
    return s.provisional.contains(path) && scope.vacant(path) &&
        (path.endsWith(".json") || scope.vacant(path + ".mcmeta"));
}
QJsonObject unsafe() { return toolError("unsafe_asset", "The indexed asset is missing or cannot be opened safely."); }
QString diagnosticText(const QString &text) {
    static const QRegularExpression absolute(R"(([A-Za-z]:[\\/]|\\\\|(?:^|[\s"'(])/[^\s/]))");
    return text.contains(absolute) ? QString("Diagnostic contains a local path.") : text.left(400);
}
QString reference(const QJsonValue &value) {
    const auto ref = value.toString();
    static const QRegularExpression allowed("^(#?[a-z0-9_.-]+)(:[a-z0-9_./-]+|/[a-z0-9_./-]+)?$");
    return ref.size() <= 128 && !ref.contains("..") && allowed.match(ref).hasMatch() ? ref : QString{};
}
QJsonObject animation(const AnimationState &state) {
    QHash<QUuid, qsizetype> indices;
    for (qsizetype i = 0; i < qsizetype(state.frames.size()); ++i)
        indices.insert(state.frames[size_t(i)].id, i);
    QJsonArray timing;
    for (const auto &e : state.entries) {
        if (timing.size() == 256)
            break;
        timing.append(QJsonObject{{"index", qint64(indices.value(e.frame, -1))},
                                  {"ticks", e.ticks > 0 ? e.ticks : state.defaultTicks}});
    }
    return {{"frameCount", qint64(state.frames.size())}, {"entryCount", qint64(state.entries.size())},
            {"defaultTicks", state.defaultTicks}, {"interpolate", state.interpolate},
            {"timing", timing}, {"timingTruncated", state.entries.size() > 256}};
}
QJsonObject metadata(const Snapshot &s, Scope &scope, const QString &path, QIODevice *file,
                     QJsonArray &warnings) {
    const auto type = kind(path);
    if (type == "texture") {
        auto result = s.textures.value(path);
        if (result.isEmpty()) {
            QImageReader reader(file, "PNG");
            reader.setAutoDetectImageFormat(false);
            const auto size = reader.size();
            if (!size.isValid())
                warnings.append("Invalid PNG header.");
            else
                result = {{"width", size.width()}, {"height", size.height()}};
            if (file->seek(24)) {
                const auto header = file->read(2);
                if (size.isValid() && header.size() == 2) {
                    const int colorType = uchar(header[1]);
                    result["bitDepth"] = uchar(header[0]);
                    result["pngColorType"] = colorType;
                    result["alphaChannel"] = colorType == 4 || colorType == 6;
                }
            }
            result["source"] = "disk";
        }
        if (s.index.records.contains(path + ".mcmeta"))
            result["animationAssetId"] = assetIdentifier(s, path + ".mcmeta");
        if (s.animations.contains(path))
            result["liveAnimation"] = true;
        return result;
    }
    if (type == "animation") {
        const auto texture = path.chopped(7);
        auto result = s.animations.value(texture);
        if (result.isEmpty()) {
            const auto source = json(file);
            auto sheet = s.index.records.contains(texture) ? scope.open(texture) : nullptr;
            QImageReader reader(sheet.get(), "PNG");
            reader.setAutoDetectImageFormat(false);
            const auto size = reader.size();
            AnimationState state;
            QString error;
            if (!size.isValid() || size.width() > 4096 || size.height() > 4096 ||
                !parseAnimation(reader.read(), source, s.version, &state, &error))
                warnings.append(error.isEmpty() ? "Invalid metadata or unavailable spritesheet."
                                                : diagnosticText(error));
            else
                result = animation(state);
            result["source"] = "disk";
        }
        if (s.index.records.contains(texture))
            result["textureAssetId"] = assetIdentifier(s, texture);
        return result;
    }
    if (type == "model") {
        const bool live = s.models.contains(path);
        const auto source = live ? s.models.value(path) : json(file);
        if (source.isEmpty())
            warnings.append("Empty, oversized or invalid model JSON.");
        const auto parent = reference(source["parent"]);
        QJsonObject textures;
        const auto raw = source["textures"].toObject();
        for (auto it = raw.begin(); it != raw.end() && textures.size() < 64; ++it)
            if (!reference(it.key()).isEmpty() && !reference(it.value()).isEmpty())
                textures[it.key()] = reference(it.value());
        const auto elements = source["elements"].toArray();
        int faceCount = 0, explicitUv = 0;
        for (qsizetype i = 0; i < std::min<qsizetype>(elements.size(), 4096); ++i) {
            const auto faces = elements[i].toObject()["faces"].toObject();
            for (const auto &name : {"north", "south", "west", "east", "up", "down"}) {
                if (faces[name].isObject()) {
                    ++faceCount;
                    explicitUv += faces[name].toObject()["uv"].isArray();
                }
            }
        }
        QJsonArray display;
        const auto transforms = source["display"].toObject();
        for (const auto &name : {"thirdperson_righthand", "thirdperson_lefthand",
                                "firstperson_righthand", "firstperson_lefthand", "gui",
                                "head", "ground", "fixed"})
            if (transforms[name].isObject())
                display.append(name);
        return {{"source", live ? "live" : "disk"}, {"modelType", "minecraft_json"},
                {"parent", parent}, {"textures", textures}, {"texturesTruncated", raw.size() > 64},
                {"elementCount", elements.size()}, {"faceCount", faceCount},
                {"explicitUvFaceCount", explicitUv}, {"facesTruncated", elements.size() > 4096},
                {"displayContexts", display}};
    }
    return {};
}
QStringList targets(const Snapshot &s, const QJsonArray &ids, const std::atomic_bool &cancel) {
    if (ids.isEmpty())
        return {};
    if (auto cache = s.relationships) {
        if (!cache->identifierRecords.isSharedWith(s.index.records)) {
            QHash<QString, QString> identifiers;
            identifiers.reserve(s.index.files.size());
            for (const auto &path : s.index.files) {
                if (cancel.load())
                    return {};
                if (logicalAsset(path))
                    identifiers.insert(assetIdentifier(s, path), path);
            }
            cache->identifiers = std::move(identifiers);
            cache->identifierRecords = s.index.records;
        }
        QStringList result;
        for (const auto &id : ids)
            result.append(cache->identifiers.value(id.toString()));
        return result;
    }
    QSet<QString> remaining;
    for (const auto &id : ids)
        remaining.insert(id.toString());
    QHash<QString, QString> paths;
    for (const auto &path : s.index.files) {
        if (cancel.load() || remaining.isEmpty())
            break;
        if (!logicalAsset(path))
            continue;
        const auto id = assetIdentifier(s, path);
        if (remaining.remove(id))
            paths.insert(id, path);
    }
    QStringList result;
    for (const auto &id : ids)
        result.append(paths.value(id.toString()));
    return result;
}

QJsonObject context(const Snapshot &s, Scope &scope, const QJsonObject &args,
                    const QStringList &paths, const std::shared_ptr<std::atomic_bool> &cancel) {
    const auto available = [&](const QString &path) {
        return s.provisional.contains(path) ? provisionalAvailable(s, scope, path)
                                            : bool(scope.open(path));
    };
    for (const auto &path : paths)
        if (path.isEmpty() || !available(path))
            return toolError("asset_not_found", "A requested asset is unknown or unavailable.");

    auto cache = s.relationships;
    if (!cache)
        cache = std::make_shared<RelationshipCache>();
    const auto &diskRecords = s.provisional.isEmpty() ? s.index.records : s.diskRecords;
    if (!cache->library || cache->generation != s.generation || cache->version != s.version ||
        !cache->records.isSharedWith(diskRecords)) {
        auto library = std::make_shared<mc::Library>(s.version);
        auto diskIndex = s.index;
        diskIndex.records = diskRecords;
        for (const auto &path : s.provisional)
            diskIndex.files.removeOne(path);
        library->update(s.root, diskIndex, {}, cancel, [&](const QString &path) {
            return scope.open(path);
        });
        if (cancel->load())
            return canceled();
        cache->generation = s.generation;
        cache->version = s.version;
        cache->records = diskRecords;
        cache->library = std::move(library);
    }
    auto library = *cache->library;
    for (auto it = s.models.cbegin(); it != s.models.cend(); ++it) {
        if (!s.index.records.contains(it.key()) || !available(it.key()))
            continue;
        if (s.provisional.contains(it.key()))
            library.setProvisionalSource(it.key(), it.value());
        else
            library.previewSource(it.key(), it.value());
    }

    constexpr int nodeLimit = 64;
    constexpr int edgeLimit = 128;
    QList<QPair<QString, int>> queue;
    QSet<QString> seen;
    QHash<QString, bool> verified;
    auto enqueue = [&](const QString &path, int depth) {
        if (!seen.contains(path) && seen.size() < nodeLimit) {
            seen.insert(path);
            queue.append({path, depth});
        }
    };
    for (const auto &path : paths)
        enqueue(path, 0);
    QJsonArray assets, references, warnings;
    constexpr int adjacencyLimit = 256;
    QSet<QPair<QString, QString>> emitted;
    QMap<QString, QString> parents;
    const int depth = args["depth"].toInt(1);
    const auto filters = args["relationships"].toArray();
    auto allows = [&](const QString &type) { return filters.isEmpty() || filters.contains(type); };
    bool truncated = !library.diagnostics.isEmpty();
    auto safe = [&](const QString &path) {
        if (!verified.contains(path))
            verified.insert(path, s.index.records.contains(path) && available(path));
        return verified.value(path);
    };
    for (qsizetype i = 0; i < queue.size(); ++i) {
        if (cancel->load())
            return canceled();
        const auto [path, level] = queue[i];
        if (!safe(path))
            continue;
        auto summary = entry(s, path);
        summary["depth"] = level;
        const auto source = library.source(path);
        if (kind(path) == "model" && source.isEmpty() && warnings.size() < nodeLimit)
            warnings.append(QJsonObject{{"assetId", summary["assetId"]}, {"code", "model_source_unavailable"}});
        assets.append(summary);
        if (level == depth)
            continue;
        auto edge = [&](const QString &from, const QString &to, const QString &type, bool reverse) {
            if (!allows(type) || emitted.contains({from, to}) || cancel->load())
                return;
            if (references.size() >= edgeLimit) {
                truncated = true;
                return;
            }
            if (!safe(from))
                return;
            const bool present = s.index.records.contains(to);
            if (present && !safe(to)) {
                if (warnings.size() < nodeLimit)
                    warnings.append(QJsonObject{{"assetId", assetIdentifier(s, from)}, {"code", "unsafe_dependency"}});
                return;
            }
            emitted.insert({from, to});
            if (present && type == "parent")
                parents.insert(from, to);
            QJsonObject row{{"fromAssetId", assetIdentifier(s, from)}, {"type", type},
                            {"status", s.provisional.contains(to) ? "provisional" : present ? "indexed" : "missing_from_pack"}};
            if (present)
                row["toAssetId"] = assetIdentifier(s, to);
            else if (logicalAsset(to))
                row["targetPath"] = to;
            references.append(row);
            if (present) {
                const auto &next = reverse ? from : to;
                if (!seen.contains(next) && seen.size() == nodeLimit)
                    truncated = true;
                enqueue(next, level + 1);
            }
        };
        auto dependencies = library.dependencies(path);
        auto dependents = library.dependents(path, false);
        if (kind(path) == "texture") {
            if (s.index.records.contains(path + ".mcmeta"))
                edge(path, path + ".mcmeta", "animation", false);
            if (s.animations.contains(path)) {
                summary["liveAnimation"] = true;
                assets[assets.size() - 1] = summary;
            }
        } else if (kind(path) == "animation") {
            edge(path.chopped(7), path, "animation", true);
            if (!s.index.records.contains(path.chopped(7)) && warnings.size() < nodeLimit)
                warnings.append(QJsonObject{{"assetId", summary["assetId"]}, {"code", "missing_spritesheet"}});
        }
        int examined = 0;
        for (const auto &to : dependencies) {
            if (++examined > adjacencyLimit || references.size() >= edgeLimit || cancel->load()) {
                truncated = true;
                break;
            }
            edge(path, to, kind(to) == "texture" ? "texture" : kind(path) == "model" ? "parent" : "model", false);
        }
        examined = 0;
        for (const auto &from : dependents) {
            if (++examined > adjacencyLimit || references.size() >= edgeLimit || cancel->load()) {
                truncated = true;
                break;
            }
            edge(from, path, kind(path) == "texture" ? "texture" : kind(from) == "model" ? "parent" : "model", true);
        }
        if (kind(path) == "model") {
            const auto bindings = source["textures"].toObject();
            examined = 0;
            for (auto it = bindings.begin(); it != bindings.end(); ++it) {
                if (++examined > adjacencyLimit || references.size() >= edgeLimit || cancel->load()) {
                    truncated = true;
                    break;
                }
                const auto value = it->isObject() ? it->toObject()["sprite"] : *it;
                const auto ref = reference(value);
                const bool alias = ref.startsWith('#');
                if (!allows("alias"))
                    break;
                const bool literal = mc::Location::parse(ref).valid();
                QJsonObject row{{"fromAssetId", summary["assetId"]}, {"type", "alias"},
                                {"status", alias ? "alias" : literal ? "binding" : "invalid_reference"}};
                const auto name = reference(it.key());
                if (!name.isEmpty())
                    row["binding"] = name;
                if (alias)
                    row["reference"] = ref;
                else if (literal) {
                    const auto location = mc::Location::parse(ref);
                    const auto target = location.asset("textures");
                    if (!s.index.records.contains(target) || safe(target)) {
                        row["reference"] = ref;
                        row["namespace"] = location.nameSpace;
                    } else {
                        row["status"] = "unsafe_dependency";
                    }
                }
                references.append(row);
            }
            const auto parent = source["parent"];
            if (source.contains("parent") && !mc::Location::parse(parent.toString()).valid() &&
                allows("parent") && warnings.size() < nodeLimit)
                warnings.append(QJsonObject{{"assetId", summary["assetId"]}, {"code", "invalid_parent"}});
        }
    }
    QSet<QString> checkedParents;
    for (const auto &from : parents.keys()) {
        QSet<QString> chain;
        auto cursor = from;
        while (parents.contains(cursor) && !checkedParents.contains(cursor)) {
            if (chain.contains(cursor)) {
                if (warnings.size() < nodeLimit)
                    warnings.append(QJsonObject{{"assetId", assetIdentifier(s, cursor)}, {"code", "parent_cycle"}});
                break;
            }
            chain.insert(cursor);
            cursor = parents.value(cursor);
        }
        checkedParents.unite(chain);
    }
    return cancel->load() ? canceled() : QJsonObject{{"generation", s.generation},
        {"projectId", s.projectId}, {"assets", assets}, {"references", references},
        {"warnings", warnings}, {"truncated", truncated}};
}

}

QJsonObject summarizeAnimation(const AnimationState &state) { return animation(state); }

QJsonObject inspect(const Snapshot &s, const QString &tool, const QJsonObject &args,
                    const std::shared_ptr<std::atomic_bool> &cancel) {
    if (cancel->load())
        return canceled();
    Scope scope(s.root, s.rootIdentity);
    if (scope.identity().isEmpty())
        return toolError("stale_project", "The authorized project directory changed or is unavailable.");
    if (s.index.files.size() > AssetLimit)
        return toolError("index_limit", "The project exceeds the 60,000-entry inspection budget.");
    if (tool == "nova.project.info") {
        auto file = s.index.records.contains("pack.mcmeta") ? scope.open("pack.mcmeta") : nullptr;
        const auto pack = json(file.get())["pack"].toObject();
        QSet<QString> namespaces;
        int count = 0;
        for (const auto &path : s.index.files) {
            if (cancel->load())
                return canceled();
            if (logicalAsset(path)) {
                ++count;
                const auto ns = s.index.records.value(path).nameSpace;
                if (!ns.isEmpty() && !namespaces.contains(ns) && namespaces.size() < 100 &&
                    scope.open(path))
                    namespaces.insert(ns);
            }
        }
        auto names = namespaces.values();
        names.sort();
        auto format = [&](const char *key, bool maximum) {
            const auto value = PackFormat::parse(pack[key], maximum);
            return value ? value->json() : QJsonValue();
        };
        return {{"projectId", s.projectId}, {"generation", s.generation},
                {"name", QFileInfo(s.root).fileName()}, {"minecraftVersion", s.version},
                {"packFormat", format("pack_format", false)}, {"minFormat", format("min_format", false)},
                {"maxFormat", format("max_format", true)}, {"assetCount", count},
                {"indexRevision", s.indexRevision},
                {"namespaces", QJsonArray::fromStringList(names)}, {"validationAvailable", true}};
    }
    if (args["generation"] != s.generation)
        return toolError("stale_project", "The project generation is no longer authorized.");
    if (tool == "nova.assets.list") {
        const auto category = args["category"].toString();
        const auto ns = args["namespace"].toString();
        const auto search = args["search"].toString();
        const int offset = args["offset"].toInt();
        const int limit = args["limit"].toInt(50);
        QJsonArray assets;
        int matched = 0;
        int examined = 0;
        bool more = false;
        for (const auto &path : s.index.files) {
            if (cancel->load())
                return canceled();
            if (!logicalAsset(path))
                continue;
            const auto &a = s.index.records.value(path);
            if ((!category.isEmpty() && category != a.category) ||
                (!ns.isEmpty() && ns != a.nameSpace) ||
                (!search.isEmpty() && !path.contains(search, Qt::CaseInsensitive)))
                continue;
            if (matched++ < offset)
                continue;
            if (examined == limit) {
                more = true;
                break;
            }
            ++examined;
            if (s.provisional.contains(path) ? provisionalAvailable(s, scope, path) : bool(scope.open(path))) {
                auto row = entry(s, path);
                if (s.provisional.contains(path))
                    row["provisional"] = true;
                assets.append(row);
            }
        }
        return {{"generation", s.generation}, {"assets", assets},
                {"nextOffset", more ? QJsonValue(offset + limit) : QJsonValue()}};
    }
    const auto ids = args["assetIds"].toArray();
    const auto paths = targets(s, ids, *cancel);
    if (cancel->load())
        return canceled();
    if (tool == "nova.assets.context")
        return context(s, scope, args, paths, cancel);
    if (tool == "nova.assets.inspect_batch" || tool == "nova.workflow.prepare") {
        const bool prepare = tool == "nova.workflow.prepare";
        const auto types = args["types"].toArray();
        QJsonArray results;
        QHash<QString, QJsonObject> rows;
        for (qsizetype i = 0; i < ids.size(); ++i) {
            if (cancel->load())
                return canceled();
            const auto id = ids[i].toString();
            if (!rows.contains(id)) {
                const auto &path = paths[i];
                QJsonObject row;
                const bool provisional = s.provisional.contains(path);
                auto file = path.isEmpty() || provisional ? nullptr : scope.open(path);
                if (path.isEmpty())
                    row = toolError("asset_not_found", "The asset identifier is unknown or stale.");
                else if (!file && !provisionalAvailable(s, scope, path))
                    row = unsafe();
                else if (!types.isEmpty() && !types.contains(kind(path)))
                    row = toolError("invalid_asset_type", "The asset is excluded by the type filter.");
                else {
                    row = entry(s, path);
                    QJsonArray warnings;
                    if (!prepare && args["detail"].toString() != "summary")
                        row["metadata"] = metadata(s, scope, path, file.get(), warnings);
                    if (prepare) {
                        const auto revision = s.revisions.value(path);
                        row["documentOpen"] = !revision.isEmpty();
                        row["revision"] = revision.isEmpty() ? QJsonValue() : QJsonValue(revision);
                        if (!revision.isEmpty()) {
                            row["editTools"] = kind(path) == "model"
                                ? QJsonArray{"nova.model.edit", "nova.uv.edit"}
                                : QJsonArray{"nova.texture.edit", "nova.layer.edit", "nova.animation.edit"};
                        }
                    }
                    row["warnings"] = warnings;
                }
                row["assetId"] = id;
                rows.insert(id, row);
            }
            results.append(rows.value(id));
        }
        auto result = QJsonObject{{"generation", s.generation}, {"projectId", s.projectId}, {"results", results}};
        if (prepare) {
            result["constraints"] = QJsonObject{{"maxOperations", 8}, {"maxDocuments", 1},
                {"maxTextureDimension", 256}, {"maxTexturePixelBytes", 4 * 1024 * 1024},
                {"requiresOpenDocument", true}, {"requiresRevision", true},
                {"inspectionTool", "nova.edit.capabilities"}, {"validationTool", "nova.workflow.validate_changes"}};
        }
        return cancel->load() ? canceled() : result;
    }
    const auto requested = args["assetId"].toString();
    const auto path = requested.isEmpty() ? QString{} : resolve(s, requested, *cancel);
    if (cancel->load())
        return canceled();
    if (!requested.isEmpty() && path.isEmpty())
        return toolError("asset_not_found", "The asset identifier is unknown or stale.");
    const bool changes = tool == "nova.workflow.validate_changes";
    if (changes || tool == "nova.pack.validate") {
        QStringList selected;
        QJsonArray revisions;
        if (changes) {
            for (qsizetype i = 0; i < paths.size(); ++i) {
                const auto &target = paths[i];
                if (target.isEmpty())
                    return toolError("asset_not_found", "A target asset identifier is unknown or stale.");
                if (s.provisional.contains(target)) {
                    if (!provisionalAvailable(s, scope, target))
                        return unsafe();
                    if (s.models.contains(target))
                        selected.append(target);
                } else {
                    if (!scope.open(target))
                        return unsafe();
                    selected.append(target);
                }
                if (args["scope"] == "animation_sidecars" && kind(target) == "texture" &&
                    s.index.records.contains(target + ".mcmeta"))
                    selected.append(target + ".mcmeta");
                const auto revision = s.revisions.value(target);
                revisions.append(QJsonObject{{"assetId", ids[i]},
                    {"revision", revision.isEmpty() ? QJsonValue() : QJsonValue(revision)}});
            }
            for (const auto &value : args["expectedRevisions"].toArray()) {
                const auto expected = value.toObject();
                qsizetype index = 0;
                while (index < ids.size() && ids[index] != expected["assetId"])
                    ++index;
                if (index == ids.size())
                    return toolError("invalid_arguments", "Expected revisions must refer to target assets.");
                if (expected["revision"] != s.revisions.value(paths[index]))
                    return toolError("revision_conflict", "A target document changed or is no longer open.");
            }
            selected.removeDuplicates();
        } else if (!path.isEmpty()) {
            selected.append(path);
        }
        Index editable;
        for (const auto &candidate : s.index.files) {
            if (cancel->load())
                return canceled();
            if (logicalAsset(candidate) && (!s.provisional.contains(candidate) ||
                (changes && s.models.contains(candidate) && provisionalAvailable(s, scope, candidate)))) {
                editable.files.append(candidate);
                editable.records.insert(candidate, s.index.records.value(candidate));
            }
        }
        mc::Library::AssetReader reader = [&](const QString &relative) {
            if (cancel->load() || !editable.records.contains(relative))
                return std::unique_ptr<QIODevice>{};
            if (s.provisional.contains(relative)) {
                auto buffer = std::make_unique<QBuffer>();
                buffer->setData(QJsonDocument(s.models.value(relative)).toJson(QJsonDocument::Compact));
                buffer->open(QIODevice::ReadOnly);
                return std::unique_ptr<QIODevice>(std::move(buffer));
            }
            return scope.open(relative);
        };
        if (!path.isEmpty() && !reader(path))
            return unsafe();
        mc::ValidationReport report;
        if (!changes || !selected.isEmpty()) {
            mc::Library library(s.version);
            library.update(s.root, editable, {}, cancel, reader);
            if (cancel->load())
                return canceled();
            QMap<QString, QJsonObject> models;
            for (auto it = s.models.begin(); it != s.models.end(); ++it)
                if (editable.records.contains(it.key()) && reader(it.key()))
                    models.insert(it.key(), it.value());
            report = mc::ValidationEngine().run(
                s.root, editable, std::move(library), selected,
                {}, cancel, {}, models, reader);
        }
        if (cancel->load() || report.canceled)
            return canceled();
        if (changes) {
            auto liveTargets = selected;
            for (const auto &target : paths)
                if (s.provisional.contains(target) && !liveTargets.contains(target)) {
                    liveTargets.append(target);
                    ++report.files;
                    ++report.inspected;
                }
            liveTargets.removeDuplicates();
            for (const auto &target : liveTargets) {
                if (cancel->load())
                    return canceled();
                const auto doc = s.animationDocuments.value(target);
                if (!doc)
                    continue;
                QString error;
                const bool valid = doc->validateAnimation(doc->animationState(), &error);
                mc::Diagnostic diagnostic;
                diagnostic.file = target;
                const bool animated = doc->animated();
                diagnostic.field = animated ? "/animation" : "";
                diagnostic.code = animated
                    ? (valid ? "live_animation_checked" : "live_animation_invalid")
                    : (valid ? "live_texture_checked" : "live_texture_invalid");
                diagnostic.severity = valid ? mc::Severity::Information : mc::Severity::Error;
                diagnostic.message = valid ? (animated ? QString("Validated native frame structure and timing.")
                                                       : QString("Validated native texture layers.")) : error;
                report.diagnostics.append(diagnostic);
            }
        }
        QJsonArray diagnostics;
        QJsonObject counts;
        QHash<QString, QJsonValue> safeIds;
        auto safeId = [&](const QString &file) -> QJsonValue {
            if (!safeIds.contains(file))
                safeIds.insert(file, (s.provisional.contains(file) ||
                    (editable.records.contains(file) && scope.open(file)))
                    ? QJsonValue(assetIdentifier(s, file)) : QJsonValue());
            return safeIds.value(file);
        };
        const int limit = args["limit"].toInt(100);
        for (const auto &d : report.diagnostics) {
            const auto severity = mc::severityName(d.severity);
            counts[severity] = counts[severity].toInt() + 1;
            if (diagnostics.size() >= limit)
                continue;
            if (cancel->load())
                return canceled();
            QJsonObject diagnostic{{"severity", severity}, {"code", d.code},
                {"assetId", editable.records.contains(d.file) ? QJsonValue(assetIdentifier(s, d.file)) : QJsonValue()},
                {"message", diagnosticText(d.message)}, {"field", d.field.left(256)}, {"line", d.line}};
            if (changes) {
                diagnostic["assetId"] = safeId(d.file);
                QJsonArray affected;
                for (const auto &file : d.affected.first(std::min<qsizetype>(8, d.affected.size()))) {
                    const auto id = safeId(file);
                    if (!id.isNull())
                        affected.append(id);
                }
                diagnostic["affectedAssetIds"] = affected;
            }
            diagnostics.append(diagnostic);
        }
        QJsonObject result{{"generation", s.generation}, {"diagnostics", diagnostics}, {"counts", counts},
                {"files", report.files}, {"inspected", report.inspected},
                {"milliseconds", report.milliseconds},
                {"truncated", report.truncated || report.diagnostics.size() > limit}};
        if (changes) {
            result["projectId"] = s.projectId;
            result["revisions"] = revisions;
            result["validationSources"] = QJsonObject{{"models", "live_when_open"},
                {"textures", "disk_headers_or_provisional_native_layers"}, {"animation_sidecars", "disk"},
                {"openAnimations", "native_frame_and_timing_checks"}};
        }
        return result;
    }
    const auto type = kind(path);
    const bool textureAnimation = tool == "nova.animation.inspect" && type == "texture";
    if (!textureAnimation && tool != "nova.asset.inspect" && tool != "nova." + type + ".inspect")
        return toolError("invalid_arguments", "The asset does not match the requested inspection type.");
    auto file = s.provisional.contains(path) ? nullptr : scope.open(path);
    if (!file && !(provisionalAvailable(s, scope, path) && (s.textures.contains(path) || s.models.contains(path))))
        return unsafe();
    auto result = entry(s, path);
    QJsonArray warnings;
    if (textureAnimation) {
        const auto meta = path + ".mcmeta";
        if (s.animations.contains(path)) {
            auto animation = s.animations.value(path);
            animation["textureAssetId"] = assetIdentifier(s, path);
            result["metadata"] = animation;
        } else if (auto sidecar = s.index.records.contains(meta) ? scope.open(meta) : nullptr) {
            result["metadata"] = metadata(s, scope, meta, sidecar.get(), warnings);
        } else {
            return toolError("asset_not_found", "The texture has no safely accessible animation.");
        }
    } else {
        result["metadata"] = metadata(s, scope, path, file.get(), warnings);
    }
    result["warnings"] = warnings;
    return cancel->load() ? canceled() : result;
}
}
