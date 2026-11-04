// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include "index.h"
#include <QJsonObject>
#include <QSet>

namespace nova::mc { class Library; }
namespace nova::mcp {
struct RelationshipCache {
    QString generation, version;
    QMap<QString, AssetIdentity> records;
    std::shared_ptr<const mc::Library> library;
    QMap<QString, AssetIdentity> identifierRecords;
    QHash<QString, QString> identifiers;
};
QJsonObject summarizeAnimation(const AnimationState &state);
struct Snapshot {
    QString root, generation, projectId, version;
    QByteArray rootIdentity, idKey;
    QString indexRevision;
    QSet<QString> provisional;
    QMap<QString, AssetIdentity> diskRecords;
    Index index;
    std::shared_ptr<RelationshipCache> relationships;
    QMap<QString, QString> revisions, openAssetPaths;
    QMap<QString, std::shared_ptr<const Document>> animationDocuments;
    QMap<QString, QJsonObject> textures, animations, models;
};
QString assetIdentifier(const Snapshot &, const QString &path);
QJsonObject inspect(const Snapshot &, const QString &tool, const QJsonObject &arguments,
                    const std::shared_ptr<std::atomic_bool> &cancel);
}
