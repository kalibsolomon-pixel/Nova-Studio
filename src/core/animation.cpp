// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>

namespace nova {
namespace {
bool reject(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}
}
AnimationState Document::animationState() const {
    auto result = animation;
    if (result.frames.empty()) {
        SourceFrame frame;
        frame.id = frameId;
        frame.name = "Frame 1";
        result.frames.push_back(frame);
        result.entries.push_back({frameId, 0});
    }
    for (auto &frame : result.frames)
        if (frame.id == frameId) {
            frame.layers = stack;
            frame.activeLayer = active;
        }
    result.active = frameId;
    return result;
}
bool Document::validateAnimation(const AnimationState &state, QString *error) const {
    if (state.frames.empty() || state.frames.size() > size_t(animationLimits.sourceFrames) ||
        state.entries.empty() || state.entries.size() > size_t(animationLimits.playbackEntries) ||
        state.defaultTicks <= 0 || (!state.enabled && state.frames.size() != 1))
        return reject(error, "Animation requires source frames, playback entries and positive timing within limits");
    QSet<QUuid> frames, layers;
    QSize size;
    qint64 decoded = 0;
    for (const auto &frame : state.frames) {
        if (frame.id.isNull() || frames.contains(frame.id) || frame.name.trimmed().isEmpty() ||
            frame.name.size() > 256 || frame.layers.empty() || frame.layers.size() > LayerLimit)
            return reject(error, "Invalid source-frame identity, label or layer count");
        frames.insert(frame.id);
        bool foundActive = false;
        for (const auto &layer : frame.layers) {
            if (!size.isValid()) size = layer.pixels.size();
            if (layer.id.isNull() || layers.contains(layer.id) || layer.pixels.isNull() ||
                layer.pixels.format() != QImage::Format_RGBA8888 || layer.pixels.size() != size ||
                size.width() > MaxDimension || size.height() > MaxDimension || layer.opacity < 0 ||
                layer.opacity > 255 || layer.name.trimmed().isEmpty() || layer.name.size() > 256)
                return reject(error, "Frame layers require unique identities, valid properties and identical bounded dimensions");
            layers.insert(layer.id);
            foundActive |= layer.id == frame.activeLayer;
            // Dimensions are bounded before multiplication; count incrementally.
            decoded += qint64(size.width()) * size.height() * 4;
            if (decoded + qint64(size.width()) * size.height() * 4 >
                std::min(PixelLimit, animationLimits.decodedBytes))
                return reject(error, "Animation exceeds the decoded pixel budget (including active composite)");
        }
        if (!foundActive) return reject(error, "Active layer must belong to its frame");
    }
    if (!frames.contains(state.active)) return reject(error, "Active source frame is missing");
    for (const auto &entry : state.entries)
        if (!frames.contains(entry.frame) || entry.ticks < 0)
            return reject(error, "Playback references must exist and durations must be positive or inherited");
    if (QJsonDocument(state.metadata).toJson(QJsonDocument::Compact).size() > 512 * 1024)
        return reject(error, "Texture metadata exceeds 512 KiB");
    return true;
}
bool Document::setAnimationLimits(const AnimationLimits &limits, QString *error) {
    if (limits.sourceFrames < 1 || limits.sourceFrames > 4096 || limits.playbackEntries < 1 ||
        limits.playbackEntries > 65536 || limits.decodedBytes < 1024 || limits.decodedBytes > PixelLimit ||
        limits.cacheBytes < 0 || limits.cacheBytes > 64LL * 1024 * 1024 ||
        limits.serializedBytes < 1024 || limits.serializedBytes > 120LL * 1024 * 1024)
        return reject(error, "Invalid animation resource limits");
    const auto previous = animationLimits;
    animationLimits = limits;
    if (!validateAnimation(animationState(), error)) { animationLimits = previous; return false; }
    frameCache.clear();
    return true;
}
void Document::installAnimation(const AnimationState &state) {
    animation = state;
    frameId = state.active;
    for (auto &frame : animation.frames)
        if (frame.id == frameId) {
            stack = std::move(frame.layers);
            active = frame.activeLayer;
            break;
        }
    image = QImage(stack[0].pixels.size(), QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    selectionMask = {};
    frameCache.clear();
    compose(image.rect());
    dirty = image.rect();
    ++serial;
}
bool Document::replaceAnimation(const AnimationState &state, const QString &label, QString *error) {
    if (lifecycle == State::Closing || !validateAnimation(state, error)) return false;
    finish();
    Operation op;
    op.kind = Operation::Animation;
    op.label = label;
    op.animationBefore = std::make_shared<AnimationState>(animationState());
    op.animationAfter = std::make_shared<AnimationState>(state);
    op.selectionBefore = selectionMask;
    if (state.active == frameId && state.frames[0].layers[0].pixels.size() == image.size()) op.selectionAfter = selectionMask;
    installAnimation(state);
    selectionMask = op.selectionAfter;
    native = true;
    commit(std::move(op));
    return true;
}
bool Document::convertToAnimation(QString *error) {
    if (animated()) return true;
    auto state = animationState();
    state.enabled = true;
    return replaceAnimation(state, "Convert to Animation", error);
}
bool Document::selectFrame(QUuid id, QString *error) {
    if (id == frameId) return true;
    auto next = std::find_if(animation.frames.begin(), animation.frames.end(),
                             [id](const auto &frame) { return frame.id == id; });
    if (next == animation.frames.end()) return reject(error, "Source frame is missing");
    finish();
    for (auto &frame : animation.frames)
        if (frame.id == frameId) {
            frame.layers = std::move(stack);
            frame.activeLayer = active;
            break;
        }
    stack = std::move(next->layers);
    active = next->activeLayer;
    frameId = id;
    animation.active = id;
    selectionMask = {};
    image = frameComposite(id);
    dirty = image.rect();
    ++serial;
    return true;
}
bool Document::prepareFrameEdit(FrameCommand command, QUuid id, int position, const QString &name,
                                AnimationState &state, QString &label, QString *error) const {
    if (id.isNull()) id = frameId;
    state = animationState();
    state.enabled = true;
    auto it = std::find_if(state.frames.begin(), state.frames.end(),
                           [id](const auto &frame) { return frame.id == id; });
    if (it == state.frames.end()) return reject(error, "Source frame is missing");
    const auto index = it - state.frames.begin();
    switch (command) {
    case FrameCommand::Add:
    case FrameCommand::Duplicate: {
        SourceFrame added = *it;
        added.id = QUuid::createUuid();
        added.name = command == FrameCommand::Add ? "Blank frame" : it->name.left(240) + " copy";
        if (command == FrameCommand::Add) {
            RasterLayer blank;
            blank.id = QUuid::createUuid(); blank.name = "Background";
            blank.pixels = QImage(image.size(), QImage::Format_RGBA8888); blank.pixels.fill(Qt::transparent);
            added.layers = {blank}; added.activeLayer = blank.id;
        } else for (auto &layer : added.layers) {
            const auto previous = layer.id; layer.id = QUuid::createUuid();
            if (previous == it->activeLayer) added.activeLayer = layer.id;
        }
        state.active = added.id;
        state.entries.push_back({added.id, 0});
        state.frames.insert(state.frames.begin() + index + 1, std::move(added));
        label = command == FrameCommand::Add ? "Add Frame" : "Duplicate Frame";
        break;
    }
    case FrameCommand::Delete:
        if (state.frames.size() == 1) return reject(error, "Keep at least one source frame");
        state.frames.erase(it);
        std::erase_if(state.entries, [id](const auto &entry) { return entry.frame == id; });
        if (state.active == id) state.active = state.frames[size_t(std::min(index, qsizetype(state.frames.size() - 1)))].id;
        if (state.entries.empty()) state.entries.push_back({state.active, 0});
        label = "Delete Frame and References";
        break;
    case FrameCommand::Move: {
        if (position < 0 || position >= int(state.frames.size())) return reject(error, "Invalid source-frame position");
        auto moved = *it;
        state.frames.erase(it);
        state.frames.insert(state.frames.begin() + position, std::move(moved));
        label = "Reorder Source Frames";
        break;
    }
    case FrameCommand::Rename:
        it->name = name.trimmed();
        label = "Rename Frame";
        break;
    }
    return true;
}
bool Document::editFrame(FrameCommand command, QUuid id, int position, const QString &name, QString *error) {
    AnimationState state;
    QString label;
    return prepareFrameEdit(command, id, position, name, state, label, error) &&
           replaceAnimation(state, label, error);
}
qint64 Document::frameCacheBytes() const {
    qint64 bytes = 0;
    for (const auto &cached : frameCache) bytes += cached.image.sizeInBytes();
    return bytes;
}
QImage Document::frameComposite(QUuid id) const {
    const std::vector<RasterLayer> *layers = nullptr;
    if (id == frameId) layers = &stack;
    else for (const auto &frame : animation.frames) if (frame.id == id) layers = &frame.layers;
    if (!layers || layers->empty()) return {};
    if (layers->size() == 1 && (*layers)[0].visible && (*layers)[0].opacity == 255)
        return (*layers)[0].pixels;
    QByteArray key;
    for (const auto &layer : *layers)
        key += QByteArray::number(layer.pixels.cacheKey()) + ':' + QByteArray::number(layer.opacity) +
               (layer.visible ? "v;" : "h;");
    for (auto it = frameCache.begin(); it != frameCache.end(); ++it)
        if (it->id == id && it->key == key) {
            auto result = *it;
            frameCache.erase(it);
            frameCache.push_back(result);
            return result.image;
        }
    QImage result(layers->front().pixels.size(), QImage::Format_RGBA8888);
    result.fill(Qt::transparent);
    composeInto(*layers, result, result.rect());
    std::erase_if(frameCache, [id](const auto &cached) { return cached.id == id; });
    while (!frameCache.empty() && frameCacheBytes() + result.sizeInBytes() > animationLimits.cacheBytes)
        frameCache.erase(frameCache.begin());
    if (result.sizeInBytes() <= animationLimits.cacheBytes) frameCache.push_back({id, key, result});
    return result;
}
bool Document::applyFrames(const std::vector<QUuid> &ids, const ops::Spec &input, QString *error,
                            const ops::Context *context) {
    auto spec = input;
    if (!ops::validate(spec, error)) return false;
    const auto info = ops::find(spec.id);
    auto state = animationState();
    QSet<QUuid> selected(ids.begin(), ids.end());
    if (selected.isEmpty()) return reject(error, "Select source frames to process");
    if (info->changesSize && selected.size() != qsizetype(state.frames.size()))
        return reject(error, "Dimension-changing operations require all source frames");
    if (spec.id == "trim") {
        QRect bounds;
        for (const auto &frame : state.frames) bounds = bounds.united(ops::alphaBounds(frameComposite(frame.id), spec.params["alpha_threshold"].toInt()));
        if (bounds.isEmpty()) return reject(error, "The animation has no visible pixels to keep");
        spec = {"crop", {{"x", bounds.x()}, {"y", bounds.y()}, {"width", bounds.width()}, {"height", bounds.height()}}};
    }
    for (auto id : selected)
        if (std::none_of(state.frames.begin(), state.frames.end(), [id](const auto &f) { return f.id == id; }))
            return reject(error, "Selected source frame is missing");
    int complete = 0;
    for (auto &frame : state.frames) {
        if (!selected.contains(frame.id)) continue;
        if (context && context->cancel && context->cancel->load()) return reject(error, "Operation canceled");
        Document scratch(path, frame.layers[0].pixels);
        scratch.stack = frame.layers;
        scratch.active = frame.activeLayer;
        scratch.compose(scratch.image.rect());
        // Selection applies only to active-frame scope; multi-frame scope is explicit full frames.
        if (selected.size() == 1 && frame.id == frameId) scratch.selectionMask = selectionMask;
        const bool ok = info->changesSize ? scratch.applyDocumentOperation(spec, error, context)
                                         : scratch.applyOperation(scratch.active, spec, error, context);
        if (!ok) return false;
        frame.layers = std::move(scratch.stack);
        frame.activeLayer = scratch.active;
        if (context && context->progress) context->progress(++complete * 100 / selected.size());
    }
    return replaceAnimation(state, info->name + " (frame scope)", error);
}
void Document::acceptAnimationExport(const Revision &png, const Revision &mcmeta, bool synchronized) {
    savedRevision = png;
    metadataRevision = mcmeta;
    exportedPixels = synchronized ? pixelDigest() : QByteArray(32, '\0');
    exportMetadataChanged = true;
    ++serial;
}
bool Document::acceptNativeSave(const Document &snapshot, quint64 expectedSerial) {
    if (snapshot.id() != id() || snapshot.path != path || snapshot.nativePath.isEmpty()) return false;
    native = true; nativePath = snapshot.nativePath; nativeRevision = snapshot.nativeRevision;
    if (serial == expectedSerial) { saved = cursor; exportMetadataChanged = false; }
    else saved.reset(); // a newer edit remains unsaved, with the new disk baseline bound
    ++serial;
    return true;
}
} // namespace nova
