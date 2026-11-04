// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QImage>
namespace nova {
namespace detail {
// A single-surface composite is a view of its raster buffer. Drop that view
// during synchronous writes so it does not force a full-image detach per point.
// Recovery/history/external QImage owners still trigger the required safe copy.
struct PixelWrite {
    QImage &composite, &pixels;
    int width;
    bool shared;
    PixelWrite(QImage &view, QImage &source)
        : composite(view), pixels(source), width(view.width()),
          shared(view.constBits() == source.constBits()) {
        if (shared)
            composite = {};
    }
    ~PixelWrite() {
        if (shared)
            composite = pixels;
    }
};
} // namespace detail
using detail::PixelWrite;
} // namespace nova
