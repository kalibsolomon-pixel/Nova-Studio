// SPDX-License-Identifier: GPL-3.0-or-later
// DocumentSession editing commands. Each validates document ownership and the
// session pixel budget, delegates to the Document model and emits changed().
#include "session.h"
#include <QJsonArray>
#include <algorithm>

namespace nova {
bool DocumentSession::replaceLayers(const std::shared_ptr<Document> &doc,
                                    const QMap<QUuid, QImage> &pixels, const QString &label,
                                    bool includeHidden, QString *error, const QImage *selection) {
    if (!owns(doc, error))
        return false;
    const bool ok = doc->replaceLayersPixels(pixels, label, includeHidden, error, selection);
    if (ok)
        emit changed(doc->id());
    return ok;
}
bool DocumentSession::animationEdit(const std::shared_ptr<Document> &doc, const AnimationState &state,
                                     const QString &label, QString *error) {
    if (!owns(doc, error)) return false;
    qint64 estimate = doc->image.sizeInBytes();
    for (const auto &frame : state.frames)
        for (const auto &layer : frame.layers) estimate += layer.pixels.sizeInBytes();
    if (!withinPixelBudget(doc, estimate - doc->pixelBytes())) {
        if (error) *error = "Animation exceeds the 512 MiB session pixel limit";
        return false;
    }
    const bool ok = doc->replaceAnimation(state, label, error);
    if (ok) emit changed(doc->id());
    return ok;
}
bool DocumentSession::frameEdit(const std::shared_ptr<Document> &doc, FrameCommand command, QUuid frame,
                                int position, const QString &name, QString *error) {
    if (!owns(doc, error)) return false;
    AnimationState state;
    QString label;
    if (!doc->prepareFrameEdit(command, frame, position, name, state, label, error))
        return false;
    return animationEdit(doc, state, label, error);
}
bool DocumentSession::frameSelect(const std::shared_ptr<Document> &doc, QUuid frame, QString *error) {
    if (!owns(doc, error)) return false;
    const bool ok = doc->selectFrame(frame, error);
    if (ok) emit changed(doc->id());
    return ok;
}
bool DocumentSession::owns(const std::shared_ptr<Document> &doc, QString *error) const {
    if (doc && std::find(documents.begin(), documents.end(), doc) != documents.end())
        return true;
    if (error)
        *error = "The target document is not open";
    return false;
}
bool DocumentSession::withinPixelBudget(const std::shared_ptr<Document> &doc, qint64 extra) const {
    qint64 bytes = extra;
    for (const auto &d : documents)
        bytes += d == doc ? d->pixelBytes() : d->pixelBytes();
    return bytes <= 512LL * 1024 * 1024;
}
bool DocumentSession::stroke(const std::shared_ptr<Document> &doc, QUuid layer, QPoint a, QPoint b,
                             const StrokeStyle &style, bool finishStroke, QString *error) {
    if (!owns(doc, error))
        return false;
    const bool ok = doc->stroke(layer, a, b, style, error);
    if (finishStroke)
        doc->finish();
    if (ok)
        emit changed(doc->id());
    return ok;
}
void DocumentSession::finishStroke(const std::shared_ptr<Document> &doc) {
    if (owns(doc)) {
        doc->finish();
        emit changed(doc->id());
    }
}
void DocumentSession::cancelStroke(const std::shared_ptr<Document> &doc) {
    if (owns(doc)) {
        doc->cancelStroke();
        emit changed(doc->id());
    }
}
bool DocumentSession::select(const std::shared_ptr<Document> &doc, const QImage &shape,
                             ops::Combine mode, const QString &label, QString *error) {
    if (!owns(doc, error))
        return false;
    const bool ok = doc->select(shape, mode, label, error);
    if (ok)
        emit changed(doc->id());
    return ok;
}
bool DocumentSession::selection(const std::shared_ptr<Document> &doc, SelectionCommand command) {
    if (!owns(doc))
        return false;
    const bool ok = command == SelectionCommand::All    ? doc->selectAll()
                    : command == SelectionCommand::None ? doc->deselect()
                                                        : doc->invertSelection();
    emit changed(doc->id());
    return ok;
}
bool DocumentSession::apply(const std::shared_ptr<Document> &doc, QUuid layer, const ops::Spec &input,
                            QString *error, const ops::Context *context) {
    if (!owns(doc, error))
        return false;
    auto spec = input;
    if (!ops::validate(spec, error))
        return false;
    const auto info = ops::find(spec.id);
    qint64 extra = 0;
    if (info->changesSize) {
        const auto size = ops::outputSize(spec, doc->image.size());
        qint64 layerCount = 0;
        for (const auto &frame : doc->animationState().frames) layerCount += frame.layers.size();
        extra = (layerCount + 1) * size.width() * size.height() * 4 - doc->pixelBytes();
    } else if (spec.id == "directional_shading" && spec.params["output"].toString() == "Shading layer") {
        extra = doc->image.sizeInBytes();
    }
    if (extra > 0 && !withinPixelBudget(doc, extra)) {
        if (error)
            *error = "Close another document first (512 MiB session pixel limit)";
        return false;
    }
    const bool ok = doc->applyOperation(layer, spec, error, context);
    if (ok)
        emit changed(doc->id());
    return ok;
}
bool DocumentSession::fill(const std::shared_ptr<Document> &doc, QUuid layer, QPoint seed, QRgb color,
                           int tolerance, ops::Distance metric, bool contiguous, bool sampleComposite,
                           int opacity, QString *error) {
    if (!owns(doc, error))
        return false;
    const bool ok = doc->fill(layer, seed, color, tolerance, metric, contiguous, sampleComposite, opacity, error);
    if (ok)
        emit changed(doc->id());
    return ok;
}
bool DocumentSession::applyLayers(const std::shared_ptr<Document> &doc, const QVector<QUuid> &layers,
                                  const ops::Spec &input, QString *error, const ops::Context *context) {
    if (!owns(doc,error)) return false;
    auto spec=input;if(!ops::validate(spec,error))return false;
    if (auto info=ops::find(spec.id);info && info->changesSize) {
        const auto size=ops::outputSize(spec,doc->image.size());
        qint64 layerCount=0;
        for(const auto &frame:doc->animationState().frames)layerCount+=frame.layers.size();
        const qint64 estimate=(layerCount+1)*qint64(size.width())*size.height()*4;
        if (!withinPixelBudget(doc,estimate-doc->pixelBytes())) {if(error)*error="Session pixel budget exceeded";return false;}
    }
    const auto ok=doc->applyLayers(layers,spec,error,context);
    if(ok)emit changed(doc->id());return ok;
}
bool DocumentSession::shape(const std::shared_ptr<Document> &doc, QUuid layer, const ops::ShapeSpec &shape,
                            QRgb color, int opacity, bool erase, QString *error) {
    if (!owns(doc, error))
        return false;
    const bool ok = doc->drawShape(layer, shape, color, opacity, erase, error);
    if (ok)
        emit changed(doc->id());
    return ok;
}
bool DocumentSession::commitFloating(const std::shared_ptr<Document> &doc, QUuid layer,
                                     const QImage &floating, const QImage *liftMask,
                                     const QString &label, bool asNewLayer, QString *error,
                                     const QImage *newSelection) {
    if (!owns(doc, error))
        return false;
    if (asNewLayer && !withinPixelBudget(doc, doc->image.sizeInBytes())) {
        if (error)
            *error = "Close another document first (512 MiB session pixel limit)";
        return false;
    }
    const bool ok = doc->commitFloating(layer, floating, liftMask, label, asNewLayer, error, newSelection);
    emit changed(doc->id());
    return ok;
}
bool DocumentSession::replacePixels(const std::shared_ptr<Document> &doc, QUuid layer,
                                    const QImage &pixels, const QString &label, QString *error) {
    if (!owns(doc, error))
        return false;
    const bool ok = doc->replaceLayerPixels(layer, pixels, label, error);
    if (ok)
        emit changed(doc->id());
    return ok;
}
bool DocumentSession::addLayer(const std::shared_ptr<Document> &doc, const QString &name,
                               const QImage &pixels, const QJsonObject &extensions, QString *error) {
    if (!owns(doc, error))
        return false;
    if (!withinPixelBudget(doc, 2 * doc->image.sizeInBytes())) {
        if (error)
            *error = "Close another document before adding layer pixels (512 MiB limit)";
        return false;
    }
    const bool ok = doc->addLayerWithPixels(name, pixels, extensions, error);
    if (ok)
        emit changed(doc->id());
    return ok;
}
bool DocumentSession::mergeDown(const std::shared_ptr<Document> &doc, QUuid layer, QString *error) {
    if (!owns(doc, error))
        return false;
    const bool ok = doc->mergeDown(layer, error);
    emit changed(doc->id());
    return ok;
}
bool DocumentSession::flatten(const std::shared_ptr<Document> &doc, QString *error) {
    if (!owns(doc, error))
        return false;
    const bool ok = doc->flatten(error);
    emit changed(doc->id());
    return ok;
}
bool DocumentSession::clear(const std::shared_ptr<Document> &doc, QUuid layer, QString *error) {
    if (!owns(doc, error))
        return false;
    const bool ok = doc->clearSelected(layer, error);
    if (ok)
        emit changed(doc->id());
    return ok;
}
bool DocumentSession::jump(const std::shared_ptr<Document> &doc, int index) {
    if (!owns(doc))
        return false;
    doc->jumpHistory(index);
    emit changed(doc->id());
    return true;
}
std::function<bool(QString *)> DocumentSession::prepareBatch(const std::shared_ptr<Document> &doc, const QJsonArray &commands,
                                   QString *error, const QString &owner, qint64 maxPixelBytes,
                                   const std::function<bool()> &continuePreparation) {
    if (!owns(doc, error) || doc->strokeOpen() || doc->compoundDepth || commands.isEmpty())
        return {};

    const auto serial = doc->changeSerial();
    if (continuePreparation && !continuePreparation())
        return {};
    DocumentSession staging;
    auto draft = doc->recoverySnapshot();
    draft->selectionMask = doc->selectionMask;
    draft->saved = 0;
    if (!staging.adopt(draft))
        return {};
    draft->beginCompound("Edit Transaction");
    for (const auto &value : commands) {
        const auto command = value.toObject();
        const auto name = command["command"].toString();
        if (name == "undo" || name == "redo" || !staging.execute(draft, command, error))
            return {};
        if (draft->pixelBytes() > maxPixelBytes) {
            if (error)
                *error = "The resulting document exceeds the transaction pixel budget";
            return {};
        }
        if (continuePreparation && !continuePreparation())
            return {};
    }
    if (!owns(doc, error) || doc->changeSerial() != serial || doc->strokeOpen() || doc->compoundDepth)
        return {};
    if (draft->compoundChildren.empty())
        return [](QString *) { return true; };

    qint64 cost = 0;
    for (const auto &child : draft->compoundChildren)
        cost += Document::operationBytes(child);
    if (doc->operationLimit < 1 || cost + 1024 > doc->historyBudget || !withinPixelBudget(doc, draft->pixelBytes() - doc->pixelBytes())) {
        if (error)
            *error = "The transaction exceeds the available history or pixel budget";
        return {};
    }
    return [this, doc, draft, owner, serial](QString *commitError) {
        if (!owns(doc, commitError) || doc->changeSerial() != serial || doc->strokeOpen() ||
            doc->compoundDepth || !withinPixelBudget(doc, draft->pixelBytes() - doc->pixelBytes()))
            return false;
        doc->history.reserve(size_t(doc->cursor) + 1);
        doc->image = std::move(draft->image);
        doc->stack = std::move(draft->stack);
        doc->active = draft->active;
        doc->animation = std::move(draft->animation);
        doc->frameId = draft->frameId;
        doc->selectionMask = std::move(draft->selectionMask);
        doc->frameCache.clear();
        doc->dirty = doc->dirty.united(draft->dirty);
        if (!doc->native && draft->native && !draft->saved)
            doc->saved.reset();
        doc->native = draft->native;
        doc->compoundDepth = 1;
        doc->compoundLabel = std::move(draft->compoundLabel);
        doc->compoundChildren = std::move(draft->compoundChildren);
        doc->endCompound(true, owner);
        emit changed(doc->id());
        return true;
    };
}

bool DocumentSession::execute(const std::shared_ptr<Document> &doc, const QJsonObject &c,
                              QString *error) {
    if (!owns(doc, error))
        return false;
    const auto command = c.value("command").toString();
    const QUuid layer = c.contains("layer") ? QUuid(c.value("layer").toString()) : doc->activeLayer();
    auto rect = [&] {
        return QRect(c.value("x").toInt(), c.value("y").toInt(), c.value("width").toInt(),
                     c.value("height").toInt());
    };
    auto mode = [&] {
        const auto m = c.value("mode").toString("replace");
        return m == "add" ? ops::Combine::Add
               : m == "subtract" ? ops::Combine::Subtract
               : m == "intersect" ? ops::Combine::Intersect
                                  : ops::Combine::Replace;
    };
    auto color = [&](const char *key) {
        bool ok = false;
        auto rgb = ops::parseColor(c.value(key).toString(), &ok);
        return ok ? rgb : qRgba(0, 0, 0, 255);
    };
    if (command == "operation")
        return apply(doc, layer, {c.value("operation").toString(), c.value("params").toObject()}, error);
    if (command == "operation_layers") {
        QVector<QUuid> ids;
        if(c["scope"].toString()=="all")for(const auto &l:doc->layers())ids<<l.id;
        else for(const auto &value:c["layers"].toArray())ids<<QUuid(value.toString());
        return applyLayers(doc,ids,{c["operation"].toString(),c["params"].toObject()},error);
    }
    if (command == "select_rect")
        return select(doc, ops::rectMask(doc->image.size(), rect()), mode(), "Rectangle Select", error);
    if (command == "select_ellipse")
        return select(doc, ops::ellipseMask(doc->image.size(), rect()), mode(), "Ellipse Select", error);
    if (command == "select_polygon") {
        QVector<QPointF> points;
        for (const auto &p : c.value("points").toArray())
            points.append(QPointF(p.toArray().at(0).toDouble(), p.toArray().at(1).toDouble()));
        return select(doc, ops::polygonMask(doc->image.size(), points), mode(), "Lasso Select", error);
    }
    if (command == "select_color") {
        const auto mask = ops::colorMask(doc->image, QPoint(c.value("x").toInt(), c.value("y").toInt()),
                                         c.value("tolerance").toInt(0),
                                         ops::distanceFromName(c.value("metric").toString("RGB")),
                                         c.value("contiguous").toBool(true), c.value("include_alpha").toBool(true));
        return select(doc, mask, mode(), "Colour Select", error);
    }
    if (command == "select_all" || command == "deselect" || command == "invert_selection")
        return selection(doc, command == "select_all" ? SelectionCommand::All
                              : command == "deselect" ? SelectionCommand::None
                                                      : SelectionCommand::Invert);
    if (command == "fill_rect") {
        ops::ShapeSpec spec;
        spec.bounds = rect();
        spec.filled = true;
        return shape(doc, layer, spec, color("color"), c["opacity"].toInt(), false, error);
    }
    if (command == "fill")
        return fill(doc, layer, QPoint(c.value("x").toInt(), c.value("y").toInt()), color("color"),
                    c.value("tolerance").toInt(0), ops::distanceFromName(c.value("metric").toString("RGB")),
                    c.value("contiguous").toBool(true), c.value("sample_composite").toBool(false),
                    c.value("opacity").toInt(255), error);
    if (command == "stroke") {
        StrokeStyle style;
        style.color = color("color");
        style.brush.size = c.value("size").toInt(1);
        style.brush.height = c.value("height").toInt(0);
        style.brush.opacity = c.value("opacity").toInt(255);
        style.brush.shape = ops::BrushShape(std::clamp(c.value("shape").toInt(0), 0, 4));
        style.erase = c.value("erase").toBool(false);
        const auto points = c.value("points").toArray();
        if (points.isEmpty()) {
            if (error)
                *error = "A stroke needs at least one point";
            return false;
        }
        QPoint last(points[0].toArray().at(0).toInt(), points[0].toArray().at(1).toInt());
        bool ok = true;
        for (int i = 0; ok && i < points.size(); ++i) {
            const QPoint p(points[i].toArray().at(0).toInt(), points[i].toArray().at(1).toInt());
            ok = doc->stroke(layer, last, p, style, error);
            last = p;
        }
        if (ok)
            doc->finish();
        else
            doc->cancelStroke();
        emit changed(doc->id());
        return ok;
    }
    if (command.startsWith("layer_")) {
        static const QMap<QString, LayerCommand> actions{
            {"layer_add", LayerCommand::Add}, {"layer_duplicate", LayerCommand::Duplicate},
            {"layer_rename", LayerCommand::Rename}, {"layer_move", LayerCommand::Move},
            {"layer_visibility", LayerCommand::Visibility}, {"layer_opacity", LayerCommand::Opacity},
            {"layer_remove", LayerCommand::Delete}};
        if (actions.contains(command)) {
            if (!doc->layer(layer)) {
                if (error)
                    *error = "Layer identity is not present";
                return false;
            }
            return edit(doc, {actions.value(command), layer, c["name"].toString(), c["value"].toInt()}, error);
        }
    }
    if (command.startsWith("frame_")) {
        static const QMap<QString, FrameCommand> actions{
            {"frame_add", FrameCommand::Add}, {"frame_duplicate", FrameCommand::Duplicate},
            {"frame_remove", FrameCommand::Delete}, {"frame_move", FrameCommand::Move}};
        if (actions.contains(command))
            return frameEdit(doc, actions.value(command), QUuid(c["frame"].toString()),
                             c["value"].toInt(), {}, error);
    }
    if (command == "animation_timing") {
        auto state = doc->animationState();
        if (!state.enabled)
            return false;
        state.defaultTicks = c["ticks"].toInt();
        state.interpolate = c["interpolate"].toBool();
        return animationEdit(doc, state, "Animation Timing", error);
    }
    if (command == "animation_duration") {
        auto state = doc->animationState();
        if (!state.enabled)
            return false;
        const int entry = c["entry"].toInt(-1);
        if (entry < 0 || size_t(entry) >= state.entries.size()) {
            if (error)
                *error = "The playback entry is missing";
            return false;
        }
        state.entries[size_t(entry)].ticks = c["ticks"].toInt();
        return animationEdit(doc, state, "Frame Duration", error);
    }
    if (command == "undo" || command == "redo")
        return dispatch(doc, command == "undo" ? Command::Undo : Command::Redo, {}, false, error);
    if (error)
        *error = "Unknown editing command: " + command;
    return false;
}
} // namespace nova
