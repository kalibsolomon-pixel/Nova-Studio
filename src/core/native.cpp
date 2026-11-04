// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QCache>
#include <QMutex>
#include <QMutexLocker>
#include <QtEndian>

namespace nova {
namespace {
constexpr qint64 EncodedLimit = 128LL * 1024 * 1024;
struct EncodedLayer { QByteArray png, hash; };
// Immutable QImage generation keys; UUIDs and metadata are always serialized afresh.
// One bounded process-wide cache serves save/recovery workers. Encode outside lock.
EncodedLayer encodeLayer(const QImage &image) {
    static QMutex mutex;
    static QCache<qint64, EncodedLayer> cache(16 * 1024 * 1024);
    const auto key = image.cacheKey();
    { QMutexLocker lock(&mutex); if (auto value = cache.object(key)) return *value; }
    EncodedLayer result;
    QBuffer buffer(&result.png); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) return {};
    result.hash = QCryptographicHash::hash(result.png, QCryptographicHash::Sha256).toHex();
    const auto cost = result.png.size() + result.hash.size() + 128;
    if (cost <= cache.maxCost()) {
        QMutexLocker lock(&mutex);
        if (cache.size() >= 512) cache.clear();
        cache.insert(key, new EncodedLayer(result), int(cost));
    }
    return result;
}
bool validAsset(const QString &asset) {
    if (asset.isEmpty() || asset.size() > 2048 || asset.contains('\\') || asset.contains(':') ||
        asset.contains(QChar(0)) || QDir::isAbsolutePath(asset) ||
        QDir::cleanPath(asset) != asset || !asset.endsWith(".png", Qt::CaseInsensitive))
        return false;
    for (const auto &part : asset.split('/'))
        if (part.isEmpty() || part == "." || part == ".." ||
            part.compare(".nova", Qt::CaseInsensitive) == 0 || part.endsWith('.') ||
            part.endsWith(' '))
            return false;
    return true;
}
QString assetFor(const QString &project, const QString &png) {
    auto root = QFileInfo(project).canonicalFilePath();
    if (root.isEmpty())
        return {};
    auto info = QFileInfo(png);
    const auto full = info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
    auto asset = QDir(root).relativeFilePath(full);
    return validAsset(asset) ? asset : QString();
}
bool safeAncestors(const QString &root, const QString &relative) {
    QString current = root;
    for (const auto &part : relative.split('/')) {
        current += '/' + part;
        QFileInfo info(current);
        if (info.isSymLink() || info.isJunction() ||
            (info.exists() && !info.isDir() && current != root + '/' + relative))
            return false;
    }
    return true;
}
QJsonObject revisionJson(const Revision &r) {
    return {{"exists", r.exists},
            {"readable", r.readable},
            {"size", double(r.size)},
            {"modified", double(r.modified)},
            {"sha256", QString::fromLatin1(r.hash.toHex())}};
}
void appendSize(QByteArray &out, quint32 n) {
    char bytes[4];
    qToLittleEndian(n, reinterpret_cast<uchar *>(bytes));
    out.append(bytes, 4);
}
} // namespace
QString nativeDocumentPath(const QString &project, const QString &png, QString *error) {
    const auto root = QFileInfo(project).canonicalFilePath();
    const auto asset = assetFor(project, png);
    auto fail = [&] {
        if (error)
            *error = "Editable documents require a PNG inside a project with safe metadata paths";
        return QString();
    };
    if (root.isEmpty() || asset.isEmpty() || !safeAncestors(root, asset))
        return fail();
    auto key =
        QCryptographicHash::hash(asset.toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex();
    const auto relative = ".nova/documents/" + QString::fromLatin1(key) + ".nvdoc";
    if (!safeAncestors(root, relative))
        return fail();
    return root + '/' + relative;
}
QByteArray Document::encodeNative(const QString &asset, QString *error) const {
    if (animated()) return encodeAnimation(asset, error);
    auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        return QByteArray();
    };
    if (!validAsset(asset) || stack.empty() || stack.size() > LayerLimit ||
        pixelBytes() > PixelLimit)
        return fail("Invalid asset or document pixel budget");
    QJsonArray descriptors;
    QList<QByteArray> payloads;
    qint64 total = 0;
    for (const auto &layer : stack) {
        const auto encoded = encodeLayer(layer.pixels);
        auto png = encoded.png;
        if (png.isEmpty())
            return fail("Layer PNG encoding failed");
        total += png.size();
        if (total > EncodedLimit - 1024 * 1024)
            return fail("Encoded document exceeds 128 MiB");
        descriptors.append(QJsonObject{
            {"id", layer.id.toString(QUuid::WithoutBraces)},
            {"name", layer.name},
            {"visible", layer.visible},
            {"opacity", layer.opacity},
            {"revision", QString::number(layer.revision)},
            {"bytes", png.size()},
            {"extensions", layer.extensions},
            {"sha256", QString::fromLatin1(encoded.hash)}});
        payloads.append(std::move(png));
    }
    QJsonObject manifest{{"schema", 1},
                         {"asset", asset},
                         {"id", identity.toString(QUuid::WithoutBraces)},
                         {"width", image.width()},
                         {"height", image.height()},
                         {"active", active.toString(QUuid::WithoutBraces)},
                         {"layers", descriptors},
                         {"png_revision", revisionJson(savedRevision)},
                         {"exported_pixels_sha256", QString::fromLatin1(exportedPixels.toHex())},
                         {"blend", "normal-encoded-srgb-integer-v1"},
                         {"extensions", extensions}};
    if (metadataRevision.exists || !animation.metadata.isEmpty()) {
        manifest["mcmeta_revision"] = revisionJson(metadataRevision);
        manifest["texture_metadata"] = animation.metadata;
    }
    auto json = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    QByteArray result("NVLD0001", 8);
    result.reserve(12 + json.size() + total + 4 * payloads.size());
    appendSize(result, quint32(json.size()));
    result.append(json);
    for (const auto &payload : payloads) {
        appendSize(result, quint32(payload.size()));
        result.append(payload);
    }
    return result;
}
bool Document::decodeNative(const QByteArray &bytes, const QString &asset, QString *error) {
    if (validAsset(asset) && bytes.size() >= 8 && bytes.first(8) == "NVAN0002") return decodeAnimation(bytes, asset, error);
    auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (!validAsset(asset) || bytes.size() < 12 || bytes.size() > EncodedLimit ||
        bytes.first(8) != "NVLD0001")
        return fail("Invalid or unsupported editable document header");
    qsizetype offset = 8;
    auto size = [&]() -> qint64 {
        if (offset + 4 > bytes.size())
            return -1;
        auto n =
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
        offset += 4;
        return n;
    };
    const auto jsonSize = size();
    if (jsonSize < 2 || jsonSize > 1024 * 1024 || offset + jsonSize > bytes.size())
        return fail("Invalid manifest length");
    QJsonParseError parse;
    const auto parsed = QJsonDocument::fromJson(bytes.mid(offset, jsonSize), &parse);
    offset += jsonSize;
    const auto m = parsed.object();
    if (parse.error != QJsonParseError::NoError || !parsed.isObject() ||
        m["schema"].toInt(-1) != 1 || m["asset"].toString() != asset ||
        m["blend"].toString() != "normal-encoded-srgb-integer-v1")
        return fail("Unsupported schema, blend policy or case-colliding asset identity");
    const int width = m["width"].toInt(), height = m["height"].toInt();
    auto descriptors = m["layers"].toArray();
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || descriptors.isEmpty() ||
        descriptors.size() > LayerLimit ||
        qint64(width) * height * 4 * (descriptors.size() + 1) > PixelLimit)
        return fail("Invalid layer dimensions or decoded pixel budget");
    std::vector<RasterLayer> loaded;
    QSet<QUuid> ids;
    for (const auto &value : descriptors) {
        const auto d = value.toObject();
        RasterLayer layer;
        layer.id = QUuid(d["id"].toString());
        layer.name = d["name"].toString();
        layer.opacity = d["opacity"].toInt(-1);
        layer.visible = d["visible"].toBool();
        layer.revision = d["revision"].toString().toULongLong();
        layer.extensions = d["extensions"].toObject();
        auto n = size();
        if (layer.id.isNull() || ids.contains(layer.id) || layer.name.trimmed().isEmpty() ||
            layer.name.size() > 256 || layer.opacity < 0 || layer.opacity > 255 ||
            !d["visible"].isBool() || n < 1 || n != d["bytes"].toInteger(-1) || n > EncodedLimit ||
            offset + n > bytes.size())
            return fail("Corrupt layer descriptor or missing payload");
        ids.insert(layer.id);
        auto png = QByteArray::fromRawData(bytes.constData() + offset, n);
        offset += n;
        if (QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex() !=
            d["sha256"].toString().toLatin1())
            return fail("Layer payload checksum mismatch");
        QBuffer buffer(&png);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "PNG");
        if (reader.size() != QSize(width, height))
            return fail("Layer payload dimensions mismatch");
        layer.pixels = reader.read().convertToFormat(QImage::Format_RGBA8888);
        if (layer.pixels.isNull())
            return fail("Layer PNG is corrupt");
        loaded.push_back(std::move(layer));
    }
    const auto selected = QUuid(m["active"].toString()), docId = QUuid(m["id"].toString());
    auto r = m["png_revision"].toObject();
    Revision revision{r["exists"].toBool(), r["readable"].toBool(), r["size"].toInteger(),
                      r["modified"].toInteger(),
                      QByteArray::fromHex(r["sha256"].toString().toLatin1())};
    auto exported = QByteArray::fromHex(m["exported_pixels_sha256"].toString().toLatin1());
    if (offset != bytes.size() || !ids.contains(selected) || docId.isNull() ||
        exported.size() != 32 ||
        (revision.exists && revision.readable && revision.hash.size() != 32))
        return fail("Corrupt document identity or export revision");
    // Commit only after every descriptor and payload validates.
    auto composite = QImage(width, height, QImage::Format_RGBA8888);
    if (composite.isNull())
        return fail("Composite allocation failed");
    composite.fill(Qt::transparent);
    stack = std::move(loaded);
    animation = {};
    animation.metadata = m["texture_metadata"].toObject();
    if (m.contains("mcmeta_revision")) {
        const auto meta = m["mcmeta_revision"].toObject();
        metadataRevision = {meta["exists"].toBool(), meta["readable"].toBool(), meta["size"].toInteger(),
                             meta["modified"].toInteger(), QByteArray::fromHex(meta["sha256"].toString().toLatin1())};
    } else metadataRevision = {};
    frameCache.clear();
    image = std::move(composite);
    active = selected;
    identity = docId;
    savedRevision = revision;
    exportedPixels = exported;
    native = true;
    extensions = m["extensions"].toObject();
    history.clear();
    history.shrink_to_fit();
    pendingStroke.clear();
    cursor = 0;
    saved = 0;
    exportMetadataChanged = false;
    ++serial;
    lifecycle = State::Ready;
    compose(image.rect());
    return true;
}
bool Document::saveNative(const QString &project, QString *error) {
    finish();
    auto destination = nativeDocumentPath(project, path, error);
    if (destination.isEmpty())
        return false;
    const auto asset = assetFor(project, path);
    const auto expected = nativePath == destination ? nativeRevision : Revision{};
    if (!nativePath.isEmpty() && nativePath != destination) {
        if (error)
            *error = "Editable source belongs to another asset";
        return false;
    }
    auto bytes = encodeNative(asset, error);
    if (bytes.isEmpty())
        return false;
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()) ||
        nativeDocumentPath(project, path, error) != destination) {
        if (error)
            *error = "Could not create safe editable document directory";
        return false;
    }
    if (!atomicWrite(destination, bytes, expected, error))
        return false;
    native = true;
    nativePath = destination;
    nativeRevision = {true, true, bytes.size(), 0,
                      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
    saved = cursor;
    exportMetadataChanged = false;
    lifecycle = State::Ready;
    ++serial;
    return true;
}
bool Document::loadNative(const QString &project, QString *error) {
    auto source = nativeDocumentPath(project, path, error);
    if (source.isEmpty())
        return false;
    auto expected = Revision::read(source);
    QFile file(source);
    if (!expected.exists || !expected.readable || expected.size > EncodedLimit ||
        !file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = "Editable document is missing or inaccessible";
        return false;
    }
    const auto bytes = file.read(EncodedLimit + 1);
    if (!expected.sameContent(Revision::read(source))) {
        if (error)
            *error = "Editable source changed during load";
        return false;
    }
    if (!decodeNative(bytes, assetFor(project, path), error))
        return false;
    nativePath = source;
    nativeRevision = expected;
    return true;
}
} // namespace nova
