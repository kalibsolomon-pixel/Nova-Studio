// SPDX-License-Identifier: GPL-3.0-or-later
#include "models.h"
#include <QJsonArray>
namespace nova::mc {
Mesh generatedMesh(const Model &model, const QMap<QString, QImage> &sprites,
                   const QMap<QString, QVector<QImage>> &frames) {
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
                                {"faces", QJsonObject{{"south", face({0, 0, 16, 16})},
                                                      {"north", face({16, 0, 0, 16})}}}};
        const int width = image.width(), height = image.height();
        const double sx = 16. / width, sy = 16. / height;
        const QPoint offsets[] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
        const QString names[] = {"up", "down", "west", "east"};
        auto samples = frames.value(texture);
        if (samples.isEmpty())
            samples << image;
        if (qint64(width) * height * samples.size() > 16 * 1024 * 1024)
            return flat("Sprite exceeds the animation contour budget: " + texture);
        // Union the edge transitions from each frame, including edges hidden by other frames.
        QVector<quint8> edges(width * height);
        qsizetype elementsWithEdges = elements.size();
        for (const auto &frame : samples) {
            if (frame.size() != image.size())
                continue;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    if (qAlpha(frame.pixel(x, y)) == 0)
                        continue;
                    auto &mask = edges[y * width + x];
                    for (int d = 0; d < 4; ++d) {
                        const QPoint neighbor = QPoint(x, y) + offsets[d];
                        if (!(mask & (1 << d)) && (!frame.rect().contains(neighbor) ||
                                                    qAlpha(frame.pixel(neighbor)) == 0)) {
                            mask |= 1 << d;
                            if (++elementsWithEdges > 4096)
                                return flat("Generated contour exceeds the 4096-face budget: " + texture);
                        }
                    }
                }
        }
        // Each wall samples an inset rectangle within one source texel.
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                for (int d = 0; d < 4; ++d) {
                    if (!(edges[y * width + x] & (1 << d)))
                        continue;
                    const double u0 = (x + .1) * sx, u1 = (x + .9) * sx;
                    QJsonArray from, to, uv;
                    if (d < 2) {
                        const double boundary = 16 - (y + (d == 1)) * sy;
                        from = vec(x * sx, boundary, 7.5);
                        to = vec((x + 1) * sx, boundary, 8.5);
                        uv = {u0, (y + .1) * sx, u1, (y + .9) * sy};
                    } else {
                        const double boundary = (x + (d == 3)) * sx;
                        from = vec(boundary, 16 - (y + 1) * sy, 7.5);
                        to = vec(boundary, 16 - y * sy, 8.5);
                        uv = {u1, (y + .1) * sy, u0, (y + .9) * sx};
                    }
                    elements << QJsonObject{{"from", from}, {"to", to},
                                            {"faces", QJsonObject{{names[d], face(uv)}}}};
                }
    }
    Model generated = model;
    generated.generated = false;
    generated.resolved["elements"] = elements;
    auto result = mesh(generated);
    for (auto &f : result.faces) {
        f.generatedLayer = f.tint;
        f.elementIndex = -1;
        f.element = "Generated " + f.element;
    }
    return result;
}
} // namespace nova::mc
