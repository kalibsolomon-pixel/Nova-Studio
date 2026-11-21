// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Built-in 3D previews for textures that no block or item model uses: worn armor and
// equipment, player skins, mobs, block entities, paintings, items, fluids and sprites.
//
// Entity geometry follows the Java Edition layer definitions (box origins, sizes, texture
// offsets, mirroring, pivots, rotations and cube deformations) and is baked with the same
// cube UV layout as special_models.cpp. Entity poses are expressed in model pixels with Y
// down, as Minecraft authors them, then flipped upright and facing +Z (towards the default
// camera) with the feet at y = -1. One block is two mesh units.
#include "models.h"
#include <QImage>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace nova::mc {
struct TexturePreviewSpec {
    QString id;       // e.g. "armor_humanoid_outer", "player_wide", "creeper", "painting"
    QString label;    // human-readable name
    QString category; // "Armor & Equipment", "Player", "Entity", "Block Entity", "Item", ...
};

struct TexturePreviewResult {
    Mesh mesh;
    QString id;
    QString label;
    // Stand-ins for companion textures the library does not provide (a neutral mannequin
    // skin, an iron-grey armor base, a wooden painting back, ...). Keys are the Face::texture
    // values that use them. Faces of the previewed texture always use the exact asset path
    // passed to buildTexturePreview, so live canvas edits and UV overlays apply to them.
    QMap<QString, QImage> syntheticTextures;
    // Companion assets (other than the previewed texture) the mesh references and the
    // library provides. Load these from the library; synthetic keys are never listed here.
    QStringList referencedAssets;
    bool valid() const { return !mesh.faces.isEmpty(); }
};

// The built-in preview that applies automatically to a texture asset such as
// "assets/minecraft/textures/entity/equipment/humanoid/diamond.png", or an empty spec
// (id.isEmpty()) when none does. Ordinary block textures (textures/block/<name>.png other
// than the fluids, fire, portal and destroy stages) return an empty spec: they are previewed
// through the models that use them.
TexturePreviewSpec detectTexturePreview(const QString &textureAsset);

// Templates that can preview any texture, for a manual target picker.
QVector<TexturePreviewSpec> manualTexturePreviews();

// The spec registered under `previewId`; empty when the id is unknown.
TexturePreviewSpec texturePreviewSpec(const QString &previewId);

// Builds the preview of `textureAsset` with `previewId`, or with the detected preview when
// `previewId` is empty. `activeImage` is the texture as currently edited; it selects legacy
// 64x32 layouts, animation strip frames and painting sizes, and supplies the sprite contour
// of extruded items. `library`, when given, supplies companion textures such as the matching
// leggings layer, the default player skin or a mob's base texture. Returns an invalid result
// (no faces) when no preview applies.
TexturePreviewResult buildTexturePreview(const QString &textureAsset, const QImage &activeImage,
                                         const Library *library = nullptr,
                                         const QString &previewId = {});
} // namespace nova::mc
