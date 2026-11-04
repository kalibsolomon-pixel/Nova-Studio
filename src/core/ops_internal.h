// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Internal operation plumbing shared by the ops_*.cpp translation units.
#include "imageops.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace nova::ops::detail {
constexpr double Pi = 3.14159265358979323846;
inline int clamp255(double v) { return int(std::clamp(std::lround(v), 0L, 255L)); }
struct Params {
    const QJsonObject &o;
    int i(const char *k) const { return o.value(k).toInt(); }
    double d(const char *k) const { return o.value(k).toDouble(); }
    bool b(const char *k) const { return o.value(k).toBool(); }
    QString s(const char *k) const { return o.value(k).toString(); }
    QRgb c(const char *k) const { return parseColor(o.value(k).toString()); }
    std::vector<QRgb> palette(const char *k, const Context &ctx) const {
        std::vector<QRgb> out;
        for (const auto &part : o.value(k).toString().split(',', Qt::SkipEmptyParts)) {
            bool ok = false;
            auto c = parseColor(part, &ok);
            if (ok)
                out.push_back(c);
        }
        return out.empty() ? ctx.palette : out;
    }
};
using Fn = QImage (*)(const QImage &, const Params &, const Context &, QString *);
// Applies f(QRgb) -> QRgb to every pixel with per-row cancellation checks.
template <class F> QImage mapPixels(const QImage &src, const Context &ctx, F f) {
    QImage out(src.size(), QImage::Format_RGBA8888);
    if (out.isNull())
        return out;
    for (int y = 0; y < src.height(); ++y) {
        if (ctx.canceled())
            return {};
        auto s = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        auto o = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            // Format_RGBA8888 bytes are R,G,B,A; convert through QImage pixel helpers.
            const uchar *b = reinterpret_cast<const uchar *>(s + x);
            const QRgb in = qRgba(b[0], b[1], b[2], b[3]);
            const QRgb r = f(in, x, y);
            uchar *w = reinterpret_cast<uchar *>(o + x);
            w[0] = uchar(qRed(r));
            w[1] = uchar(qGreen(r));
            w[2] = uchar(qBlue(r));
            w[3] = uchar(qAlpha(r));
        }
        if (ctx.progress && (y & 31) == 0)
            ctx.progress(y * 100 / std::max(1, src.height()));
    }
    return out;
}
inline QRgb px(const QImage &im, int x, int y) {
    const uchar *b = im.constScanLine(y) + x * 4;
    return qRgba(b[0], b[1], b[2], b[3]);
}
inline void setPx(QImage &im, int x, int y, QRgb c) {
    uchar *b = im.scanLine(y) + x * 4;
    b[0] = uchar(qRed(c));
    b[1] = uchar(qGreen(c));
    b[2] = uchar(qBlue(c));
    b[3] = uchar(qAlpha(c));
}
inline QImage blank(QSize size) {
    QImage out(size, QImage::Format_RGBA8888);
    out.fill(Qt::transparent);
    return out;
}
// Edge-handling sample: clamp or wrap (tileable textures).
inline QRgb sample(const QImage &im, int x, int y, bool wrap) {
    const int w = im.width(), h = im.height();
    if (wrap) {
        x = ((x % w) + w) % w;
        y = ((y % h) + h) % h;
    } else {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
    }
    return px(im, x, y);
}
enum class Dither { None, Ordered, FloydSteinberg };
Dither ditherFromName(const QString &name);
QImage ditherToPalette(const QImage &src, const std::vector<QRgb> &palette, Dither mode,
                       Distance metric, const Context &ctx);
int bayer4(int x, int y); // 0..15
QImage heightField(const QImage &src, const QString &source, const Context &ctx, double blur);
QImage gaussian(const QImage &src, double sigma, bool wrap, const Context &ctx);

#define NOVA_OP(name) QImage name(const QImage &src, const Params &p, const Context &ctx, QString *error)
// colour
NOVA_OP(brightnessContrast); NOVA_OP(hueSaturation); NOVA_OP(exposure); NOVA_OP(gammaOp);
NOVA_OP(temperatureTint); NOVA_OP(levels); NOVA_OP(curves); NOVA_OP(channels);
NOVA_OP(replaceColor); NOVA_OP(grayscale); NOVA_OP(invert); NOVA_OP(colorize);
NOVA_OP(posterize); NOVA_OP(quantize); NOVA_OP(paletteMap); NOVA_OP(alphaThreshold);
NOVA_OP(transparencyCleanup); NOVA_OP(hsvNoise); NOVA_OP(fillColor); NOVA_OP(gradient);
// filters
NOVA_OP(boxBlur); NOVA_OP(gaussianBlur); NOVA_OP(sharpen); NOVA_OP(unsharpMask); NOVA_OP(median);
NOVA_OP(noise); NOVA_OP(pixelate); NOVA_OP(edgeDetect); NOVA_OP(emboss); NOVA_OP(outline);
NOVA_OP(edgeEnhance); NOVA_OP(normalMap); NOVA_OP(heightmap); NOVA_OP(seamless);
// transforms
NOVA_OP(flip); NOVA_OP(rotate); NOVA_OP(offset); NOVA_OP(scaleContent); NOVA_OP(alignContent);
NOVA_OP(imageRotate); NOVA_OP(imageFlip); NOVA_OP(resizeImage); NOVA_OP(resizeCanvas);
NOVA_OP(crop); NOVA_OP(trim);
// shading + generators
NOVA_OP(directionalShading); NOVA_OP(genStone); NOVA_OP(genBricks); NOVA_OP(genTiles);
NOVA_OP(genWood); NOVA_OP(genOre); NOVA_OP(genMetal); NOVA_OP(genPanel); NOVA_OP(genItem);
NOVA_OP(genNoise);
#undef NOVA_OP
enum class Resample { Nearest, Bilinear, Bicubic };
Resample resampleFromName(const QString &name);
QRgb sampleAt(const QImage &im, double x, double y, Resample mode, bool wrap);
QImage resample(const QImage &src, QSize size, Resample mode);
} // namespace nova::ops::detail
