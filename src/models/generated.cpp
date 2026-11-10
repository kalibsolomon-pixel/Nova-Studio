// SPDX-License-Identifier: GPL-3.0-or-later
#include "models.h"
#include <QJsonArray>
namespace nova::mc {
Mesh generatedMesh(const Model &model, const QMap<QString, QImage> &sprites) {
    if (!model.valid || !model.generated || !model.resolved["elements"].toArray().isEmpty())
        return mesh(model);
    auto flat = [&](const QString &reason) {
        auto result = mesh(model);
        result.previewLimitation = reason;
        result.diagnostics << "Generated item uses flat layers: " + reason;
        return result;
    };
    QJsonArray elements;
    auto vec = [](double x, double y, double z) { return QJsonArray{x, y, z}; };
    for (int layer = 0; layer < 5; ++layer) {
        const auto key = "layer" + QString::number(layer), texture = model.textures.value(key);
        if (texture.isEmpty())
            break;
        const auto image = sprites.value(texture);
        if (image.isNull())
            return flat("Sprite unavailable: " + texture);
        if (image.width() > 1024 || image.height() > 1024)
            return flat("Sprite exceeds the 1024-pixel contour limit: " + texture);
        auto face = [&](QJsonArray uv) {
            return QJsonObject{{"texture", '#' + key}, {"uv", uv}, {"tintindex", layer}};
        };
        elements << QJsonObject{{"from", vec(0, 0, 7.5)},
                                {"to", vec(16, 16, 8.5)},
                                {"shade", false},
                                {"faces", QJsonObject{{"south", face({0, 0, 16, 16})},
                                                      {"north", face({16, 0, 0, 16})}}}};
        auto opaque = [&](int x, int y) {
            return image.rect().contains(x, y) && qAlpha(image.pixel(x, y)) > 0;
        };
        const double sx = 16. / image.width(), sy = 16. / image.height();
        // Merge contiguous boundary pixels. Interior transparent holes receive
        // their own walls; opaque neighbors never add internal geometry.
        for (int direction = 0; direction < 4; ++direction) {
            const bool horizontal = direction < 2;
            const int lines = horizontal ? image.height() : image.width(),
                      length = horizontal ? image.width() : image.height();
            for (int line = 0; line < lines; ++line)
                for (int a = 0; a < length;) {
                    auto edge = [&](int n) {
                        int x = horizontal ? n : line, y = horizontal ? line : n;
                        return opaque(x, y) && !opaque(x + (direction == 2   ? -1
                                                            : direction == 3 ? 1
                                                                             : 0),
                                                       y + (direction == 0   ? -1
                                                            : direction == 1 ? 1
                                                                             : 0));
                    };
                    if (!edge(a)) {
                        ++a;
                        continue;
                    }
                    int b = a + 1;
                    while (b < length && edge(b))
                        ++b;
                    QJsonArray from, to, uv;
                    QString name;
                    if (horizontal) {
                        const double y = 16 - (line + (direction == 1)) * sy;
                        from = vec(a * sx, y, 7.5);
                        to = vec(b * sx, y, 8.5);
                        uv = {a * sx, (line + .5) * sy, b * sx, (line + .5) * sy};
                        name = direction == 0 ? "up" : "down";
                    } else {
                        const double x = (line + (direction == 3)) * sx;
                        from = vec(x, 16 - b * sy, 7.5);
                        to = vec(x, 16 - a * sy, 8.5);
                        uv = {(line + .5) * sx, a * sy, (line + .5) * sx, b * sy};
                        name = direction == 2 ? "west" : "east";
                    }
                    elements << QJsonObject{{"from", from},
                                            {"to", to},
                                            {"shade", false},
                                            {"faces", QJsonObject{{name, face(uv)}}}};
                    if (elements.size() > 4096)
                        return flat("Generated contour exceeds the 4096-span budget: " + texture);
                    a = b;
                }
        }
    }
    Model generated = model;
    generated.generated = false;
    generated.resolved["elements"] = elements;
    if (Profile::forVersion(model.version).shadeOverride) {
        for (int i = 0; i < elements.size(); ++i) {
            auto e = elements[i].toObject();
            e.remove("shade");
            e["shade_direction_override"] = "up";
            elements[i] = e;
        }
        generated.resolved["elements"] = elements;
    }
    auto result = mesh(generated);
    for (auto &f : result.faces) {
        f.generatedLayer = f.tint;
        f.elementIndex = -1;
        f.element = "Generated " + f.element;
    }
    return result;
}
} // namespace nova::mc
