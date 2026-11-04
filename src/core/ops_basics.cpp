// SPDX-License-Identifier: GPL-3.0-or-later
#include "imageops.h"
#include <QColor>
#include <QHash>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <algorithm>
#include <cmath>
#include <map>

namespace nova::ops {
namespace {
constexpr double Pi = 3.14159265358979323846;
inline int clamp255(double v) { return int(std::clamp(std::lround(v), 0L, 255L)); }
} // namespace

// ---------------------------------------------------------------- random
quint64 Random::next() {
    quint64 z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
double Random::unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
int Random::range(int low, int high) {
    if (high <= low)
        return low;
    return low + int(next() % quint64(qint64(high) - low + 1));
}
quint64 hash3(quint64 seed, qint64 x, qint64 y) {
    Random r(seed ^ (quint64(x) * 0xD6E8FEB86659FD93ull) ^ (quint64(y) * 0xA0761D6478BD642Full));
    r.next();
    return r.next();
}
double hashUnit(quint64 seed, qint64 x, qint64 y) {
    return double(hash3(seed, x, y) >> 11) * (1.0 / 9007199254740992.0);
}
double periodicNoise(double u, double v, int cellsX, int cellsY, quint64 seed) {
    cellsX = std::max(1, cellsX);
    cellsY = std::max(1, cellsY);
    const double x = u * cellsX, y = v * cellsY;
    const auto ix = qint64(std::floor(x)), iy = qint64(std::floor(y));
    const double fx = x - double(ix), fy = y - double(iy);
    auto wrap = [](qint64 i, int n) { return ((i % n) + n) % n; };
    auto at = [&](qint64 a, qint64 b) { return hashUnit(seed, wrap(a, cellsX), wrap(b, cellsY)); };
    const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const double top = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * sx;
    const double bottom = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * sx;
    return top + (bottom - top) * sy;
}
double fbm(double u, double v, int cells, int octaves, double roughness, quint64 seed) {
    double sum = 0, amplitude = 1, total = 0;
    for (int o = 0; o < std::clamp(octaves, 1, 8); ++o) {
        const int c = std::max(1, cells) << o;
        sum += amplitude * periodicNoise(u, v, c, c, seed + quint64(o) * 7919);
        total += amplitude;
        amplitude *= std::clamp(roughness, 0.05, 1.0);
    }
    return sum / total;
}

// ---------------------------------------------------------------- colour
Hsv toHsv(QRgb c) {
    const double r = qRed(c) / 255.0, g = qGreen(c) / 255.0, b = qBlue(c) / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    Hsv out;
    out.v = mx;
    out.s = mx > 0 ? d / mx : 0;
    if (d > 0) {
        if (mx == r)
            out.h = 60 * std::fmod((g - b) / d + 6, 6.0);
        else if (mx == g)
            out.h = 60 * ((b - r) / d + 2);
        else
            out.h = 60 * ((r - g) / d + 4);
    }
    return out;
}
static QRgb fromChroma(double h, double c, double x, double m, int alpha) {
    double r = 0, g = 0, b = 0;
    h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0);
    switch (int(h / 60) % 6) {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
    }
    return qRgba(clamp255((r + m) * 255), clamp255((g + m) * 255), clamp255((b + m) * 255),
                 std::clamp(alpha, 0, 255));
}
QRgb fromHsv(const Hsv &hsv, int alpha) {
    const double s = std::clamp(hsv.s, 0.0, 1.0), v = std::clamp(hsv.v, 0.0, 1.0);
    const double c = v * s, h = std::fmod(std::fmod(hsv.h, 360.0) + 360.0, 360.0);
    const double x = c * (1 - std::abs(std::fmod(h / 60, 2.0) - 1));
    return fromChroma(h, c, x, v - c, alpha);
}
Hsl toHsl(QRgb c) {
    const double r = qRed(c) / 255.0, g = qGreen(c) / 255.0, b = qBlue(c) / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    Hsl out;
    out.l = (mx + mn) / 2;
    out.s = d == 0 ? 0 : d / (1 - std::abs(2 * out.l - 1));
    out.h = toHsv(c).h;
    return out;
}
QRgb fromHsl(const Hsl &hsl, int alpha) {
    const double s = std::clamp(hsl.s, 0.0, 1.0), l = std::clamp(hsl.l, 0.0, 1.0);
    const double c = (1 - std::abs(2 * l - 1)) * s, h = std::fmod(std::fmod(hsl.h, 360.0) + 360, 360.0);
    const double x = c * (1 - std::abs(std::fmod(h / 60, 2.0) - 1));
    return fromChroma(h, c, x, l - c / 2, alpha);
}
double srgbToLinear(double e) {
    return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
}
double linearToSrgb(double l) {
    l = std::clamp(l, 0.0, 1.0);
    return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
}
int luma(QRgb c) { return (qRed(c) * 2126 + qGreen(c) * 7152 + qBlue(c) * 722 + 5000) / 10000; }
QStringList distanceNames() { return {"RGB", "RGBA", "Perceptual (redmean)", "Hue"}; }
Distance distanceFromName(const QString &name) {
    auto i = distanceNames().indexOf(name);
    return i < 0 ? Distance::Rgb : Distance(i);
}
int colorDistance(QRgb a, QRgb b, Distance metric) {
    const int dr = qRed(a) - qRed(b), dg = qGreen(a) - qGreen(b), db = qBlue(a) - qBlue(b),
              da = qAlpha(a) - qAlpha(b);
    switch (metric) {
    case Distance::Rgb:
        return std::max({std::abs(dr), std::abs(dg), std::abs(db)});
    case Distance::Rgba:
        return std::max({std::abs(dr), std::abs(dg), std::abs(db), std::abs(da)});
    case Distance::Perceptual: {
        // "Redmean" low-cost approximation; max value ≈ 765, scaled to 255.
        const double rm = (qRed(a) + qRed(b)) / 2.0;
        const double d =
            std::sqrt((2 + rm / 256) * dr * dr + 4.0 * dg * dg + (2 + (255 - rm) / 256) * db * db);
        return std::min(255, int(std::lround(d / 3.0)));
    }
    case Distance::Hue: {
        const auto ha = toHsv(a), hb = toHsv(b);
        if (ha.s < 0.08 || hb.s < 0.08) // achromatic colours: fall back to value
            return std::min(255, int(std::lround(std::abs(ha.v - hb.v) * 255)));
        double dh = std::abs(ha.h - hb.h);
        dh = std::min(dh, 360 - dh);
        return std::min(255, int(std::lround(dh / 180.0 * 255)));
    }
    }
    return 0;
}
bool colorMatches(QRgb c, QRgb ref, int tolerance, Distance metric, bool includeAlpha) {
    if (qAlpha(c) == 0 && qAlpha(ref) == 0)
        return true;
    if (qAlpha(c) == 0 || qAlpha(ref) == 0)
        return includeAlpha ? std::abs(qAlpha(c) - qAlpha(ref)) <= tolerance : false;
    int d = colorDistance(c, ref, metric);
    if (includeAlpha)
        d = std::max(d, std::abs(qAlpha(c) - qAlpha(ref)));
    return d <= tolerance;
}
QRgb blend(QRgb dst, QRgb src, int coverage) {
    const int a = (qAlpha(src) * std::clamp(coverage, 0, 255) + 127) / 255;
    if (a == 0)
        return dst;
    const int d = qAlpha(dst);
    const int weight = d * (255 - a), den = a * 255 + weight;
    if (den == 0)
        return qRgba(0, 0, 0, 0);
    auto ch = [&](int s, int t) { return (s * a * 255 + t * weight + den / 2) / den; };
    return qRgba(ch(qRed(src), qRed(dst)), ch(qGreen(src), qGreen(dst)), ch(qBlue(src), qBlue(dst)),
                 (den + 127) / 255);
}
QRgb nearest(QRgb c, const std::vector<QRgb> &palette, Distance metric) {
    if (palette.empty())
        return c;
    QRgb best = palette.front();
    int bestDistance = INT_MAX;
    for (auto p : palette) {
        int d = colorDistance(c, p | 0xff000000u, metric);
        if (d < bestDistance) {
            bestDistance = d;
            best = p;
        }
    }
    return qRgba(qRed(best), qGreen(best), qBlue(best), qAlpha(c));
}
QString colorName(QRgb c) { return QColor::fromRgba(c).name(QColor::HexArgb).toUpper(); }
QRgb parseColor(const QString &text, bool *ok) {
    QColor color(text.trimmed());
    if (ok)
        *ok = color.isValid();
    return color.isValid() ? color.rgba() : qRgba(0, 0, 0, 255);
}

// ---------------------------------------------------------------- masks
QImage emptyMask(QSize size) {
    QImage m(size, QImage::Format_Grayscale8);
    m.fill(0);
    return m;
}
QImage fullMask(QSize size) {
    QImage m(size, QImage::Format_Grayscale8);
    m.fill(255);
    return m;
}
QImage combine(const QImage &current, const QImage &shape, Combine mode) {
    // For combination, a null current mask is an empty selection.
    if (mode == Combine::Replace || (current.isNull() && mode == Combine::Add))
        return shape;
    if (current.isNull())
        return emptyMask(shape.size());
    QImage out = current.copy();
    for (int y = 0; y < out.height(); ++y) {
        auto o = out.scanLine(y);
        auto s = shape.constScanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            switch (mode) {
            case Combine::Add: o[x] = std::max(o[x], s[x]); break;
            case Combine::Subtract: o[x] = uchar(std::max(0, o[x] - s[x])); break;
            case Combine::Intersect: o[x] = std::min(o[x], s[x]); break;
            case Combine::Replace: break;
            }
        }
    }
    return out;
}
QImage invertMask(const QImage &mask) {
    QImage out = mask.copy();
    for (int y = 0; y < out.height(); ++y) {
        auto o = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x)
            o[x] = uchar(255 - o[x]);
    }
    return out;
}
QRect maskBounds(const QImage &mask) {
    int left = mask.width(), right = -1, top = mask.height(), bottom = -1;
    for (int y = 0; y < mask.height(); ++y) {
        auto s = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x)
            if (s[x]) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = y;
            }
    }
    return right < 0 ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}
bool maskEmpty(const QImage &mask) { return mask.isNull() || maskBounds(mask).isEmpty(); }
QImage rectMask(QSize size, QRect rect) {
    auto m = emptyMask(size);
    rect = rect.normalized().intersected(QRect(QPoint(), size));
    for (int y = rect.top(); y <= rect.bottom() && !rect.isEmpty(); ++y)
        std::fill_n(m.scanLine(y) + rect.left(), rect.width(), uchar(255));
    return m;
}
QImage ellipseMask(QSize size, QRect rect) {
    ShapeSpec spec;
    spec.kind = ShapeKind::Ellipse;
    spec.filled = true;
    spec.bounds = QRectF(rect.normalized());
    return rasterizeShape(size, spec);
}
static bool insidePolygon(const QVector<QPointF> &poly, QPointF p) {
    bool in = false;
    for (int i = 0, j = int(poly.size()) - 1; i < poly.size(); j = i++) {
        const auto &a = poly[i], &b = poly[j];
        if ((a.y() > p.y()) != (b.y() > p.y()) &&
            p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x())
            in = !in;
    }
    return in;
}
QImage polygonMask(QSize size, const QVector<QPointF> &points) {
    auto m = emptyMask(size);
    if (points.size() < 3)
        return m;
    // Manual bounds: QRectF::united ignores zero-size rectangles.
    double left = points[0].x(), right = left, top = points[0].y(), bottom = top;
    for (auto p : points) {
        left = std::min(left, p.x()), right = std::max(right, p.x());
        top = std::min(top, p.y()), bottom = std::max(bottom, p.y());
    }
    const int x0 = std::max(0, int(std::floor(left))),
              x1 = std::min(size.width() - 1, int(std::ceil(right))),
              y0 = std::max(0, int(std::floor(top))),
              y1 = std::min(size.height() - 1, int(std::ceil(bottom)));
    // Scanline fill with the same even-odd, pixel-centre rule as insidePolygon:
    // a centre px is inside when it lies in [xs[2k], xs[2k+1]) of the sorted
    // crossings. O(rows × edges + area) instead of O(area × edges).
    std::vector<double> xs;
    for (int y = y0; y <= y1; ++y) {
        const double py = y + .5;
        xs.clear();
        for (int i = 0, j = int(points.size()) - 1; i < points.size(); j = i++) {
            const auto &a = points[i], &b = points[j];
            if ((a.y() > py) != (b.y() > py))
                xs.push_back((b.x() - a.x()) * (py - a.y()) / (b.y() - a.y()) + a.x());
        }
        std::sort(xs.begin(), xs.end());
        auto row = m.scanLine(y);
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int from = std::max(x0, int(std::ceil(xs[k] - .5)));
            const int to = std::min(x1, int(std::ceil(xs[k + 1] - .5)) - 1);
            if (to >= from)
                std::fill(row + from, row + to + 1, uchar(255));
        }
    }
    return m;
}
QImage colorMask(const QImage &source, QPoint seed, int tolerance, Distance metric,
                 bool contiguous, bool includeAlpha, bool diagonal) {
    const auto image = source.format() == QImage::Format_RGBA8888
                           ? source
                           : source.convertToFormat(QImage::Format_RGBA8888);
    auto m = emptyMask(image.size());
    if (!image.rect().contains(seed))
        return m;
    const QRgb ref = image.pixel(seed);
    const int w = image.width(), h = image.height();
    auto matches = [&](int x, int y) {
        return colorMatches(image.pixel(x, y), ref, tolerance, metric, includeAlpha);
    };
    if (!contiguous) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (matches(x, y))
                    m.scanLine(y)[x] = 255;
        return m;
    }
    // Scanline fill: the explicit stack holds at most one entry per pixel row
    // segment, so storage is bounded by the image area (no recursion).
    std::vector<QPoint> stack{seed};
    stack.reserve(std::min<size_t>(size_t(w) * h, 1u << 16));
    while (!stack.empty()) {
        auto p = stack.back();
        stack.pop_back();
        int y = p.y(), x = p.x();
        auto row = m.scanLine(y);
        if (row[x] || !matches(x, y))
            continue;
        int left = x, right = x;
        while (left > 0 && !row[left - 1] && matches(left - 1, y))
            --left;
        while (right < w - 1 && !row[right + 1] && matches(right + 1, y))
            ++right;
        std::fill(row + left, row + right + 1, uchar(255));
        const int from = diagonal ? std::max(0, left - 1) : left,
                  to = diagonal ? std::min(w - 1, right + 1) : right;
        for (int ny : {y - 1, y + 1}) {
            if (ny < 0 || ny >= h)
                continue;
            auto nrow = m.constScanLine(ny);
            bool open = false;
            for (int nx = from; nx <= to; ++nx) {
                const bool candidate = !nrow[nx] && matches(nx, ny);
                if (candidate && !open)
                    stack.push_back({nx, ny});
                open = candidate;
            }
        }
    }
    return m;
}
QImage applyMask(const QImage &original, const QImage &processed, const QImage *mask) {
    if (!mask || mask->isNull() || mask->size() != original.size() ||
        processed.size() != original.size())
        return processed;
    QImage out = processed.copy();
    for (int y = 0; y < out.height(); ++y) {
        auto o = out.scanLine(y);
        auto s = original.constScanLine(y);
        auto m = mask->constScanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            const int k = m[x];
            if (k == 255)
                continue;
            for (int c = 0; c < 4; ++c)
                o[x * 4 + c] = uchar((o[x * 4 + c] * k + s[x * 4 + c] * (255 - k) + 127) / 255);
        }
    }
    return out;
}
QImage maskAlpha(const QImage &overlay, const QImage &mask) {
    if (mask.isNull() || mask.size() != overlay.size())
        return overlay;
    auto out = overlay.convertToFormat(QImage::Format_RGBA8888).copy();
    for (int y = 0; y < out.height(); ++y) {
        auto row = out.scanLine(y);
        const auto m = mask.constScanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            row[x * 4 + 3] = uchar(row[x * 4 + 3] * m[x] / 255);
            if (!row[x * 4 + 3])
                row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0;
        }
    }
    return out;
}
QImage paintCoverage(const QImage &target, const QImage &coverage, QRgb color, int opacity,
                     bool erase, const QImage *selection) {
    QImage out = target.convertToFormat(QImage::Format_RGBA8888).copy();
    for (int y = 0; y < out.height(); ++y) {
        auto m = coverage.constScanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            const int k = m[x] * maskValue(selection, x, y) / 255 * std::clamp(opacity, 0, 255) / 255;
            if (!k)
                continue;
            const QRgb before = out.pixel(x, y);
            if (erase) {
                const int a = qAlpha(before) * (255 - k) / 255;
                out.setPixel(x, y, a ? qRgba(qRed(before), qGreen(before), qBlue(before), a) : 0);
            } else {
                out.setPixel(x, y, k >= 255 ? color : blend(before, color, k));
            }
        }
    }
    return out;
}
QVector<QLineF> maskOutline(const QImage &mask) {
    QVector<QLineF> lines;
    if (mask.isNull())
        return lines;
    const int w = mask.width(), h = mask.height();
    auto on = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h && mask.constScanLine(y)[x] >= 128;
    };
    // Merge horizontal and vertical runs so a large selection stays cheap to draw.
    for (int y = 0; y <= h; ++y) {
        int start = -1;
        for (int x = 0; x <= w; ++x) {
            const bool edge = x < w && on(x, y) != on(x, y - 1);
            if (edge && start < 0)
                start = x;
            if (!edge && start >= 0) {
                lines.append(QLineF(start, y, x, y));
                start = -1;
            }
        }
    }
    for (int x = 0; x <= w; ++x) {
        int start = -1;
        for (int y = 0; y <= h; ++y) {
            const bool edge = y < h && on(x, y) != on(x - 1, y);
            if (edge && start < 0)
                start = y;
            if (!edge && start >= 0) {
                lines.append(QLineF(x, start, x, y));
                start = -1;
            }
        }
    }
    return lines;
}

// ---------------------------------------------------------------- shapes
QStringList shapeNames() {
    return {"Rectangle", "Rounded rectangle", "Ellipse", "Polygon", "Regular polygon", "Star",
            "Line"};
}
static QVector<QPointF> starPoints(const ShapeSpec &s, bool star) {
    QVector<QPointF> out;
    const int n = std::clamp(s.sides, 3, 64);
    const QPointF c = s.bounds.center();
    const double rx = s.bounds.width() / 2, ry = s.bounds.height() / 2;
    const int count = star ? n * 2 : n;
    for (int i = 0; i < count; ++i) {
        const double t = -Pi / 2 + 2 * Pi * i / count;
        const double k = star && (i % 2) ? std::clamp(s.innerRatio, 0.05, 0.95) : 1.0;
        out.append(c + QPointF(std::cos(t) * rx * k, std::sin(t) * ry * k));
    }
    return out;
}
static double segmentDistance(QPointF p, QPointF a, QPointF b) {
    const QPointF ab = b - a, ap = p - a;
    const double len = QPointF::dotProduct(ab, ab);
    const double t = len > 0 ? std::clamp(QPointF::dotProduct(ap, ab) / len, 0.0, 1.0) : 0.0;
    const QPointF d = p - (a + ab * t);
    return std::hypot(d.x(), d.y());
}
QImage rasterizeShape(QSize canvas, const ShapeSpec &spec) {
    auto m = emptyMask(canvas);
    if (canvas.isEmpty())
        return m;
    QPointF c = spec.bounds.center();
    if (spec.kind == ShapeKind::Polygon && spec.bounds.isEmpty() && !spec.points.isEmpty()) {
        // Free polygons rotate and flip about their own bounding-box centre.
        double l = spec.points[0].x(), r = l, t = spec.points[0].y(), bt = t;
        for (auto q : spec.points)
            l = std::min(l, q.x()), r = std::max(r, q.x()), t = std::min(t, q.y()), bt = std::max(bt, q.y());
        c = QPointF((l + r) / 2, (t + bt) / 2);
    }
    const double angle = spec.rotation * Pi / 180, cs = std::cos(-angle), sn = std::sin(-angle);
    if (spec.smooth) {
        // Use Qt's bounded CPU rasterizer to produce coverage, then share exactly
        // the same masking, alpha and command path as hard-edged shapes.
        QPainterPath path;
        const auto bounds = spec.bounds.normalized();
        switch (spec.kind) {
        case ShapeKind::Rectangle: path.addRect(bounds); break;
        case ShapeKind::RoundedRectangle: {
            const double r = std::clamp(spec.radius, 0.0, std::min(bounds.width(), bounds.height()) / 2);
            path.addRoundedRect(bounds, r, r, Qt::AbsoluteSize);
            break;
        }
        case ShapeKind::Ellipse: path.addEllipse(bounds); break;
        case ShapeKind::Line:
            if (spec.points.size() < 2)
                return m;
            path.moveTo(spec.points[0] + QPointF(.5, .5));
            path.lineTo(spec.points[1] + QPointF(.5, .5));
            break;
        default: {
            const auto points = spec.kind == ShapeKind::Polygon ? spec.points
                                : starPoints(spec, spec.kind == ShapeKind::Star);
            if (points.size() < 3)
                return m;
            path.addPolygon(QPolygonF(points));
            path.closeSubpath();
            break;
        }
        }
        if (!spec.filled || spec.kind == ShapeKind::Line) {
            QPainterPathStroker stroker;
            stroker.setWidth(std::clamp(spec.thickness, 1, 64) * (spec.kind == ShapeKind::Line ? 1 : 2));
            stroker.setJoinStyle(Qt::MiterJoin);
            stroker.setCapStyle(Qt::RoundCap);
            // Closed shape outlines stay inside the specified bounds. Construct
            // them before clipping to the canvas, so clipping adds no false edge.
            const auto stroke = stroker.createStroke(path);
            path = spec.kind == ShapeKind::Line ? stroke : path.intersected(stroke);
        }
        QTransform transform;
        transform.translate(c.x(), c.y());
        transform.rotate(spec.rotation);
        transform.scale(spec.flipH ? -1 : 1, spec.flipV ? -1 : 1);
        transform.translate(-c.x(), -c.y());
        QImage coverage(canvas, QImage::Format_ARGB32_Premultiplied);
        if (coverage.isNull())
            return m;
        coverage.fill(Qt::transparent);
        {
            QPainter painter(&coverage);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.fillPath(transform.map(path), Qt::white);
        }
        for (int y = 0; y < canvas.height(); ++y) {
            const auto pixels = reinterpret_cast<const QRgb *>(coverage.constScanLine(y));
            auto row = m.scanLine(y);
            for (int x = 0; x < canvas.width(); ++x)
                row[x] = uchar(qAlpha(pixels[x]));
        }
        return m;
    }
    // Inverse transform a pixel centre into the unrotated, unflipped shape frame.
    auto local = [&](QPointF p) {
        QPointF d = p - c;
        QPointF r(d.x() * cs - d.y() * sn, d.x() * sn + d.y() * cs);
        if (spec.flipH)
            r.setX(-r.x());
        if (spec.flipV)
            r.setY(-r.y());
        return r + c;
    };
    if (spec.kind == ShapeKind::Line) {
        if (spec.points.size() < 2)
            return m;
        // A Bresenham core keeps 1px lines identical to the pencil; thicker
        // lines add every pixel whose centre is within half the width.
        const QPoint a = spec.points[0].toPoint(), b = spec.points[1].toPoint();
        int x = a.x(), y = a.y(), dx = std::abs(b.x() - x), dy = -std::abs(b.y() - y),
            sx = x < b.x() ? 1 : -1, sy = y < b.y() ? 1 : -1, err = dx + dy;
        const int t = std::max(1, spec.thickness);
        for (;;) {
            for (int oy = -(t - 1) / 2; oy <= t / 2; ++oy)
                for (int ox = -(t - 1) / 2; ox <= t / 2; ++ox) {
                    QPoint q(x + ox, y + oy);
                    if (QRect(QPoint(), canvas).contains(q) &&
                        (t == 1 || segmentDistance(QPointF(q) + QPointF(.5, .5),
                                                   QPointF(a) + QPointF(.5, .5),
                                                   QPointF(b) + QPointF(.5, .5)) <= t / 2.0))
                        m.scanLine(q.y())[q.x()] = 255;
                }
            if (x == b.x() && y == b.y())
                break;
            const int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y += sy;
            }
        }
        return m;
    }
    QVector<QPointF> polygon;
    if (spec.kind == ShapeKind::Polygon)
        polygon = spec.points;
    else if (spec.kind == ShapeKind::RegularPolygon || spec.kind == ShapeKind::Star)
        polygon = starPoints(spec, spec.kind == ShapeKind::Star);
    const QRectF b = spec.bounds.normalized();
    auto inside = [&](QPointF p) {
        p = local(p);
        switch (spec.kind) {
        case ShapeKind::Rectangle:
            return b.contains(p) && p.x() < b.right() && p.y() < b.bottom();
        case ShapeKind::RoundedRectangle: {
            if (!(p.x() >= b.left() && p.x() < b.right() && p.y() >= b.top() && p.y() < b.bottom()))
                return false;
            const double r = std::clamp(spec.radius, 0.0, std::min(b.width(), b.height()) / 2);
            const double qx = std::max({b.left() + r - p.x(), p.x() - (b.right() - r), 0.0});
            const double qy = std::max({b.top() + r - p.y(), p.y() - (b.bottom() - r), 0.0});
            return qx * qx + qy * qy <= r * r;
        }
        case ShapeKind::Ellipse: {
            const double rx = b.width() / 2, ry = b.height() / 2;
            if (rx <= 0 || ry <= 0)
                return false;
            const double dx = (p.x() - b.center().x()) / rx, dy = (p.y() - b.center().y()) / ry;
            return dx * dx + dy * dy <= 1.0;
        }
        default:
            return polygon.size() >= 3 && insidePolygon(polygon, p);
        }
    };
    // Only scan the shape's extent (rotation-safe circumscribed box).
    QRectF extent = b;
    if (spec.kind == ShapeKind::Polygon || spec.kind == ShapeKind::RegularPolygon || spec.kind == ShapeKind::Star) {
        for (auto q : polygon)
            extent = extent.isEmpty() ? QRectF(q, QSizeF(1e-9, 1e-9)) : extent.united(QRectF(q, QSizeF(1e-9, 1e-9)));
    }
    const double reach = std::hypot(extent.width(), extent.height()) / 2 + 1;
    const QPointF mid = extent.center();
    const int sx0 = std::max(0, int(std::floor(mid.x() - reach))), sx1 = std::min(canvas.width() - 1, int(std::ceil(mid.x() + reach)));
    const int sy0 = std::max(0, int(std::floor(mid.y() - reach))), sy1 = std::min(canvas.height() - 1, int(std::ceil(mid.y() + reach)));
    // Rotation is about the bounds centre, so include it in the scanned box.
    const int cx0 = std::max(0, std::min(sx0, int(std::floor(c.x() - reach)))), cx1 = std::min(canvas.width() - 1, std::max(sx1, int(std::ceil(c.x() + reach))));
    const int cy0 = std::max(0, std::min(sy0, int(std::floor(c.y() - reach)))), cy1 = std::min(canvas.height() - 1, std::max(sy1, int(std::ceil(c.y() + reach))));
    for (int y = cy0; y <= cy1; ++y)
        for (int x = cx0; x <= cx1; ++x)
            if (inside(QPointF(x + .5, y + .5)))
                m.scanLine(y)[x] = 255;
    if (spec.filled)
        return m;
    // Outline: subtract repeated 4-neighbour erosions (diamond structuring element).
    QImage eroded = m;
    for (int i = 0; i < std::max(1, spec.thickness); ++i) {
        QImage next = eroded.copy();
        for (int y = 0; y < canvas.height(); ++y)
            for (int x = 0; x < canvas.width(); ++x) {
                if (!eroded.constScanLine(y)[x])
                    continue;
                auto off = [&](int ax, int ay) {
                    return ax < 0 || ay < 0 || ax >= canvas.width() || ay >= canvas.height() ||
                           !eroded.constScanLine(ay)[ax];
                };
                if (off(x - 1, y) || off(x + 1, y) || off(x, y - 1) || off(x, y + 1))
                    next.scanLine(y)[x] = 0;
            }
        eroded = next;
    }
    return combine(m, eroded, Combine::Subtract);
}

// ---------------------------------------------------------------- brushes
QStringList brushShapeNames() {
    return {"Square", "Circle", "Diamond", "Horizontal line", "Vertical line", "Custom stamp"};
}
QSize brushDimensions(const Brush &brush) {
    if (brush.shape == BrushShape::Custom && !brush.stamp.isNull() && brush.height == 0)
        return brush.stamp.size().boundedTo({Brush::MaxSize, Brush::MaxSize});
    return {std::clamp(brush.size, 1, Brush::MaxSize),
            std::clamp(brush.height ? brush.height : brush.size, 1, Brush::MaxSize)};
}
std::vector<Dab> footprint(const Brush &brush) {
    std::vector<Dab> out;
    const auto dimensions = brushDimensions(brush);
    const int n = dimensions.width(), h = dimensions.height();
    // Offsets span [-(n-1)/2, n/2] so odd sizes centre on the cursor pixel and
    // even sizes extend one extra pixel right/down (deterministic convention).
    const int lo = -(n - 1) / 2, hi = n / 2, top = -(h - 1) / 2, bottom = h / 2;
    const double centre = (lo + hi) / 2.0, radius = n / 2.0;
    const double centreY = (top + bottom) / 2.0, radiusY = h / 2.0;
    const double hard = std::clamp(brush.hardness, 0, 100) / 100.0;
    auto falloff = [&](double distanceRatio) {
        // Hard brushes are binary; softness fades coverage across the outer part.
        if (hard >= 1.0 || distanceRatio <= hard)
            return 255;
        const double t = std::clamp((distanceRatio - hard) / std::max(1e-6, 1 - hard), 0.0, 1.0);
        return int(std::lround(255 * (1 - t)));
    };
    if (brush.shape == BrushShape::Custom && !brush.stamp.isNull()) {
        const auto stamp = brush.stamp.scaled(dimensions, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                               .convertToFormat(QImage::Format_RGBA8888);
        const int ox = -(stamp.width() - 1) / 2, oy = -(stamp.height() - 1) / 2;
        for (int y = 0; y < stamp.height(); ++y)
            for (int x = 0; x < stamp.width(); ++x) {
                const int a = qAlpha(stamp.pixel(x, y));
                if (a)
                    out.push_back({QPoint(ox + x, oy + y), a,
                                   qRgba(qRed(stamp.pixel(x,y)),qGreen(stamp.pixel(x,y)),qBlue(stamp.pixel(x,y)),255)});
            }
        return out;
    }
    for (int y = top; y <= bottom; ++y)
        for (int x = lo; x <= hi; ++x) {
            const double dx = x - centre, dy = y - centreY;
            double ratio = 0;
            bool inside = true;
            switch (brush.shape) {
            case BrushShape::Square:
            case BrushShape::Custom:
                ratio = std::max(std::abs(dx) / std::max(.5, radius - .5),
                                 std::abs(dy) / std::max(.5, radiusY - .5));
                break;
            case BrushShape::Circle:
                ratio = std::hypot(dx / std::max(.5, radius - .5), dy / std::max(.5, radiusY - .5));
                inside = (n <= 2 && h <= 2) ||
                         std::hypot(dx / std::max(.25, radius - .25), dy / std::max(.25, radiusY - .25)) <= 1;
                break;
            case BrushShape::Diamond:
                ratio = std::abs(dx) / std::max(.5, radius - .5) + std::abs(dy) / std::max(.5, radiusY - .5);
                inside = (n <= 2 && h <= 2) || ratio <= 1 + 1e-9;
                break;
            case BrushShape::HorizontalLine:
                inside = y == 0;
                ratio = std::abs(dx) / std::max(.5, radius - .5);
                break;
            case BrushShape::VerticalLine:
                inside = x == 0;
                ratio = std::abs(dy) / std::max(.5, radiusY - .5);
                break;
            }
            if (!inside)
                continue;
            const int coverage = n == 1 && h == 1 ? 255 : falloff(std::min(ratio, 1.0));
            if (coverage > 0)
                out.push_back({QPoint(x, y), coverage});
        }
    return out;
}
QStringList symmetryNames() { return {"None", "Horizontal mirror", "Vertical mirror", "Both", "Radial"}; }
std::vector<QPoint> symmetricPoints(QPoint p, QSize canvas, Symmetry s, int radial, QPointF c) {
    std::vector<QPoint> out{p};
    auto add = [&](QPoint q) {
        if (std::find(out.begin(), out.end(), q) == out.end())
            out.push_back(q);
    };
    // Mirror about the axis through c (in pixel-edge coordinates): x' = 2c - 1 - x.
    const int mx = int(std::lround(2 * c.x())) - 1 - p.x();
    const int my = int(std::lround(2 * c.y())) - 1 - p.y();
    if (s == Symmetry::Horizontal || s == Symmetry::Both)
        add({mx, p.y()});
    if (s == Symmetry::Vertical || s == Symmetry::Both)
        add({p.x(), my});
    if (s == Symmetry::Both)
        add({mx, my});
    if (s == Symmetry::Radial) {
        const int n = std::clamp(radial, 2, 32);
        const double px = p.x() + .5 - c.x(), py = p.y() + .5 - c.y();
        for (int k = 1; k < n; ++k) {
            const double t = 2 * Pi * k / n;
            add(QPoint(int(std::floor(c.x() + px * std::cos(t) - py * std::sin(t))),
                       int(std::floor(c.y() + px * std::sin(t) + py * std::cos(t)))));
        }
    }
    std::erase_if(out, [&](QPoint q) { return !QRect(QPoint(), canvas).contains(q); });
    return out;
}

// ---------------------------------------------------------------- analysis
QImage tile(const QImage &image, int columns, int rows) {
    QImage out(image.width() * columns, image.height() * rows, QImage::Format_RGBA8888);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c)
            for (int y = 0; y < image.height(); ++y)
                std::copy_n(image.constScanLine(y), image.width() * 4,
                            out.scanLine(r * image.height() + y) + c * image.width() * 4);
    return out;
}
QImage wrapOffset(const QImage &image, int dx, int dy) {
    QImage out(image.size(), QImage::Format_RGBA8888);
    const int w = image.width(), h = image.height();
    if (w == 0 || h == 0)
        return out;
    for (int y = 0; y < h; ++y) {
        auto o = reinterpret_cast<QRgb *>(out.scanLine(((y + dy) % h + h) % h));
        auto s = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < w; ++x)
            o[((x + dx) % w + w) % w] = s[x];
    }
    return out;
}
SeamReport seamReport(const QImage &image) {
    SeamReport r;
    const int w = image.width(), h = image.height();
    if (w < 2 || h < 2)
        return r;
    auto diff = [](QRgb a, QRgb b) {
        return (std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) +
                std::abs(qBlue(a) - qBlue(b)) + std::abs(qAlpha(a) - qAlpha(b))) /
               4.0;
    };
    double worst = -1;
    for (int y = 0; y < h; ++y) {
        const double d = diff(image.pixel(0, y), image.pixel(w - 1, y));
        r.horizontal += d;
        if (d > worst) {
            worst = d;
            r.worstRow = y;
        }
    }
    worst = -1;
    for (int x = 0; x < w; ++x) {
        const double d = diff(image.pixel(x, 0), image.pixel(x, h - 1));
        r.vertical += d;
        if (d > worst) {
            worst = d;
            r.worstColumn = x;
        }
    }
    r.horizontal /= h;
    r.vertical /= w;
    return r;
}
std::vector<QRgb> extractPalette(const QImage &source, int maximum) {
    const auto image = source.convertToFormat(QImage::Format_RGBA8888);
    QHash<QRgb, int> counts;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QRgb c = image.pixel(x, y);
            if (qAlpha(c))
                ++counts[c | 0xff000000u];
        }
    std::vector<std::pair<QRgb, int>> sorted(counts.keyValueBegin(), counts.keyValueEnd());
    std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (int(sorted.size()) > maximum)
        return medianCut(image, maximum);
    std::vector<QRgb> out;
    for (auto [c, n] : sorted)
        out.push_back(c);
    return out;
}
std::vector<QRgb> medianCut(const QImage &source, int colors) {
    const auto image = source.convertToFormat(QImage::Format_RGBA8888);
    std::vector<QRgb> pixels;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)))
                pixels.push_back(image.pixel(x, y) | 0xff000000u);
    if (pixels.empty())
        return {};
    std::sort(pixels.begin(), pixels.end());
    struct Box {
        size_t begin, end;
    };
    std::vector<Box> boxes{{0, pixels.size()}};
    auto channel = [](QRgb c, int k) { return k == 0 ? qRed(c) : k == 1 ? qGreen(c) : qBlue(c); };
    while (int(boxes.size()) < std::clamp(colors, 1, 256)) {
        int best = -1, bestRange = 0, bestChannel = 0;
        for (int i = 0; i < int(boxes.size()); ++i) {
            if (boxes[i].end - boxes[i].begin < 2)
                continue;
            for (int k = 0; k < 3; ++k) {
                int lo = 255, hi = 0;
                for (size_t j = boxes[i].begin; j < boxes[i].end; ++j) {
                    lo = std::min(lo, channel(pixels[j], k));
                    hi = std::max(hi, channel(pixels[j], k));
                }
                if (hi - lo > bestRange) {
                    bestRange = hi - lo;
                    best = i;
                    bestChannel = k;
                }
            }
        }
        if (best < 0)
            break;
        auto box = boxes[best];
        std::stable_sort(pixels.begin() + qsizetype(box.begin), pixels.begin() + qsizetype(box.end),
                         [&](QRgb a, QRgb b) { return channel(a, bestChannel) < channel(b, bestChannel); });
        const size_t mid = (box.begin + box.end) / 2;
        boxes[best] = {box.begin, mid};
        boxes.push_back({mid, box.end});
    }
    std::vector<QRgb> out;
    for (auto box : boxes) {
        qint64 r = 0, g = 0, b = 0;
        for (size_t j = box.begin; j < box.end; ++j) {
            r += qRed(pixels[j]);
            g += qGreen(pixels[j]);
            b += qBlue(pixels[j]);
        }
        const qint64 n = qint64(box.end - box.begin);
        out.push_back(qRgb(int((r + n / 2) / n), int((g + n / 2) / n), int((b + n / 2) / n)));
    }
    std::sort(out.begin(), out.end(), [](QRgb a, QRgb b) { return luma(a) < luma(b); });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
QRect alphaBounds(const QImage &image, int threshold) {
    int left = image.width(), right = -1, top = image.height(), bottom = -1;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)) > threshold) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = y;
            }
    return right < 0 ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}
} // namespace nova::ops
