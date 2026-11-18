// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "models.h"
#include <optional>

namespace nova::mc {
std::optional<QRgb> tintRgb(const QJsonValue &value);

class TintColors {
    const Library &library;
    QHash<QString, QImage> maps;
    QRgb climateColor(const QString &map, double temperature, double downfall, QRgb fallback);

  public:
    explicit TintColors(const Library &library) : library(library) {}
    std::optional<QRgb> item(const QString &type, const QJsonObject &source);
    QVector<QRgb> block(const QString &id, const QJsonObject &inputs);
};
} // namespace nova::mc
