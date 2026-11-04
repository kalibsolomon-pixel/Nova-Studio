#include "core.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QSaveFile>
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
    QImageReader reader(path, "png");
    QImageReader::setAllocationLimit(128);
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
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QImageWriter writer(&file, "png");
    if (!writer.write(image)) {
        if (error)
            *error = writer.errorString();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
Document::Document(QString p, QImage im)
    : path(std::move(p)), image(std::move(im)), dirty(image.rect()) {}
bool Document::modified() const { return saved != cursor || !stroke.isEmpty(); }
void Document::draw(QPoint a, QPoint b, QRgb color) {
    for (auto p : line(a, b)) {
        if (!image.rect().contains(p))
            continue;
        auto before = image.pixel(p);
        if (before == color)
            continue;
        int index = p.y() * image.width() + p.x();
        if (stroke.contains(index))
            stroke[index].after = color;
        else
            stroke.insert(index, {index, before, color});
        image.setPixel(p, color);
        dirty = dirty.united(QRect(p, QSize(1, 1)));
    }
}
void Document::finish() {
    if (stroke.isEmpty())
        return;
    if (saved && *saved > cursor)
        saved.reset();
    history.resize(cursor);
    std::vector<Change> changes;
    for (auto c : stroke)
        if (c.before != c.after)
            changes.push_back(c);
    history.push_back(std::move(changes));
    stroke.clear();
    ++cursor;
}
void Document::apply(bool forward) {
    for (auto c : history.at(cursor)) {
        QPoint p(c.index % image.width(), c.index / image.width());
        image.setPixel(p, forward ? c.after : c.before);
        dirty = dirty.united(QRect(p, QSize(1, 1)));
    }
}
void Document::undo() {
    finish();
    if (cursor > 0) {
        --cursor;
        apply(false);
    }
}
void Document::redo() {
    finish();
    if (cursor < int(history.size())) {
        apply(true);
        ++cursor;
    }
}
bool Document::save(QString *error) {
    finish();
    if (!savePng(path, image, error))
        return false;
    saved = cursor;
    return true;
}
Index Index::scan(const QString &root) {
    Index index;
    if (!QFileInfo(root).isDir()) {
        index.error = "Project directory does not exist";
        return index;
    }
    index.directories.insert("", {});
    QDir base(root);
    QDirIterator it(root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString path = it.next();
        if (QFileInfo(path).suffix().compare("png", Qt::CaseInsensitive) != 0)
            continue;
        QString child = base.relativeFilePath(path);
        if (child.count('/') > 32)
            continue;
        index.files.append(child);
        for (;;) {
            int slash = child.lastIndexOf('/');
            QString parent = slash < 0 ? "" : child.left(slash);
            const bool newDirectory = !index.directories.contains(parent);
            index.directories[parent].append(child);
            if (!newDirectory || parent.isEmpty())
                break;
            child = parent;
        }
    }
    index.files.sort();
    for (auto &entries : index.directories)
        entries.sort();
    return index;
}
QStringList Navigation::entries(const Index &index) const {
    QStringList result;
    if (query.isEmpty())
        result = index.directories.value(directory);
    else
        for (auto p : index.files)
            if ((directory.isEmpty() || p.startsWith(directory + '/')) &&
                p.contains(query, Qt::CaseInsensitive))
                result.append(p);
    std::sort(result.begin(), result.end(), [&](const QString &a, const QString &b) {
        bool ad = index.directories.contains(a), bd = index.directories.contains(b);
        return ad != bd ? ad : a < b;
    });
    if (reverse)
        std::reverse(result.begin(), result.end());
    return result;
}
} // namespace nova
