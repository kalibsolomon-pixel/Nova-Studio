// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "models.h"
#include "revision.h"
#include <functional>
namespace nova::mc {
// Validates every existing ancestor, rejecting links and paths outside the pack.
bool safeDestination(const QString &root, const QString &relative, QString *absolute,
                     QString *error = nullptr);
enum class AssetType {
    BlockModel,
    ItemModel,
    Blockstate,
    ItemDefinition,
    Texture,
    AnimatedTexture,
    ModelTexturePair
};
enum class ModelTemplate { Cube, Cuboid, Cross, FlatItem, LayeredItem, MultiElement };
QJsonObject modelTemplate(ModelTemplate type, const QString &texture);
struct CreationRequest {
    AssetType type = AssetType::BlockModel;
    ModelTemplate shape = ModelTemplate::Cube;
    QString id, texture, version = "26.1";
    int textureSize = 16;
};
struct CreationFile {
    QString relative;
    QByteArray bytes;
};
struct CreationPlan {
    QString root, error;
    QVector<CreationFile> files;
    bool valid() const { return error.isEmpty() && !files.isEmpty(); }
};
CreationPlan planCreation(const QString &root, const CreationRequest &);
struct CreationResult {
    bool complete = false;
    QString journal, error;
    QStringList written;
};
// Individual atomic replacements with a retained write-ahead journal, not multi-file atomicity.
CreationResult executeCreation(const CreationPlan &,
                               const std::function<void(int)> &beforeWrite = {});
// Only removes files created by this journal whose bytes still match. Journal stays for audit.
bool rollbackCreation(const QString &root, const QString &journal, QString *error = nullptr);
} // namespace nova::mc
