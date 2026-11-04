// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryFile>
#include <QtEndian>

namespace nova {
namespace {
void length(QByteArray &out, quint32 value) {
    char bytes[4]; qToLittleEndian(value, reinterpret_cast<uchar *>(bytes)); out.append(bytes, 4);
}
QJsonObject revisionObject(const Revision &r) {
    return {{"exists", r.exists}, {"readable", r.readable}, {"size", double(r.size)},
            {"modified", double(r.modified)}, {"sha256", QString::fromLatin1(r.hash.toHex())}};
}
Revision readRevision(const QJsonObject &o) {
    return {o["exists"].toBool(), o["readable"].toBool(), o["size"].toInteger(),
            o["modified"].toInteger(), QByteArray::fromHex(o["sha256"].toString().toLatin1())};
}
}
QByteArray Document::encodeAnimation(const QString &asset, QString *error) const {
    const auto state = animationState();
    if (!validateAnimation(state, error)) return {};
    QJsonArray frames, entries;
    QList<QByteArray> payloads;
    std::unique_ptr<QTemporaryFile> spool;
    auto writePayload = [&](const QByteArray &payload) {
        QByteArray prefix;
        length(prefix, quint32(payload.size()));
        return spool->write(prefix) == prefix.size() && spool->write(payload) == payload.size();
    };
    qint64 total = 0;
    for (const auto &frame : state.frames) {
        Document scratch(path, frame.layers[0].pixels);
        scratch.stack = frame.layers;
        scratch.active = frame.activeLayer;
        scratch.savedRevision = savedRevision;
        auto payload = scratch.encodeNative(asset, error);
        if (payload.isEmpty()) return {};
        total += payload.size() + 4;
        if (total > animationLimits.serializedBytes) {
            if (error) *error = "Animation exceeds serialized/recovery size limit";
            return {};
        }
        frames.append(QJsonObject{{"id", frame.id.toString()}, {"name", frame.name},
                                   {"bytes", payload.size()},
                                   {"sha256", QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex())}});
        if (!spool && total > 8 * 1024 * 1024) {
            spool = std::make_unique<QTemporaryFile>();
            if (!spool->open()) {
                if (error)
                    *error = "Cannot stage large animation payloads: " + spool->errorString();
                return {};
            }
            for (const auto &previous : payloads)
                if (!writePayload(previous)) {
                    if (error)
                        *error = "Animation staging write failed";
                    return {};
                }
            payloads.clear();
        }
        if (spool) {
            if (!writePayload(payload)) {
                if (error)
                    *error = "Animation staging write failed";
                return {};
            }
        } else
            payloads.push_back(std::move(payload));
    }
    for (const auto &entry : state.entries)
        entries.append(QJsonObject{{"frame", entry.frame.toString()}, {"ticks", entry.ticks}});
    QJsonObject manifest{{"schema", 2}, {"asset", asset}, {"id", identity.toString()},
                         {"active", state.active.toString()}, {"frames", frames}, {"entries", entries},
                         {"default_ticks", state.defaultTicks}, {"interpolate", state.interpolate},
                         {"target", state.target}, {"metadata", state.metadata}, {"extensions", extensions},
                         {"png_revision", revisionObject(savedRevision)}, {"mcmeta_revision", revisionObject(metadataRevision)},
                         {"exported_sha256", QString::fromLatin1(exportedPixels.toHex())}};
    auto json = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (json.size() > 8 * 1024 * 1024 || total + json.size() + 12 > animationLimits.serializedBytes) {
        if (error) *error = "Animation manifest exceeds 8 MiB or the complete container exceeds its serialized budget";
        return {};
    }
    QByteArray bytes("NVAN0002", 8);
    bytes.reserve(12 + json.size() + total);
    length(bytes, quint32(json.size())); bytes += json;
    if (spool) {
        if (!spool->seek(0)) {
            if (error)
                *error = "Animation staging seek failed";
            return {};
        }
        qint64 remaining = total;
        while (remaining > 0) {
            const auto chunk = spool->read(std::min(remaining, qint64(1024 * 1024)));
            if (chunk.isEmpty()) {
                if (error)
                    *error = "Animation staging read failed";
                return {};
            }
            bytes.append(chunk);
            remaining -= chunk.size();
        }
    } else
        for (const auto &payload : payloads) {
            length(bytes, quint32(payload.size()));
            bytes += payload;
        }
    return bytes;
}
bool Document::decodeAnimation(const QByteArray &bytes, const QString &asset, QString *error) {
    auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    if (bytes.size() < 12 || bytes.size() > animationLimits.serializedBytes) return fail("Animation container size limit");
    qsizetype offset = 8;
    auto size = [&]() -> qint64 {
        if (offset + 4 > bytes.size()) return -1;
        auto n = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
        offset += 4; return n;
    };
    auto count = size();
    if (count < 2 || count > 8 * 1024 * 1024 || offset + count > bytes.size()) return fail("Invalid animation manifest length");
    QJsonParseError parse;
    auto parsed = QJsonDocument::fromJson(bytes.mid(offset, count), &parse);
    offset += count;
    auto m = parsed.object();
    const auto docId = QUuid(m["id"].toString());
    if (parse.error != QJsonParseError::NoError || !parsed.isObject() || m["schema"].toInt() != 2 ||
        m["asset"].toString() != asset || docId.isNull() || !m["frames"].isArray() || !m["entries"].isArray() ||
        !m["metadata"].isObject() || !m["interpolate"].isBool()) return fail("Invalid animation schema or identity");
    const auto descriptors = m["frames"].toArray(), timeline = m["entries"].toArray();
    if (descriptors.isEmpty() || descriptors.size() > animationLimits.sourceFrames ||
        timeline.isEmpty() || timeline.size() > animationLimits.playbackEntries) return fail("Animation counts exceed limits");
    AnimationState state;
    state.enabled = true;
    state.active = QUuid(m["active"].toString());
    state.defaultTicks = m["default_ticks"].toInt(-1);
    state.interpolate = m["interpolate"].toBool();
    state.target = m["target"].toString();
    state.metadata = m["metadata"].toObject();
    qint64 decoded = 0;
    for (const auto &descriptor : descriptors) {
        const auto d = descriptor.toObject();
        auto n = size();
        if (n < 1 || n != d["bytes"].toInteger(-1) || n > animationLimits.serializedBytes || offset + n > bytes.size())
            return fail("Invalid frame payload length");
        auto payload = QByteArray::fromRawData(bytes.constData() + offset, n);
        offset += n;
        if (payload.size() < 12 || payload.first(8) != "NVLD0001" || QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex() != d["sha256"].toString().toLatin1())
            return fail("Invalid frame payload checksum or nested schema");
        // Inspect nested dimensions/count before decoding any pixels for this frame.
        if (payload.size() < 12) return fail("Truncated frame");
        auto jsonLength = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData() + 8));
        if (jsonLength > 1024 * 1024 || qint64(jsonLength) + 12 > payload.size()) return fail("Invalid nested manifest");
        auto inner = QJsonDocument::fromJson(payload.mid(12, jsonLength)).object();
        const auto w = inner["width"].toInt(), h = inner["height"].toInt();
        auto layerCount = inner["layers"].toArray().size();
        if (w < 1 || h < 1 || w > MaxDimension || h > MaxDimension || layerCount < 1 || layerCount > LayerLimit)
            return fail("Invalid nested dimensions");
        decoded += qint64(w) * h * 4 * layerCount;
        if (decoded + qint64(w) * h * 4 > animationLimits.decodedBytes) return fail("Animation decoded allocation limit");
        Document scratch(path, QImage{});
        if (!scratch.decodeNative(payload, asset, error)) return false;
        SourceFrame frame;
        frame.id = QUuid(d["id"].toString()); frame.name = d["name"].toString();
        frame.layers = std::move(scratch.stack); frame.activeLayer = scratch.active;
        state.frames.push_back(std::move(frame));
    }
    for (const auto &value : timeline) {
        auto entry = value.toObject();
        if (!entry["ticks"].isDouble() || entry["ticks"].toDouble() != entry["ticks"].toInt(-1)) return fail("Invalid playback duration");
        state.entries.push_back({QUuid(entry["frame"].toString()), entry["ticks"].toInt(-1)});
    }
    const auto png = readRevision(m["png_revision"].toObject()), meta = readRevision(m["mcmeta_revision"].toObject());
    const auto exported = QByteArray::fromHex(m["exported_sha256"].toString().toLatin1());
    if (offset != bytes.size() || exported.size() != 32 ||
        (png.exists && (!png.readable || png.hash.size() != 32)) ||
        (meta.exists && (!meta.readable || meta.hash.size() != 32)) || !validateAnimation(state, error))
        return fail(error && !error->isEmpty() ? *error : "Invalid animation revisions or trailing bytes");
    installAnimation(state);
    identity = docId;
    savedRevision = png; metadataRevision = meta; exportedPixels = exported;
    extensions = m["extensions"].toObject(); native = true;
    history.clear(); history.shrink_to_fit(); pendingStroke.clear(); cursor = 0; saved = 0;
    exportMetadataChanged = false; lifecycle = State::Ready;
    return true;
}
} // namespace nova
