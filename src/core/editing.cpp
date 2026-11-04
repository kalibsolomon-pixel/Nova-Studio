// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 2-C3 document commands: selections, the unified stroke engine, raster
// operations, document resizes, floating-selection commits and layer merges.
// Every mutation becomes exactly one bounded history entry (or a compound).
#include "core.h"
#include "pixelwrite.h"
#include <QDataStream>
#include <QIODevice>
#include <algorithm>

namespace nova {
namespace {
bool failWith(QString *error, const QString &text) {
    if (error)
        *error = text;
    return false;
}
bool sameMask(const QImage &a, const QImage &b) {
    if (a.isNull() || b.isNull())
        return a.isNull() == b.isNull();
    return a == b;
}
QRect diffRect(const QImage &a, const QImage &b) {
    int left = a.width(), right = -1, top = a.height(), bottom = -1;
    for (int y = 0; y < a.height(); ++y) {
        auto pa = reinterpret_cast<const quint32 *>(a.constScanLine(y));
        auto pb = reinterpret_cast<const quint32 *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            if (pa[x] != pb[x]) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = y;
            }
    }
    return right < 0 ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}
} // namespace

const RasterLayer *Document::layer(QUuid id) const {
    for (const auto &l : stack)
        if (l.id == id)
            return &l;
    return nullptr;
}
void Document::beginCompound(const QString &label) {
    finish();
    if (compoundDepth++ == 0) {
        compoundChildren.clear();
        compoundLabel = label;
    }
}
bool Document::endCompound(bool keep, const QString &owner) {
    if (--compoundDepth > 0)
        return keep;
    auto children = std::move(compoundChildren);
    compoundChildren.clear();
    if (!keep) {
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            applyOperation(*it, false);
        return false;
    }
    if (children.empty())
        return true;
    if (children.size() == 1) {
        children[0].label = compoundLabel;
        children[0].automationOwner = owner;
        commit(std::move(children[0]));
        return true;
    }
    Operation op;
    op.kind = Operation::Compound;
    op.label = compoundLabel;
    op.automationOwner = owner;
    op.children = std::move(children);
    commit(std::move(op));
    return true;
}
// ---------------------------------------------------------------- selection
bool Document::setSelectionInternal(const QImage &mask, const QString &label) {
    finish();
    QImage next = mask;
    if (!next.isNull()) {
        if (next.size() != image.size())
            return false;
        next = next.convertToFormat(QImage::Format_Grayscale8);
        // An allocated zero mask selects zero pixels. Only an explicit null
        // mask (Deselect) removes the editing restriction.
    }
    if (sameMask(next, selectionMask))
        return true;
    Operation op;
    op.kind = Operation::Selection;
    op.label = label;
    op.selectionBefore = selectionMask;
    op.selectionAfter = next;
    selectionMask = next;
    commit(std::move(op));
    return true;
}
bool Document::setSelection(const QImage &mask, const QString &label, QString *error) {
    if (image.isNull() || lifecycle == State::Closing)
        return failWith(error, "The document is not editable");
    if (!mask.isNull() && mask.size() != image.size())
        return failWith(error, "Selection mask must match the document dimensions");
    return setSelectionInternal(mask, label);
}
bool Document::select(const QImage &shape, ops::Combine mode, const QString &label, QString *error) {
    if (shape.size() != image.size())
        return failWith(error, "Selection shape must match the document dimensions");
    return setSelection(ops::combine(selectionMask, shape.convertToFormat(QImage::Format_Grayscale8), mode),
                        label, error);
}
bool Document::selectAll() { return setSelection(ops::fullMask(image.size()), "Select All"); }
bool Document::deselect() { return setSelection({}, "Deselect"); }
bool Document::invertSelection() {
    return setSelection(selectionMask.isNull() ? ops::fullMask(image.size()) : ops::invertMask(selectionMask),
                        "Invert Selection");
}
// ---------------------------------------------------------------- strokes
bool Document::stroke(QUuid id, QPoint a, QPoint b, const StrokeStyle &style, QString *error) {
    auto target = findLayer(id);
    if (!target || id != active)
        return failWith(error, "Select the target layer before drawing");
    if (!target->visible)
        return failWith(error, "Show the selected layer before drawing");
    const QRect reach = image.rect().adjusted(-ops::Brush::MaxSize, -ops::Brush::MaxSize,
                                              ops::Brush::MaxSize, ops::Brush::MaxSize);
    if (lifecycle == State::Closing || image.isNull() || !reach.contains(a) || !reach.contains(b))
        return failWith(error, "Invalid drawing coordinates or closed document");
    if (style.brush.size < 1 || style.brush.size > ops::Brush::MaxSize || style.brush.opacity < 0 ||
        style.brush.opacity > 255 || style.brush.height < 0 || style.brush.height > ops::Brush::MaxSize)
        return failWith(error, "Brush size must be 1â€“256 and opacity 0â€“255");
    // A different layer or style starts a new transaction (and coverage map).
    QByteArray key;
    {
        QDataStream k(&key, QIODevice::WriteOnly);
        k << style.color << style.erase << int(style.brush.shape) << style.brush.size << style.brush.height
          << style.brush.opacity << style.brush.hardness << style.brush.spacing << style.replaceOnly
          << style.replaceColor << style.replaceTolerance << style.lockAlpha << int(style.symmetry)
          << style.radial << qint64(style.brush.stamp.cacheKey()) << style.brush.sourceColors;
        for (auto color : style.palette) k << color;
    }
    if (!pendingStroke.isEmpty() && (strokeLayer != id || strokeKey != key))
        finish();
    strokeKey = key;
    strokeLayer = id;
    lifecycle = State::Ready;
    const auto dabs = ops::footprint(style.brush);
    const auto dimensions = ops::brushDimensions(style.brush);
    const int size = std::max(dimensions.width(), dimensions.height());
    const int step = style.brush.spacing <= 0
                         ? 1
                         : std::max(1, int(std::lround(size * style.brush.spacing / 100.0)));
    const QPointF centre = style.centre.value_or(QPointF(image.width() / 2.0, image.height() / 2.0));
    const QRgb paintColor =
        style.palette.empty() ? style.color : ops::nearest(style.color, style.palette);
    const int width = image.width();
    // Captured up front: PixelWrite temporarily releases the composite view.
    const QRect bounds = image.rect();
    const QSize canvas = image.size();
    QRect region;
    auto apply = [&](int x, int y, int coverage, QRgb dabColor) {
        if (!bounds.contains(x, y))
            return;
        coverage = coverage * ops::maskValue(&selectionMask, x, y) / 255;
        if (coverage <= 0)
            return;
        const int index = y * width + x;
        auto cover = strokeCover.find(index);
        // Max coverage per stroke: overlapping dabs never compound opacity.
        if (cover != strokeCover.end() && *cover >= coverage)
            return;
        strokeCover.insert(index, uchar(coverage));
        auto existing = pendingStroke.find(index);
        const QRgb before =
            existing != pendingStroke.end() ? existing->before : target->pixels.pixel(x, y);
        if (style.replaceOnly && !ops::colorMatches(before, style.replaceColor, style.replaceTolerance,
                                                    ops::Distance::Rgba, true))
            return;
        const int effective = (coverage * style.brush.opacity + 127) / 255;
        const QRgb pixelColor = style.brush.sourceColors && style.brush.shape == ops::BrushShape::Custom && !style.brush.stamp.isNull()
            ? (style.palette.empty() ? dabColor : ops::nearest(dabColor,style.palette)) : paintColor;
        QRgb after = before;
        if (style.erase) {
            const int alpha = qAlpha(before) * (255 - effective) / 255;
            after = alpha == 0 ? qRgba(0, 0, 0, 0)
                               : qRgba(qRed(before), qGreen(before), qBlue(before), alpha);
        } else if (style.lockAlpha) {
            if (!qAlpha(before))
                return;
            const QRgb mixed = ops::blend(qRgba(qRed(before), qGreen(before), qBlue(before), 255),
                                          pixelColor, effective);
            after = qRgba(qRed(mixed), qGreen(mixed), qBlue(mixed), qAlpha(before));
        } else if (effective >= 255) {
            after = pixelColor; // exact pixel-art replacement, as the classic pencil
        } else {
            after = ops::blend(before, pixelColor, effective);
        }
        if (existing != pendingStroke.end())
            existing->after = after;
        else if (after != before)
            pendingStroke.insert(index, {index, before, after});
        else
            return;
        target->pixels.setPixel(x, y, after);
        region = region.united(QRect(x, y, 1, 1));
    };
    {
        PixelWrite write(image, target->pixels);
        const auto points = line(a, b);
        for (int i = 0; i < points.size(); ++i) {
            const QPoint p = points[i];
            // A continued stroke starts where the previous segment ended.
            if (i == 0 && strokeLast && *strokeLast == p)
                continue;
            // Distance-based spacing carried across segments, independent of
            // how often the pointer reports positions.
            const bool place = strokeTravel % step == 0;
            ++strokeTravel;
            if (!place)
                continue;
            for (auto q : ops::symmetricPoints(p, canvas, style.symmetry, style.radial, centre)) {
                const bool mirroredX = q.x() != p.x() && (style.symmetry == ops::Symmetry::Horizontal ||
                                                          style.symmetry == ops::Symmetry::Both);
                const bool mirroredY = q.y() != p.y() && (style.symmetry == ops::Symmetry::Vertical ||
                                                          style.symmetry == ops::Symmetry::Both);
                for (const auto &d : dabs)
                    apply(q.x() + (mirroredX ? -d.offset.x() : d.offset.x()),
                          q.y() + (mirroredY ? -d.offset.y() : d.offset.y()), d.coverage, d.color);
            }
        }
        strokeLast = b;
        if (qint64(pendingStroke.size()) * 80 >= historyBudget / 2)
            finish();
    }
    if (!region.isEmpty()) {
        ++target->revision;
        ++serial;
        compose(region);
    }
    return true;
}
void Document::cancelStroke() {
    if (pendingStroke.isEmpty()) {
        strokeCover.clear();
        strokeTravel = 0;
        strokeLast.reset();
        return;
    }
    auto target = findLayer(strokeLayer);
    QRect region;
    if (target) {
        PixelWrite write(image, target->pixels);
        for (const auto &c : std::as_const(pendingStroke)) {
            const QPoint p(c.index % write.width, c.index / write.width);
            target->pixels.setPixel(p, c.before);
            region = region.united(QRect(p, QSize(1, 1)));
        }
        ++target->revision;
    }
    pendingStroke.clear();
    strokeCover.clear();
    strokeTravel = 0;
    strokeLast.reset();
    ++serial;
    compose(region);
}
// ---------------------------------------------------------------- raster ops
bool Document::rasterCommit(std::vector<Patch> patches, const QString &label, QSize newSize,
                            const QImage *newSelection) {
    Operation op;
    op.kind = Operation::Raster;
    op.label = label;
    op.sizeBefore = image.size();
    op.sizeAfter = newSize.isValid() ? newSize : image.size();
    op.patches = std::move(patches);
    op.layer = op.patches.size() == 1 ? op.patches[0].layer : active;
    if (newSelection && !sameMask(*newSelection, selectionMask)) {
        op.selectionChanged = true;
        op.selectionBefore = selectionMask;
        op.selectionAfter = *newSelection;
    }
    if (op.patches.empty() && !op.selectionChanged && op.sizeBefore == op.sizeAfter)
        return true;
    applyOperation(op, true);
    commit(std::move(op));
    return true;
}
bool Document::replaceLayerPixels(QUuid id, const QImage &input, const QString &label, QString *error,
                                  const QImage *newSelection) {
    finish();
    auto target = findLayer(id);
    if (!target)
        return failWith(error, "Layer identity is not present");
    if (!target->visible)
        return failWith(error, "Show the selected layer before editing it");
    if (image.isNull() || lifecycle == State::Closing)
        return failWith(error, "The document is not editable");
    if (input.size() != image.size())
        return failWith(error, "Replacement pixels must match the document dimensions");
    const QImage pixels = input.convertToFormat(QImage::Format_RGBA8888);
    const QRect rect = diffRect(target->pixels, pixels);
    std::vector<Patch> patches;
    if (!rect.isEmpty())
        patches.push_back({id, rect, target->pixels.copy(rect), pixels.copy(rect)});
    return rasterCommit(std::move(patches), label, {}, newSelection);
}
QImage Document::compositeWith(const QMap<QUuid, QImage> &replacements) const {
    auto layers = stack;
    for (auto &layer : layers)
        if (replacements.contains(layer.id))
            layer.pixels = replacements[layer.id];
    QImage result(image.size(), QImage::Format_RGBA8888);
    result.fill(Qt::transparent);
    composeInto(layers, result, image.rect());
    return result;
}
bool Document::replaceLayersPixels(const QMap<QUuid, QImage> &pixels, const QString &label,
                                   bool includeHidden, QString *error, const QImage *newSelection) {
    if (pixels.isEmpty() || image.isNull() || lifecycle == State::Closing)
        return failWith(error, "No editable transform targets");
    std::vector<Patch> patches;
    finish();
    for (auto i = pixels.cbegin(); i != pixels.cend(); ++i) {
        const auto target = layer(i.key());
        if (!target || (!target->visible && !includeHidden) || i->size() != image.size())
            return failWith(error,
                            "A transform target changed, became hidden or has invalid dimensions");
        const auto replacement = i->convertToFormat(QImage::Format_RGBA8888);
        const auto rect = diffRect(target->pixels, replacement);
        if (!rect.isEmpty())
            patches.push_back({i.key(), rect, target->pixels.copy(rect), replacement.copy(rect)});
    }
    Operation estimate;
    estimate.kind = Operation::Raster;
    estimate.label = label;
    estimate.patches = patches;
    if (newSelection) {
        estimate.selectionBefore = selectionMask;
        estimate.selectionAfter = *newSelection;
    }
    if (!patches.empty() &&
        (operationLimit < 1 || operationBytes(estimate) + sizeof(Operation) > historyBudget))
        return failWith(error, "Transform exceeds the undo history budget; increase File > Undo "
                               "history limits before committing");
    return rasterCommit(std::move(patches), label, {}, newSelection);
}
bool Document::applyOperation(QUuid id, const ops::Spec &input, QString *error,
                              const ops::Context *context, QUuid *createdLayer) {
    auto spec = input;
    if (!ops::validate(spec, error))
        return false;
    const auto info = ops::find(spec.id);
    if (info->changesSize)
        return applyDocumentOperation(spec, error, context);
    auto target = findLayer(id);
    if (!target)
        return failWith(error, "Layer identity is not present");
    if (!target->visible)
        return failWith(error, "Show the selected layer before editing it");
    finish();
    ops::Context ctx = context ? *context : ops::Context{};
    if (hasSelection() && ops::maskEmpty(selectionMask))
        return true; // no selected pixels: no effect, generator or empty shading layer
    ctx.mask = hasSelection() ? &selectionMask : nullptr;
    if (info->generator) {
        if (spec.params["width"].toInt() != image.width() || spec.params["height"].toInt() != image.height())
            return failWith(error, "Generator size must match the document dimensions");
        ctx.mask = nullptr;
    }
    const bool shadingLayer =
        spec.id == "directional_shading" && spec.params["output"].toString() == "Shading layer";
    if (shadingLayer)
        ctx.mask = nullptr; // the overlay is masked below instead of blended with the source
    const auto result = ops::run(spec, target->pixels, ctx);
    if (!result.ok())
        return failWith(error, result.canceled ? "Operation canceled" : result.error);
    QImage out = result.image;
    if (info->generator && hasSelection())
        out = ops::applyMask(target->pixels, out, &selectionMask);
    if (shadingLayer) {
        if (hasSelection())
            out = ops::maskAlpha(out, selectionMask);
        const auto name = target->name.left(200) + " shading";
        const bool ok = addLayerWithPixels(name, out, {{"nova.effect", spec.toJson()}}, error, createdLayer);
        if (ok && !compoundDepth && !history.empty())
            history.back().label = info->name;
        return ok;
    }
    return replaceLayerPixels(id, out, info->name, error);
}
bool Document::applyLayers(const QVector<QUuid> &ids, const ops::Spec &input, QString *error,
                           const ops::Context *context) {
    auto spec = input;
    if (!ops::validate(spec,error)) return false;
    if (ids.isEmpty() || image.isNull() || lifecycle == State::Closing)
        return failWith(error,"Select at least one layer in an editable document");
    QSet<QUuid> unique;
    for (auto id : ids) {
        if (unique.contains(id) || !layer(id)) return failWith(error,"Duplicate or missing target layer");
        unique.insert(id);
    }
    const auto info=ops::find(spec.id);
    if (info->changesSize) {
        if (unique.size()!=qsizetype(stack.size())) return failWith(error,"Canvas resize requires All layers scope; use Scale Content to resize selected pixels");
        return applyDocumentOperation(spec,error,context);
    }
    if (info->generator || (spec.id=="directional_shading" && spec.params["output"].toString()=="Shading layer"))
        return failWith(error,"Layer-creating operations require active-layer scope");
    // Prepare every result before changing state. One raster transaction preserves
    // each layer's pixels/identity and rolls undo/redo across the whole scope.
    const qint64 needed=qint64(unique.size())*image.sizeInBytes()*3;
    if (needed>PixelLimit*3) return failWith(error,"Multi-layer operation exceeds temporary pixel budget");
    ops::Context ctx=context?*context:ops::Context{};
    ctx.mask=hasSelection()?&selectionMask:nullptr;
    std::vector<Patch> patches;
    for (auto id:ids) {
        const auto target=layer(id);
        auto result=ops::run(spec,target->pixels,ctx);
        if (!result.ok()) return failWith(error,result.canceled?"Operation canceled":result.error);
        const auto r=diffRect(target->pixels,result.image);
        if (!r.isEmpty()) patches.push_back({id,r,target->pixels.copy(r),result.image.copy(r)});
    }
    finish();
    return rasterCommit(std::move(patches),info->name+QString(" (%1 layers)").arg(ids.size()));
}
bool Document::applyDocumentOperation(ops::Spec spec, QString *error, const ops::Context *context) {
    if (animated()) {
        std::vector<QUuid> ids;
        for (const auto &frame : animationState().frames) ids.push_back(frame.id);
        return applyFrames(ids, spec, error, context);
    }
    if (!ops::validate(spec, error))
        return false;
    const auto info = ops::find(spec.id);
    if (!info->changesSize)
        return failWith(error, info->name + " is a layer operation");
    if (image.isNull() || lifecycle == State::Closing)
        return failWith(error, "The document is not editable");
    finish();
    if (spec.id == "trim") {
        // Trim the whole document to the visible composite bounds.
        const auto bounds = ops::alphaBounds(image, spec.params["alpha_threshold"].toInt());
        if (bounds.isEmpty())
            return failWith(error, "The image has no visible pixels to keep");
        spec = {"crop", {{"x", bounds.x()}, {"y", bounds.y()}, {"width", bounds.width()},
                         {"height", bounds.height()}}};
        if (!ops::validate(spec, error))
            return false;
    }
    const QSize size = ops::outputSize(spec, image.size());
    if (size.isEmpty() || size.width() > MaxDimension || size.height() > MaxDimension)
        return failWith(error, "Result must be 1â€“4096 pixels per side");
    if ((qint64(stack.size()) + 1) * size.width() * size.height() * 4 > PixelLimit)
        return failWith(error, "Result exceeds the 128 MiB document pixel budget");
    std::vector<Patch> patches;
    for (const auto &l : stack) {
        auto result = ops::run(spec, l.pixels, context ? *context : ops::Context{});
        if (!result.ok())
            return failWith(error, result.error);
        if (result.image.size() != size)
            return failWith(error, "Layer results have inconsistent dimensions");
        patches.push_back({l.id, QRect(QPoint(), size), l.pixels, result.image});
    }
    const QImage none;
    return rasterCommit(std::move(patches), info->name, size, &none);
}
bool Document::addLayerWithPixels(const QString &name, const QImage &pixels, const QJsonObject &layerExtensions,
                                  QString *error, QUuid *created) {
    LayerEdit edit{LayerCommand::Add, active, name.trimmed().isEmpty() ? QString("Layer") : name};
    edit.pixels = pixels;
    edit.extensions = layerExtensions;
    if (!editLayer(edit, error))
        return false;
    if (created)
        *created = active;
    return true;
}
bool Document::commitFloating(QUuid id, const QImage &floating, const QImage *liftMask,
                              const QString &label, bool asNewLayer, QString *error,
                              const QImage *newSelection) {
    auto target = findLayer(id);
    if (!target)
        return failWith(error, "Layer identity is not present");
    if (floating.size() != image.size() || (liftMask && liftMask->size() != image.size()))
        return failWith(error, "Floating pixels must match the document dimensions");
    beginCompound(label);
    bool ok = true;
    QImage base = target->pixels.copy();
    if (liftMask) {
        for (int y = 0; y < base.height(); ++y) {
            auto row = base.scanLine(y);
            auto m = liftMask->constScanLine(y);
            for (int x = 0; x < base.width(); ++x) {
                const int a = row[x * 4 + 3] * (255 - m[x]) / 255;
                row[x * 4 + 3] = uchar(a);
                if (!a)
                    row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0;
            }
        }
        ok = replaceLayerPixels(id, base, label, error);
    }
    if (ok && asNewLayer) {
        ok = addLayerWithPixels(label, floating.convertToFormat(QImage::Format_RGBA8888), {}, error);
    } else if (ok) {
        const QImage source = floating.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < base.height(); ++y)
            for (int x = 0; x < base.width(); ++x) {
                const QRgb s = source.pixel(x, y);
                if (qAlpha(s))
                    base.setPixel(x, y, ops::blend(base.pixel(x, y), s, 255));
            }
        ok = replaceLayerPixels(id, base, label, error);
    }
    if (ok && newSelection)
        ok = setSelection(*newSelection, label, error);
    return endCompound(ok);
}
bool Document::fill(QUuid id, QPoint seed, QRgb color, int tolerance, ops::Distance metric,
                    bool contiguous, bool sampleComposite, int opacity, QString *error) {
    auto target = findLayer(id);
    if (!target || id != active)
        return failWith(error, "Select the target layer before filling");
    if (!image.rect().contains(seed))
        return failWith(error, "Fill seed is outside the image");
    const auto region = ops::colorMask(sampleComposite ? image : target->pixels, seed,
                                       std::clamp(tolerance, 0, 255), metric, contiguous, true);
    const QImage out = ops::paintCoverage(target->pixels, region, color, opacity, false, &selectionMask);
    return replaceLayerPixels(id, out, contiguous ? "Fill" : "Global Fill", error);
}
bool Document::drawShape(QUuid id, const ops::ShapeSpec &shape, QRgb color, int opacity, bool erase,
                         QString *error) {
    auto target = findLayer(id);
    if (!target || id != active)
        return failWith(error, "Select the target layer before drawing");
    const auto mask = ops::rasterizeShape(image.size(), shape);
    const QImage out = ops::paintCoverage(target->pixels, mask, color, opacity, erase, &selectionMask);
    return replaceLayerPixels(id, out, ops::shapeNames().value(int(shape.kind), "Shape"), error);
}
bool Document::clearSelected(QUuid id, QString *error) {
    auto target = findLayer(id);
    if (!target)
        return failWith(error, "Layer identity is not present");
    QImage out = target->pixels.copy();
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) {
            const int m = ops::maskValue(&selectionMask, x, y);
            if (!m)
                continue;
            const QRgb c = out.pixel(x, y);
            const int a = qAlpha(c) * (255 - m) / 255;
            out.setPixel(x, y, a ? qRgba(qRed(c), qGreen(c), qBlue(c), a) : 0);
        }
    return replaceLayerPixels(id, out, "Clear", error);
}
bool Document::mergeDown(QUuid id, QString *error) {
    auto upper = findLayer(id);
    if (!upper)
        return failWith(error, "Layer identity is not present");
    const int index = int(upper - stack.data());
    if (index == 0)
        return failWith(error, "There is no layer below to merge into");
    const auto top = *upper, bottom = stack[size_t(index - 1)];
    if (!bottom.visible)
        return failWith(error, "Show the layer below before merging");
    QImage merged = bottom.pixels.copy();
    if (top.visible && top.opacity)
        for (int y = 0; y < merged.height(); ++y)
            for (int x = 0; x < merged.width(); ++x) {
                const QRgb s = top.pixels.pixel(x, y);
                if (qAlpha(s))
                    merged.setPixel(x, y, ops::blend(merged.pixel(x, y), s, top.opacity));
            }
    beginCompound("Merge Down");
    bool ok = replaceLayerPixels(bottom.id, merged, "Merge Down", error) &&
              editLayer({LayerCommand::Delete, top.id}, error) &&
              editLayer({LayerCommand::Select, bottom.id}, error);
    return endCompound(ok);
}
bool Document::flatten(QString *error) {
    if (stack.size() == 1 && stack[0].visible && stack[0].opacity == 255)
        return true;
    const QImage composite = image.copy();
    const QUuid bottom = stack[0].id;
    std::vector<QUuid> others;
    for (size_t i = 1; i < stack.size(); ++i)
        others.push_back(stack[i].id);
    beginCompound("Flatten Image");
    bool ok = editLayer({LayerCommand::Visibility, bottom, {}, 1}, error) &&
              editLayer({LayerCommand::Opacity, bottom, {}, 255}, error) &&
              editLayer({LayerCommand::Select, bottom}, error) &&
              replaceLayerPixels(bottom, composite, "Flatten Image", error);
    for (auto it = others.rbegin(); ok && it != others.rend(); ++it)
        ok = editLayer({LayerCommand::Delete, *it}, error);
    return endCompound(ok);
}
// ---------------------------------------------------------------- previews
QImage Document::compositeWith(QUuid id, const QImage &replacement) const {
    auto layers = stack;
    for (auto &l : layers)
        if (l.id == id)
            l.pixels = replacement.convertToFormat(QImage::Format_RGBA8888);
    if (layers.size() == 1 && layers[0].visible && layers[0].opacity == 255)
        return layers[0].pixels;
    QImage out(image.size(), QImage::Format_RGBA8888);
    out.fill(Qt::transparent);
    composeInto(layers, out, out.rect());
    return out;
}
QImage Document::compositeWithInserted(QUuid above, const QImage &pixels) const {
    auto layers = stack;
    RasterLayer extra;
    extra.pixels = pixels.convertToFormat(QImage::Format_RGBA8888);
    size_t position = layers.size();
    for (size_t i = 0; i < layers.size(); ++i)
        if (layers[i].id == above)
            position = i + 1;
    layers.insert(layers.begin() + qsizetype(position), extra);
    QImage out(image.size(), QImage::Format_RGBA8888);
    out.fill(Qt::transparent);
    composeInto(layers, out, out.rect());
    return out;
}
QImage Document::selectedPixels(QUuid id) const {
    auto l = layer(id);
    if (!l)
        return {};
    QImage out = l->pixels.copy();
    if (selectionMask.isNull())
        return out;
    for (int y = 0; y < out.height(); ++y) {
        auto row = out.scanLine(y);
        auto m = selectionMask.constScanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            const int a = row[x * 4 + 3] * m[x] / 255;
            row[x * 4 + 3] = uchar(a);
            if (!a)
                row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0;
        }
    }
    return out;
}
QStringList Document::historyLabels() const {
    QStringList out;
    for (const auto &op : history)
        out.append(op.label.isEmpty() ? QString("Edit") : op.label);
    return out;
}
void Document::jumpHistory(int target) {
    target = std::clamp(target, 0, int(history.size()));
    finish();
    while (cursor > target)
        undo();
    while (cursor < target)
        redo();
}
} // namespace nova
