// SPDX-License-Identifier: GPL-3.0-or-later
#include "minecraft_animation.h"
#include "modelpanel.h"
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
namespace nova {
QImage ModelPanel::loadMaterial(const QString &asset, QStringList &messages) {
    for (const auto &doc : session->all())
        if (QDir(root).relativeFilePath(doc->path) == asset) {
            liveMaterialBindings[doc->id()] = asset;
            if (doc->animated())
                materialDocuments[asset] = doc;
            return doc->animated()
                       ? doc->frameComposite(doc->animationState().entries.front().frame)
                       : doc->image;
        }
    if (auto doc = materialDocuments.value(asset))
        return doc->frameComposite(doc->animationState().entries.front().frame);
    const auto file = library->assetPath(asset);
    if (file.isEmpty())
        return {};
    const auto metadata = library->assetPath(asset + ".mcmeta");
    if (metadata.isEmpty())
        return loadPng(file);
    QFile f(metadata);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 512 * 1024) {
        messages << "Unreadable texture metadata: " + asset;
        return loadPng(file);
    }
    const auto json = QJsonDocument::fromJson(f.readAll()).object();
    if (!json.contains("animation"))
        return loadPng(file);
    QImageReader reader(file, "PNG");
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 32768 || size.height() > 32768 ||
        qint64(size.width()) * size.height() * 4 > 64 * 1024 * 1024) {
        messages << "Animated material sheet exceeds preview dimensions/memory budget: " + asset;
        return {};
    }
    auto image = reader.read().convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull())
        return {};
    QImage placeholder(1, 1, QImage::Format_RGBA8888);
    placeholder.fill(Qt::transparent);
    auto doc = std::make_shared<Document>(file, placeholder);
    QString error;
    if (!doc->importAnimation(image, json, library->version(), &error)) {
        messages << "Animation material: " + asset + " · " + error;
        return image;
    }
    qint64 used = doc->pixelBytes();
    for (const auto &other : materialDocuments)
        used += other->pixelBytes();
    if (used > 64 * 1024 * 1024) {
        messages << "Animated material pixel budget (64 MiB); static first frame: " + asset;
        return doc->image;
    }
    materialDocuments[asset] = doc;
    return doc->frameComposite(doc->animationState().entries.front().frame);
}
} // namespace nova
