// SPDX-License-Identifier: GPL-3.0-or-later
#include "palette.h"
#include "imageops.h"
#include "revision.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace nova {
namespace {
constexpr qint64 FileLimit = 1024 * 1024;
bool fail(QString *error, const QString &text) {
    if (error)
        *error = text;
    return false;
}
} // namespace
QString paletteFileFilter() {
    return "Palettes (*.gpl *.pal *.txt *.hex);;GIMP palette (*.gpl);;JASC-PAL (*.pal);;"
           "Paint.NET palette (*.txt);;Hex list (*.hex)";
}
bool paletteFormatFor(const QString &path, PaletteFormat *format) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "gpl")
        *format = PaletteFormat::Gpl;
    else if (suffix == "pal")
        *format = PaletteFormat::JascPal;
    else if (suffix == "txt")
        *format = PaletteFormat::PaintNet;
    else if (suffix == "hex")
        *format = PaletteFormat::Hex;
    else
        return false;
    return true;
}
QByteArray encodePalette(const Palette &p, PaletteFormat format) {
    QByteArray out;
    switch (format) {
    case PaletteFormat::Gpl:
        out += "GIMP Palette\nName: " + p.name.simplified().toUtf8() + "\nColumns: 8\n#\n";
        for (auto c : p.colors)
            out += QString("%1 %2 %3\t%4\n")
                       .arg(qRed(c), 3)
                       .arg(qGreen(c), 3)
                       .arg(qBlue(c), 3)
                       .arg(ops::colorName(c).mid(3))
                       .toUtf8();
        break;
    case PaletteFormat::JascPal:
        out += "JASC-PAL\r\n0100\r\n" + QByteArray::number(qsizetype(p.colors.size())) + "\r\n";
        for (auto c : p.colors)
            out += QString("%1 %2 %3\r\n").arg(qRed(c)).arg(qGreen(c)).arg(qBlue(c)).toUtf8();
        break;
    case PaletteFormat::PaintNet:
        out += ";paint.net Palette File\n;Palette Name: " + p.name.simplified().toUtf8() + "\n";
        for (auto c : p.colors)
            out += ops::colorName(c).mid(1).toUtf8() + "\n";
        break;
    case PaletteFormat::Hex:
        for (auto c : p.colors)
            out += ops::colorName(c).mid(3).toLower().toUtf8() + "\n";
        break;
    }
    return out;
}
bool decodePalette(const QByteArray &bytes, PaletteFormat format, Palette *out, QString *error) {
    if (bytes.size() > FileLimit)
        return fail(error, "Palette files are limited to 1 MiB");
    Palette p;
    const auto text = QString::fromUtf8(bytes);
    auto lines = text.split(QRegularExpression("\r?\n"));
    auto add = [&](QRgb c) {
        if (int(p.colors.size()) >= Palette::MaxColors)
            return false;
        p.colors.push_back(c);
        return true;
    };
    auto byte = [](const QString &s, bool *ok) {
        const int v = s.toInt(ok);
        *ok = *ok && v >= 0 && v <= 255;
        return v;
    };
    switch (format) {
    case PaletteFormat::Gpl: {
        if (lines.isEmpty() || !lines[0].trimmed().startsWith("GIMP Palette"))
            return fail(error, "Missing 'GIMP Palette' header");
        for (int i = 1; i < lines.size(); ++i) {
            const auto line = lines[i].trimmed();
            if (line.startsWith("Name:"))
                p.name = line.mid(5).trimmed();
            if (line.isEmpty() || line.startsWith('#') || line.contains(':'))
                continue;
            const auto parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            bool a = false, b = false, c = false;
            if (parts.size() < 3)
                return fail(error, QString("Invalid colour on line %1").arg(i + 1));
            const int r = byte(parts[0], &a), g = byte(parts[1], &b), bl = byte(parts[2], &c);
            if (!a || !b || !c)
                return fail(error, QString("Invalid colour on line %1").arg(i + 1));
            if (!add(qRgb(r, g, bl)))
                return fail(error, "Palette exceeds 1024 colours");
        }
        break;
    }
    case PaletteFormat::JascPal: {
        if (lines.size() < 3 || lines[0].trimmed() != "JASC-PAL" || lines[1].trimmed() != "0100")
            return fail(error, "Missing JASC-PAL 0100 header");
        bool ok = false;
        const int count = lines[2].trimmed().toInt(&ok);
        if (!ok || count < 0 || count > Palette::MaxColors)
            return fail(error, "Invalid JASC-PAL colour count");
        for (int i = 3; i < lines.size() && int(p.colors.size()) < count; ++i) {
            const auto parts = lines[i].split(' ', Qt::SkipEmptyParts);
            bool a = false, b = false, c = false;
            if (parts.size() < 3)
                return fail(error, QString("Invalid colour on line %1").arg(i + 1));
            const int r = byte(parts[0], &a), g = byte(parts[1], &b), bl = byte(parts[2], &c);
            if (!a || !b || !c)
                return fail(error, QString("Invalid colour on line %1").arg(i + 1));
            add(qRgb(r, g, bl));
        }
        if (int(p.colors.size()) != count)
            return fail(error, "JASC-PAL file has fewer colours than declared");
        break;
    }
    case PaletteFormat::PaintNet:
    case PaletteFormat::Hex: {
        const int digits = format == PaletteFormat::PaintNet ? 8 : 6;
        static const QRegularExpression hex("^[0-9A-Fa-f]+$");
        for (int i = 0; i < lines.size(); ++i) {
            auto line = lines[i].trimmed();
            if (line.startsWith('#') && format == PaletteFormat::Hex)
                line = line.mid(1);
            if (line.isEmpty() || line.startsWith(';'))
                continue;
            if (line.size() != digits || !hex.match(line).hasMatch())
                return fail(error, QString("Invalid colour on line %1").arg(i + 1));
            const uint v = line.toUInt(nullptr, 16);
            if (!add(digits == 8 ? QRgb(v) : QRgb(0xff000000u | v)))
                return fail(error, "Palette exceeds 1024 colours");
            if (format == PaletteFormat::PaintNet && line.startsWith(";Palette Name:"))
                p.name = line.mid(14).trimmed();
        }
        break;
    }
    }
    if (p.colors.empty())
        return fail(error, "The palette contains no colours");
    *out = std::move(p);
    return true;
}
bool importPalette(const QString &path, Palette *out, QString *error) {
    PaletteFormat format;
    if (!paletteFormatFor(path, &format))
        return fail(error, "Unsupported palette format (use .gpl, .pal, .txt or .hex)");
    QFile file(path);
    if (QFileInfo(path).size() > FileLimit || !file.open(QIODevice::ReadOnly))
        return fail(error, "Palette file is unreadable or larger than 1 MiB");
    if (!decodePalette(file.read(FileLimit + 1), format, out, error))
        return false;
    if (out->name.isEmpty())
        out->name = QFileInfo(path).completeBaseName();
    return true;
}
bool exportPalette(const QString &path, const Palette &palette, QString *error) {
    PaletteFormat format;
    if (!paletteFormatFor(path, &format))
        return fail(error, "Choose a .gpl, .pal, .txt or .hex file name");
    return atomicWrite(path, encodePalette(palette, format), Revision::read(path), error);
}
void sortPalette(Palette &p, PaletteSort mode) {
    auto key = [&](QRgb c) {
        const auto h = ops::toHsv(c);
        switch (mode) {
        case PaletteSort::Hue: return std::tuple(h.s < .08 ? 1 : 0, h.s < .08 ? 0.0 : h.h, double(ops::luma(c)));
        case PaletteSort::Luminance: return std::tuple(0, double(ops::luma(c)), h.h);
        case PaletteSort::Saturation: return std::tuple(0, h.s, double(ops::luma(c)));
        }
        return std::tuple(0, 0.0, 0.0);
    };
    std::stable_sort(p.colors.begin(), p.colors.end(), [&](QRgb a, QRgb b) { return key(a) < key(b); });
}
int removeDuplicateColors(Palette &p) {
    QSet<QRgb> seen;
    const auto before = p.colors.size();
    std::erase_if(p.colors, [&](QRgb c) {
        if (seen.contains(c))
            return true;
        seen.insert(c);
        return false;
    });
    return int(before - p.colors.size());
}
const std::vector<Palette> &builtInPalettes() {
    static const std::vector<Palette> list = [] {
        auto make = [](const char *name, std::initializer_list<uint> rgb) {
            Palette p;
            p.name = name;
            p.builtIn = true;
            for (auto v : rgb)
                p.colors.push_back(0xff000000u | v);
            return p;
        };
        return std::vector<Palette>{
            make("Nova · Stone", {0x2b2d31, 0x404349, 0x575a61, 0x6e7179, 0x868991, 0x9fa2a8, 0xb8bbc0, 0xd3d5d8}),
            make("Nova · Earth & Wood", {0x2e1f17, 0x4a3122, 0x6b4630, 0x8c5f3e, 0xa87b4f, 0xc49a66, 0xdcbb88, 0x5a4a3a,
                                         0x7a6650, 0x9c8668}),
            make("Nova · Foliage", {0x1c2e18, 0x2a4422, 0x3b5e2c, 0x4f7a36, 0x669640, 0x80b04c, 0x9fc95e, 0xc0de7a}),
            make("Nova · Ore Accents", {0x1f6f6a, 0x3fb8ac, 0x8de8dc, 0x7a2a1e, 0xc4503a, 0xf2a07a, 0x6a5a12, 0xd8b830,
                                        0xfbe68c, 0x2c3a8a, 0x5a72e0, 0xb0c0ff}),
            make("Nova · Nether", {0x240c0e, 0x3d1418, 0x5c1e22, 0x7f2c2c, 0xa84434, 0xd2683c, 0xf29a4a, 0x4a2a3a,
                                   0x6e3a4c}),
            make("Nova · End", {0x14101e, 0x241c34, 0x3a2c52, 0x584476, 0xd9d6a4, 0xece8bc, 0xf8f6dc, 0x8a6ab0}),
            make("Nova · GUI Greys", {0x000000, 0x373737, 0x555555, 0x8b8b8b, 0xc6c6c6, 0xdbdbdb, 0xffffff}),
        };
    }();
    return list;
}
bool PaletteStore::load(QString *error) {
    palettes.clear();
    QFile f(file);
    if (file.isEmpty() || !f.exists())
        return true;
    if (f.size() > 4 * FileLimit || !f.open(QIODevice::ReadOnly))
        return fail(error, "Palette library is unreadable");
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || doc["schema"].toInt() != 1)
        return fail(error, "Palette library has an unsupported schema; it was left untouched");
    for (const auto &value : doc["palettes"].toArray()) {
        Palette p;
        const auto object = value.toObject();
        p.name = object["name"].toString().left(128);
        for (const auto &c : object["colors"].toArray()) {
            bool ok = false;
            const auto rgb = ops::parseColor(c.toString(), &ok);
            if (ok && int(p.colors.size()) < Palette::MaxColors)
                p.colors.push_back(rgb);
        }
        if (!p.name.isEmpty())
            palettes.push_back(std::move(p));
    }
    return true;
}
bool PaletteStore::save(QString *error) const {
    if (file.isEmpty() || !QDir().mkpath(QFileInfo(file).absolutePath()))
        return fail(error, "Palette library directory could not be created");
    QJsonArray list;
    for (const auto &p : palettes) {
        QJsonArray colors;
        for (auto c : p.colors)
            colors.append(ops::colorName(c));
        list.append(QJsonObject{{"name", p.name}, {"colors", colors}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"palettes", list}}).toJson();
    return atomicWrite(file, bytes, Revision::read(file), error);
}
std::vector<Palette> PaletteStore::all() const {
    auto out = builtInPalettes();
    out.insert(out.end(), palettes.begin(), palettes.end());
    return out;
}
Palette *PaletteStore::find(const QString &name) {
    for (auto &p : palettes)
        if (p.name == name)
            return &p;
    return nullptr;
}
QString PaletteStore::uniqueName(const QString &base) const {
    auto taken = [&](const QString &n) {
        for (const auto &p : all())
            if (p.name == n)
                return true;
        return false;
    };
    QString name = base.trimmed().isEmpty() ? QString("Palette") : base.trimmed().left(120);
    for (int i = 2; taken(name); ++i)
        name = QString("%1 %2").arg(base.trimmed().left(120)).arg(i);
    return name;
}
} // namespace nova
