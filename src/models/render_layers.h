// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>

namespace nova::mc {
enum class RenderLayer { Solid, Cutout, Translucent, Tripwire };

// The fancy-leaves setting is used for the isolated preview.
RenderLayer blockRenderLayer(const QString &blockId);
RenderLayer itemRenderLayer(const QString &blockId, bool itemAtlas);
float alphaCutoff(RenderLayer layer, bool terrain);
} // namespace nova::mc
