// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// MCP Phase 3: read-only access to Nova's verified local vanilla *texture* cache
// (docs/vanilla-baseline/DESIGN.md §3). Requests name a Minecraft version and a resource path;
// they never name a file, never scan other folders, never download and never change the cache.
#include <QJsonObject>
#include <atomic>
#include <memory>

namespace nova::mcp {
struct Snapshot;
constexpr qint64 VanillaImageLimit = 256 * 1024;   // PNG bytes returned inline as an image block
constexpr qint64 VanillaMetadataLimit = 16 * 1024; // .png.mcmeta text returned inline
QJsonObject vanillaList(const Snapshot &, const QJsonObject &arguments, const std::shared_ptr<std::atomic_bool> &cancel);
QJsonObject vanillaTexture(const Snapshot &, const QJsonObject &arguments);
} // namespace nova::mcp
