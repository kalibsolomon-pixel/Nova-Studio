// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QRgb>
#include <QString>
#include <QStringList>
#include <vector>

namespace nova {
struct Palette {
    QString name;
    std::vector<QRgb> colors;
    bool builtIn = false;
    static constexpr int MaxColors = 1024;
};
// Supported, documented formats only: GIMP .gpl, JASC-PAL .pal, Paint.NET .txt
// (AARRGGBB lines) and plain .hex (RRGGBB lines, e.g. Lospec exports).
enum class PaletteFormat { Gpl, JascPal, PaintNet, Hex };
QString paletteFileFilter();
bool paletteFormatFor(const QString &path, PaletteFormat *format);
QByteArray encodePalette(const Palette &palette, PaletteFormat format);
bool decodePalette(const QByteArray &bytes, PaletteFormat format, Palette *out, QString *error = nullptr);
bool importPalette(const QString &path, Palette *out, QString *error = nullptr);
bool exportPalette(const QString &path, const Palette &palette, QString *error = nullptr);
enum class PaletteSort { Hue, Luminance, Saturation };
void sortPalette(Palette &palette, PaletteSort mode);
int removeDuplicateColors(Palette &palette);
// Independently chosen colour values inspired by common block-texture ramps;
// no game artwork is sampled or bundled.
const std::vector<Palette> &builtInPalettes();
// Versioned JSON persistence: {"schema":1,"palettes":[{"name":..,"colors":["#AARRGGBB",..]}]}.
class PaletteStore {
    QString file;

  public:
    std::vector<Palette> palettes; // user palettes only
    explicit PaletteStore(QString path = {}) : file(std::move(path)) {}
    QString path() const { return file; }
    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr) const;
    std::vector<Palette> all() const; // built-ins followed by user palettes
    Palette *find(const QString &name);
    QString uniqueName(const QString &base) const;
};
} // namespace nova
