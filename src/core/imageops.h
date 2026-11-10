// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Shared, deterministic image-operation engine. Every interactive tool, menu
// command, preview, batch pipeline and future macro/agent call reaches pixels
// through these functions, so results are identical regardless of entry point.
//
// Pixel convention: QImage::Format_RGBA8888, straight (non-premultiplied) alpha,
// encoded sRGB channels. Selection masks are Format_Grayscale8 images matching
// the target size: 0 = unselected, 255 = selected, values between are partial.
// A null mask means "the whole image".
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <functional>
#include <vector>

namespace nova::ops {
// ---------------------------------------------------------------- random
// SplitMix64: portable and bit-identical on every compiler (std distributions
// are implementation-defined and would break seed reproducibility).
struct Random {
    quint64 state;
    explicit Random(quint64 seed) : state(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    quint64 next();
    double unit();                 // [0, 1)
    int range(int low, int high);  // inclusive
};
quint64 hash3(quint64 seed, qint64 x, qint64 y);
double hashUnit(quint64 seed, qint64 x, qint64 y);
// Periodic value noise in [0,1]; cellsX/cellsY lattice cells span the image
// exactly once, so output tiles seamlessly when sampled over [0,w)×[0,h).
double periodicNoise(double u, double v, int cellsX, int cellsY, quint64 seed);
double fbm(double u, double v, int cells, int octaves, double roughness, quint64 seed);

// ---------------------------------------------------------------- colour
struct Hsv {
    double h = 0, s = 0, v = 0; // h in degrees [0,360), s/v in [0,1]
};
struct Hsl {
    double h = 0, s = 0, l = 0;
};
Hsv toHsv(QRgb c);
QRgb fromHsv(const Hsv &hsv, int alpha);
Hsl toHsl(QRgb c);
QRgb fromHsl(const Hsl &hsl, int alpha);
double srgbToLinear(double encoded);  // both in [0,1]
double linearToSrgb(double linear);
int luma(QRgb c);  // Rec.709 weights on encoded channels, 0..255
enum class Distance { Rgb, Rgba, Perceptual, Hue };
QStringList distanceNames();
Distance distanceFromName(const QString &name);
// Distance normalised to a 0..255 scale so one tolerance slider fits all metrics.
int colorDistance(QRgb a, QRgb b, Distance metric);
// Fully transparent pixels compare equal regardless of hidden RGB values.
bool colorMatches(QRgb candidate, QRgb reference, int tolerance, Distance metric,
                  bool includeAlpha);
QRgb blend(QRgb destination, QRgb source, int coverage); // straight-alpha source-over
QRgb nearest(QRgb c, const std::vector<QRgb> &palette, Distance metric = Distance::Perceptual);
QString colorName(QRgb c);           // "#AARRGGBB"
QRgb parseColor(const QString &text, bool *ok = nullptr);

// ---------------------------------------------------------------- masks
enum class Combine { Replace, Add, Subtract, Intersect };
QImage emptyMask(QSize size);
QImage fullMask(QSize size);
QImage combine(const QImage &current, const QImage &shape, Combine mode);
QImage invertMask(const QImage &mask);
QRect maskBounds(const QImage &mask);
bool maskEmpty(const QImage &mask);
inline int maskValue(const QImage *mask, int x, int y) {
    return mask && !mask->isNull() ? mask->constScanLine(y)[x] : 255;
}
QImage rectMask(QSize size, QRect rect);
QImage ellipseMask(QSize size, QRect rect);
QImage polygonMask(QSize size, const QVector<QPointF> &points);
// Contiguous (scanline, 4- or 8-connected) or global colour matching. The
// visited/stack storage is bounded by the image area; no recursion is used.
QImage colorMask(const QImage &source, QPoint seed, int tolerance, Distance metric,
                 bool contiguous, bool includeAlpha, bool diagonal = false);
// Universal Fill colour contract, on straight-alpha 8-bit sRGB (Format_RGBA8888):
// tolerance 0..255 (clamped); a candidate matches when max(|dR|,|dG|,|dB|) <= tolerance
// and alpha is exactly equal. Two fully transparent pixels match whatever their hidden
// RGB. Tolerance 0 therefore means exact RGBA equality for visible pixels. This is
// deliberately independent of colorMatches(), which Contiguous fill keeps using.
bool universalFillMatches(QRgb candidate, QRgb reference, int tolerance);
// Every pixel of source (any format; converted to Format_RGBA8888 first) matching the
// seed pixel under universalFillMatches(), connected or not. Empty mask if the seed is outside.
QImage universalFillMask(const QImage &source, QPoint seed, int tolerance);
// out = lerp(original, processed, mask) per pixel; null mask returns processed.
QImage applyMask(const QImage &original, const QImage &processed, const QImage *mask);
// Clip an overlay's alpha without darkening its straight RGB channels.
QImage maskAlpha(const QImage &overlay, const QImage &mask);
// Paints colour through a coverage mask (× optional selection × opacity).
// Full coverage replaces pixels exactly (pixel-art semantics); partial
// coverage uses straight-alpha source-over; erase reduces alpha.
QImage paintCoverage(const QImage &target, const QImage &coverage, QRgb color, int opacity,
                     bool erase, const QImage *selection);
// Pixel-boundary edges of a mask (for marching ants): each line is in pixel units.
QVector<QLineF> maskOutline(const QImage &mask);

// ---------------------------------------------------------------- shapes
enum class ShapeKind { Rectangle, RoundedRectangle, Ellipse, Polygon, RegularPolygon, Star, Line };
QStringList shapeNames();
struct ShapeSpec {
    ShapeKind kind = ShapeKind::Rectangle;
    QRectF bounds;         // pixel units; a 1×1 rectangle covers exactly one pixel
    double rotation = 0;   // degrees clockwise around bounds centre
    bool filled = false;
    bool smooth = false;  // opt-in area coverage; default keeps exact pixel centres
    int thickness = 1;     // outline thickness in pixels (also line width)
    double radius = 0;     // rounded-rectangle corner radius
    int sides = 5;         // regular polygon sides / star points
    double innerRatio = .5;
    bool flipH = false, flipV = false;
    QVector<QPointF> points; // free polygon vertices / line endpoints
};
// Grayscale8 coverage: pixel-centre sampled (0/255) by default, Qt raster area
// coverage when smooth is enabled. Smooth outlines use an interior vector stroke.
// Hard outlines are the
// filled region minus its 4-neighbour erosion, giving 8-connected 1px edges.
QImage rasterizeShape(QSize canvas, const ShapeSpec &spec);

// ---------------------------------------------------------------- brushes
enum class BrushShape { Square, Circle, Diamond, HorizontalLine, VerticalLine, Custom };
QStringList brushShapeNames();
struct Brush {
    BrushShape shape = BrushShape::Square;
    int size = 1;          // 1..256 pixels
    int height = 0;        // 0 = square/legacy stamp size; otherwise size × height
    int opacity = 255;     // 0..255
    int hardness = 100;    // 0..100, 100 = hard pixel edges (pixel-art default)
    int spacing = 0;       // percent of size between dabs; 0 = every pixel
    QImage stamp;          // Custom: alpha (or luminance) defines coverage
    bool sourceColors = false; // opt-in RGBA stamps; legacy uses foreground
    static constexpr int MaxSize = 256;
};
// Custom stamps retain their original size unless height is explicitly set.
QSize brushDimensions(const Brush &brush);
struct Dab {
    QPoint offset;
    int coverage; // 0..255
    QRgb color = 0; // source RGB, opaque; alpha is applied once through coverage
};
std::vector<Dab> footprint(const Brush &brush);
enum class Symmetry { None, Horizontal, Vertical, Both, Radial };
QStringList symmetryNames();
std::vector<QPoint> symmetricPoints(QPoint p, QSize canvas, Symmetry symmetry, int radialCount,
                                    QPointF centre);

// ---------------------------------------------------------------- operations
enum class ParamType { Int, Double, Bool, Color, Choice, Palette };
struct ParamInfo {
    QString key, label, tooltip;
    ParamType type = ParamType::Int;
    double minimum = 0, maximum = 100, value = 0, step = 1;
    QStringList choices;
    QString text; // default colour / choice / palette text
};
struct OperationInfo {
    QString id, name, category, description;
    std::vector<ParamInfo> params;
    bool changesSize = false;     // document-wide: applies to every layer
    bool generator = false;       // ignores source pixels, uses width/height
    bool transformsContent = false; // with a selection, lifts and moves content
    bool expensive = false;       // previewed in the background with cancellation
    int workingCopies = 3;        // RGBA8-equivalent buffers for the memory estimate
};
struct Spec {
    QString id;
    QJsonObject params;
    bool enabled = true;
    QJsonObject toJson() const;
    static Spec fromJson(const QJsonObject &json);
};
struct Context {
    const QImage *mask = nullptr;
    std::vector<QRgb> palette;
    QRgb foreground = qRgba(0, 0, 0, 255), background = qRgba(255, 255, 255, 255);
    const QImage *heightmap = nullptr; // user-supplied height or normal map layer
    const std::atomic_bool *cancel = nullptr;
    std::function<void(int)> progress; // 0..100
    qint64 memoryLimit = 512LL * 1024 * 1024;
    bool canceled() const { return cancel && cancel->load(); }
};
struct Result {
    QImage image;
    QString error;
    bool canceled = false;
    bool ok() const { return error.isEmpty() && !canceled && !image.isNull(); }
};
const std::vector<OperationInfo> &registry();
const OperationInfo *find(const QString &id);
QStringList categories();
QJsonObject defaults(const QString &id);
// Fills missing parameters with defaults, rejects unknown ids, wrong types and
// out-of-range values. The spec is normalised in place on success.
bool validate(Spec &spec, QString *error = nullptr);
QSize outputSize(const Spec &spec, QSize input);
qint64 estimateBytes(const Spec &spec, QSize input);
Result run(Spec spec, const QImage &source, const Context &context = {});
Result runPipeline(const std::vector<Spec> &steps, const QImage &source, const Context &context);

// Built-in parameter presets (material shading, adjustment looks).
struct Preset {
    QString name, operation;
    QJsonObject params;
};
const std::vector<Preset> &builtInPresets();

// ---------------------------------------------------------------- analysis
QImage tile(const QImage &image, int columns, int rows);
QImage wrapOffset(const QImage &image, int dx, int dy);
// Mean absolute RGBA difference across opposite edges (0 = perfectly tileable).
struct SeamReport {
    double horizontal = 0, vertical = 0;
    int worstColumn = 0, worstRow = 0;
};
SeamReport seamReport(const QImage &image);
std::vector<QRgb> extractPalette(const QImage &image, int maximum);
std::vector<QRgb> medianCut(const QImage &image, int colors);
QRect alphaBounds(const QImage &image, int threshold = 0);
} // namespace nova::ops
