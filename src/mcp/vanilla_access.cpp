// SPDX-License-Identifier: GPL-3.0-or-later
#include "vanilla_access.h"
#include "inspection.h"
#include "protocol.h"
#include "sources.h"
#include "vanilla/cache.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>

namespace nova::mcp {
namespace {
constexpr auto Prefix = "assets/minecraft/textures/";
const QString Workflow = "Nova never downloads from an MCP request. Ask the user to add the version in Nova "
                         "(Resources › Vanilla Baseline › Use Vanilla Baseline for This Project…, or File › New "
                         "Resource Pack… with vanilla textures); Nova asks before downloading.";

// The verified entry for the request, or a tool error explaining exactly why there is none.
QJsonObject select(const Snapshot &s, const QJsonObject &args, CacheSelection &out) {
    const auto version = args["version"].toString();
    out = selectTextureCache(s.vanillaCacheRoot, version, args["identity"].toString(), s.baselineVersion,
                             s.baselineIdentity);
    QJsonObject details{{"pointer", "/version"}, {"version", version.left(16)}, {"status", out.status}};
    if (out.status == "unsupported_version")
        return toolError("unsupported_version", out.reason, details);
    if (out.status == "not_cached") {
        details["nextStep"] = Workflow;
        return toolError("vanilla_not_cached", out.reason + ' ' + Workflow, details);
    }
    if (out.status == "identity_not_cached") {
        details["pointer"] = "/identity";
        return toolError("vanilla_not_cached", out.reason + ' ' + Workflow, details);
    }
    if (out.status == "invalid") {
        details["entries"] = out.entries;
        details["damaged"] = out.damaged;
        details["nextStep"] = "Nova did not use the damaged entry and did not change it. Ask the user to repair "
                              "or remove it in Nova (Resources › Vanilla Baseline › Manage Cached Baselines…).";
        return toolError("vanilla_cache_invalid", out.reason, details);
    }
    return {};
}
QJsonObject identity(const Snapshot &s, const CacheSelection &c, const QString &version) {
    const auto &e = *c.entry;
    const auto client = e.source["client"].toObject();
    const auto index = e.source["asset_index"].toObject();
    return {{"projectId", s.projectId}, {"generation", s.generation}, {"version", version},
            {"identity", e.identity}, {"selection", c.selection}, {"cached", e.created.toUTC().toString(Qt::ISODate)},
            {"projectVersion", s.version}, {"matchesProjectVersion", s.version == version},
            {"provenance", QJsonObject{{"edition", "java"}, {"publisher", "Mojang official metadata"},
                {"clientSha1", client["sha1"]}, {"clientSize", client["size"]},
                {"assetIndexId", index["id"]}, {"assetIndexSha1", index["sha1"]},
                {"versionJsonSha1", e.source["version_sha1"]}}}};
}
// Width and height from the PNG IHDR chunk of a cache object, without decoding pixels.
std::optional<QSize> header(const vanilla::BaselineEntry &entry, const QString &relative) {
    const vanilla::BaselineCache cache(entry.root);
    const auto path = entry.objectPath(relative);
    if (path.isEmpty() || !cache.safePath(path))
        return std::nullopt;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const auto bytes = file.read(24);
    if (bytes.size() != 24 || !bytes.startsWith("\x89PNG\r\n\x1a\n") || bytes.mid(12, 4) != "IHDR")
        return std::nullopt;
    const auto width = qFromBigEndian<quint32>(bytes.constData() + 16);
    const auto height = qFromBigEndian<quint32>(bytes.constData() + 20);
    if (!width || !height || width > 65535 || height > 65535)
        return std::nullopt;
    return QSize(int(width), int(height));
}
QJsonValue dimension(const std::optional<QSize> &size, bool width) {
    return size ? QJsonValue(width ? size->width() : size->height()) : QJsonValue();
}
} // namespace

QJsonObject vanillaList(const Snapshot &s, const QJsonObject &args, const std::shared_ptr<std::atomic_bool> &cancel) {
    CacheSelection cache;
    if (const auto error = select(s, args, cache); !error.isEmpty())
        return error;
    const auto &entry = *cache.entry;
    const auto prefix = args["prefix"].toString();
    const auto search = args["search"].toString();
    const bool animatedOnly = args["animated"].toBool();
    const int offset = args["offset"].toInt();
    const int limit = args["limit"].toInt(50);
    QJsonArray textures;
    int matched = 0;
    for (auto it = entry.files.cbegin(); it != entry.files.cend(); ++it) {
        if (cancel->load())
            return toolError("operation_canceled", "The operation was canceled.");
        const auto &path = it.key();
        if (!path.endsWith(".png") || !path.mid(qsizetype(strlen(Prefix))).startsWith(prefix) ||
            (!search.isEmpty() && !path.contains(search, Qt::CaseInsensitive)))
            continue;
        const bool animation = entry.contains(path + ".mcmeta");
        if (animatedOnly && !animation)
            continue;
        if (matched++ < offset || textures.size() >= limit)
            continue;
        const auto size = header(entry, path);
        textures.append(QJsonObject{{"path", path}, {"sha256", it->sha256}, {"bytes", it->size},
            {"origin", it->origin}, {"width", dimension(size, true)}, {"height", dimension(size, false)},
            {"animationMetadata", animation}, {"projectOverride", s.index.records.contains(path)}});
    }
    auto result = identity(s, cache, args["version"].toString());
    result["textures"] = textures;
    result["matched"] = matched;
    result["entryFiles"] = qint64(entry.files.size());
    result["nextOffset"] = offset + textures.size() < matched ? QJsonValue(offset + textures.size()) : QJsonValue();
    result["verification"] = QJsonObject{{"manifest", "digest_verified"},
        {"objects", "present_with_recorded_sizes"}, {"dimensions", "png_header_unverified"},
        {"content", "sha256_verified_by_nova.vanilla.texture"}};
    return result;
}

QJsonObject vanillaTexture(const Snapshot &s, const QJsonObject &args) {
    if (!vanilla::validBaselinePath(args["path"].toString()))
        return toolError("invalid_arguments", "Invalid argument at /path: expected a cached texture path such as "
                         "assets/minecraft/textures/block/stone.png.", {{"pointer", "/path"}});
    CacheSelection cache;
    if (const auto error = select(s, args, cache); !error.isEmpty())
        return error;
    const auto &entry = *cache.entry;
    const auto path = args["path"].toString();
    const auto version = args["version"].toString();
    if (!entry.contains(path))
        return toolError("texture_not_in_cache",
            QString("%1 is not a texture of the verified Java %2 cache entry. Use nova.vanilla.list to find "
                    "cached paths.").arg(path, version), {{"pointer", "/path"}, {"version", version}});
    const vanilla::BaselineCache reader(entry.root);
    QByteArray bytes;
    QString why;
    if (!reader.readVerified(entry, path, &bytes, &why))
        return toolError("vanilla_integrity_failed",
            QString("The cached copy of %1 failed its SHA-256 check, so Nova did not return it and did not "
                    "change the cache. Ask the user to repair the baseline in Nova (Resources › Vanilla Baseline › "
                    "Manage Cached Baselines…).").arg(path), {{"pointer", "/path"}, {"version", version}});
    const auto size = header(entry, path);
    const auto file = entry.files.value(path);
    const bool image = args["include"].toString("image") == "image";
    if (image && bytes.size() > VanillaImageLimit)
        return toolError("texture_too_large",
            QString("%1 is %2 bytes; nova.vanilla.texture returns at most %3 bytes of PNG inline. Call it again "
                    "with include \"metadata\" for its dimensions, hash and provenance without the image.")
                .arg(path).arg(bytes.size()).arg(VanillaImageLimit),
            {{"pointer", "/include"}, {"bytes", qint64(bytes.size())}, {"maxImageBytes", VanillaImageLimit},
             {"width", dimension(size, true)}, {"height", dimension(size, false)}, {"alternative", "metadata"}});
    auto result = identity(s, cache, version);
    result["path"] = path;
    result["sha256"] = file.sha256;
    result["bytes"] = qint64(bytes.size());
    result["origin"] = file.origin;
    result["width"] = dimension(size, true);
    result["height"] = dimension(size, false);
    result["mimeType"] = "image/png";
    result["imageIncluded"] = image;
    result["projectOverride"] = s.index.records.contains(path);
    result["verification"] = QJsonObject{{"manifest", "digest_verified"}, {"content", "sha256_verified"}};
    result["limits"] = QJsonObject{{"maxImageBytes", VanillaImageLimit}, {"maxMetadataBytes", VanillaMetadataLimit}};
    QJsonValue animation;
    if (entry.contains(path + ".mcmeta")) {
        QByteArray meta;
        QJsonObject row{{"path", path + ".mcmeta"}, {"sha256", entry.files.value(path + ".mcmeta").sha256}};
        if (!reader.readVerified(entry, path + ".mcmeta", &meta, &why)) {
            row["status"] = "integrity_failed";
        } else if (meta.size() > VanillaMetadataLimit) {
            row["status"] = "omitted_too_large";
            row["bytes"] = qint64(meta.size());
        } else {
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(meta, &parse);
            row["status"] = document.isObject() ? "included" : "invalid_json";
            if (document.isObject())
                row["json"] = document.object();
            row["bytes"] = qint64(meta.size());
        }
        animation = row;
    }
    result["animationMetadata"] = animation;
    if (image)
        result["_pngImage"] = QString::fromLatin1(bytes.toBase64());
    return result;
}
} // namespace nova::mcp
