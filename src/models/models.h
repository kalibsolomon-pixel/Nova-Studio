// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "index.h"
#include "resources.h"
#include <QHash>
#include <QIODevice>
#include <functional>
#include <memory>
#include <QImage>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QSet>
#include <QVector2D>
#include <QVector3D>
#include <array>

namespace nova::mc {
struct Profile {
    QString version;
    bool spriteObjects = false, shadeOverride = false, itemTransforms = false;
    static Profile forVersion(const QString &version);
    bool valid() const { return !version.isEmpty(); }
};
struct Location {
    QString nameSpace, path;
    static Location parse(const QString &text);
    static QString fromAsset(const QString &asset, const QString &kind);
    bool valid() const { return !nameSpace.isEmpty(); }
    QString id() const { return valid() ? nameSpace + ':' + path : QString(); }
    QString asset(const QString &kind) const;
};
struct Model {
    QString id, version;
    QJsonObject source, resolved;
    QStringList parents, diagnostics;
    QMap<QString, QString> textures;
    // Effective display transform per context and the {model id, JSON key} supplying it.
    QJsonObject display;
    QMap<QString, QPair<QString, QString>> displayOrigin;
    bool generated = false, valid = false;
};
struct Instance {
    QString model, label;
    int x = 0, y = 0, z = 0;
    bool uvlock = false;
    QMatrix4x4 transformation;
    QVector<QRgb> tints;
};
struct Dispatch {
    QVector<Instance> instances;
    QStringList diagnostics, variants;
};
// Snapshot-friendly: copied to the existing indexing worker; no extra watcher/index.
class Library {
    QString root;
    Profile profile;
    QMap<QString, QJsonObject> sources, provisionalSources;
    QMap<QString, QString> failures;
    QMap<QString, AssetIdentity> records;
    QMap<QString, QSet<QString>> forward, reverse;
    QHash<QString, Model> cache;
    Index editableSnapshot;
    QVector<ResourceSource> fallbacks;
    ResourceStack stack;
    Model resolve(const QString &id, QSet<QString> &visiting, int depth);
    void replaceEdges(const QString &path);

  public:
    quint64 parsedFiles = 0, cacheHits = 0;
    QStringList diagnostics;
    explicit Library(QString version = "26.1") : profile(Profile::forVersion(version)) {}
    // Optional guarded byte source. The caller owns its filesystem policy.
    using AssetReader = std::function<std::unique_ptr<QIODevice>(const QString &)>;
    void update(const QString &project, const Index &index, const QStringList &changed = {},
                const std::shared_ptr<std::atomic_bool> &cancel = {},
                const AssetReader &reader = {});
    void setVersion(const QString &version);
    QString version() const { return profile.version; }
    void setFallbacks(QVector<ResourceSource> sources, const QString &version = {});
    bool hasFallbacks() const { return !fallbacks.isEmpty(); }
    QString assetPath(const QString &asset, int provider = 0) const {
        return stack.path(asset, provider);
    }
    bool editable(const QString &asset) const { return provisionalSources.contains(asset) || stack.editable(asset); }
    QVector<ResourceCandidate> providers(const QString &asset) const {
        return stack.providers(asset);
    }
    void releaseModelSource(const QString &asset);
    void setProvisionalSource(const QString &asset, const QJsonObject &source);
    void previewSource(const QString &asset, const QJsonObject &source);
    void refreshLocal(const QStringList &paths) { update(root, Index::refresh(root, editableSnapshot, paths), paths); }
    Model model(const QString &id);
    QStringList modelsForTexture(const QString &asset);
    QStringList dependencies(const QString &asset) const;
    QStringList dependents(const QString &asset, bool transitive = true) const;
    QStringList modelIds() const;
    QStringList definitionPaths() const;
    QStringList texturePaths() const {
        QStringList paths;
        for (const auto &f : stack.index().files)
            if (!Location::fromAsset(f, "textures").isEmpty()) paths << f;
        return paths;
    }
    Dispatch dispatch(const QString &asset, const QJsonObject &inputs = {},
                      int weightedChoice = 0) const;
    QJsonObject source(const QString &asset) const { return sources.value(asset); }
};
struct Face {
    std::array<QVector3D, 4> points;
    std::array<QVector2D, 4> uv;
    QVector3D normal;
    QString texture, name, element, cullface;
    QString modelId;
    int elementIndex = -1;
    QRgb color = qRgb(255, 255, 255);
    int rotation = 0, tint = -1;
    bool shade = true, forceTranslucent = false;
    QString shadeDirection;
    QRectF uvRect() const;
};
struct Mesh {
    QVector<Face> faces;
    QStringList diagnostics;
    QVector3D minimum, maximum;
};
Mesh mesh(const Model &model);
Mesh generatedMesh(const Model &model, const QMap<QString, QImage> &sprites);
Mesh transformed(Mesh mesh, const Instance &instance);
Mesh applyTransform(Mesh mesh, const QMatrix4x4 &matrix);
bool itemTransform(const QJsonValue &value, QMatrix4x4 &out, QString *error = nullptr);
QMatrix4x4 displayTransform(const Model &model, const QString &context);
} // namespace nova::mc
