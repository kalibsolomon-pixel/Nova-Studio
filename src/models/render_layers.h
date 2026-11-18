// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>

namespace nova::mc {
enum class RenderLayer { Solid, Cutout, Translucent, Tripwire };
class Library;

// The fancy-leaves setting is used for the isolated preview.
RenderLayer blockRenderLayer(const QString &blockId);
RenderLayer itemRenderLayer(const QString &blockId, bool itemAtlas);
float alphaCutoff(RenderLayer layer, bool terrain);
QString previewBlockId(const QString &asset, const Library &library);
} // namespace nova::mc
