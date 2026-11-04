// SPDX-License-Identifier: GPL-3.0-or-later
// Spatial filters. Convolutions average premultiplied colour so transparent
// neighbours never bleed hidden RGB into visible pixels (no dark fringes).
#include "ops_internal.h"
#include <QHash>
#include <vector>

namespace nova::ops::detail {
namespace {
// Separable convolution: horizontal pass into a premultiplied float buffer,
// vertical pass straight into RGBA8 (≈16 B/pixel temporary storage).
QImage separable(const QImage &src, const std::vector<double> &kernel, bool wrap,
                 const Context &ctx) {
    const int w = src.width(), h = src.height(), r = int(kernel.size() / 2);
    std::vector<float> buffer(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        if (ctx.canceled())
            return {};
        for (int x = 0; x < w; ++x) {
            double acc[4] = {0, 0, 0, 0};
            for (int k = -r; k <= r; ++k) {
                const QRgb c = sample(src, x + k, y, wrap);
                const double wk = kernel[size_t(k + r)], a = qAlpha(c) / 255.0;
                acc[0] += qRed(c) * a * wk;
                acc[1] += qGreen(c) * a * wk;
                acc[2] += qBlue(c) * a * wk;
                acc[3] += qAlpha(c) * wk;
            }
            for (int c = 0; c < 4; ++c)
                buffer[(size_t(y) * w + x) * 4 + c] = float(acc[c]);
        }
        if (ctx.progress && (y & 31) == 0)
            ctx.progress(y * 50 / std::max(1, h));
    }
    QImage out(src.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        if (ctx.canceled())
            return {};
        for (int x = 0; x < w; ++x) {
            double acc[4] = {0, 0, 0, 0};
            for (int k = -r; k <= r; ++k) {
                int yy = y + k;
                yy = wrap ? ((yy % h) + h) % h : std::clamp(yy, 0, h - 1);
                const float *b = &buffer[(size_t(yy) * w + x) * 4];
                const double wk = kernel[size_t(k + r)];
                for (int c = 0; c < 4; ++c)
                    acc[c] += b[c] * wk;
            }
            const double a = acc[3];
            if (a < 0.5) {
                setPx(out, x, y, qRgba(0, 0, 0, 0));
                continue;
            }
            // acc[0..2] hold colour × (alpha/255); divide by the averaged alpha.
            const double k = 255.0 / a;
            setPx(out, x, y,
                  qRgba(clamp255(acc[0] * k), clamp255(acc[1] * k), clamp255(acc[2] * k),
                        clamp255(a)));
        }
        if (ctx.progress && (y & 31) == 0)
            ctx.progress(50 + y * 50 / std::max(1, h));
    }
    return out;
}
std::vector<double> gaussianKernel(double sigma) {
    const int r = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<double> k(size_t(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i)
        sum += k[size_t(i + r)] = std::exp(-(i * i) / (2 * sigma * sigma));
    for (auto &v : k)
        v /= sum;
    return k;
}
// 3×3 kernel on straight colour for visible pixels; alpha is preserved.
template <class K> QImage kernel3(const QImage &src, const K &kernel, bool wrap, const Context &ctx) {
    return mapPixels(src, ctx, [&](QRgb c, int x, int y) {
        if (!qAlpha(c))
            return c;
        double acc[3] = {0, 0, 0};
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                QRgb n = sample(src, x + i, y + j, wrap);
                if (!qAlpha(n))
                    n = c; // treat transparency as the centre colour (no halo)
                const double k = kernel[j + 1][i + 1];
                acc[0] += qRed(n) * k;
                acc[1] += qGreen(n) * k;
                acc[2] += qBlue(n) * k;
            }
        return qRgba(clamp255(acc[0]), clamp255(acc[1]), clamp255(acc[2]), qAlpha(c));
    });
}
double lumaAt(const QImage &im, int x, int y, bool wrap) {
    const QRgb c = sample(im, x, y, wrap);
    return luma(c) * qAlpha(c) / 255.0;
}
// Chamfer (3-4) distance from each opaque pixel to the nearest transparent pixel.
std::vector<double> alphaDistance(const QImage &src, int threshold) {
    const int w = src.width(), h = src.height();
    const double inf = 1e9;
    std::vector<double> d(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            d[size_t(y) * w + x] = qAlpha(px(src, x, y)) > threshold ? inf : 0;
    auto at = [&](int x, int y) -> double {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return 0; // image border counts as transparent edge
        return d[size_t(y) * w + x];
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto &v = d[size_t(y) * w + x];
            v = std::min({v, at(x - 1, y) + 1, at(x, y - 1) + 1, at(x - 1, y - 1) + 1.4142,
                          at(x + 1, y - 1) + 1.4142});
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            auto &v = d[size_t(y) * w + x];
            v = std::min({v, at(x + 1, y) + 1, at(x, y + 1) + 1, at(x + 1, y + 1) + 1.4142,
                          at(x - 1, y + 1) + 1.4142});
        }
    return d;
}
} // namespace

QImage gaussian(const QImage &src, double sigma, bool wrap, const Context &ctx) {
    if (sigma <= 0.01)
        return src.copy();
    return separable(src, gaussianKernel(sigma), wrap, ctx);
}
QImage boxBlur(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const int r = p.i("radius");
    return separable(src, std::vector<double>(size_t(2 * r + 1), 1.0 / (2 * r + 1)), p.b("wrap"), ctx);
}
QImage gaussianBlur(const QImage &src, const Params &p, const Context &ctx, QString *) {
    return gaussian(src, p.d("sigma"), p.b("wrap"), ctx);
}
QImage sharpen(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double a = p.i("amount") / 100.0;
    const double k[3][3] = {{0, -a, 0}, {-a, 1 + 4 * a, -a}, {0, -a, 0}};
    return kernel3(src, k, p.b("wrap"), ctx);
}
QImage edgeEnhance(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double a = p.i("amount") / 100.0;
    const double k[3][3] = {{-a, -a, -a}, {-a, 1 + 8 * a, -a}, {-a, -a, -a}};
    return kernel3(src, k, p.b("wrap"), ctx);
}
QImage unsharpMask(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const auto blurred = gaussian(src, p.d("radius"), p.b("wrap"), ctx);
    if (blurred.isNull())
        return {};
    const double amount = p.i("amount") / 100.0;
    const int threshold = p.i("threshold");
    return mapPixels(src, ctx, [&](QRgb c, int x, int y) {
        if (!qAlpha(c))
            return c;
        const QRgb b = px(blurred, x, y);
        auto ch = [&](int v, int bv) {
            const int d = v - bv;
            return std::abs(d) < threshold ? v : clamp255(v + d * amount);
        };
        return qRgba(ch(qRed(c), qRed(b)), ch(qGreen(c), qGreen(b)), ch(qBlue(c), qBlue(b)),
                     qAlpha(c));
    });
}
QImage median(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const int r = p.i("radius");
    const bool wrap = p.b("wrap");
    std::vector<int> rs, gs, bs, as;
    return mapPixels(src, ctx, [&](QRgb, int x, int y) {
        rs.clear(), gs.clear(), bs.clear(), as.clear();
        for (int j = -r; j <= r; ++j)
            for (int i = -r; i <= r; ++i) {
                const QRgb n = sample(src, x + i, y + j, wrap);
                as.push_back(qAlpha(n));
                if (qAlpha(n)) {
                    rs.push_back(qRed(n));
                    gs.push_back(qGreen(n));
                    bs.push_back(qBlue(n));
                }
            }
        auto mid = [](std::vector<int> &v) {
            std::nth_element(v.begin(), v.begin() + qsizetype(v.size() / 2), v.end());
            return v[v.size() / 2];
        };
        const int a = mid(as);
        if (!a || rs.empty())
            return qRgba(0, 0, 0, 0);
        return qRgba(mid(rs), mid(gs), mid(bs), a);
    });
}
QImage noise(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const int amount = p.i("amount");
    const bool mono = p.b("monochrome"), gaussianNoise = p.s("distribution") == "Gaussian";
    const quint64 seed = quint64(p.i("seed"));
    auto sampleNoise = [&](quint64 s, int x, int y) {
        if (!gaussianNoise)
            return hashUnit(s, x, y) * 2 - 1;
        const double u1 = std::max(1e-12, hashUnit(s, x, y)), u2 = hashUnit(s + 99, x, y);
        return std::clamp(std::sqrt(-2 * std::log(u1)) * std::cos(2 * Pi * u2) / 3, -1.0, 1.0);
    };
    return mapPixels(src, ctx, [&](QRgb c, int x, int y) {
        if (!qAlpha(c))
            return c;
        const double n0 = sampleNoise(seed, x, y);
        const double n1 = mono ? n0 : sampleNoise(seed + 1, x, y),
                     n2 = mono ? n0 : sampleNoise(seed + 2, x, y);
        return qRgba(clamp255(qRed(c) + n0 * amount), clamp255(qGreen(c) + n1 * amount),
                     clamp255(qBlue(c) + n2 * amount), qAlpha(c));
    });
}
QImage pixelate(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const int n = p.i("block");
    const auto mode = p.s("mode");
    QImage out(src.size(), QImage::Format_RGBA8888);
    for (int by = 0; by < src.height(); by += n) {
        if (ctx.canceled())
            return {};
        for (int bx = 0; bx < src.width(); bx += n) {
            const int x1 = std::min(src.width(), bx + n), y1 = std::min(src.height(), by + n);
            QRgb value = 0;
            if (mode == "Centre sample")
                value = px(src, std::min(x1 - 1, bx + n / 2), std::min(y1 - 1, by + n / 2));
            else if (mode == "Dominant") {
                QHash<QRgb, int> counts;
                int best = 0;
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x) {
                        const QRgb c = qAlpha(px(src, x, y)) ? px(src, x, y) : 0;
                        const int k = ++counts[c];
                        if (k > best || (k == best && c < value)) {
                            best = k;
                            value = c;
                        }
                    }
            } else {
                double r = 0, g = 0, b = 0, a = 0;
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x) {
                        const QRgb c = px(src, x, y);
                        r += qRed(c) * qAlpha(c), g += qGreen(c) * qAlpha(c);
                        b += qBlue(c) * qAlpha(c), a += qAlpha(c);
                    }
                const int count = (x1 - bx) * (y1 - by);
                value = a > 0 ? qRgba(clamp255(r / a), clamp255(g / a), clamp255(b / a),
                                      clamp255(a / count))
                              : 0;
            }
            for (int y = by; y < y1; ++y)
                for (int x = bx; x < x1; ++x)
                    setPx(out, x, y, value);
        }
    }
    return out;
}
QImage edgeDetect(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const bool laplace = p.s("method") == "Laplacian", colored = p.s("output") == "Coloured edges";
    const int threshold = p.i("threshold");
    const bool wrap = p.b("wrap");
    return mapPixels(src, ctx, [&](QRgb c, int x, int y) {
        auto L = [&](int i, int j) { return lumaAt(src, x + i, y + j, wrap); };
        double m;
        if (laplace)
            m = std::abs(4 * L(0, 0) - L(-1, 0) - L(1, 0) - L(0, -1) - L(0, 1));
        else {
            const double gx = L(1, -1) + 2 * L(1, 0) + L(1, 1) - L(-1, -1) - 2 * L(-1, 0) - L(-1, 1);
            const double gy = L(-1, 1) + 2 * L(0, 1) + L(1, 1) - L(-1, -1) - 2 * L(0, -1) - L(1, -1);
            m = std::hypot(gx, gy) / 4;
        }
        int v = clamp255(m);
        if (v < threshold)
            v = 0;
        if (colored)
            return qRgba(qRed(c) * v / 255, qGreen(c) * v / 255, qBlue(c) * v / 255, qAlpha(c));
        return qRgba(v, v, v, qAlpha(c) ? 255 : (v ? 255 : 0));
    });
}
QImage emboss(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const double a = p.i("angle") * Pi / 180, s = p.d("strength");
    const double dx = std::cos(a), dy = -std::sin(a);
    const bool over = p.s("blend") == "Over original";
    const bool wrap = p.b("wrap");
    return mapPixels(src, ctx, [&](QRgb c, int x, int y) {
        if (!qAlpha(c))
            return c;
        const double g = (lumaAt(src, x + int(std::lround(dx)), y + int(std::lround(dy)), wrap) -
                          lumaAt(src, x - int(std::lround(dx)), y - int(std::lround(dy)), wrap)) *
                         s;
        if (over)
            return qRgba(clamp255(qRed(c) + g), clamp255(qGreen(c) + g), clamp255(qBlue(c) + g),
                         qAlpha(c));
        const int v = clamp255(128 + g);
        return qRgba(v, v, v, qAlpha(c));
    });
}
QImage outline(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const QRgb color = p.c("color");
    const int t = p.i("thickness"), threshold = p.i("alpha_threshold");
    const bool inside = p.s("position") == "Inside", diagonal = p.s("corners") == "Square";
    const int w = src.width(), h = src.height();
    std::vector<uchar> solid(size_t(w) * h), ring(size_t(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            solid[size_t(y) * w + x] = qAlpha(px(src, x, y)) >= threshold;
    // Grow (outside) or shrink (inside) one pixel per iteration.
    std::vector<uchar> region = solid;
    for (int i = 0; i < t; ++i) {
        if (ctx.canceled())
            return {};
        auto next = region;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const bool on = region[size_t(y) * w + x];
                if (on != inside)
                    continue;
                bool touch = false;
                for (int j = -1; j <= 1 && !touch; ++j)
                    for (int k = -1; k <= 1 && !touch; ++k) {
                        if ((!j && !k) || (!diagonal && j && k))
                            continue;
                        const int xx = x + k, yy = y + j;
                        const bool n = xx >= 0 && yy >= 0 && xx < w && yy < h && region[size_t(yy) * w + xx];
                        touch = inside ? !n : n;
                    }
                if (touch) {
                    next[size_t(y) * w + x] = !inside;
                    ring[size_t(y) * w + x] = 1;
                }
            }
        region = next;
    }
    QImage out = src.copy();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (ring[size_t(y) * w + x])
                setPx(out, x, y, inside ? color : blend(px(src, x, y), color, 255));
    return out;
}
QImage heightField(const QImage &src, const QString &source, const Context &ctx, double blur) {
    QImage hgt(src.size(), QImage::Format_RGBA8888);
    if (source.startsWith("Alpha")) {
        const auto d = alphaDistance(src, 0);
        double mx = 1;
        for (double v : d)
            if (v < 1e8)
                mx = std::max(mx, v);
        mx = std::min(mx, 8.0);
        for (int y = 0; y < src.height(); ++y)
            for (int x = 0; x < src.width(); ++x) {
                const int v = clamp255(std::min(d[size_t(y) * src.width() + x], mx) / mx * 255);
                setPx(hgt, x, y, qRgba(v, v, v, 255));
            }
    } else {
        const QImage &from = source.startsWith("Height") && ctx.heightmap &&
                                     ctx.heightmap->size() == src.size()
                                 ? *ctx.heightmap
                                 : src;
        for (int y = 0; y < src.height(); ++y)
            for (int x = 0; x < src.width(); ++x) {
                const QRgb c = px(from, x, y);
                const int v = luma(c) * qAlpha(c) / 255;
                setPx(hgt, x, y, qRgba(v, v, v, 255));
            }
    }
    return blur > 0.01 ? gaussian(hgt, blur, true, ctx) : hgt;
}
QImage normalMap(const QImage &src, const Params &p, const Context &ctx, QString *error) {
    if (p.s("source").startsWith("Height") && (!ctx.heightmap || ctx.heightmap->size() != src.size())) {
        if (error)
            *error = "Choose a heightmap layer with the document's dimensions";
        return {};
    }
    const auto hgt = heightField(src, p.s("source"), ctx, p.d("blur"));
    if (hgt.isNull())
        return {};
    const double s = p.d("strength");
    const bool wrap = p.b("wrap"), invertY = p.b("invert_y");
    return mapPixels(src, ctx, [&](QRgb c, int x, int y) {
        auto H = [&](int i, int j) { return qRed(sample(hgt, x + i, y + j, wrap)) / 255.0; };
        const double gx = (H(1, -1) + 2 * H(1, 0) + H(1, 1) - H(-1, -1) - 2 * H(-1, 0) - H(-1, 1)) / 4;
        const double gy = (H(-1, 1) + 2 * H(0, 1) + H(1, 1) - H(-1, -1) - 2 * H(0, -1) - H(1, -1)) / 4;
        double nx = -gx * s, ny = (invertY ? gy : -gy) * s, nz = 1;
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        nx /= len, ny /= len, nz /= len;
        return qRgba(clamp255((nx * .5 + .5) * 255), clamp255((ny * .5 + .5) * 255),
                     clamp255((nz * .5 + .5) * 255), qAlpha(c) ? 255 : 0);
    });
}
QImage heightmap(const QImage &src, const Params &p, const Context &ctx, QString *) {
    const auto hgt = heightField(src, p.s("source"), ctx, p.d("blur"));
    if (hgt.isNull())
        return {};
    const double c = p.i("contrast") * 2.55, f = (259 * (c + 255)) / (255 * (259 - c));
    const bool inv = p.b("invert");
    return mapPixels(src, ctx, [&](QRgb s, int x, int y) {
        int v = clamp255(f * (qRed(px(hgt, x, y)) - 128) + 128);
        if (inv)
            v = 255 - v;
        return qRgba(v, v, v, qAlpha(s) ? 255 : 0);
    });
}
QImage seamless(const QImage &src, const Params &p, const Context &ctx, QString *) {
    // Heuristic: near the edges, cross-fade toward a half-offset copy whose
    // wrapped neighbours are interior neighbours, making opposite edges agree.
    const int w = src.width(), h = src.height();
    const auto shifted = wrapOffset(src, w / 2, h / 2);
    const double band = std::max(1, std::min({p.i("blend"), w / 2, h / 2}));
    return mapPixels(src, ctx, [&](QRgb c, int x, int y) {
        const double ex = std::min(x + .5, w - x - .5), ey = std::min(y + .5, h - y - .5);
        const double k = std::clamp(1 - std::min(ex, ey) / band, 0.0, 1.0);
        const QRgb s = px(shifted, x, y);
        auto mix = [&](int a, int b) { return clamp255(a + (b - a) * k); };
        return qRgba(mix(qRed(c), qRed(s)), mix(qGreen(c), qGreen(s)), mix(qBlue(c), qBlue(s)),
                     mix(qAlpha(c), qAlpha(s)));
    });
}
} // namespace nova::ops::detail
