// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include "revision.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <algorithm>
#include <cmath>

namespace nova {
std::optional<QPoint> coordinate(QPointF point, QPointF origin, double zoom, QSize size) {
    if (!std::isfinite(zoom) || zoom <= 0)
        return {};
    const auto p = (point - origin) / zoom;
    if (p.x() < 0 || p.y() < 0 || p.x() >= size.width() || p.y() >= size.height())
        return {};
    return QPoint(int(std::floor(p.x())), int(std::floor(p.y())));
}
double zoomBy(double current, double delta) {
    return std::clamp(current * std::pow(1.002, delta), 0.25, 64.0);
}
QVector<QPoint> line(QPoint a, QPoint b) {
    QVector<QPoint> out;
    int x = a.x(), y = a.y(), dx = std::abs(b.x() - x), dy = -std::abs(b.y() - y),
        sx = x < b.x() ? 1 : -1, sy = y < b.y() ? 1 : -1, error = dx + dy;
    for (;;) {
        out.append({x, y});
        if (x == b.x() && y == b.y())
            break;
        int twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x += sx;
        }
        if (twice <= dx) {
            error += dx;
            y += sy;
        }
    }
    return out;
}
QImage loadPng(const QString &path, QString *error) {
    if (QFileInfo(path).suffix().compare("png", Qt::CaseInsensitive) != 0) {
        if (error)
            *error = "Only PNG images are supported";
        return {};
    }
    // Read and close before decoding so a concurrent save is not blocked by this handle.
    QString readError;
    auto bytes = readFileBytes(path, 96LL * 1024 * 1024, &readError);
    if (bytes.isEmpty()) {
        if (error)
            *error = readError.isEmpty() ? QStringLiteral("Invalid PNG dimensions or image exceeds 4096px limit")
                                         : readError;
        return {};
    }
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png");
    auto size = reader.size();
    if (!size.isValid() || size.width() > 4096 || size.height() > 4096) {
        if (error)
            *error = "Invalid PNG dimensions or image exceeds 4096px limit";
        return {};
    }
    auto image = reader.read();
    if (image.isNull() && error)
        *error = reader.errorString();
    return image.convertToFormat(QImage::Format_RGBA8888);
}
bool savePng(const QString &path, const QImage &image, QString *error) {
    if (image.isNull() || image.width() > 4096 || image.height() > 4096 ||
        QFileInfo(path).suffix().compare("png", Qt::CaseInsensitive) != 0) {
        if (error)
            *error = "Save requires a valid PNG image up to 4096 pixels per side";
        return false;
    }
    QString writeError;
    const bool ok = atomicReplace(
        path,
        [&](QIODevice &file) {
            QImageWriter writer(&file, "png");
            if (writer.write(image))
                return true;
            writeError = writer.errorString();
            return false;
        },
        error);
    if (!ok && error && !writeError.isEmpty())
        *error = writeError;
    return ok;
}
} // namespace nova
