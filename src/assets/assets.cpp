// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets.h"
#include "core.h"
#include "cube.h"
#include "display.h"
#include "minecraft_animation.h"
#include "render_layers.h"
#include "tints.h"
#include <QApplication>
#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QRunnable>
#include <QStyle>
#include <functional>
namespace nova {
namespace {
bool isModelIcon(const QString &path) {
    return path.endsWith(".json") && (!mc::Location::fromAsset(path, "models").isEmpty() ||
                                      !mc::Location::fromAsset(path, "items").isEmpty() ||
                                      !mc::Location::fromAsset(path, "blockstates").isEmpty());
}
struct IconRecipe {
    std::shared_ptr<mc::Mesh> mesh = std::make_shared<mc::Mesh>();
    QMap<QString, QImage> materials;
    QImage fallback;
    QString blockId;
};
IconRecipe buildIcon(mc::Library library, const QString &asset) {
    IconRecipe result;
    result.blockId = mc::previewBlockId(asset, library);
    const auto id = mc::Location::fromAsset(asset, "models");
    QVector<mc::Instance> instances;
    if (id.isEmpty()) {
        instances = library.dispatch(asset, {{"minecraft:display_context", "gui"}}).instances;
    } else {
        instances << mc::Instance{id, {}};
        instances[0].tints = mc::TintColors(library).block(result.blockId, {});
    }
    qint64 pixelsLeft = 4 * 1024 * 1024;
    auto load = [&](const QString &texture) {
        if (texture.isEmpty() || result.materials.contains(texture) || result.materials.size() >= 32)
            return;
        QImage image;
        const auto path = library.assetPath(texture);
        if (!path.isEmpty()) {
            const auto size = QImageReader(path, "png").size();
            const auto pixels = qint64(size.width()) * size.height();
            if (size.isValid() && pixels <= pixelsLeft) {
                pixelsLeft -= pixels;
                if (QFileInfo::exists(path + ".mcmeta")) {
                    if (auto animation = openMinecraftAnimation(path, library.version()))
                        image = animation->frameComposite(animation->animationState().entries.front().frame);
                }
                if (image.isNull())
                    image = loadPng(path);
            }
        }
        if (result.fallback.isNull() && !image.isNull())
            result.fallback = image;
        result.materials.insert(texture, image);
    };
    for (const auto &instance : instances) {
        const auto model = library.model(instance.model);
        if (model.generated)
            for (const auto &texture : model.textures)
                load(texture);
        auto part = mc::displayMesh(model, instance, "gui", result.materials);
        if (result.mesh->faces.size() + part.faces.size() > 8192)
            break;
        if (result.mesh->faces.isEmpty())
            result.mesh->guiLight = part.guiLight;
        for (const auto &face : part.faces)
            load(face.texture);
        if (part.faces.isEmpty() && result.fallback.isNull())
            for (const auto &texture : model.textures)
                load(texture);
        result.mesh->faces += part.faces;
    }
    return result;
}
} // namespace
AssetModel::AssetModel(QObject *p) : QAbstractItemModel(p), root(std::make_unique<Node>()) {
    root->directory = true;
    pool.setMaxThreadCount(2);
}
AssetModel::~AssetModel() { pool.waitForDone(); }
AssetModel::Node *AssetModel::node(const QModelIndex &i) const {
    return i.isValid() ? static_cast<Node *>(i.internalPointer()) : root.get();
}
std::unique_ptr<AssetModel::Node> AssetModel::build(const Index &idx, QString path, Node *parent,
                                                    int row) {
    auto n = std::make_unique<Node>();
    n->path = path;
    n->parent = parent;
    n->row = row;
    n->directory = idx.directories.contains(path);
    nodes[path] = n.get();
    if (n->directory) {
        auto children = idx.directories.value(path);
        if (path.isEmpty() && !category.isEmpty()) {
            children.clear();
            for (auto it = idx.records.cbegin(); it != idx.records.cend(); ++it)
                if (it->category == category)
                    children.append(it.key());
        }
        std::sort(children.begin(), children.end(), [&](auto a, auto b) {
            bool ad = idx.directories.contains(a), bd = idx.directories.contains(b);
            return ad != bd ? ad : a < b;
        });
        int r = 0;
        for (auto p : children)
            n->children.push_back(build(idx, p, n.get(), r++));
    }
    return n;
}
void AssetModel::setIndex(const Index &idx, const QString &path) {
    beginResetModel();
    ++generation;
    pool.clear();
    project = path;
    sourceIndex = idx;
    nodes.clear();
    root = build(idx, "", nullptr, 0);
    cache.clear();
    pending.clear();
    failed.clear();
    endResetModel();
}
void AssetModel::applyIndex(const Index &idx, const QStringList &changed) {
    // Content-only notifications leave the tree and persistent indexes intact.
    // Avoid removing/reinserting/sorting the entire catalog for one changed PNG.
    const bool sameStructure = idx.files == sourceIndex.files && idx.directories == sourceIndex.directories;
    sourceIndex = idx;
    if (!category.isEmpty() && !sameStructure) {
        setIndex(idx, project);
        return;
    }
    ++generation;
    pool.clear();
    pending.clear();
    if (!sameStructure) {
        auto keys = nodes.keys();
        std::function<void(Node *)> forget = [&](Node *n) {
            for (auto &child : n->children)
                forget(child.get());
            cache.remove(n->path);
            failed.remove(n->path);
            nodes.remove(n->path);
        };
        for (auto path : keys) {
            if (path.isEmpty() || !nodes.contains(path))
                continue;
            auto n = nodes.value(path);
            if ((!idx.records.contains(path) && !idx.directories.contains(path)) ||
                n->directory != idx.directories.contains(path)) {
                auto p = n->parent;
                int row = n->row;
                beginRemoveRows(indexFor(p->path), row, row);
                forget(n);
                p->children.erase(p->children.begin() + row);
                for (int i = row; i < int(p->children.size()); ++i)
                    p->children[i]->row = i;
                endRemoveRows();
            }
        }
        QStringList all = idx.directories.keys();
        all.append(idx.files);
        all.sort();
        for (auto path : all) {
            if (path.isEmpty() || nodes.contains(path))
                continue;
            auto parentPath = QFileInfo(path).path();
            if (parentPath == ".")
                parentPath.clear();
            auto p = nodes.value(parentPath, nullptr);
            if (!p)
                continue;
            bool directory = idx.directories.contains(path);
            int row = 0;
            while (row < int(p->children.size())) {
                auto &sibling = p->children[row];
                if (directory != sibling->directory ? directory : path < sibling->path)
                    break;
                ++row;
            }
            beginInsertRows(indexFor(parentPath), row, row);
            auto n = std::make_unique<Node>();
            n->path = path;
            n->directory = directory;
            n->parent = p;
            n->row = row;
            nodes[path] = n.get();
            p->children.insert(p->children.begin() + row, std::move(n));
            for (int i = row + 1; i < int(p->children.size()); ++i)
                p->children[i]->row = i;
            endInsertRows();
        }
    }
    QSet<QString> exact(changed.begin(), changed.end());
    QStringList directories;
    for (auto path : changed)
        if (idx.directories.contains(path))
            directories.append(path);
    auto invalidated = [&](const QString &path) {
        if (exact.contains("") || exact.contains(path)) return true;
        for (const auto &directory : directories)
            if (path.startsWith(directory + '/')) return true;
        return false;
    };
    if (exact.contains("")) {
        cache.clear();
        failed.clear();
    } else {
        // setCategory() retains the bounded cache for quick navigation. Entries
        // outside the visible node set still need content invalidation.
        for (const auto &path : cache.keys())
            if (invalidated(path)) cache.remove(path);
        failed.removeIf(invalidated);
    }
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it) {
        if (invalidated(it.key())) {
            auto i = indexFor(it.key());
            if (i.isValid())
                emit dataChanged(i, i, {Qt::DecorationRole, Qt::ForegroundRole, Qt::ToolTipRole, OriginRole});
        }
    }
}
void AssetModel::setCategory(const QString &value) {
    if (category == value)
        return;
    beginResetModel();
    category = value;
    ++generation;
    pool.clear();
    pending.clear();
    nodes.clear();
    root = build(sourceIndex, "", nullptr, 0);
    endResetModel();
}
void AssetModel::setModelSources(const std::shared_ptr<mc::Library> &library) {
    modelSources = library ? std::make_shared<mc::Library>(*library) : nullptr;
    ++modelRevision;
    for (const auto &path : cache.keys())
        if (isModelIcon(path))
            cache.remove(path);
    pending.removeIf(isModelIcon);
    failed.removeIf(isModelIcon);
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it)
        if (!it.value()->directory && isModelIcon(it.key())) {
            const auto index = indexFor(it.key());
            emit dataChanged(index, index, {Qt::DecorationRole});
        }
}
QModelIndex AssetModel::index(int r, int c, const QModelIndex &p) const {
    auto n = node(p);
    if (c != 0 || r < 0 || r >= int(n->children.size()))
        return {};
    return createIndex(r, c, n->children[size_t(r)].get());
}
QModelIndex AssetModel::parent(const QModelIndex &c) const {
    auto n = node(c)->parent;
    if (!n || n == root.get())
        return {};
    return createIndex(n->row, 0, n);
}
int AssetModel::rowCount(const QModelIndex &p) const {
    return p.column() > 0 ? 0 : int(node(p)->children.size());
}
QModelIndex AssetModel::indexFor(const QString &path) const {
    auto n = nodes.value(path, nullptr);
    return !n || n == root.get() ? QModelIndex{} : createIndex(n->row, 0, n);
}
QString AssetModel::pathFor(const QModelIndex &i) const { return node(i)->path; }
bool AssetModel::isDirectory(const QModelIndex &i) const { return node(i)->directory; }
const QIcon &AssetModel::standardIcon(bool directory) const {
    const auto style = QApplication::style();
    if (style != iconStyle) {
        iconStyle = style;
        directoryIcon = style->standardIcon(QStyle::SP_DirIcon);
        fileIcon = style->standardIcon(QStyle::SP_FileIcon);
    }
    return directory ? directoryIcon : fileIcon;
}
AssetModel::RowOrigin AssetModel::origin(const QString &path) const {
    const auto record = sourceIndex.records.constFind(path);
    if (record == sourceIndex.records.cend())
        return ProjectRow;
    if (record->origin == AssetIdentity::Origin::Baseline)
        return BaselineRow;
    return record->shadowsBaseline ? OverrideRow : ProjectRow;
}
QVariant AssetModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid())
        return {};
    auto n = node(i);
    if (role == Qt::DisplayRole)
        return QFileInfo(n->path).fileName();
    if (role == OriginRole)
        return n->directory ? int(ProjectRow) : int(origin(n->path));
    if (role == Qt::ToolTipRole && !baselineVersion.isEmpty() && !n->directory) {
        const auto o = origin(n->path);
        if (o == BaselineRow)
            return n->path + QStringLiteral("\nVanilla Java ") + baselineVersion + " · not in project (read-only baseline)";
        if (o == OverrideRow)
            return n->path + QStringLiteral("\nProject override of vanilla Java ") + baselineVersion;
    }
    if (role == Qt::ToolTipRole || role == Qt::UserRole)
        return n->path;
    if (role == Qt::ForegroundRole) {
        if (baselineVersion.isEmpty() || n->directory || !originForeground)
            return {};
        const auto o = origin(n->path);
        return o == ProjectRow ? QVariant() : originForeground(int(o));
    }
    if (role != Qt::DecorationRole)
        return {};
    if (n->directory)
        return standardIcon(true);
    static const bool noThumbnails = QApplication::arguments().contains("--no-thumbnails");
    if (noThumbnails)
        return standardIcon(false);
    if (auto icon = cache.object(n->path))
        return *icon;
    const auto sources = isModelIcon(n->path) ? modelSources : nullptr;
    if ((sources || n->path.endsWith(".png", Qt::CaseInsensitive)) && !pending.contains(n->path) &&
        !failed.contains(n->path) && pending.size() < 128) {
        // Baseline rows come from the shared read-only cache; project rows from the project.
        const auto record = sourceIndex.records.constFind(n->path);
        const bool cached = record != sourceIndex.records.cend() &&
                            record->origin == AssetIdentity::Origin::Baseline && !record->filesystemPath.isEmpty();
        QString path = n->path, full = cached ? record->filesystemPath : project + '/' + path;
        int gen = generation;
        int revision = modelRevision;
        pending.insert(path);
        pool.start(QRunnable::create([this, path, full, gen, revision, sources] {
            auto recipe = sources ? buildIcon(*sources, path) : IconRecipe{};
            if (!sources)
                recipe.fallback = loadPng(full);
            if (!recipe.fallback.isNull())
                recipe.fallback = recipe.fallback.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            QMetaObject::invokeMethod(
                const_cast<AssetModel *>(this),
                [this, path, gen, revision, fromModel = bool(sources), recipe = std::move(recipe)] {
                    if (gen != generation || (fromModel && revision != modelRevision))
                        return;
                    pending.remove(path);
                    auto im = recipe.fallback;
                    if (fromModel && !thumbnailContextFailed && !recipe.mesh->faces.isEmpty()) {
                        if (!thumbnailCube) {
                            thumbnailMetrics = std::make_unique<Metrics>();
                            thumbnailCube = std::make_unique<Cube>(thumbnailMetrics.get());
                            thumbnailCube->setAttribute(Qt::WA_DontShowOnScreen);
                            thumbnailCube->setAttribute(Qt::WA_ShowWithoutActivating);
                            thumbnailCube->resize(200, 150);
                        }
                        thumbnailCube->setPreviewContext("gui", false, recipe.blockId);
                        thumbnailCube->setModel(recipe.mesh, {}, recipe.materials);
                        if (!thumbnailCube->isValid())
                            thumbnailCube->grabFramebuffer();
                        thumbnailContextFailed = !thumbnailCube->isValid();
                        const auto rendered = thumbnailCube->captureInventoryIcon(4);
                        if (!rendered.isNull())
                            im = rendered;
                    }
                    if (im.isNull()) {
                        if (failed.size() < 512)
                            failed.insert(path);
                        return;
                    }
                    cache.insert(path, new QIcon(QPixmap::fromImage(im)), 64 * 64 * 4);
                    auto idx = indexFor(path);
                    emit const_cast<AssetModel *>(this)->dataChanged(idx, idx,
                                                                     {Qt::DecorationRole});
                },
                Qt::QueuedConnection);
        }));
    }
    return standardIcon(false);
}
} // namespace nova
