// SPDX-License-Identifier: GPL-3.0-or-later
// Colour adjustments. Unless stated otherwise, maths runs on encoded sRGB
// channels with straight alpha, and alpha is preserved. Exposure and colour
// temperature convert through linear light because they model light intensity.
#include "ops_internal.h"

namespace nova::ops::detail {
namespace {
QRgb withRgb(QRgb c, int r, int g, int b) { return qRgba(r, g, b, qAlpha(c)); }
std::array<uchar, 256> lut(auto f) {
    std::array<uchar, 256> t{};
    for (int i = 0; i < 256; ++i)
        t[size_t(i)] = uchar(clamp255(f(i)));
    return t;
}
QRgb applyLut(QRgb c, const std::array<uchar, 256> &t) {
    return withRgb(c, t[size_t(qRed(c))], t[size_t(qGreen(c))], t[size_t(qBlue(c))]);
}
const int Bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
} // namespace
int bayer4(int x, int y) { return Bayer[y & 3][x & 3]; }
Dither ditherFromName(const QString &name) {
    if (name.startsWith("Ordered"))
        return Dither::Ordered;
    if (name.startsWith("Floyd"))
        return Dither::FloydSteinberg;
    return Dither::None;
}
QImage ditherToPalette(const QImage &src, const std::vector<QRgb> &palette, Dither mode,
                       Distance metric, const Context &ctx) {
    if (palette.empty())
        return src.copy();
    if (mode == Dither::None)
        return mapPixels(src, ctx, [&](QRgb c, int, int) {
            return qAlpha(c) ? nearest(c, palette, metric) : c;
        });
    if (mode == Dither::Ordered) {
        // Spread scales with the mean gap between palette entries.
        const int spread = std::clamp(255 / int(std::max<size_t>(2, palette.size())) * 2, 8, 64);
        return mapPixels(src, ctx, [&](QRgb c, int x, int y) {
            if (!qAlpha(c))
                return c;
            const int o = (bayer4(x, y) - 8) * spread / 16;
            return nearest(qRgba(clamp255(qRed(c) + o), clamp255(qGreen(c) + o),
                                 clamp255(qBlue(c) + o), qAlpha(c)),
                           palette, metric);
        });
    }
    // Floyd–Steinberg in integer RGB with serpentine-free left-to-right order
    // (deterministic; bounded to two error rows).
    QImage out = src.copy();
    const int w = src.width();
    std::vector<int> current(size_t(w + 2) * 3, 0), next(size_t(w + 2) * 3, 0);
    for (int y = 0; y < src.height(); ++y) {
        if (ctx.canceled())
            return {};
        std::fill(next.begin(), next.end(), 0);
        for (int x = 0; x < w; ++x) {
            const QRgb c = px(src, x, y);
            if (!qAlpha(c))
                continue;
            const size_t i = size_t(x + 1) * 3;
            const int r = std::clamp(qRed(c) + current[i] / 16, 0, 255),
                      g = std::clamp(qGreen(c) + current[i + 1] / 16, 0, 255),
                      b = std::clamp(qBlue(c) + current[i + 2] / 16, 0, 255);
            const QRgb q = nearest(qRgba(r, g, b, qAlpha(c)), palette, metric);
            setPx(out, x, y, q);
            const int er = r - qRed(q), eg = g - qGreen(q), eb = b - qBlue(q);
            auto spread = [&](std::vector<int> &row, int dx, int k) {
                const size_t j = size_t(x + 1 + dx) * 3;
                row[j] += er * k;
                row[j + 1] += eg * k;
                row[j + 2] += eb * k;
            };
            spread(current, 1, 7);
            spread(next, -1, 3);
            spread(next, 0, 5);
            spread(next, 1, 1);
        }
        std::swap(current, next);
    }
    return out;
}
QImage brightnessContrast(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double b = p.i("brightness") * 2.55, c = p.i("contrast") * 2.55;
    const double f = (259 * (c + 255)) / (255 * (259 - c));
    const auto t = lut([&](int v) { return f * (v - 128) + 128 + b; });
    return mapPixels(src, ctx, [&](QRgb px, int, int) { return applyLut(px, t); });
}
QImage hueSaturation(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double h = p.i("hue"), s = p.i("saturation") / 100.0, l = p.i("lightness") / 100.0;
    return mapPixels(src, ctx, [&](QRgb c, int, int) {
        if (!qAlpha(c))
            return c;
        auto hsl = toHsl(c);
        hsl.h += h;
        hsl.s = s >= 0 ? hsl.s + (1 - hsl.s) * s * hsl.s : hsl.s * (1 + s);
        hsl.l = l >= 0 ? hsl.l + (1 - hsl.l) * l : hsl.l * (1 + l);
        return fromHsl(hsl, qAlpha(c));
    });
}
QImage exposure(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double k = std::pow(2.0, p.d("stops"));
    const auto t = lut([&](int v) { return linearToSrgb(srgbToLinear(v / 255.0) * k) * 255; });
    return mapPixels(src, ctx, [&](QRgb c, int, int) { return applyLut(c, t); });
}
QImage gammaOp(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double g = std::max(0.01, p.d("gamma"));
    const auto t = lut([&](int v) { return std::pow(v / 255.0, 1 / g) * 255; });
    return mapPixels(src, ctx, [&](QRgb c, int, int) { return applyLut(c, t); });
}
QImage temperatureTint(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double t = p.i("temperature") / 100.0, n = p.i("tint") / 100.0;
    const double kr = 1 + 0.25 * t, kb = 1 - 0.25 * t, kg = 1 - 0.2 * n;
    auto make = [](double k) {
        return lut([k](int v) { return linearToSrgb(srgbToLinear(v / 255.0) * k) * 255; });
    };
    const auto r = make(kr), g = make(kg), b = make(kb);
    return mapPixels(src, ctx, [&](QRgb c, int, int) {
        return withRgb(c, r[size_t(qRed(c))], g[size_t(qGreen(c))], b[size_t(qBlue(c))]);
    });
}
QImage levels(const QImage &src, const Params &p, const Context &ctx, QString *error) {
    const int ib = p.i("input_black"), iw = p.i("input_white"), ob = p.i("output_black"),
              ow = p.i("output_white");
    const double mid = p.d("midtone");
    if (iw <= ib) {
        if (error)
            *error = "Input white must exceed input black";
        return {};
    }
    const auto t = lut([&](int v) {
        const double x = std::clamp((v - ib) / double(iw - ib), 0.0, 1.0);
        return ob + std::pow(x, 1 / mid) * (ow - ob);
    });
    const auto channel = p.s("channel");
    return mapPixels(src, ctx, [&](QRgb c, int, int) {
        if (channel == "Alpha")
            return qRgba(qRed(c), qGreen(c), qBlue(c), t[size_t(qAlpha(c))]);
        return withRgb(c, channel == "RGB" || channel == "Red" ? t[size_t(qRed(c))] : qRed(c),
                       channel == "RGB" || channel == "Green" ? t[size_t(qGreen(c))] : qGreen(c),
                       channel == "RGB" || channel == "Blue" ? t[size_t(qBlue(c))] : qBlue(c));
    });
}
QImage curves(const QImage &src, const Params &p, const Context &ctx, QString *) {
    // Monotone cubic (Fritsch–Carlson) through five fixed-x control points.
    const double xs[5] = {0, 64, 128, 192, 255};
    const double ys[5] = {double(p.i("black")), double(p.i("shadows")), double(p.i("midtones")),
                          double(p.i("highlights")), double(p.i("white"))};
    double m[5], d[4];
    for (int i = 0; i < 4; ++i)
        d[i] = (ys[i + 1] - ys[i]) / (xs[i + 1] - xs[i]);
    m[0] = d[0];
    m[4] = d[3];
    for (int i = 1; i < 4; ++i)
        m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
    for (int i = 0; i < 4; ++i)
        if (d[i] == 0)
            m[i] = m[i + 1] = 0;
        else {
            const double a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
            if (s > 9) {
                const double k = 3 / std::sqrt(s);
                m[i] = k * a * d[i];
                m[i + 1] = k * b * d[i];
            }
        }
    const auto t = lut([&](int v) {
        int i = std::min(3, int(v / 64));
        if (v >= 192)
            i = 3;
        const double h = xs[i + 1] - xs[i], u = (v - xs[i]) / h;
        const double h00 = 2 * u * u * u - 3 * u * u + 1, h10 = u * u * u - 2 * u * u + u,
                     h01 = -2 * u * u * u + 3 * u * u, h11 = u * u * u - u * u;
        return h00 * ys[i] + h10 * h * m[i] + h01 * ys[i + 1] + h11 * h * m[i + 1];
    });
    const auto channel = p.s("channel");
    return mapPixels(src, ctx, [&](QRgb c, int, int) {
        return withRgb(c, channel == "RGB" || channel == "Red" ? t[size_t(qRed(c))] : qRed(c),
                       channel == "RGB" || channel == "Green" ? t[size_t(qGreen(c))] : qGreen(c),
                       channel == "RGB" || channel == "Blue" ? t[size_t(qBlue(c))] : qBlue(c));
    });
}
QImage channels(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double r = p.i("red") / 100.0, g = p.i("green") / 100.0, b = p.i("blue") / 100.0,
                 a = p.i("alpha") / 100.0;
    const auto swap = p.s("swap");
    return mapPixels(src, ctx, [&](QRgb c, int, int) {
        int cr = qRed(c), cg = qGreen(c), cb = qBlue(c);
        if (swap == "Red ↔ Blue")
            std::swap(cr, cb);
        else if (swap == "Red ↔ Green")
            std::swap(cr, cg);
        else if (swap == "Green ↔ Blue")
            std::swap(cg, cb);
        return qRgba(clamp255(cr * r), clamp255(cg * g), clamp255(cb * b),
                     a == 1 ? qAlpha(c) : clamp255(qAlpha(c) * a));
    });
}
QImage replaceColor(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const QRgb from = p.c("from"), to = p.c("to");
    const int tolerance = p.i("tolerance");
    const auto metric = distanceFromName(p.s("metric"));
    const bool shading = p.b("preserve_shading"), alpha = p.b("include_alpha");
    const auto fromHslv = toHsl(from), toHslv = toHsl(to);
    return mapPixels(src, ctx, [&](QRgb c, int, int) {
        if (!qAlpha(c) || !colorMatches(c, from, tolerance, metric, alpha))
            return c;
        if (!shading)
            return qRgba(qRed(to), qGreen(to), qBlue(to), alpha ? qAlpha(to) : qAlpha(c));
        // Keep the pixel's lightness/saturation offsets relative to the source colour.
        auto h = toHsl(c);
        h.h = toHslv.h + (h.h - fromHslv.h);
        h.s = std::clamp(toHslv.s + (h.s - fromHslv.s), 0.0, 1.0);
        h.l = std::clamp(toHslv.l + (h.l - fromHslv.l), 0.0, 1.0);
        return fromHsl(h, qAlpha(c));
    });
}
QImage grayscale(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const auto method = p.s("method");
    const int amount = p.i("amount");
    return mapPixels(src, ctx, [&](QRgb c, int, int) {
        int g = luma(c);
        if (method == "Average")
            g = (qRed(c) + qGreen(c) + qBlue(c) + 1) / 3;
        else if (method == "Lightness")
            g = (std::max({qRed(c), qGreen(c), qBlue(c)}) + std::min({qRed(c), qGreen(c), qBlue(c)}) + 1) / 2;
        auto mix = [&](int v) { return (v * (100 - amount) + g * amount + 50) / 100; };
        return withRgb(c, mix(qRed(c)), mix(qGreen(c)), mix(qBlue(c)));
    });
}
QImage invert(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const bool alpha = p.b("invert_alpha");
    return mapPixels(src, ctx, [&](QRgb c, int, int) {
        return qRgba(255 - qRed(c), 255 - qGreen(c), 255 - qBlue(c),
                     alpha ? 255 - qAlpha(c) : qAlpha(c));
    });
}
QImage colorize(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const auto target = toHsl(p.c("color"));
    const bool lock = p.b("lock_saturation_lightness");
    const int amount = p.i("amount");
    return mapPixels(src, ctx, [&](QRgb c, int, int) {
        if (!qAlpha(c))
            return c;
        auto h = toHsl(c);
        h.h = target.h;
        if (!lock) {
            h.s = target.s;
            h.l = std::clamp(h.l * 0.5 + target.l * 0.5 + (h.l - 0.5) * 0.5, 0.0, 1.0);
        }
        const QRgb out = fromHsl(h, qAlpha(c));
        auto mix = [&](int a, int b) { return (a * (100 - amount) + b * amount + 50) / 100; };
        return qRgba(mix(qRed(c), qRed(out)), mix(qGreen(c), qGreen(out)), mix(qBlue(c), qBlue(out)),
                     qAlpha(c));
    });
}
QImage posterize(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const int n = std::max(2, p.i("levels"));
    const auto t = lut([&](int v) { return std::lround(v * (n - 1) / 255.0) * 255.0 / (n - 1); });
    return mapPixels(src, ctx, [&](QRgb c, int, int) { return applyLut(c, t); });
}
QImage quantize(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const auto palette = medianCut(src, p.i("colors"));
    return ditherToPalette(src, palette, ditherFromName(p.s("dither")), Distance::Perceptual, ctx);
}
QImage paletteMap(const QImage &src, const Params &p, const Context &ctx, QString *error) {
    const auto palette = p.palette("palette", ctx);
    if (palette.empty()) {
        if (error)
            *error = "Choose a palette with at least one colour";
        return {};
    }
    return ditherToPalette(src, palette, ditherFromName(p.s("dither")),
                           distanceFromName(p.s("metric")), ctx);
}
QImage alphaThreshold(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const int t = p.i("threshold");
    return mapPixels(src, ctx, [&](QRgb c, int, int) {
        return qAlpha(c) >= t ? qRgba(qRed(c), qGreen(c), qBlue(c), 255) : qRgba(0, 0, 0, 0);
    });
}
QImage transparencyCleanup(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const int t = p.i("min_alpha");
    return mapPixels(src, ctx,
                     [&](QRgb c, int, int) { return qAlpha(c) <= t ? qRgba(0, 0, 0, 0) : c; });
}
QImage hsvNoise(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double hv = p.i("hue"), sv = p.i("saturation") / 100.0, vv = p.i("value") / 100.0;
    const quint64 seed = quint64(p.i("seed"));
    return mapPixels(src, ctx, [&](QRgb c, int x, int y) {
        if (!qAlpha(c))
            return c;
        auto h = toHsv(c);
        h.h += (hashUnit(seed, x, y) * 2 - 1) * hv;
        h.s += (hashUnit(seed + 1, x, y) * 2 - 1) * sv;
        h.v += (hashUnit(seed + 2, x, y) * 2 - 1) * vv;
        return fromHsv(h, qAlpha(c));
    });
}
QImage fillColor(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const QRgb color = p.c("color");
    const int opacity = p.i("opacity");
    return mapPixels(src, ctx, [&](QRgb c, int, int) { return blend(c, color, opacity); });
}
QImage gradient(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const QPointF a(p.d("x0"), p.d("y0")), b(p.d("x1"), p.d("y1"));
    const QRgb ca = p.c("color_a"), cb = p.c("color_b");
    const auto type = p.s("type");
    const int steps = p.i("steps");
    const bool dither = p.s("dither").startsWith("Ordered");
    const QPointF d = b - a;
    const double len2 = std::max(1e-9, QPointF::dotProduct(d, d));
    return mapPixels(src, ctx, [&](QRgb c, int x, int y) {
        const QPointF q = QPointF(x + .5, y + .5) - a;
        double t = type == "Radial" ? std::sqrt(QPointF::dotProduct(q, q) / len2)
                                    : QPointF::dotProduct(q, d) / len2;
        if (type == "Reflected")
            t = std::abs(t);
        t = std::clamp(t, 0.0, 1.0);
        if (dither)
            t = std::clamp(t + (bayer4(x, y) - 7.5) / 16.0 / std::max(2, steps ? steps : 8), 0.0, 1.0);
        if (steps >= 2)
            t = std::round(t * (steps - 1)) / (steps - 1);
        auto mix = [&](int u, int v) { return clamp255(u + (v - u) * t); };
        const QRgb g = qRgba(mix(qRed(ca), qRed(cb)), mix(qGreen(ca), qGreen(cb)),
                             mix(qBlue(ca), qBlue(cb)), mix(qAlpha(ca), qAlpha(cb)));
        return blend(c, g, p.i("opacity"));
    });
}
} // namespace nova::ops::detail
