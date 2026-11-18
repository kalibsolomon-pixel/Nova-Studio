// SPDX-License-Identifier: GPL-3.0-or-later
#include "tints.h"
#include <QImageReader>
#include <QJsonArray>
#include <cmath>

namespace nova::mc {
std::optional<QRgb> tintRgb(const QJsonValue &value) {
    if (value.isDouble()) {
        const auto n = value.toDouble();
        if (n >= -2147483648. && n <= 4294967295. && std::floor(n) == n)
            return (quint32(value.toInteger()) & 0xffffffu) | 0xff000000u;
    }
    const auto components = value.toArray();
    if (components.size() != 3)
        return {};
    int rgb[3];
    for (int i = 0; i < 3; ++i) {
        const auto n = components[i].toDouble(-1);
        if (!(n >= 0 && n <= 1))
            return {};
        rgb[i] = int(float(n) * 255);
    }
    return qRgb(rgb[0], rgb[1], rgb[2]);
}

QRgb TintColors::climateColor(const QString &map, double temperature, double downfall, QRgb fallback) {
    if (!maps.contains(map)) {
        const auto path = library.assetPath("assets/minecraft/textures/colormap/" + map + ".png");
        QImage image;
        if (!path.isEmpty()) {
            QImageReader reader(path, "png");
            if (reader.size() == QSize(256, 256))
                image = reader.read();
        }
        maps.insert(map, image);
    }
    const auto &image = maps[map];
    if (image.isNull())
        return fallback;
    const int x = int((1 - temperature) * 255);
    const int y = int((1 - temperature * downfall) * 255);
    return image.pixel(x, y) | 0xff000000u;
}

std::optional<QRgb> TintColors::item(const QString &type, const QJsonObject &source) {
    if (type == "minecraft:constant")
        return tintRgb(source["value"]);
    if (type == "minecraft:grass") {
        const double temperature = std::clamp(float(source["temperature"].toDouble(.5)), 0.f, 1.f);
        const double downfall = std::clamp(float(source["downfall"].toDouble(1)), 0.f, 1.f);
        const auto fallback = temperature == double(.8f) && downfall == double(.4f)
                                  ? 0xff91bd59u : 0xff7cbd6bu;
        return climateColor("grass", temperature, downfall, fallback);
    }
    static const QHash<QString, QRgb> defaults{
        {"minecraft:dye", 0xffffffffu}, {"minecraft:potion", 0xff385dc6u},
        {"minecraft:map_color", 0xff46402eu}, {"minecraft:firework", 0xff8a8a8au},
        {"minecraft:team", 0xffffffffu}, {"minecraft:custom_model_data", 0xffffffffu}};
    const auto it = defaults.constFind(type);
    if (it == defaults.cend())
        return {};
    return tintRgb(source["default"]).value_or(it.value());
}

QVector<QRgb> TintColors::block(const QString &blockId, const QJsonObject &inputs) {
    const auto id = Location::parse(blockId);
    QVector<QRgb> colors;
    const auto &name = id.path;
    if (id.nameSpace == "minecraft") {
        static const QSet<QString> grass{"grass_block", "short_grass", "tall_grass", "fern",
                                         "large_fern", "potted_fern", "bush", "pink_petals",
                                         "wildflowers", "sugar_cane"};
        static const QSet<QString> foliage{"oak_leaves", "jungle_leaves", "acacia_leaves",
                                           "dark_oak_leaves", "vine", "mangrove_leaves"};
        static const QHash<QString, QRgb> fixed{
            {"spruce_leaves", 0xff619961u}, {"birch_leaves", 0xff80a755u},
            {"water", 0xff3f76e4u}, {"bubble_column", 0xff3f76e4u},
            {"water_cauldron", 0xff3f76e4u}, {"lily_pad", 0xff208030u},
            {"attached_melon_stem", 0xffe0c71cu}, {"attached_pumpkin_stem", 0xffe0c71cu}};
        constexpr double Temperature = .8f, Downfall = .4f;
        if (grass.contains(name))
            colors << climateColor("grass", Temperature, Downfall, 0xff91bd59u);
        else if (foliage.contains(name))
            colors << climateColor("foliage", Temperature, Downfall, 0xff77ab2fu);
        else if (name == "leaf_litter")
            colors << climateColor("dry_foliage", Temperature, Downfall, 0xffa37546u);
        else if (const auto it = fixed.constFind(name); it != fixed.cend())
            colors << it.value();
        else if (name == "redstone_wire") {
            const auto power = inputs["power"];
            const int n = std::clamp(power.isString() ? power.toString().toInt() : power.toInt(), 0, 15);
            const float f = n / 15.f;
            const float g = std::clamp(.7f * f * f - .5f, 0.f, 1.f);
            colors << qRgb(int((.6f * f + (n ? .4f : .3f)) * 255), int(g * 255), 0);
        } else if (name == "melon_stem" || name == "pumpkin_stem") {
            const auto age = inputs["age"];
            const int n = std::clamp(age.isString() ? age.toString().toInt() : age.toInt(), 0, 7);
            colors << qRgb(32 * n, 255 - 8 * n, 4 * n);
        }
    }
    const auto manual = inputs["_tints"].toArray();
    for (int i = 0; i < std::min<qsizetype>(manual.size(), 256); ++i)
        if (const auto color = tintRgb(manual[i])) {
            while (i >= colors.size())
                colors << qRgb(255, 255, 255);
            colors[i] = *color;
        }
    return colors;
}
} // namespace nova::mc
