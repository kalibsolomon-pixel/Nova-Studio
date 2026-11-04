// SPDX-License-Identifier: GPL-3.0-or-later
#include "minecraft_animation.h"
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <cstring>

namespace nova {
namespace {
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
bool integer(const QJsonValue &value, int minimum, int *out) {
    if (!value.isDouble() || value.toDouble() < minimum || value.toDouble() > INT_MAX ||
        std::floor(value.toDouble()) != value.toDouble()) return false;
    *out = int(value.toDouble());
    return true;
}
}
bool supportedAnimationTarget(const QString &target) {
    return target == "1.21.11" || target == "26.1" || target == "26.3";
}
bool parseAnimation(const QImage &sheet, const QJsonObject &metadata, const QString &target,
                    AnimationState *out, QString *error, const AnimationLimits &limits) {
    if (!supportedAnimationTarget(target)) return fail(error, "Unverified Minecraft target; supported profiles: 1.21.11, 26.1, 26.3");
    if (sheet.isNull() || sheet.sizeInBytes() > limits.decodedBytes || !metadata["animation"].isObject())
        return fail(error, "A bounded PNG and an animation object are required");
    const auto object = metadata["animation"].toObject();
    int width = 0, height = 0, ticks = 1;
    if (object.contains("width") && !integer(object["width"], 1, &width)) return fail(error, "width must be a positive integer");
    if (object.contains("height") && !integer(object["height"], 1, &height)) return fail(error, "height must be a positive integer");
    // Java calculateFrameSize: a single specified dimension leaves the other at sheet size.
    if (!width && !height) width = height = std::min(sheet.width(), sheet.height());
    else { if (!width) width = sheet.width(); if (!height) height = sheet.height(); }
    if (width > Document::MaxDimension || height > Document::MaxDimension || sheet.width() % width || sheet.height() % height)
        return fail(error, "Spritesheet must divide into exact bounded frame cells");
    const qint64 columns = sheet.width() / width, rows = sheet.height() / height, count = columns * rows;
    if (count < 1 || count > limits.sourceFrames || qint64(width) * height * 4 * (count + 1) > limits.decodedBytes)
        return fail(error, "Spritesheet exceeds frame or decoded-memory limits");
    if (object.contains("frametime") && !integer(object["frametime"], 1, &ticks)) return fail(error, "frametime must be a positive integer");
    if (object.contains("interpolate") && !object["interpolate"].isBool()) return fail(error, "interpolate must be a boolean");
    AnimationState state;
    state.enabled = true;
    state.target = target;
    state.defaultTicks = ticks;
    state.interpolate = object["interpolate"].toBool();
    state.metadata = metadata;
    for (int i = 0; i < count; ++i) {
        SourceFrame frame;
        frame.name = QString("Frame %1").arg(i + 1);
        RasterLayer layer;
        layer.id = QUuid::createUuid();
        layer.name = "Background";
        layer.pixels = sheet.copy((i % columns) * width, (i / columns) * height, width, height)
                           .convertToFormat(QImage::Format_RGBA8888);
        if (layer.pixels.isNull()) return fail(error, "Frame allocation failed");
        frame.activeLayer = layer.id;
        frame.layers.push_back(std::move(layer));
        state.frames.push_back(std::move(frame));
    }
    if (object.contains("frames")) {
        if (!object["frames"].isArray() || object["frames"].toArray().isEmpty() ||
            object["frames"].toArray().size() > limits.playbackEntries)
            return fail(error, "frames must be a nonempty bounded array");
        for (const auto &entry : object["frames"].toArray()) {
            int index = -1, duration = 0;
            if (entry.isObject()) {
                const auto item = entry.toObject();
                for (auto key = item.begin(); key != item.end(); ++key)
                    if (key.key() != "index" && key.key() != "time")
                        return fail(error, "Unknown playback-entry fields cannot be safely remapped");
                if (!integer(item["index"], 0, &index) ||
                    (item.contains("time") && !integer(item["time"], 1, &duration)))
                    return fail(error, "Frame objects require a nonnegative index and positive time");
            } else if (!integer(entry, 0, &index)) return fail(error, "Frame index must be a nonnegative integer");
            if (index >= count) return fail(error, "Playback index exceeds available source frames");
            state.entries.push_back({state.frames[size_t(index)].id, duration});
        }
    } else for (const auto &frame : state.frames) state.entries.push_back({frame.id, 0});
    state.active = state.entries.front().frame;
    *out = std::move(state);
    return true;
}
bool Document::importAnimation(const QImage &sheet, const QJsonObject &metadata,
                                const QString &target, QString *error) {
    AnimationState state;
    if (!parseAnimation(sheet, metadata, target, &state, error, animationLimits)) return false;
    return replaceAnimation(state, "Replace with Imported Animation", error);
}
std::shared_ptr<Document> openMinecraftAnimation(const QString &path, const QString &target, QString *error) {
    const auto pngBefore = Revision::read(path), metaBefore = Revision::read(path + ".mcmeta");
    QFile metadata(path + ".mcmeta");
    if (!metaBefore.exists || !metaBefore.readable || metaBefore.size > 512 * 1024 || !metadata.open(QIODevice::ReadOnly)) {
        fail(error, "Animation metadata is missing, unreadable or exceeds 512 KiB"); return {};
    }
    QJsonParseError parse;
    auto json = QJsonDocument::fromJson(metadata.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !json.isObject()) { fail(error, "Invalid animation JSON: " + parse.errorString()); return {}; }
    // Check dimensions and decoded allocation before Qt decompresses the spritesheet.
    // Decode from memory so the spritesheet is not held open while it decompresses.
    auto sheetBytes = readFileBytes(path, 256LL * 1024 * 1024);
    QBuffer sheetBuffer(&sheetBytes);
    sheetBuffer.open(QIODevice::ReadOnly);
    QImageReader reader(&sheetBuffer, "PNG");
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 32768 || size.height() > 32768 ||
        qint64(size.width()) * size.height() * 4 > Document::PixelLimit) { fail(error, "Spritesheet exceeds decoded image limits"); return {}; }
    auto sheet = reader.read().convertToFormat(QImage::Format_RGBA8888);
    auto doc = std::make_shared<Document>(path, QImage(1, 1, QImage::Format_RGBA8888));
    if (!doc->importAnimation(sheet, json.object(), target, error)) return {};
    if (!pngBefore.sameContent(Revision::read(path)) || !metaBefore.sameContent(Revision::read(path + ".mcmeta"))) {
        fail(error, "Animation changed during import"); return {};
    }
    doc->acceptAnimationExport(pngBefore, metaBefore);
    // The imported state matches the on-disk pair exactly; record it as the
    // pristine baseline so an untouched animation is not "unsaved" (the undo
    // step stays available, and real edits still diverge from this cursor).
    doc->markOpenedBaseline();
    return doc;
}
AnimationExport planAnimationExport(const Document &document, int columns, const QString &requested, QString *error) {
    AnimationExport result;
    auto state = document.animationState();
    const auto target = requested.isEmpty() ? state.target : requested;
    if (!document.animated() || !document.validateAnimation(state, error)) return result;
    if (!supportedAnimationTarget(target)) { fail(error, "Target version has no verified animation profile"); return result; }
    if (columns < 1 || columns > int(state.frames.size())) { fail(error, "Invalid spritesheet column count"); return result; }
    const int rows = (int(state.frames.size()) + columns - 1) / columns;
    const auto size = document.image.size();
    const qint64 width = qint64(size.width()) * columns, height = qint64(size.height()) * rows;
    if (width > 32768 || height > 32768 || width * height * 4 > document.frameLimits().decodedBytes) {
        fail(error, "Export spritesheet exceeds dimension or pixel limits"); return result;
    }
    result.sheet = QImage(int(width), int(height), QImage::Format_RGBA8888);
    if (result.sheet.isNull()) { fail(error, "Spritesheet allocation failed"); return {}; }
    result.sheet.fill(Qt::transparent);
    QHash<QUuid, int> indices;
    for (int i = 0; i < int(state.frames.size()); ++i) {
        const auto &frame = state.frames[size_t(i)];
        indices[frame.id] = i;
        auto pixels = document.frameComposite(frame.id);
        for (int y = 0; y < size.height(); ++y)
            std::memcpy(result.sheet.scanLine((i / columns) * size.height() + y) + (i % columns) * size.width() * 4,
                        pixels.constScanLine(y), size_t(size.width()) * 4);
    }
    QJsonArray entries;
    for (const auto &entry : state.entries) {
        if (entry.ticks) entries.append(QJsonObject{{"index", indices[entry.frame]}, {"time", entry.ticks}});
        else entries.append(indices[entry.frame]);
    }
    result.metadata = state.metadata;
    auto animation = result.metadata["animation"].toObject();
    for (auto key = animation.begin(); key != animation.end(); ++key)
        if (!QStringList{"width", "height", "frames", "frametime", "interpolate"}.contains(key.key()))
            result.diagnostics.append("Preserved unrecognized animation field: " + key.key());
    animation["width"] = size.width();
    animation["height"] = size.height();
    animation["frametime"] = state.defaultTicks;
    animation["interpolate"] = state.interpolate;
    animation["frames"] = entries;
    result.metadata["animation"] = animation;
    result.mcmeta = QJsonDocument(result.metadata).toJson(QJsonDocument::Indented);
    QBuffer buffer(&result.png);
    buffer.open(QIODevice::WriteOnly);
    if (!result.sheet.save(&buffer, "PNG") || result.png.size() > document.frameLimits().serializedBytes) {
        fail(error, "PNG encoding exceeded the export budget"); return {};
    }
    result.diagnostics.append("Format profile: Java " + target + "; runtime animation depends on the target atlas and asset type.");
    return result;
}
QImage interpolateFrames(const QImage &from, const QImage &to, double fraction) {
    if (from.size() != to.size() || from.format() != QImage::Format_RGBA8888 || to.format() != from.format()) return from;
    QImage result(from.size(), from.format());
    const double weight = std::clamp(fraction, 0.0, 1.0);
    for (int y = 0; y < from.height(); ++y) {
        auto a = from.constScanLine(y), b = to.constScanLine(y);
        auto p = result.scanLine(y);
        for (int x = 0; x < from.width(); ++x) {
            for (int c = 0; c < 4; ++c)
                p[x * 4 + c] = uchar(std::lround(a[x * 4 + c] * (1 - weight) + b[x * 4 + c] * weight));
        }
    }
    return result;
}
} // namespace nova
