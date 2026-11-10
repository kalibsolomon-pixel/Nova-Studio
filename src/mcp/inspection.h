// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include "index.h"
#include "resources.h"
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
// An open document whose content differs from the pack (nova.pack.validate, nova.asset.relationships).
struct DraftInfo {
    QString type, revision; // revision: this connection's token
    bool provisional = false;
    // The file content the document was opened or last saved from (SHA-256), for draft/disk conflicts.
    QByteArray savedHash;
    bool savedExists = false;
};
// One entry of the resource stack the user configured for this project and version (Models ›
// Resource stack). Agents see its position, kind, status and folder name, never its location.
struct StackEntry {
    QString label, status, reason; // status: loaded, unavailable
    QString root;                  // canonical folder of a loaded source (internal only)
    bool vanilla = false;          // a verified vanilla resource source (nova-vanilla.json)
};
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
    QJsonObject styleNotes;                 // nova.project.info, nova://project/style-notes
    int unsavedCount = 0;                   // open documents of this project not fully saved
    QMap<QString, DraftInfo> drafts;        // nova.pack.validate: open unsaved documents
    QMap<QString, QString> unavailableDrafts; // nova.pack.validate: path → why it could not be read
    // MCP Phase 3 (nova.vanilla.*, nova.asset.relationships, nova.pack.validate).
    QVector<mc::ResourceSource> resourceSources; // loaded stack sources, precedence order
    QList<StackEntry> resourceStack;             // every configured stack entry, precedence order
    QString vanillaCacheRoot;                    // Nova's managed cache (never reported)
    QString baselineVersion, baselineIdentity;   // the project's vanilla baseline binding, if any
};
QString assetIdentifier(const Snapshot &, const QString &path);
QJsonObject inspect(const Snapshot &, const QString &tool, const QJsonObject &arguments,
                    const std::shared_ptr<std::atomic_bool> &cancel);
}
