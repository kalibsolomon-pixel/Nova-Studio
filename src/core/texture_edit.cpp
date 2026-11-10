// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_edit.h"
#include "imageops.h"
#include <QJsonArray>
#include <algorithm>
#include <vector>

namespace nova::texture {
namespace {
Failure fail(const QString &pointer, const QString &rule) {
    return {pointer, QString("Invalid argument at %1: %2").arg(pointer, rule)};
}
int hex(QChar c) {
    const auto u = c.unicode();
    return u >= '0' && u <= '9' ? u - '0' : u >= 'a' && u <= 'f' ? u - 'a' + 10 : u >= 'A' && u <= 'F' ? u - 'A' + 10 : -1;
}
int indexOf(QChar c) {
    const auto u = c.unicode();
    return u >= '0' && u <= '9' ? u - '0' : u >= 'a' && u <= 'z' ? u - 'a' + 10 : u >= 'A' && u <= 'Z' ? u - 'A' + 36 : -1;
}
// Pixels written with zero alpha are stored as #00000000, as Nova's eraser and Clear do.
QRgb stored(QRgb c) { return qAlpha(c) ? c : 0; }
int distance(int a, int b) { return a > b ? a - b : b - a; }

// Integer reads with the same bounds as the published schema; the schema check runs first in MCP,
// so these mainly guard other callers.
bool integer(const QJsonObject &args, const char *key, int minimum, int maximum, int *value, Failure *failure,
             const QString &prefix = {}, int fallback = -1) {
    const auto pointer = prefix + '/' + key;
    if (!args.contains(key)) {
        if (fallback >= 0) {
            *value = fallback;
            return true;
        }
        *failure = {pointer, QString("Missing required argument %1.").arg(pointer)};
        return false;
    }
    const auto v = args[key];
    if (!v.isDouble() || v.toDouble() != double(qint64(v.toDouble())) || v.toDouble() < minimum ||
        v.toDouble() > maximum) {
        *failure = fail(pointer, QString("expected an integer from %1 to %2.").arg(minimum).arg(maximum));
        return false;
    }
    *value = v.toInt();
    return true;
}
bool choice(const QJsonObject &args, const char *key, const QStringList &values, QString *value, Failure *failure) {
    *value = args.contains(key) ? args[key].toString() : values.first();
    if (values.contains(*value) && (!args.contains(key) || args[key].isString()))
        return true;
    *failure = fail(QString("/") + key, "expected one of \"" + values.join("\", \"") + "\".");
    return false;
}
// A rectangle that must lie inside the canvas: the overflow check is done in 64 bits.
bool rectangle(const QJsonObject &args, QSize canvas, const QString &prefix, QRect *rect, Failure *failure) {
    int x = 0, y = 0, width = 0, height = 0;
    if (!integer(args, "x", 0, MaxSide - 1, &x, failure, prefix) || !integer(args, "y", 0, MaxSide - 1, &y, failure, prefix) ||
        !integer(args, "width", 1, MaxSide, &width, failure, prefix) ||
        !integer(args, "height", 1, MaxSide, &height, failure, prefix))
        return false;
    if (qint64(x) + width > canvas.width()) {
        *failure = fail(prefix + "/width", QString("the region extends past the canvas width (%1 pixels); x + width "
                                                   "must be at most %1.").arg(canvas.width()));
        return false;
    }
    if (qint64(y) + height > canvas.height()) {
        *failure = fail(prefix + "/height", QString("the region extends past the canvas height (%1 pixels); y + height "
                                                    "must be at most %1.").arg(canvas.height()));
        return false;
    }
    *rect = QRect(x, y, width, height);
    return true;
}
bool optionalRegion(const QJsonObject &args, QSize canvas, QRect *rect, Failure *failure) {
    if (!args.contains("region")) {
        *rect = QRect(QPoint(), canvas);
        return true;
    }
    if (!args["region"].isObject()) {
        *failure = fail("/region", "expected an object.");
        return false;
    }
    return rectangle(args["region"].toObject(), canvas, "/region", rect, failure);
}
bool color(const QJsonValue &value, const QString &pointer, QRgb *rgba, bool *withAlpha, Failure *failure) {
    if (value.isString() && parseColor(value.toString(), rgba, withAlpha))
        return true;
    *failure = fail(pointer, "expected a colour #RRGGBB or #RRGGBBAA.");
    return false;
}
bool list(const QJsonObject &args, const char *key, int minimum, int maximum, QJsonArray *items, Failure *failure) {
    const auto pointer = QString("/") + key;
    if (!args.contains(key)) {
        *failure = {pointer, QString("Missing required argument %1.").arg(pointer)};
        return false;
    }
    *items = args[key].toArray();
    if (args[key].isArray() && items->size() >= minimum && items->size() <= maximum)
        return true;
    *failure = fail(pointer, QString("expected %1 to %2 items.").arg(minimum).arg(maximum));
    return false;
}

struct PutPixels {
    QRect rect;
    std::vector<QRgb> palette;
    std::vector<qint8> indices; // row-major over rect; -1 keeps the pixel
    bool over = false;
};
Failure decode(const QJsonObject &args, QSize canvas, PutPixels *out) {
    Failure failure;
    QJsonArray palette, rows;
    QString mode;
    if (!rectangle(args, canvas, {}, &out->rect, &failure) || !list(args, "palette", 1, MaxPalette, &palette, &failure) ||
        !list(args, "rows", 1, MaxSide, &rows, &failure) || !choice(args, "mode", {"replace", "over"}, &mode, &failure))
        return failure;
    out->over = mode == "over";
    for (qsizetype i = 0; i < palette.size(); ++i) {
        QRgb rgba = 0;
        if (!color(palette[i], QString("/palette/%1").arg(i), &rgba, nullptr, &failure))
            return failure;
        out->palette.push_back(rgba);
    }
    const int width = out->rect.width(), height = out->rect.height();
    if (rows.size() != height)
        return fail("/rows", QString("expected %1 rows (one per pixel row of the region), got %2.").arg(height).arg(rows.size()));
    out->indices.reserve(size_t(qint64(width) * height));
    for (int r = 0; r < height; ++r) {
        const auto pointer = QString("/rows/%1").arg(r);
        if (!rows[r].isString())
            return fail(pointer, "expected a string of palette index characters.");
        const auto row = rows[r].toString();
        if (row.size() != width)
            return fail(pointer, QString("expected %1 characters (the region width), got %2.").arg(width).arg(row.size()));
        for (int column = 0; column < width; ++column) {
            if (row[column] == QChar(Keep)) {
                out->indices.push_back(-1);
                continue;
            }
            const int index = indexOf(row[column]);
            if (index < 0)
                return fail(pointer, QString("column %1 is not a palette index; use 0-9, a-z, A-Z or '.'.").arg(column));
            if (index >= int(out->palette.size()))
                return fail(pointer, QString("column %1 uses palette index %2, but the palette has %3 entries.")
                                         .arg(column).arg(index).arg(out->palette.size()));
            out->indices.push_back(qint8(index));
        }
    }
    return {};
}

struct Mapping {
    QRgb from = 0, to = 0;
    bool fromAlpha = false, toAlpha = false;
};
struct Remap {
    QRect rect;
    std::vector<Mapping> mappings;
    int tolerance = 0;
};
Failure decode(const QJsonObject &args, QSize canvas, Remap *out) {
    Failure failure;
    QJsonArray mappings;
    if (!list(args, "mappings", 1, MaxMappings, &mappings, &failure) ||
        !integer(args, "tolerance", 0, 255, &out->tolerance, &failure, {}, 0) ||
        !optionalRegion(args, canvas, &out->rect, &failure))
        return failure;
    for (qsizetype i = 0; i < mappings.size(); ++i) {
        const auto pointer = QString("/mappings/%1").arg(i);
        const auto entry = mappings[i].toObject();
        Mapping mapping;
        if (!mappings[i].isObject())
            return fail(pointer, "expected an object with from and to.");
        if (!color(entry["from"], pointer + "/from", &mapping.from, &mapping.fromAlpha, &failure) ||
            !color(entry["to"], pointer + "/to", &mapping.to, &mapping.toAlpha, &failure))
            return failure;
        if (mapping.fromAlpha && !qAlpha(mapping.from) && !mapping.toAlpha)
            return fail(pointer + "/to", "a mapping from a transparent colour needs an #RRGGBBAA target, because "
                                         "#RRGGBB keeps the pixel's alpha.");
        for (size_t j = 0; j < out->mappings.size(); ++j)
            if (out->mappings[j].fromAlpha == mapping.fromAlpha && out->mappings[j].from == mapping.from)
                return fail(pointer + "/from", QString("the same colour is already mapped by /mappings/%1/from.").arg(j));
        out->mappings.push_back(mapping);
    }
    return {};
}

struct Stop {
    int position = 0;
    QRgb color = 0;
};
struct Gradient {
    QRect rect;
    std::vector<Stop> stops;
    bool step = false, multiply = false;
};
Failure decode(const QJsonObject &args, QSize canvas, Gradient *out) {
    Failure failure;
    QJsonArray stops;
    QString interpolation, alpha, source;
    if (!list(args, "stops", 2, MaxStops, &stops, &failure) ||
        !choice(args, "interpolation", {"linear", "step"}, &interpolation, &failure) ||
        !choice(args, "alpha", {"preserve", "multiply"}, &alpha, &failure) ||
        !choice(args, "source", {"luma709"}, &source, &failure) || !optionalRegion(args, canvas, &out->rect, &failure))
        return failure;
    out->step = interpolation == "step";
    out->multiply = alpha == "multiply";
    for (qsizetype i = 0; i < stops.size(); ++i) {
        const auto pointer = QString("/stops/%1").arg(i);
        if (!stops[i].isObject())
            return fail(pointer, "expected an object with position and color.");
        const auto entry = stops[i].toObject();
        Stop stop;
        bool withAlpha = false;
        if (!integer(entry, "position", 0, 255, &stop.position, &failure, pointer) ||
            !color(entry["color"], pointer + "/color", &stop.color, &withAlpha, &failure))
            return failure;
        if (withAlpha && !out->multiply)
            return fail(pointer + "/color", "#RRGGBBAA stop colours need alpha \"multiply\"; with \"preserve\" use "
                                            "#RRGGBB.");
        if (!out->stops.empty() && stop.position <= out->stops.back().position)
            return fail(pointer + "/position", "stop positions must increase strictly.");
        out->stops.push_back(stop);
    }
    return {};
}

struct Erase {
    QRect rect;
    int strength = 255;
};
Failure decode(const QJsonObject &args, QSize canvas, Erase *out) {
    Failure failure;
    if (!args.contains("region"))
        return {"/region", "Missing required argument /region."};
    if (!optionalRegion(args, canvas, &out->rect, &failure) ||
        !integer(args, "strength", 1, 255, &out->strength, &failure, {}, 255))
        return failure;
    return {};
}

// Selection coverage mixes the full-strength result with the original per straight RGBA channel,
// exactly as Nova's Adjust operations do (ops::applyMask). A written pixel left with zero alpha is
// stored as #00000000; pixels that were not written keep their bytes.
QImage mixed(const QImage &original, const QImage &processed, const QImage &selection, QRect rect) {
    QImage out = ops::applyMask(original, processed, &selection);
    if (out.constBits() == processed.constBits())
        out = processed.copy();
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const auto before = reinterpret_cast<const quint32 *>(original.constScanLine(y));
        const auto after = reinterpret_cast<const quint32 *>(processed.constScanLine(y));
        auto result = reinterpret_cast<quint32 *>(out.scanLine(y));
        for (int x = rect.left(); x <= rect.right(); ++x)
            if (before[x] != after[x] && !(reinterpret_cast<const uchar *>(result + x)[3]))
                result[x] = 0;
    }
    return out;
}
template <typename Map>
QImage mapRegion(const QImage &source, QRect rect, Map map) {
    QImage out = source.copy();
    for (int y = rect.top(); y <= rect.bottom(); ++y)
        for (int x = rect.left(); x <= rect.right(); ++x)
            out.setPixel(x, y, map(source.pixel(x, y), x, y));
    return out;
}
} // namespace

bool parseColor(const QString &text, QRgb *color, bool *withAlpha) {
    if ((text.size() != 7 && text.size() != 9) || text[0] != '#')
        return false;
    int channels[4] = {0, 0, 0, 255};
    for (int i = 0; i < (text.size() - 1) / 2; ++i) {
        const int high = hex(text[1 + 2 * i]), low = hex(text[2 + 2 * i]);
        if (high < 0 || low < 0)
            return false;
        channels[i] = high * 16 + low;
    }
    if (color)
        *color = qRgba(channels[0], channels[1], channels[2], channels[3]);
    if (withAlpha)
        *withAlpha = text.size() == 9;
    return true;
}
bool compactCommand(const QString &command) {
    return command == "put_pixels" || command == "palette_remap" || command == "gradient_map" || command == "erase";
}
QRect region(const QString &command, const QJsonObject &args, QSize canvas) {
    QRect rect;
    Failure ignored;
    if (command == "put_pixels" ? rectangle(args, canvas, {}, &rect, &ignored) : optionalRegion(args, canvas, &rect, &ignored))
        return rect;
    return {};
}
Failure validate(const QString &command, const QJsonObject &args, QSize canvas) {
    if (command == "put_pixels") {
        PutPixels edit;
        return decode(args, canvas, &edit);
    }
    if (command == "palette_remap") {
        Remap edit;
        return decode(args, canvas, &edit);
    }
    if (command == "gradient_map") {
        Gradient edit;
        return decode(args, canvas, &edit);
    }
    if (command == "erase") {
        Erase edit;
        return decode(args, canvas, &edit);
    }
    return {"/command", "Unknown compact texture command."};
}
Failure apply(const QString &command, const QJsonObject &args, const QImage &layer, const QImage &selection,
              QImage *out) {
    const QImage source = layer.convertToFormat(QImage::Format_RGBA8888);
    if (!selection.isNull() && selection.size() != source.size())
        return {"/", "The selection does not match the layer."};
    if (command == "put_pixels") {
        PutPixels edit;
        if (const auto failure = decode(args, source.size(), &edit); !failure.ok())
            return failure;
        const int width = edit.rect.width();
        const auto processed = mapRegion(source, edit.rect, [&](QRgb before, int x, int y) {
            const int index = edit.indices[size_t(qint64(y - edit.rect.top()) * width + (x - edit.rect.left()))];
            if (index < 0)
                return before;
            const QRgb color = edit.palette[size_t(index)];
            return edit.over ? ops::blend(before, color, 255) : stored(color);
        });
        *out = mixed(source, processed, selection, edit.rect);
        return {};
    }
    if (command == "palette_remap") {
        Remap edit;
        if (const auto failure = decode(args, source.size(), &edit); !failure.ok())
            return failure;
        const int t = edit.tolerance;
        // Every pixel is compared with its original colour, so A→B and B→A swap; the first
        // matching mapping wins.
        const auto processed = mapRegion(source, edit.rect, [&](QRgb c, int, int) {
            const QRgb n = stored(c); // fully transparent pixels compare as #00000000
            for (const auto &m : edit.mappings) {
                if (!m.fromAlpha && !qAlpha(c))
                    continue;
                if (std::max({distance(qRed(n), qRed(m.from)), distance(qGreen(n), qGreen(m.from)),
                              distance(qBlue(n), qBlue(m.from)),
                              m.fromAlpha ? distance(qAlpha(n), qAlpha(m.from)) : 0}) > t)
                    continue;
                return stored(m.toAlpha ? m.to : qRgba(qRed(m.to), qGreen(m.to), qBlue(m.to), qAlpha(c)));
            }
            return c;
        });
        *out = mixed(source, processed, selection, edit.rect);
        return {};
    }
    if (command == "gradient_map") {
        Gradient edit;
        if (const auto failure = decode(args, source.size(), &edit); !failure.ok())
            return failure;
        // Lookup table over the 256 luma values: rounded integer interpolation per channel.
        QRgb table[256];
        const auto &stops = edit.stops;
        for (int v = 0; v < 256; ++v) {
            size_t i = 0;
            while (i + 1 < stops.size() && stops[i + 1].position <= v)
                ++i;
            if (v <= stops.front().position || i + 1 == stops.size() || edit.step) {
                table[v] = v <= stops.front().position ? stops.front().color : stops[i].color;
                continue;
            }
            const auto &a = stops[i], &b = stops[i + 1];
            const int d = b.position - a.position, w0 = b.position - v, w1 = v - a.position;
            auto channel = [&](int c0, int c1) { return (2 * (c0 * w0 + c1 * w1) + d) / (2 * d); };
            table[v] = qRgba(channel(qRed(a.color), qRed(b.color)), channel(qGreen(a.color), qGreen(b.color)),
                             channel(qBlue(a.color), qBlue(b.color)), channel(qAlpha(a.color), qAlpha(b.color)));
        }
        const auto processed = mapRegion(source, edit.rect, [&](QRgb c, int, int) {
            if (!qAlpha(c))
                return c; // fully transparent pixels are never made visible
            const QRgb g = table[ops::luma(c)];
            const int alpha = edit.multiply ? (qAlpha(c) * qAlpha(g) + 127) / 255 : qAlpha(c);
            return stored(qRgba(qRed(g), qGreen(g), qBlue(g), alpha));
        });
        *out = mixed(source, processed, selection, edit.rect);
        return {};
    }
    if (command == "erase") {
        Erase edit;
        if (const auto failure = decode(args, source.size(), &edit); !failure.ok())
            return failure;
        // Nova's eraser: alpha × (255 − coverage) / 255, where coverage = selection × strength.
        *out = ops::paintCoverage(source, ops::rectMask(source.size(), edit.rect), 0, edit.strength, true, &selection);
        return {};
    }
    return {"/command", "Unknown compact texture command."};
}
} // namespace nova::texture
