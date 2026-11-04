// SPDX-License-Identifier: GPL-3.0-or-later
// Geometric transforms. Nearest neighbour is the default everywhere so pixel
// art never blurs silently; interpolating modes work in premultiplied space.
#include "ops_internal.h"

namespace nova::ops::detail {
Resample resampleFromName(const QString &name) {
    if (name.startsWith("Bilinear"))
        return Resample::Bilinear;
    if (name.startsWith("Bicubic"))
        return Resample::Bicubic;
    return Resample::Nearest;
}
namespace {
struct Premul {
    double r = 0, g = 0, b = 0, a = 0;
    void add(QRgb c, double w) {
        const double k = qAlpha(c) / 255.0 * w;
        r += qRed(c) * k, g += qGreen(c) * k, b += qBlue(c) * k, a += qAlpha(c) * w;
    }
    QRgb rgba() const {
        if (a < 0.5)
            return qRgba(0, 0, 0, 0);
        const double k = 255.0 / a;
        return qRgba(clamp255(r * k), clamp255(g * k), clamp255(b * k), clamp255(a));
    }
};
double cubic(double x) { // Keys, a = -0.5 (Catmull–Rom)
    x = std::abs(x);
    if (x < 1)
        return 1.5 * x * x * x - 2.5 * x * x + 1;
    if (x < 2)
        return -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2;
    return 0;
}
QRgb fetch(const QImage &im, int x, int y, bool wrap) {
    if (!wrap && (x < 0 || y < 0 || x >= im.width() || y >= im.height()))
        return qRgba(0, 0, 0, 0);
    return sample(im, x, y, wrap);
}
int anchorIndex(const QString &name) {
    static const QStringList names = {"Top left", "Top",    "Top right",   "Left",  "Centre",
                                      "Right",    "Bottom left", "Bottom", "Bottom right"};
    return std::max(0, int(names.indexOf(name)));
}
QPoint anchoredOffset(QSize content, QSize canvas, const QString &anchor, int margin = 0) {
    const int i = anchorIndex(anchor), ax = i % 3, ay = i / 3;
    auto place = [&](int c, int n, int a) {
        return a == 0 ? margin : a == 1 ? (n - c) / 2 : n - c - margin;
    };
    return {place(content.width(), canvas.width(), ax), place(content.height(), canvas.height(), ay)};
}
QImage transformed(const QImage &src, QSize size, auto inverse, Resample mode, bool wrap,
                   const Context &ctx) {
    QImage out(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        if (ctx.canceled())
            return {};
        for (int x = 0; x < size.width(); ++x) {
            const QPointF s = inverse(QPointF(x + .5, y + .5));
            setPx(out, x, y, sampleAt(src, s.x(), s.y(), mode, wrap));
        }
    }
    return out;
}
} // namespace
QRgb sampleAt(const QImage &im, double x, double y, Resample mode, bool wrap) {
    // (x, y) are continuous coordinates where pixel centres sit at i + 0.5.
    if (mode == Resample::Nearest)
        return fetch(im, int(std::floor(x + 1e-9)), int(std::floor(y + 1e-9)), wrap);
    const double fx = x - .5, fy = y - .5;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    const double tx = fx - ix, ty = fy - iy;
    Premul acc;
    if (mode == Resample::Bilinear) {
        acc.add(fetch(im, ix, iy, wrap), (1 - tx) * (1 - ty));
        acc.add(fetch(im, ix + 1, iy, wrap), tx * (1 - ty));
        acc.add(fetch(im, ix, iy + 1, wrap), (1 - tx) * ty);
        acc.add(fetch(im, ix + 1, iy + 1, wrap), tx * ty);
    } else {
        for (int j = -1; j <= 2; ++j)
            for (int i = -1; i <= 2; ++i)
                acc.add(fetch(im, ix + i, iy + j, wrap), cubic(i - tx) * cubic(j - ty));
    }
    return acc.rgba();
}
QImage resample(const QImage &src, QSize size, Resample mode) {
    const double sx = double(src.width()) / size.width(), sy = double(src.height()) / size.height();
    if (mode != Resample::Nearest && (sx > 1 || sy > 1)) {
        // Downscaling with interpolation: box-average the covered source area.
        QImage out(size, QImage::Format_RGBA8888);
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x) {
                Premul acc;
                const int x0 = int(std::floor(x * sx)), x1 = std::max(x0 + 1, int(std::ceil((x + 1) * sx)));
                const int y0 = int(std::floor(y * sy)), y1 = std::max(y0 + 1, int(std::ceil((y + 1) * sy)));
                for (int j = y0; j < std::min(y1, src.height()); ++j)
                    for (int i = x0; i < std::min(x1, src.width()); ++i)
                        acc.add(px(src, i, j), 1);
                const int n = (std::min(x1, src.width()) - x0) * (std::min(y1, src.height()) - y0);
                acc.r /= n, acc.g /= n, acc.b /= n, acc.a /= n;
                setPx(out, x, y, acc.rgba());
            }
        return out;
    }
    return transformed(
        src, size, [&](QPointF p) { return QPointF(p.x() * sx, p.y() * sy); }, mode, false, {});
}
QImage flip(const QImage &src, const Params &p, const Context &, QString *) {
    return p.s("direction") == "Vertical" ? src.flipped(Qt::Vertical) : src.flipped(Qt::Horizontal);
}
QImage imageFlip(const QImage &src, const Params &p, const Context &ctx, QString *e) {
    return flip(src, p, ctx, e);
}
QImage rotate(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double angle = p.d("angle");
    const auto mode = resampleFromName(p.s("resampling"));
    const double r = std::fmod(std::fmod(angle, 360.0) + 360.0, 360.0);
    const QPointF c(src.width() / 2.0, src.height() / 2.0);
    const double t = -angle * Pi / 180, cs = std::cos(t), sn = std::sin(t);
    if (src.width() == src.height() && std::abs(std::fmod(r, 90.0)) < 1e-9) {
        // Exact quarter turns: integer index mapping, no resampling at all.
        const int turns = int(std::lround(r / 90)) % 4;
        QImage out(src.size(), QImage::Format_RGBA8888);
        const int n = src.width();
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                int sx = x, sy = y;
                if (turns == 1)
                    sx = y, sy = n - 1 - x;
                else if (turns == 2)
                    sx = n - 1 - x, sy = n - 1 - y;
                else if (turns == 3)
                    sx = n - 1 - y, sy = x;
                setPx(out, x, y, px(src, sx, sy));
            }
        return out;
    }
    return transformed(
        src, src.size(),
        [&](QPointF q) {
            const QPointF d = q - c;
            return c + QPointF(d.x() * cs - d.y() * sn, d.x() * sn + d.y() * cs);
        },
        mode, false, ctx);
}
QImage imageRotate(const QImage &src, const Params &p, const Context &, QString *) {
    const auto turns = p.s("turns");
    const int n = turns.startsWith("90° clockwise") ? 1 : turns.startsWith("180") ? 2 : 3;
    const int w = src.width(), h = src.height();
    QImage out(n == 2 ? src.size() : QSize(h, w), QImage::Format_RGBA8888);
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) {
            int sx, sy;
            if (n == 1)
                sx = y, sy = h - 1 - x;
            else if (n == 2)
                sx = w - 1 - x, sy = h - 1 - y;
            else
                sx = w - 1 - y, sy = x;
            setPx(out, x, y, px(src, sx, sy));
        }
    return out;
}
QImage offset(const QImage &src, const Params &p, const Context &, QString *) {
    const int dx = p.i("dx"), dy = p.i("dy");
    if (p.b("wrap"))
        return wrapOffset(src, dx, dy);
    QImage out = blank(src.size());
    for (int y = 0; y < src.height(); ++y)
        for (int x = 0; x < src.width(); ++x) {
            const int tx = x + dx, ty = y + dy;
            if (tx >= 0 && ty >= 0 && tx < src.width() && ty < src.height())
                setPx(out, tx, ty, px(src, x, y));
        }
    return out;
}
QImage scaleContent(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double kx = p.i("scale_x") / 100.0, ky = p.i("scale_y") / 100.0;
    const auto mode = resampleFromName(p.s("resampling"));
    const QPointF c(src.width() / 2.0, src.height() / 2.0);
    return transformed(
        src, src.size(),
        [&](QPointF q) { return c + QPointF((q.x() - c.x()) / kx, (q.y() - c.y()) / ky); }, mode,
        false, ctx);
}
QImage alignContent(const QImage &src, const Params &p, const Context &, QString *) {
    const auto bounds = alphaBounds(src, 0);
    if (bounds.isEmpty())
        return src.copy();
    const auto target = anchoredOffset(bounds.size(), src.size(), p.s("anchor"), p.i("margin"));
    QImage out = blank(src.size());
    const QPoint d = target - bounds.topLeft();
    for (int y = bounds.top(); y <= bounds.bottom(); ++y)
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            const QPoint t(x + d.x(), y + d.y());
            if (out.rect().contains(t))
                setPx(out, t.x(), t.y(), px(src, x, y));
        }
    return out;
}
QImage resizeImage(const QImage &src, const Params &p, const Context &, QString *) {
    return resample(src, QSize(p.i("width"), p.i("height")), resampleFromName(p.s("resampling")));
}
QImage resizeCanvas(const QImage &src, const Params &p, const Context &, QString *) {
    const QSize size(p.i("width"), p.i("height"));
    const QPoint o = anchoredOffset(src.size(), size, p.s("anchor"));
    const auto pad = p.s("padding");
    QImage out = blank(size);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x) {
            int sx = x - o.x(), sy = y - o.y();
            const bool inside = sx >= 0 && sy >= 0 && sx < src.width() && sy < src.height();
            if (!inside && pad == "Transparent")
                continue;
            if (!inside && pad == "Repeat (wrap)") {
                sx = ((sx % src.width()) + src.width()) % src.width();
                sy = ((sy % src.height()) + src.height()) % src.height();
            } else if (!inside) {
                sx = std::clamp(sx, 0, src.width() - 1);
                sy = std::clamp(sy, 0, src.height() - 1);
            }
            setPx(out, x, y, px(src, sx, sy));
        }
    return out;
}
QImage crop(const QImage &src, const Params &p, const Context &, QString *error) {
    const QRect r(p.i("x"), p.i("y"), p.i("width"), p.i("height"));
    if (!src.rect().contains(r)) {
        if (error)
            *error = "Crop rectangle must lie inside the image";
        return {};
    }
    return src.copy(r);
}
QImage trim(const QImage &src, const Params &p, const Context &, QString *error) {
    const auto r = alphaBounds(src, p.i("alpha_threshold"));
    if (r.isEmpty()) {
        if (error)
            *error = "The image has no visible pixels to keep";
        return {};
    }
    return src.copy(r);
}
} // namespace nova::ops::detail
