// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// MCP Phase 3: read-only access to Nova's verified local vanilla *texture* cache
// (docs/vanilla-baseline/DESIGN.md §3). Requests name a Minecraft version and a resource path;
// they never name a file, never scan other folders, never download and never change the cache.
#include <QImage>
#include <QJsonObject>
#include <atomic>
#include <memory>

namespace nova::mcp {
struct Snapshot;
constexpr qint64 VanillaImageLimit = 256 * 1024;   // PNG bytes returned inline as an image block
constexpr qint64 VanillaMetadataLimit = 16 * 1024; // .png.mcmeta text returned inline
struct VanillaTextureData {
    QByteArray png, metadata;
};
QString vanillaTexturePath(const QString &assetId);
// A preview for vision models: fits the longest edge to `edge` (nearest neighbour when enlarging,
// so texels stay sharp), optionally with lines between source texels once each is 4 px or more.
QImage rasterPreview(const QImage &image, int edge = 256, bool grid = false);
QJsonObject vanillaList(const Snapshot &, const QJsonObject &arguments, const std::shared_ptr<std::atomic_bool> &cancel);
// Internal callers receive verified source bytes instead of base64 MCP content.
QJsonObject vanillaTexture(const Snapshot &, const QJsonObject &arguments, VanillaTextureData *data = nullptr);
} // namespace nova::mcp
