// SPDX-License-Identifier: GPL-3.0-or-later
#include "canvas.h"
#include <QPainter>
namespace nova {
void Canvas::paintOnions(QPainter &painter, const QRectF &target) {
    if (!onion.enabled || !document->animated()) return;
    // Frame identities come from zero-copy accessors: animationState() deep-copies every layer.
    const int frameIndex = document->activeFrameIndex(), frames = document->frameCount();
    if (frameIndex < 0) return;
    painter.save();
    for (int direction : {-1, 1}) {
        const int count = std::clamp(direction < 0 ? onion.previous : onion.next, 0, 8);
        for (int distance = count; distance >= 1; --distance) {
            const int index = frameIndex + direction * distance;
            if (index < 0 || index >= frames) continue;
            auto source = document->frameComposite(document->frameIdAt(index));
            const auto key = QString::number(source.cacheKey()) + (direction < 0 ? ":prev" : ":next") + (onion.tint ? ":tint" : ":plain");
            QImage overlay;
            if (auto cached = onionCache.object(key)) overlay = *cached;
            else {
                overlay = source;
                if (onion.tint) for (int y = 0; y < overlay.height(); ++y) {
                    auto row = overlay.scanLine(y);
                    for (int x = 0; x < overlay.width(); ++x) {
                        row[x * 4] = direction < 0 ? 255 : 70;
                        row[x * 4 + 1] = direction < 0 ? 85 : 190;
                        row[x * 4 + 2] = direction < 0 ? 105 : 255;
                    }
                }
                if (overlay.sizeInBytes() <= onionCache.maxCost()) onionCache.insert(key, new QImage(overlay), int(overlay.sizeInBytes()));
            }
            painter.setOpacity(std::clamp(onion.opacity, 1, 100) / (100.0 * distance));
            painter.drawImage(target, overlay);
        }
    }
    painter.restore();
}
} // namespace nova
