// SPDX-License-Identifier: GPL-3.0-or-later
// Directional shading and procedural texture generators. Everything here is
// deterministic: identical parameters (including seed) yield identical pixels.
// Generators draw original procedural artwork; no game assets are embedded.
#include "ops_internal.h"
#include <vector>

namespace nova::ops::detail {
namespace {
// Pixel-art colour ramp: darker shades drift toward blue/purple and lose a
// little saturation; lighter shades drift toward yellow. t ∈ [-1, 1].
QRgb shadeOf(QRgb base, double t, double hueShift = 12) {
    auto h = toHsl(base);
    h.l = std::clamp(h.l + t * (t > 0 ? (1 - h.l) * .55 : h.l * .6), 0.0, 1.0);
    // Shift toward 240° (cool) when darker and 60° (warm) when lighter.
    const double target = t < 0 ? 240.0 : 60.0;
    double d = std::fmod(target - h.h + 540.0, 360.0) - 180.0;
    h.h += std::clamp(d, -hueShift, hueShift) * std::abs(t);
    h.s = std::clamp(h.s * (1 - .15 * std::abs(t)), 0.0, 1.0);
    return fromHsl(h, qAlpha(base));
}
double quantizeLevel(double v, int steps) {
    if (steps < 2)
        return v;
    return std::round(std::clamp(v, 0.0, 1.0) * (steps - 1)) / (steps - 1);
}
QRgb rampColor(QRgb base, double v01, int steps, double contrast) {
    return shadeOf(base, (quantizeLevel(v01, steps) * 2 - 1) * contrast);
}
QRgb mixRgb(QRgb a, QRgb b, double t) {
    auto m = [&](int x, int y) { return clamp255(x + (y - x) * t); };
    return qRgba(m(qRed(a), qRed(b)), m(qGreen(a), qGreen(b)), m(qBlue(a), qBlue(b)),
                 m(qAlpha(a), qAlpha(b)));
}
QSize genSize(const Params &p) { return {p.i("width"), p.i("height")}; }
template <class F> QImage generate(QSize size, const Context &ctx, F f) {
    QImage out(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        if (ctx.canceled())
            return {};
        for (int x = 0; x < size.width(); ++x)
            setPx(out, x, y, f(x, y));
    }
    return out;
}
double stoneValue(int x, int y, QSize s, quint64 seed, int cells, double rough) {
    const double u = (x + .5) / s.width(), v = (y + .5) / s.height();
    return fbm(u, v, cells, 3, rough, seed);
}
// Simple bevel for rectangular cells: +1 on top/left edge pixels, -1 bottom/right.
int bevelAt(int lx, int ly, int w, int h) {
    if (lx == 0 || ly == 0)
        return 1;
    if (lx == w - 1 || ly == h - 1)
        return -1;
    return 0;
}
QImage outlineAround(QImage im, QRgb color) {
    const QImage src = im.copy();
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            if (qAlpha(px(src, x, y)))
                continue;
            bool touch = false;
            for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                const int xx = x + dx, yy = y + dy;
                touch |= xx >= 0 && yy >= 0 && xx < im.width() && yy < im.height() &&
                         qAlpha(px(src, xx, yy)) > 0;
            }
            if (touch)
                setPx(im, x, y, color);
        }
    return im;
}
} // namespace

QImage directionalShading(const QImage &src, const Params &p, const Context &ctx, QString *error) {
    const auto source = p.s("height_source");
    const bool useNormals = source.startsWith("Normal");
    if ((useNormals || source.startsWith("Height")) &&
        (!ctx.heightmap || ctx.heightmap->size() != src.size())) {
        if (error)
            *error = "Choose a heightmap or normal-map layer with the document's dimensions";
        return {};
    }
    const int w = src.width(), h = src.height();
    // Height field (estimate): luminance, alpha-distance bevel or a supplied map.
    std::vector<double> height(size_t(w) * h, 0);
    if (source.startsWith("Alpha")) {
        const int bevel = std::max(1, p.i("bevel"));
        // Distance (in pixels) to the nearest transparent pixel, capped at bevel width.
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!qAlpha(px(src, x, y)))
                    continue;
                int d = bevel;
                for (int r = 1; r < bevel && d == bevel; ++r)
                    for (int j = -r; j <= r && d == bevel; ++j)
                        for (int i = -r; i <= r; ++i) {
                            const int xx = x + i, yy = y + j;
                            if (xx < 0 || yy < 0 || xx >= w || yy >= h || !qAlpha(px(src, xx, yy))) {
                                d = r;
                                break;
                            }
                        }
                if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
                    d = std::min(d, 1);
                height[size_t(y) * w + x] = double(d) / bevel;
            }
    } else if (!useNormals) {
        const auto field = heightField(src, source, ctx, p.i("bevel") > 0 ? p.i("bevel") * .35 : 0);
        if (field.isNull())
            return {};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                height[size_t(y) * w + x] = qRed(px(field, x, y)) / 255.0;
    }
    const double az = p.i("light_direction") * Pi / 180, el = p.i("elevation") * Pi / 180;
    const double lx = std::sin(az) * std::cos(el), ly = -std::cos(az) * std::cos(el),
                 lz = std::sin(el);
    const double strength = p.d("height_strength"), intensity = p.i("intensity") / 100.0,
                 ambient = p.i("ambient") / 100.0, shadow = p.i("shadow") / 100.0,
                 highlight = p.i("highlight") / 100.0, edgeLight = p.i("edge_highlight") / 100.0,
                 edgeDark = p.i("edge_darkening") / 100.0, noiseAmount = p.i("noise") / 100.0,
                 temperature = p.i("temperature") / 100.0;
    const int steps = p.i("steps");
    const bool dither = p.b("dither");
    const quint64 seed = quint64(p.i("seed"));
    QRgb shadowTint = p.c("shadow_tint"), highlightTint = p.c("highlight_tint");
    if (temperature > 0) {
        highlightTint = mixRgb(highlightTint, qRgb(255, 196, 110), temperature * .5);
        shadowTint = mixRgb(shadowTint, qRgb(40, 60, 150), temperature * .5);
    } else if (temperature < 0) {
        highlightTint = mixRgb(highlightTint, qRgb(170, 210, 255), -temperature * .5);
        shadowTint = mixRgb(shadowTint, qRgb(120, 60, 40), -temperature * .5);
    }
    auto H = [&](int x, int y) {
        x = std::clamp(x, 0, w - 1), y = std::clamp(y, 0, h - 1);
        return height[size_t(y) * w + x];
    };
    auto opaque = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h && qAlpha(px(src, x, y)) > 0;
    };
    // Signed shade per pixel in [-1, 1].
    std::vector<double> shade(size_t(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        if (ctx.canceled())
            return {};
        for (int x = 0; x < w; ++x) {
            if (!opaque(x, y))
                continue;
            double nx, ny, nz;
            if (useNormals) {
                const QRgb n = px(*ctx.heightmap, x, y);
                nx = qRed(n) / 127.5 - 1, ny = qGreen(n) / 127.5 - 1, nz = qBlue(n) / 127.5 - 1;
                ny = -ny; // OpenGL-style green-up maps
            } else {
                const double gx = (H(x + 1, y - 1) + 2 * H(x + 1, y) + H(x + 1, y + 1) -
                                   H(x - 1, y - 1) - 2 * H(x - 1, y) - H(x - 1, y + 1)) / 4;
                const double gy = (H(x - 1, y + 1) + 2 * H(x, y + 1) + H(x + 1, y + 1) -
                                   H(x - 1, y - 1) - 2 * H(x, y - 1) - H(x + 1, y - 1)) / 4;
                nx = -gx * strength, ny = -gy * strength, nz = 1;
            }
            const double len = std::max(1e-9, std::sqrt(nx * nx + ny * ny + nz * nz));
            const double lambert = (nx * lx + ny * ly + nz * lz) / len;
            double s = lambert >= lz ? (lambert - lz) / std::max(1e-6, 1 - lz)
                                     : (lambert - lz) / std::max(1e-6, lz);
            // Edge light/shadow from the silhouette relative to the light.
            const int ex = lx < -.2 ? -1 : lx > .2 ? 1 : 0, ey = ly < -.2 ? -1 : ly > .2 ? 1 : 0;
            if ((ex || ey) && !opaque(x + ex, y + ey))
                s += edgeLight;
            if ((ex || ey) && !opaque(x - ex, y - ey))
                s -= edgeLight * .75;
            // Ambient edge darkening: share of lower/transparent neighbours.
            int occluded = 0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i)
                    if ((i || j) && (!opaque(x + i, y + j) || H(x + i, y + j) > H(x, y) + .15))
                        ++occluded;
            s -= edgeDark * occluded / 8.0;
            if (noiseAmount > 0)
                s += (hashUnit(seed, x, y) - .5) * noiseAmount;
            shade[size_t(y) * w + x] = std::clamp(s * intensity * (1 - ambient * .8), -1.0, 1.0);
        }
    }
    // Discrete brightness steps (pixel-art clusters instead of smooth gradients).
    std::vector<int> level(size_t(w) * h, 0);
    if (steps > 0) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double s = shade[size_t(y) * w + x];
                if (dither)
                    s += (bayer4(x, y) - 7.5) / 16.0 / steps;
                level[size_t(y) * w + x] = int(std::lround(std::clamp(s, -1.0, 1.0) * steps));
            }
        if (p.b("preserve_clusters")) {
            // Remove isolated single-pixel steps: adopt the most common 4-neighbour level.
            auto copy = level;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (!opaque(x, y))
                        continue;
                    const int v = copy[size_t(y) * w + x];
                    int counts[64] = {}, same = 0, best = v, bestCount = 0;
                    for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                        if (!opaque(x + dx, y + dy))
                            continue;
                        const int n = copy[size_t(y + dy) * w + x + dx];
                        same += n == v;
                        const int k = ++counts[std::clamp(n + 32, 0, 63)];
                        if (k > bestCount || (k == bestCount && n < best))
                            bestCount = k, best = n;
                    }
                    if (!same && bestCount >= 2)
                        level[size_t(y) * w + x] = best;
                }
        }
        for (size_t i = 0; i < shade.size(); ++i)
            shade[i] = double(level[i]) / steps;
    }
    std::vector<QRgb> palette;
    const auto paletteMode = p.s("palette_mode");
    if (paletteMode == "Image colours")
        palette = extractPalette(src, 64);
    else if (paletteMode == "Active palette")
        palette = ctx.palette;
    const bool layer = p.s("output") == "Shading layer";
    QImage out = layer ? blank(src.size()) : src.copy();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const QRgb c = px(src, x, y);
            const double s = shade[size_t(y) * w + x];
            if (!qAlpha(c) || std::abs(s) < 1e-6)
                continue;
            const QRgb tint = s > 0 ? highlightTint : shadowTint;
            const double a = std::clamp(std::abs(s) * (s > 0 ? highlight : shadow), 0.0, 1.0);
            QRgb lit = mixRgb(c, qRgba(qRed(tint), qGreen(tint), qBlue(tint), qAlpha(c)), a);
            if (!palette.empty())
                lit = nearest(lit, palette, Distance::Perceptual);
            if (!layer) {
                setPx(out, x, y, lit);
            } else if (palette.empty()) {
                setPx(out, x, y, qRgba(qRed(tint), qGreen(tint), qBlue(tint), clamp255(a * qAlpha(c))));
            } else if (lit != c) {
                setPx(out, x, y, qRgba(qRed(lit), qGreen(lit), qBlue(lit), qAlpha(c)));
            }
        }
    return out;
}
QImage genStone(const QImage &, const Params &p, const Context &ctx, QString *) {
    const auto size = genSize(p);
    const quint64 seed = quint64(p.i("seed"));
    const int cells = p.i("scale"), steps = p.i("steps");
    const double rough = p.i("roughness") / 100.0, cracks = p.i("cracks") / 100.0,
                 contrast = p.i("contrast") / 100.0;
    const QRgb base = p.c("base");
    return generate(size, ctx, [&](int x, int y) {
        double v = stoneValue(x, y, size, seed, cells, rough);
        v = std::clamp((v - .5) * 1.8 + .5, 0.0, 1.0);
        const double u = (x + .5) / size.width(), w = (y + .5) / size.height();
        const double c = periodicNoise(u, w, cells * 2, cells * 2, seed + 17);
        if (cracks > 0 && std::abs(c - .5) < cracks * .04)
            return shadeOf(base, -contrast);
        return rampColor(base, v, steps, contrast);
    });
}
QImage genBricks(const QImage &, const Params &p, const Context &ctx, QString *) {
    const auto size = genSize(p);
    const int bw = p.i("brick_width"), bh = p.i("brick_height"), m = p.i("mortar");
    const int pw = bw + m, ph = bh + m, shift = pw * p.i("row_offset") / 100;
    const QRgb base = p.c("base"), mortar = p.c("mortar_color");
    const double variation = p.i("variation") / 100.0, grain = p.i("noise") / 100.0;
    const bool bevel = p.b("bevel");
    const quint64 seed = quint64(p.i("seed"));
    const int columns = std::max(1, size.width() / pw);
    return generate(size, ctx, [&](int x, int y) {
        const int row = y / ph, ly = y % ph;
        const int sx = x + (row % 2 ? shift : 0);
        const int col = (sx / pw) % columns, lx = sx % pw;
        if (lx >= bw || ly >= bh)
            return shadeOf(mortar, (hashUnit(seed + 5, x, y) - .5) * grain * .5);
        double t = (hashUnit(seed, col, row) - .5) * variation * 1.2 +
                   (hashUnit(seed + 1, x, y) - .5) * grain * .5;
        if (bevel)
            t += bevelAt(lx, ly, bw, bh) * .25;
        return shadeOf(base, std::clamp(t, -1.0, 1.0));
    });
}
QImage genTiles(const QImage &, const Params &p, const Context &ctx, QString *) {
    const auto size = genSize(p);
    const int tile = p.i("tile_size"), g = p.i("grout"), period = tile + g;
    const QRgb base = p.c("base"), grout = p.c("grout_color");
    const double variation = p.i("variation") / 100.0;
    const bool bevel = p.b("bevel");
    const quint64 seed = quint64(p.i("seed"));
    return generate(size, ctx, [&](int x, int y) {
        const int lx = x % period, ly = y % period;
        if (lx >= tile || ly >= tile)
            return grout;
        double t = (hashUnit(seed, x / period, y / period) - .5) * variation +
                   (hashUnit(seed + 3, x, y) - .5) * variation * .25;
        if (bevel)
            t += bevelAt(lx, ly, tile, tile) * .3;
        return shadeOf(base, std::clamp(t, -1.0, 1.0));
    });
}
QImage genWood(const QImage &, const Params &p, const Context &ctx, QString *) {
    const auto size = genSize(p);
    const auto style = p.s("style");
    const QRgb base = p.c("base");
    const int plank = p.i("plank_height"), knots = p.i("knots");
    const double grain = p.i("grain") / 100.0;
    const quint64 seed = quint64(p.i("seed"));
    Random random(seed);
    struct Knot {
        double x, y, r;
    };
    std::vector<Knot> knotList;
    for (int i = 0; i < knots; ++i)
        knotList.push_back({random.unit() * size.width(), random.unit() * size.height(),
                            1 + random.unit() * std::max(1, size.width() / 12)});
    return generate(size, ctx, [&](int x, int y) {
        const double u = (x + .5) / size.width(), v = (y + .5) / size.height();
        if (style == "Log rings") {
            const double dx = x + .5 - size.width() / 2.0, dy = y + .5 - size.height() / 2.0;
            const double d = std::hypot(dx, dy) + periodicNoise(u, v, 4, 4, seed) * 2.5 * grain;
            const double ring = .5 + .5 * std::sin(d * 1.6);
            if (x == 0 || y == 0 || x == size.width() - 1 || y == size.height() - 1)
                return shadeOf(base, -.7);
            return rampColor(base, ring, 3, .45);
        }
        if (style == "Bark") {
            const double streak = periodicNoise(u, v, std::max(2, size.width() / 2), 2, seed);
            const double groove = periodicNoise(u, v, std::max(2, size.width() / 3), 3, seed + 9);
            if (groove < .32)
                return shadeOf(base, -.75);
            return rampColor(base, streak, 4, .5);
        }
        const int board = y / plank, ly = y % plank;
        if (ly == plank - 1)
            return shadeOf(base, -.6);
        const int joint = int(hashUnit(seed, board, 0) * size.width());
        if (x == joint)
            return shadeOf(base, -.45);
        double g = periodicNoise(u, v, 2, std::max(2, size.height() / 2), seed + board);
        g = .5 + (g - .5) * grain * 2;
        for (const auto &k : knotList) {
            double dx = std::abs(x + .5 - k.x), dy = std::abs(y + .5 - k.y);
            dx = std::min(dx, size.width() - dx), dy = std::min(dy, size.height() - dy);
            if (dx * dx / (k.r * k.r * 2.2) + dy * dy / (k.r * k.r * .5) < 1)
                return shadeOf(base, -.55);
        }
        return rampColor(base, g + (ly == 0 ? .2 : 0), 4, .4);
    });
}
QImage genOre(const QImage &blankSource, const Params &p, const Context &ctx, QString *e) {
    auto out = genStone(blankSource, p, ctx, e);
    if (out.isNull())
        return out;
    const auto size = out.size();
    const QRgb ore = p.c("ore_color");
    const quint64 seed = quint64(p.i("seed"));
    Random random(seed + 1000);
    const int clusters = std::max(1, int(size.width() * size.height() * p.i("density") / 100.0 / 40));
    const int clusterSize = p.i("cluster_size");
    for (int k = 0; k < clusters; ++k) {
        int x = random.range(0, size.width() - 1), y = random.range(0, size.height() - 1);
        for (int i = 0; i < clusterSize * 2; ++i) {
            const int wx = ((x % size.width()) + size.width()) % size.width(),
                      wy = ((y % size.height()) + size.height()) % size.height();
            setPx(out, wx, wy, shadeOf(ore, i == 0 ? .45 : (random.unit() - .5) * .5));
            // Bottom-right shadow pixel for a raised gem look.
            const int sx = (wx + 1) % size.width(), sy = (wy + 1) % size.height();
            if (random.unit() < .5)
                setPx(out, sx, sy, shadeOf(ore, -.55));
            x += random.range(-1, 1);
            y += random.range(-1, 1);
        }
    }
    return out;
}
QImage genMetal(const QImage &, const Params &p, const Context &ctx, QString *) {
    const auto size = genSize(p);
    const auto style = p.s("style");
    const QRgb base = p.c("base");
    const quint64 seed = quint64(p.i("seed"));
    const int rivet = std::max(2, p.i("rivet_spacing"));
    return generate(size, ctx, [&](int x, int y) {
        const double u = (x + .5) / size.width(), v = (y + .5) / size.height();
        const double streak = periodicNoise(u, v, 2, std::max(2, size.height()), seed);
        double t = (streak - .5) * .35 + (.5 - v) * .25;
        if (style == "Plate with rivets") {
            t += bevelAt(x, y, size.width(), size.height()) * .45;
            const int rx = (x - 2 + rivet) % rivet, ry = (y - 2 + rivet) % rivet;
            if ((x >= 2 && y >= 2 && x < size.width() - 2 && y < size.height() - 2) &&
                (rx == 0 || rx == 1) && (ry == 0 || ry == 1) &&
                (x < 4 || x >= size.width() - 4) && (y < 4 || y >= size.height() - 4))
                t = rx == 0 && ry == 0 ? .8 : -.4;
        } else if (style == "Diamond plate") {
            const int dx = x % 4, dy = y % 4;
            if ((dx + dy == 3) || (dx == dy && ((x / 4 + y / 4) % 2)))
                t += .45;
            else if (dx + dy == 4)
                t -= .35;
        }
        return shadeOf(base, std::clamp(t, -1.0, 1.0), 6);
    });
}
QImage genPanel(const QImage &, const Params &p, const Context &ctx, QString *) {
    const auto size = genSize(p);
    const int border = p.i("border"), radius = p.i("corner_radius");
    const QRgb base = p.c("base");
    const auto style = p.s("style");
    const bool outline = p.b("outline");
    const int o = outline ? 1 : 0;
    return generate(size, ctx, [&](int x, int y) -> QRgb {
        const int w = size.width(), h = size.height();
        // Rounded corners: cut pixels outside the radius.
        auto corner = [&](int cx, int cy) {
            return radius > 0 && std::abs(x - cx) + std::abs(y - cy) < radius;
        };
        if (corner(0, 0) || corner(w - 1, 0) || corner(0, h - 1) || corner(w - 1, h - 1))
            return qRgba(0, 0, 0, 0);
        if (outline && (x == 0 || y == 0 || x == w - 1 || y == h - 1))
            return qRgba(0, 0, 0, 255);
        const int lx = x - o, ly = y - o, iw = w - 2 * o, ih = h - 2 * o;
        const bool topLeft = lx < border || ly < border;
        const bool bottomRight = lx >= iw - border || ly >= ih - border;
        if (style == "Flat" || (!topLeft && !bottomRight))
            return base;
        const double sign = style == "Inset" ? -1 : 1;
        if (topLeft && bottomRight)
            return base;
        return shadeOf(base, (topLeft ? .7 : -.6) * sign, 4);
    });
}
QImage genItem(const QImage &, const Params &p, const Context &ctx, QString *) {
    const auto size = genSize(p);
    const auto shape = p.s("shape");
    const QRgb base = p.c("base");
    const double w = size.width(), h = size.height();
    ShapeSpec spec;
    spec.filled = true;
    QImage mask;
    if (shape == "Gem") {
        spec.kind = ShapeKind::Polygon;
        spec.points = {{w * .5, h * .12}, {w * .85, h * .38}, {w * .5, h * .9}, {w * .15, h * .38}};
    } else if (shape == "Ingot") {
        spec.kind = ShapeKind::Polygon;
        spec.points = {{w * .28, h * .34}, {w * .78, h * .34}, {w * .92, h * .7}, {w * .1, h * .7}};
    } else if (shape == "Orb") {
        spec.kind = ShapeKind::Ellipse;
        spec.bounds = QRectF(w * .16, h * .16, w * .68, h * .68);
    } else if (shape == "Rod") {
        spec.kind = ShapeKind::Line;
        spec.thickness = std::max(2, size.width() / 8);
        spec.points = {QPointF(w * .2, h * .8), QPointF(w * .8, h * .2)};
    } else if (shape == "Bottle") {
        auto body = ShapeSpec{};
        body.kind = ShapeKind::Ellipse;
        body.filled = true;
        body.bounds = QRectF(w * .2, h * .38, w * .6, h * .55);
        auto neck = ShapeSpec{};
        neck.kind = ShapeKind::Rectangle;
        neck.filled = true;
        neck.bounds = QRectF(w * .4, h * .12, w * .2, h * .32);
        mask = combine(rasterizeShape(size, body), rasterizeShape(size, neck), Combine::Add);
    } else { // Shard: a deterministic irregular star
        spec.kind = ShapeKind::Polygon;
        Random r(quint64(p.i("seed")));
        for (int i = 0; i < 7; ++i) {
            const double t = -Pi / 2 + 2 * Pi * i / 7, k = i % 2 ? .25 + r.unit() * .15 : .4 + r.unit() * .1;
            spec.points.append({w * .5 + std::cos(t) * w * k, h * .5 + std::sin(t) * h * k});
        }
    }
    if (mask.isNull())
        mask = rasterizeShape(size, spec);
    const auto bounds = maskBounds(mask);
    const bool shading = p.b("shading");
    QImage out = generate(size, ctx, [&](int x, int y) -> QRgb {
        if (!mask.constScanLine(y)[x])
            return qRgba(0, 0, 0, 0);
        if (!shading || bounds.isEmpty())
            return base;
        const double nx = (x + .5 - bounds.center().x()) / std::max(1, bounds.width()),
                     ny = (y + .5 - bounds.center().y()) / std::max(1, bounds.height());
        double t = -(nx + ny) * 1.4;
        if (shape == "Orb" && std::hypot(nx + .2, ny + .2) < .12)
            t = 1;
        return shadeOf(base, std::clamp(std::round(t * 2) / 2, -1.0, 1.0));
    });
    if (out.isNull())
        return out;
    return p.b("outline") ? outlineAround(out, shadeOf(base, -.95)) : out;
}
QImage genNoise(const QImage &, const Params &p, const Context &ctx, QString *) {
    const auto size = genSize(p);
    const auto type = p.s("type");
    const int cells = p.i("scale"), octaves = p.i("octaves"), steps = p.i("steps");
    const double rough = p.i("roughness") / 100.0;
    const QRgb a = p.c("color_a"), b = p.c("color_b");
    const quint64 seed = quint64(p.i("seed"));
    return generate(size, ctx, [&](int x, int y) {
        const double u = (x + .5) / size.width(), v = (y + .5) / size.height();
        double n;
        if (type == "Cellular") {
            // Worley distance with one feature point per periodic cell.
            const double fx = u * cells, fy = v * cells;
            const int cx = int(std::floor(fx)), cy = int(std::floor(fy));
            double best = 9;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    const int gx = cx + i, gy = cy + j;
                    const int wx = ((gx % cells) + cells) % cells, wy = ((gy % cells) + cells) % cells;
                    const double px0 = gx + hashUnit(seed, wx, wy), py0 = gy + hashUnit(seed + 1, wx, wy);
                    best = std::min(best, std::hypot(fx - px0, fy - py0));
                }
            n = std::clamp(best / 1.1, 0.0, 1.0);
        } else {
            n = fbm(u, v, cells, octaves, rough, seed);
            if (type == "Ridged")
                n = 1 - std::abs(n * 2 - 1);
        }
        n = std::clamp((n - .5) * 1.6 + .5, 0.0, 1.0);
        return mixRgb(a, b, steps >= 2 ? quantizeLevel(n, steps) : n);
    });
}
} // namespace nova::ops::detail
