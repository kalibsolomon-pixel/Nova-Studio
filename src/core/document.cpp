// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include "pixelwrite.h"
#include "minecraft_animation.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QImageReader>
#include <QFile>
#include <QSet>
#include <QJsonDocument>
#include <QVarLengthArray>
#include <algorithm>

namespace nova {
std::shared_ptr<Document> Document::recoverySnapshot() const {
    auto copy = std::make_shared<Document>(path, QImage{});
    copy->identity = identity;
    copy->image = image;
    copy->stack = stack;
    copy->active = active;
    copy->native = native;
    copy->nativePath = nativePath;
    copy->nativeRevision = nativeRevision;
    copy->savedRevision = savedRevision;
    copy->exportedPixels = exportedPixels;
    copy->extensions = extensions;
    copy->animation = animation;
    copy->frameId = frameId;
    copy->animationLimits = animationLimits;
    copy->historyBudget = historyBudget;
    copy->operationLimit = operationLimit;
    copy->metadataRevision = metadataRevision;
    copy->serial = serial;
    copy->saved.reset();
    copy->lifecycle = State::Modified;
    return copy;
}
Document::Document(QString p, QImage im)
    : path(std::move(p)), image(im.convertToFormat(QImage::Format_RGBA8888)), dirty(image.rect()) {
    if (image.isNull() || image.width() > 4096 || image.height() > 4096) {
        image = {};
        lifecycle = State::Error;
        return;
    }
    RasterLayer base;
    base.id = QUuid::createUuid();
    base.name = "Background";
    base.pixels = image;
    active = base.id;
    stack.push_back(std::move(base));
    lifecycle = State::Ready;
    savedRevision = Revision::read(path);
    exportedPixels = pixelDigest();
}
std::shared_ptr<Document> Document::open(const QString &path, QString *error) {
    QJsonObject metadata;
    Revision metaRevision = Revision::read(path + ".mcmeta");
    if (metaRevision.exists) {
        QFile file(path + ".mcmeta");
        if (!metaRevision.readable || metaRevision.size > 512 * 1024 || !file.open(QIODevice::ReadOnly)) {
            if (error) *error = "Unreadable or oversized texture metadata";
            return {};
        }
        QJsonParseError parse;
        auto json = QJsonDocument::fromJson(file.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !json.isObject()) {
            if (error) *error = "Invalid texture metadata JSON";
            return {};
        }
        metadata = json.object();
        if (metadata.contains("animation")) return openMinecraftAnimation(path, "26.1", error);
        if (!metaRevision.sameContent(Revision::read(path + ".mcmeta"))) return {};
    }
    auto before = Revision::read(path);
    auto pixels = loadPng(path, error);
    if (!before.readable || !before.sameContent(Revision::read(path))) {
        if (error)
            *error = "File changed while opening or is inaccessible";
        return {};
    }
    if (pixels.isNull())
        return {};
    auto doc = std::make_shared<Document>(path, pixels);
    doc->savedRevision = before;
    doc->animation.metadata = metadata;
    doc->metadataRevision = metaRevision;
    return doc;
}
std::shared_ptr<Document> Document::fromBaseline(const QString &target, const QByteArray &png,
                                                 const QByteArray &mcmeta, const QString &version,
                                                 QString *error) {
    auto reject = [&](const QString &message) {
        if (error) *error = message;
        return std::shared_ptr<Document>{};
    };
    const QFileInfo file(target), meta(target + ".mcmeta");
    auto occupied = [](const QFileInfo &f) { return f.exists() || f.isSymLink(); };
    if (occupied(file))
        return reject("The project already contains this texture; open the project file instead");
    if (png.isEmpty() || png.size() > 128LL * 1024 * 1024 || mcmeta.size() > 512 * 1024)
        return reject("Baseline texture or metadata is missing or oversized");
    QJsonObject metadata;
    if (!mcmeta.isEmpty()) {
        QJsonParseError parse;
        const auto json = QJsonDocument::fromJson(mcmeta, &parse);
        if (parse.error != QJsonParseError::NoError || !json.isObject())
            return reject("Invalid baseline texture metadata JSON");
        metadata = json.object();
    }
    // Check dimensions and decoded allocation before Qt decompresses the texture.
    QByteArray bytes = png;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "PNG");
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 32768 || size.height() > 32768 ||
        qint64(size.width()) * size.height() * 4 > PixelLimit)
        return reject("Baseline texture exceeds decoded image limits");
    const auto pixels = reader.read().convertToFormat(QImage::Format_RGBA8888);
    if (pixels.isNull())
        return reject("Baseline texture is not a readable PNG");
    std::shared_ptr<Document> doc;
    if (metadata.contains("animation")) {
        // Both outputs are created together by the journaled pair export; neither may exist.
        if (occupied(meta))
            return reject("The project already contains this texture's .mcmeta; open the project file instead");
        // Unprofiled versions use the same animation profile Document::open applies to every file.
        const auto profile = supportedAnimationTarget(version) ? version : QString("26.1");
        doc = std::make_shared<Document>(target, QImage(1, 1, QImage::Format_RGBA8888));
        if (!doc->importAnimation(pixels, metadata, profile, error))
            return {};
        doc->acceptAnimationExport(Revision{}, Revision{});
        doc->markOpenedBaseline();
    } else {
        doc = std::make_shared<Document>(target, pixels);
        if (doc->image.isNull())
            return reject("Baseline texture exceeds the 4096 px editing limit");
        doc->animation.metadata = metadata;
    }
    doc->savedRevision = Revision{};
    doc->metadataRevision = Revision{};
    if (occupied(QFileInfo(target)))
        return reject("A file appeared at the texture's project path while opening");
    return doc;
}
Document::State Document::state() const {
    if (lifecycle == State::Error || lifecycle == State::Saving || lifecycle == State::Closing ||
        lifecycle == State::Opening)
        return lifecycle;
    return modified() ? State::Modified : State::Ready;
}
bool Document::modified() const {
    if (!pendingStroke.isEmpty() || (native && exportMetadataChanged) || !saved)
        return true;
    // Selection-only history entries are view state: they never dirty content.
    const int lo = std::min(*saved, cursor), hi = std::max(*saved, cursor);
    for (int i = lo; i < hi; ++i)
        if (history[size_t(i)].kind != Operation::Selection)
            return true;
    return false;
}
QByteArray Document::pixelDigest() const {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (animated()) {
        auto state = animationState();
        for (const auto &frame : state.frames) {
            auto composite = frameComposite(frame.id);
            hash.addData(QByteArrayView(reinterpret_cast<const char *>(composite.constBits()), composite.sizeInBytes()));
            hash.addData(frame.id.toRfc4122());
        }
        for (const auto &entry : state.entries) {
            hash.addData(entry.frame.toRfc4122());
            hash.addData(QByteArray::number(entry.ticks) + ';');
        }
        hash.addData(QByteArray::number(state.defaultTicks));
        hash.addData(state.interpolate ? "interpolate" : "discrete");
        hash.addData(QJsonDocument(state.metadata).toJson(QJsonDocument::Compact));
        return hash.result();
    }
    hash.addData(
        QByteArrayView(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()));
    return hash.result();
}
bool Document::exportModified() const { return pixelDigest() != exportedPixels; }
qint64 Document::pixelBytes() const {
    // A pristine legacy PNG shares its single buffer with the composite.
    qint64 bytes = image.sizeInBytes();
    for (const auto &layer : stack)
        if (layer.pixels.constBits() != image.constBits())
            bytes += layer.pixels.sizeInBytes();
    for (const auto &frame : animation.frames)
        for (const auto &layer : frame.layers)
            bytes += layer.pixels.sizeInBytes();
    return bytes;
}
RasterLayer *Document::findLayer(QUuid id) {
    for (auto &layer : stack)
        if (layer.id == id)
            return &layer;
    return nullptr;
}
void Document::compose(QRect region) {
    region = region.intersected(image.rect());
    if (region.isEmpty())
        return;
    // Preserve exact stored RGBA (including hidden RGB at alpha zero) for legacy
    // single-surface PNGs. Layered composites normalize fully transparent RGB.
    if (stack.size() == 1 && stack[0].visible && stack[0].opacity == 255) {
        image = stack[0].pixels;
    } else {
        composeInto(stack, image, region);
    }
    dirty = dirty.united(region);
}
void Document::composeInto(const std::vector<RasterLayer> &stack, QImage &image, QRect region) {
    // Detach the destination before taking source pointers. Snapshots and layers
    // may still share its previous buffer. Resolve visibility and scanlines once
    // per composition/row rather than invoking QImage for every pixel/layer.
    auto destination = image.bits();
    struct Source { const uchar *row; qsizetype stride; int opacity; };
    QVarLengthArray<Source, 64> sources;
    for (const auto &layer : stack)
        if (layer.visible && layer.opacity)
            sources.append({layer.pixels.constScanLine(region.top()), layer.pixels.bytesPerLine(), layer.opacity});
    for (int y = region.top(); y <= region.bottom(); ++y) {
        auto out = destination + y * image.bytesPerLine();
        for (int x = region.left(); x <= region.right(); ++x) {
            int r = 0, g = 0, b = 0, alpha = 0;
            for (const auto &source : sources) {
                auto p = source.row + x * 4;
                const int a = (int(p[3]) * source.opacity + 127) / 255;
                if (!a)
                    continue;
                // Exact identities of the existing integer source-over:
                // an opaque source replaces the accumulator; a first visible
                // source contributes its stored RGB without any division.
                if (a == 255 || alpha == 0) {
                    r = p[0]; g = p[1]; b = p[2]; alpha = a;
                    continue;
                }
                // Deterministic integer source-over in encoded sRGB channels.
                const int weight = alpha * (255 - a);
                const int denominator = a * 255 + weight;
                r = (int(p[0]) * a * 255 + r * weight + denominator / 2) / denominator;
                g = (int(p[1]) * a * 255 + g * weight + denominator / 2) / denominator;
                b = (int(p[2]) * a * 255 + b * weight + denominator / 2) / denominator;
                alpha = (denominator + 127) / 255;
            }
            auto p = out + x * 4;
            p[0] = uchar(r);
            p[1] = uchar(g);
            p[2] = uchar(b);
            p[3] = uchar(alpha);
        }
        for (auto &source : sources) source.row += source.stride;
    }
}
bool Document::paint(QUuid id, QPoint a, QPoint b, QRgb color, QString *error) {
    auto layer = findLayer(id);
    auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (!layer || id != active)
        return fail("Select the target layer before drawing");
    if (!layer->visible)
        return fail("Show the selected layer before drawing");
    if (lifecycle == State::Closing || !image.rect().contains(a) || !image.rect().contains(b))
        return fail("Invalid drawing coordinates or closed document");
    if (!pendingStroke.isEmpty() && strokeLayer != id)
        finish();
    strokeLayer = id;
    QRect region;
    lifecycle = State::Ready;
    {
        PixelWrite write(image, layer->pixels);
        for (auto p : line(a, b)) {
            const auto before = layer->pixels.pixel(p);
            if (before == color)
                continue;
            int index = p.y() * write.width + p.x();
            if (pendingStroke.contains(index))
                pendingStroke[index].after = color;
            else
                pendingStroke.insert(index, {index, before, color});
            layer->pixels.setPixel(p, color);
            region = region.united(QRect(p, QSize(1, 1)));
            if (qint64(pendingStroke.size()) * 80 >= historyBudget / 2)
                finish();
        }
    }
    if (!region.isEmpty()) {
        ++layer->revision;
        ++serial;
        compose(region);
    }
    return true;
}
void Document::draw(QPoint a, QPoint b, QRgb color) { paint(active, a, b, color); }
void Document::commit(Operation operation) {
    if (operation.frame.isNull()) operation.frame = frameId;
    if (compoundDepth > 0) {
        // Inside a transaction: mutations are already applied; collect them.
        compoundChildren.push_back(std::move(operation));
        ++serial;
        return;
    }
    if (saved && *saved > cursor)
        saved.reset();
    history.resize(cursor);
    history.push_back(std::move(operation));
    ++cursor;
    ++serial;
    lifecycle = State::Ready;
    pruneHistory();
}
void Document::finish() {
    if (pendingStroke.isEmpty())
        return;
    Operation operation;
    operation.layer = strokeLayer;
    operation.pixels.reserve(pendingStroke.size());
    for (auto c : pendingStroke)
        if (c.before != c.after)
            operation.pixels.push_back(c);
    pendingStroke.clear();
    strokeCover.clear();
    strokeTravel = 0;
    strokeLast.reset();
    operation.label = "Paint";
    if (!operation.pixels.empty())
        commit(std::move(operation));
}
bool Document::editLayer(const LayerEdit &edit, QString *error) {
    auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (image.isNull() || lifecycle == State::Closing)
        return fail("The document is not editable");
    finish();
    auto layer = findLayer(edit.layer);
    if (edit.command != LayerCommand::Add && !layer)
        return fail("Layer identity is not present");
    if (edit.command == LayerCommand::Select) {
        active = edit.layer;
        ++serial;
        return true;
    }
    if ((edit.command == LayerCommand::Rename || edit.command == LayerCommand::Add) &&
        (edit.name.trimmed().isEmpty() || edit.name.size() > 256))
        return fail("Use a layer name of 1–256 characters");
    Operation op;
    op.layer = edit.layer;
    op.activeBefore = active;
    op.activeAfter = active;
    const int index = layer ? int(layer - stack.data()) : int(stack.size()) - 1;
    switch (edit.command) {
    case LayerCommand::Add:
    case LayerCommand::Duplicate: {
        if (stack.size() >= LayerLimit ||
            pixelBytes() + 2 * image.sizeInBytes() > std::min(PixelLimit, animationLimits.decodedBytes))
            return fail("Layer limit: 64 layers / 128 MiB pixels including composite");
        RasterLayer added;
        added.id = QUuid::createUuid();
        if (edit.command == LayerCommand::Duplicate) {
            added = *layer;
            added.id = QUuid::createUuid();
            added.name = layer->name.left(240) + " copy";
        } else {
            added.name = edit.name.trimmed();
            if (!edit.pixels.isNull()) {
                if (edit.pixels.size() != image.size())
                    return fail("New layer pixels must match the document dimensions");
                added.pixels = edit.pixels.convertToFormat(QImage::Format_RGBA8888);
            } else {
                added.pixels = QImage(image.size(), QImage::Format_RGBA8888);
                if (!added.pixels.isNull())
                    added.pixels.fill(Qt::transparent);
            }
            if (added.pixels.isNull())
                return fail("Layer allocation failed");
            added.extensions = edit.extensions;
        }
        op.kind = Operation::Insert;
        op.layer = added.id;
        op.after = index + 1;
        op.snapshot = added;
        op.activeAfter = added.id;
        stack.insert(stack.begin() + op.after, std::move(added));
        active = op.activeAfter;
        if (!native) {
            native = true;
            saved.reset();
        }
        break;
    }
    case LayerCommand::Delete:
        if (stack.size() == 1)
            return fail("Keep at least one raster layer");
        op.kind = Operation::Remove;
        op.snapshot = *layer;
        op.before = index;
        stack.erase(stack.begin() + index);
        if (active == edit.layer)
            active = stack[size_t(std::min(index, int(stack.size()) - 1))].id;
        op.activeAfter = active;
        break;
    case LayerCommand::Rename:
        if (layer->name == edit.name.trimmed())
            return true;
        op.kind = Operation::Name;
        op.beforeName = layer->name;
        op.afterName = edit.name.trimmed();
        layer->name = op.afterName;
        ++layer->revision;
        break;
    case LayerCommand::Move:
        if (edit.value < 0 || edit.value >= int(stack.size()))
            return fail("Invalid layer position");
        if (index == edit.value)
            return true;
        op.kind = Operation::Order;
        op.before = index;
        op.after = edit.value;
        {
            auto moved = *layer;
            stack.erase(stack.begin() + index);
            stack.insert(stack.begin() + edit.value, std::move(moved));
        }
        break;
    case LayerCommand::Visibility:
        if (edit.value != 0 && edit.value != 1)
            return fail("Visibility must be zero or one");
        if (layer->visible == bool(edit.value))
            return true;
        op.kind = Operation::Visible;
        op.before = layer->visible;
        op.after = edit.value;
        layer->visible = bool(edit.value);
        ++layer->revision;
        break;
    case LayerCommand::Opacity:
        if (edit.value < 0 || edit.value > 255)
            return fail("Opacity must be between 0 and 255");
        if (layer->opacity == edit.value)
            return true;
        op.kind = Operation::Alpha;
        op.before = layer->opacity;
        op.after = edit.value;
        layer->opacity = edit.value;
        ++layer->revision;
        break;
    case LayerCommand::Select:
        break;
    }
    // Any layer property is editable-source state even with one remaining layer.
    if (!native) {
        native = true;
        saved.reset();
    }
    static const char *labels[] = {"Add Layer",   "Delete Layer",     "Duplicate Layer",
                                   "Rename Layer", "Move Layer",       "Layer Visibility",
                                   "Layer Opacity", "Select Layer"};
    op.label = labels[int(edit.command)];
    if (op.kind != Operation::Name)
        compose(image.rect());
    commit(std::move(op));
    return true;
}
void Document::apply(bool forward) { applyOperation(history.at(size_t(cursor)), forward); }
void Document::applyOperation(Operation &op, bool forward) {
    if (op.kind == Operation::Animation) {
        installAnimation(*(forward ? op.animationAfter : op.animationBefore));
        selectionMask = forward ? op.selectionAfter : op.selectionBefore;
        return;
    }
    if (op.kind == Operation::Compound) {
        if (forward)
            for (auto &child : op.children)
                applyOperation(child, true);
        else
            for (auto it = op.children.rbegin(); it != op.children.rend(); ++it)
                applyOperation(*it, false);
        return;
    }
    if (!op.frame.isNull() && op.frame != frameId) selectFrame(op.frame);
    if (op.kind == Operation::Selection) {
        selectionMask = forward ? op.selectionAfter : op.selectionBefore;
        ++serial;
        return;
    }
    if (op.kind == Operation::Raster) {
        const QSize target = forward ? op.sizeAfter : op.sizeBefore;
        const bool resized = op.sizeBefore != op.sizeAfter;
        for (auto &patch : op.patches) {
            auto layer = findLayer(patch.layer);
            if (!layer)
                continue;
            const QImage &pixels = forward ? patch.after : patch.before;
            if (resized) {
                layer->pixels = pixels;
            } else {
                PixelWrite write(image, layer->pixels);
                for (int y = 0; y < patch.rect.height(); ++y)
                    std::copy_n(pixels.constScanLine(y), patch.rect.width() * 4,
                                layer->pixels.scanLine(patch.rect.top() + y) +
                                    patch.rect.left() * 4);
            }
            ++layer->revision;
        }
        if (op.selectionChanged)
            selectionMask = forward ? op.selectionAfter : op.selectionBefore;
        if (!op.layer.isNull() && findLayer(op.layer))
            active = op.layer;
        ++serial;
        lifecycle = State::Ready;
        if (resized) {
            image = QImage(target, QImage::Format_RGBA8888);
            image.fill(Qt::transparent);
            compose(image.rect());
            dirty = image.rect();
        } else {
            QRect region;
            for (const auto &patch : op.patches)
                region = region.united(patch.rect);
            compose(region);
        }
        return;
    }
    auto layer = findLayer(op.layer);
    QRect region;
    if (op.kind == Operation::Pixels && layer) {
        {
            PixelWrite write(image, layer->pixels);
            for (auto c : op.pixels) {
                QPoint p(c.index % write.width, c.index / write.width);
                layer->pixels.setPixel(p, forward ? c.after : c.before);
                region = region.united(QRect(p, QSize(1, 1)));
            }
        }
        active = op.layer;
        ++layer->revision;
    } else if ((op.kind == Operation::Insert && forward) ||
               (op.kind == Operation::Remove && !forward)) {
        stack.insert(stack.begin() + (forward ? op.after : op.before), op.snapshot);
    } else if (op.kind == Operation::Insert || op.kind == Operation::Remove) {
        std::erase_if(stack, [&](const auto &l) { return l.id == op.layer; });
    } else if (op.kind == Operation::Order && layer) {
        auto moved = *layer;
        stack.erase(stack.begin() + (layer - stack.data()));
        stack.insert(stack.begin() + (forward ? op.after : op.before), std::move(moved));
    } else if (layer) {
        if (op.kind == Operation::Name)
            layer->name = forward ? op.afterName : op.beforeName;
        if (op.kind == Operation::Visible)
            layer->visible = bool(forward ? op.after : op.before);
        if (op.kind == Operation::Alpha)
            layer->opacity = forward ? op.after : op.before;
        ++layer->revision;
    }
    if (op.kind != Operation::Pixels) {
        active = forward ? op.activeAfter : op.activeBefore;
        if (op.kind != Operation::Name)
            region = image.rect();
    }
    ++serial;
    lifecycle = State::Ready;
    compose(region);
}
void Document::undo() {
    finish();
    if (cursor > 0) {
        --cursor;
        apply(false);
    }
}
void Document::redo() {
    finish();
    if (cursor < int(history.size())) {
        apply(true);
        ++cursor;
    }
}
qint64 Document::operationBytes(const Operation &op) {
    qint64 bytes = qint64(op.pixels.capacity()) * sizeof(Change);
    for (const auto &state : {op.animationBefore, op.animationAfter})
        if (state) {
            bytes += sizeof(AnimationState) + qint64(state->entries.size()) * sizeof(PlaybackEntry);
            for (const auto &frame : state->frames)
                for (const auto &layer : frame.layers)
                    bytes += sizeof(RasterLayer) + layer.pixels.sizeInBytes();
        }
    bytes += op.snapshot.pixels.sizeInBytes();
    bytes += (op.snapshot.name.capacity() + op.beforeName.capacity() + op.afterName.capacity() +
              op.label.capacity()) *
             sizeof(QChar);
    if (!op.snapshot.extensions.isEmpty())
        bytes += QJsonDocument(op.snapshot.extensions).toJson(QJsonDocument::Compact).size() * 16;
    for (const auto &patch : op.patches)
        bytes += sizeof(Patch) + patch.before.sizeInBytes() + patch.after.sizeInBytes();
    bytes += op.selectionBefore.sizeInBytes() + op.selectionAfter.sizeInBytes();
    bytes += qint64(op.children.capacity()) * sizeof(Operation);
    for (const auto &child : op.children)
        bytes += operationBytes(child);
    return bytes;
}
qint64 Document::historyBytes() const {
    qint64 bytes = qint64(history.capacity()) * sizeof(Operation);
    const bool frameHistory = animated() || std::any_of(history.begin(), history.end(), [](const auto &op) { return op.kind == Operation::Animation; });
    if (frameHistory) {
        // Charge each retained pixel buffer once, excluding buffers already owned by the
        // live document. This makes COW structural edits cheap without unbounding history.
        QSet<const uchar *> seen;
        if (!image.isNull()) seen.insert(image.constBits());
        for (const auto &layer : stack) seen.insert(layer.pixels.constBits());
        for (const auto &frame : animation.frames) for (const auto &layer : frame.layers) seen.insert(layer.pixels.constBits());
        auto pixels = [&](const QImage &im) -> qint64 {
            if (im.isNull() || seen.contains(im.constBits())) return 0;
            seen.insert(im.constBits()); return im.sizeInBytes();
        };
        auto layerBytes = [&](const RasterLayer &layer) {
            return pixels(layer.pixels) + layer.name.capacity() * qint64(sizeof(QChar)) +
                   QJsonDocument(layer.extensions).toJson(QJsonDocument::Compact).size() * 16;
        };
        std::function<qint64(const Operation &)> retained = [&](const Operation &op) {
            qint64 result = qint64(op.pixels.capacity()) * sizeof(Change) + layerBytes(op.snapshot) +
                (op.beforeName.capacity() + op.afterName.capacity() + op.label.capacity()) * qint64(sizeof(QChar));
            result += pixels(op.selectionBefore) + pixels(op.selectionAfter);
            for (const auto &patch : op.patches) result += sizeof(Patch) + pixels(patch.before) + pixels(patch.after);
            for (const auto &state : {op.animationBefore, op.animationAfter}) if (state) {
                result += sizeof(AnimationState) + qint64(state->frames.capacity()) * sizeof(SourceFrame) +
                    qint64(state->entries.capacity()) * sizeof(PlaybackEntry) +
                    QJsonDocument(state->metadata).toJson(QJsonDocument::Compact).size() * 16 + state->target.capacity() * qint64(sizeof(QChar));
                for (const auto &frame : state->frames) {
                    result += frame.name.capacity() * qint64(sizeof(QChar)) + qint64(frame.layers.capacity()) * sizeof(RasterLayer);
                    for (const auto &layer : frame.layers) result += layerBytes(layer);
                }
            }
            result += qint64(op.children.capacity()) * sizeof(Operation);
            for (const auto &child : op.children) result += retained(child);
            return result;
        };
        for (const auto &op : history) bytes += retained(op);
        return bytes;
    }
    for (const auto &op : history)
        bytes += operationBytes(op);
    return bytes;
}
void Document::setHistoryLimits(qint64 bytes, int operations) {
    finish();
    historyBudget = std::max<qint64>(128, bytes);
    operationLimit = std::max(0, operations);
    pruneHistory();
}
void Document::pruneHistory() {
    while (!history.empty() &&
           (historyBytes() > historyBudget || int(history.size()) > operationLimit)) {
        if (cursor > 0) {
            history.erase(history.begin());
            --cursor;
            if (saved) {
                if (*saved == 0)
                    saved.reset();
                else
                    --*saved;
            }
        } else {
            history.pop_back();
            if (saved && *saved > int(history.size()))
                saved.reset();
        }
        history.shrink_to_fit();
    }
    if (history.empty())
        history.shrink_to_fit();
}
bool Document::save(QString *error) {
    if (native) {
        if (error)
            *error = "Save the editable document or explicitly update the Minecraft PNG";
        return false;
    }
    return saveAsRevision(path, savedRevision, error);
}
bool Document::saveOver(const Revision &confirmed, QString *error) {
    return saveAsRevision(path, confirmed, error);
}
bool Document::saveAs(const QString &target, bool overwrite, QString *error) {
    if (pathIdentity(target) == pathIdentity(path))
        return saveAsRevision(path, savedRevision, error);
    auto expected = Revision::read(target);
    if (expected.exists && !overwrite) {
        if (error)
            *error = "The destination exists; confirm replacement before saving";
        return false;
    }
    return saveAsRevision(target, expected, error);
}
bool Document::saveAsRevision(const QString &target, const Revision &expected, QString *error) {
    finish();
    if (animated()) {
        if (error) *error = "Animated textures require guarded PNG + .mcmeta export from Animation > Export Minecraft Animation";
        return false;
    }
    lifecycle = State::Saving;
    lastError.clear();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (image.isNull() || QFileInfo(target).suffix().compare("png", Qt::CaseInsensitive) != 0 ||
        !image.save(&buffer, "PNG"))
        lastError = "Could not encode PNG";
    else
        atomicWrite(target, bytes, expected, &lastError);
    if (!lastError.isEmpty()) {
        lifecycle = State::Error;
        if (error)
            *error = lastError;
        return false;
    }
    // Export As keeps a native document bound to its original asset and baseline.
    if (!native || pathIdentity(target) == pathIdentity(path)) {
        path = QFileInfo(target).absoluteFilePath();
        savedRevision = {true, true, bytes.size(), 0,
                         QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
        exportedPixels = pixelDigest();
        if (native)
            exportMetadataChanged = true;
        else
            saved = cursor;
        ++serial;
    }
    lifecycle = State::Ready;
    return true;
}
bool Document::reload(QString *error) {
    auto replacement = open(path, error);
    if (!replacement)
        return false;
    auto keepId = identity;
    auto keepNativePath = nativePath;
    auto keepNativeRevision = nativeRevision;
    const auto nextSerial = serial + 1;
    const bool wasNative = native;
    *this = std::move(*replacement);
    identity = keepId;
    serial = nextSerial;
    if (wasNative) {
        native = true;
        nativePath = keepNativePath;
        nativeRevision = keepNativeRevision;
        saved.reset();
    }
    return true;
}
void Document::restorePixels(const QImage &pixels, const Revision &originalRevision) {
    Document restored(path, pixels);
    auto keepId = identity;
    const auto nextSerial = serial + 1;
    *this = std::move(restored);
    identity = keepId;
    savedRevision = originalRevision;
    saved.reset();
    dirty = image.rect();
    lifecycle = State::Modified;
    serial = nextSerial;
}
} // namespace nova
