// SPDX-License-Identifier: GPL-3.0-or-later
#include "canvas.h"
#include "session.h"
#include "tools.h"
#include <QPainter>
#include <QSet>
#include <algorithm>
#include <cmath>
namespace nova {
namespace {
constexpr qint64 WorkingBudget = 128LL * 1024 * 1024;
// `source` over `area` with the lifted selection removed; hidden RGB is cleared once the
// alpha reaches zero (rows outside the mask are plain copies).
void liftRows(QImage &out, const QImage &source, const QImage &lift, const QRect &area) {
    for (int y = area.top(); y <= area.bottom(); ++y) {
        auto s = source.constScanLine(y);
        auto row = out.scanLine(y);
        auto mask = lift.constScanLine(y);
        std::copy_n(s + area.left() * 4, area.width() * 4, row + area.left() * 4);
        for (int x = area.left(); x <= area.right(); ++x)
            if (mask[x]) {
                row[x * 4 + 3] = uchar(row[x * 4 + 3] * (255 - mask[x]) / 255);
                if (!row[x * 4 + 3])
                    row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0;
            }
    }
}
}
bool Canvas::beginLayerTransform(const QVector<QUuid> &requested, bool includeHidden) {
    cancelFloating();
    QVector<QUuid> ids;
    QSet<QUuid> seen;
    QRect bounds;
    for (auto id : requested) {
        const auto layer = document->layer(id);
        if (!layer || seen.contains(id))
            continue;
        seen.insert(id);
        if (!layer->visible && !includeHidden)
            continue;
        ids << id;
        bounds = bounds.united(document->hasSelection() ? ops::maskBounds(document->selection())
                                                        : ops::alphaBounds(layer->pixels, 0));
    }
    auto fail = [&](QString message) {
        if (error)
            error(message);
        return false;
    };
    if (ids.isEmpty() || bounds.isEmpty())
        return fail("No selected visible layer content to transform");
    bounds = bounds.intersected(document->image.rect());
    const qint64 groupBytes = qint64(bounds.width()) * bounds.height() * 4 * ids.size();
    if (groupBytes * 3 + document->image.sizeInBytes() * ids.size() +
            document->image.sizeInBytes() >
        WorkingBudget)
        return fail("Selected-layer transform exceeds the 128 MiB preview working budget");
    tools->scaleX = tools->scaleY = 100;
    tools->angle = 0;
    tools->transformFlipH = tools->transformFlipV = false;
    tools->touch();
    floating = {};
    floating.active = true;
    floating.includeHidden = includeHidden;
    floating.sourceSerial = document->changeSerial();
    floating.origin = bounds.topLeft();
    floating.pixels = document->image.copy(bounds);
    floating.lift =
        document->hasSelection() ? document->selection() : ops::fullMask(document->image.size());
    floating.label = QString("Transform %1 selected layers%2")
                         .arg(ids.size())
                         .arg(includeHidden ? " (including hidden)" : " (visible only)");
    for (auto id : ids)
        floating.layers.append({id, document->selectedPixels(id).copy(bounds), {}, {}});
    refreshLayerFloating();
    return floating.valid;
}
void Canvas::refreshLayerFloating() {
    if (!floating.active || floating.layers.isEmpty())
        return;
    auto fail = [&](QString message) {
        floating.valid = false;
        if (error)
            error(message);
    };
    if (document->changeSerial() != floating.sourceSerial) {
        fail("Document changed during the transform; cancel and start again");
        return;
    }
    const double sx = floating.scaleX / 100. * (tools->transformFlipH ? -1 : 1),
                 sy = floating.scaleY / 100. * (tools->transformFlipV ? -1 : 1);
    QTransform linear;
    linear.rotate(floating.angle);
    linear.scale(sx, sy);
    const QSize size = floating.pixels.size();
    const auto extent = linear.mapRect(
        QRectF(-size.width() / 2., -size.height() / 2., size.width(), size.height()));
    const QRect out(0, 0, int(std::ceil(extent.width() - 1e-8)),
                    int(std::ceil(extent.height() - 1e-8)));
    const qint64 bytes = qint64(out.width()) * out.height() * 4 * floating.layers.size();
    if (out.isEmpty() || out.width() > 4096 || out.height() > 4096 ||
        bytes * 2 + qint64(size.width()) * size.height() * 4 * floating.layers.size() +
                document->image.sizeInBytes() * (floating.layers.size() + 1) >
            WorkingBudget) {
        fail("Transform preview exceeds dimension/memory limits; reduce scale");
        return;
    }
    const QByteArray key = QByteArray::number(floating.scaleX) + ',' +
                           QByteArray::number(floating.scaleY) + ',' +
                           QByteArray::number(floating.angle) + floating.resampling.toUtf8() +
                           char(tools->transformFlipH) + char(tools->transformFlipV);
    if (key != floating.cacheKey) {
        for (auto &layer : floating.layers) {
            QImage pixels(out.size(), QImage::Format_RGBA8888);
            pixels.fill(Qt::transparent);
            QPainter painter(&pixels);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.setRenderHint(QPainter::SmoothPixmapTransform,
                                  floating.resampling != "Nearest neighbour");
            painter.translate(out.width() / 2., out.height() / 2.);
            painter.setTransform(linear, true);
            painter.translate(-size.width() / 2., -size.height() / 2.);
            painter.drawImage(0, 0, layer.original);
            painter.end();
            layer.cached = std::move(pixels);
        }
        floating.cacheKey = key;
    }
    const auto center = floating.origin + QPointF(size.width() / 2., size.height() / 2.);
    floating.placed = QPoint(int(std::lround(center.x() - out.width() / 2.)),
                             int(std::lround(center.y() - out.height() / 2.)));
    floating.cached = floating.layers[0].cached;
    for (auto &item : floating.layers) {
        const auto target = document->layer(item.id);
        if (!target) {
            fail("A transform layer was removed");
            return;
        }
        // The lifted layer is constant for the session (the serial check above guards it): build
        // it once, then per move only rewind the previous stamp and place the new one.
        auto &base = floating.replacements[item.id];
        if (base.isNull() || base.size() != target->pixels.size() ||
            base.format() != target->pixels.format()) {
            base = QImage(target->pixels.size(), target->pixels.format());
            item.stamped = {};
            if (!base.isNull())
                liftRows(base, target->pixels, floating.lift, base.rect());
        } else if (!item.stamped.isEmpty()) {
            liftRows(base, target->pixels, floating.lift, item.stamped);
        }
        blendPlaced(base, item.cached, floating.placed);
        item.stamped = QRect(floating.placed, item.cached.size()).intersected(base.rect());
    }
    floating.valid = true;
    preview = document->compositeWith(floating.replacements);
    update();
}
bool Canvas::commitLayerFloating() {
    if (!floating.valid || document->changeSerial() != floating.sourceSerial) {
        if (error)
            error("Transform is stale or over budget; correct it or cancel before committing");
        return false;
    }
    QImage selection(document->image.size(), QImage::Format_Grayscale8);
    selection.fill(0);
    for (const auto &item : floating.layers)
        for (int y = 0; y < item.cached.height(); ++y)
            for (int x = 0; x < item.cached.width(); ++x) {
                auto q = floating.placed + QPoint(x, y);
                if (selection.rect().contains(q) && qAlpha(item.cached.pixel(x, y)))
                    selection.scanLine(q.y())[q.x()] = 255;
            }
    QString message;
    const bool ok =
        session ? session->replaceLayers(document, floating.replacements, floating.label,
                                         floating.includeHidden, &message, &selection)
                : document->replaceLayersPixels(floating.replacements, floating.label,
                                                floating.includeHidden, &message, &selection);
    if (!ok) {
        if (error)
            error(message);
        return false;
    }
    floating = {};
    preview = {};
    tools->scaleX = tools->scaleY = 100;
    tools->angle = 0;
    tools->transformFlipH = tools->transformFlipV = false;
    tools->touch();
    notify();
    return true;
}
} // namespace nova
