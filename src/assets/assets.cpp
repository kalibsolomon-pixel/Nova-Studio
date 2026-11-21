// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets.h"
#include "core.h"
#include "cube.h"
#include "display.h"
#include "minecraft_animation.h"
#include "render_layers.h"
#include "tints.h"
#include <QApplication>
#include <QFile>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QRunnable>
#include <QStyle>
#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
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
// Decoded PNGs shared by every icon on every worker: one block texture such as
// block/stone.png is used by hundreds of icons. An entry is only reused while the file's
// size and modification time are unchanged. QImage is implicitly shared, so handing one
// out copies nothing until somebody writes to it.
struct CachedPng {
    qint64 size = 0, modified = 0;
    QImage image;
};
QMutex &pngLock() {
    static QMutex lock;
    return lock;
}
QCache<QString, CachedPng> &pngCache() {
    static QCache<QString, CachedPng> cache(64 * 1024 * 1024); // bytes of pixel data
    return cache;
}
QImage cachedPng(const QString &path, bool decode) {
    const QFileInfo info(path);
    const qint64 size = info.size(), modified = info.lastModified().toMSecsSinceEpoch();
    {
        QMutexLocker lock(&pngLock());
        if (const auto *hit = pngCache().object(path); hit && hit->size == size && hit->modified == modified)
            return hit->image;
    }
    if (!decode)
        return {};
    auto image = loadPng(path);
    if (!image.isNull()) {
        QMutexLocker lock(&pngLock());
        pngCache().insert(path, new CachedPng{size, modified, image},
                          int(qMin<qsizetype>(image.sizeInBytes(), std::numeric_limits<int>::max())));
    }
    return image;
}
// The animation of a texture from any source. The baseline cache stores files under their hashes,
// so the .png.mcmeta is found through the library, never beside the PNG.
std::shared_ptr<Document> textureAnimation(const mc::Library &library, const QString &texture,
                                           const QString &file) {
    const auto metadata = library.assetPath(texture + ".mcmeta");
    QFile meta(metadata);
    if (metadata.isEmpty() || !meta.open(QIODevice::ReadOnly) || meta.size() > 512 * 1024)
        return {};
    const auto json = QJsonDocument::fromJson(meta.readAll()).object();
    if (!json.contains("animation"))
        return {};
    const auto sheet = cachedPng(file, true).convertToFormat(QImage::Format_RGBA8888);
    QImage placeholder(1, 1, QImage::Format_RGBA8888);
    placeholder.fill(Qt::transparent);
    auto document = std::make_shared<Document>(file, placeholder);
    return !sheet.isNull() && document->importAnimation(sheet, json, library.version()) ? document : nullptr;
}
// animationEntry picks the playback entry of animated textures (wrapping per texture); Explorer
// uses the first.
// The library is only read (modelConst() is thread-safe), so worker threads share one
// snapshot instead of each copying it.
IconRecipe buildIcon(const mc::Library &library, const QString &asset, int animationEntry = 0) {
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
            // A decoded copy already in the cache answers the size check without reading
            // the header again.
            const auto known = cachedPng(path, false);
            const auto size = known.isNull() ? QImageReader(path, "png").size() : known.size();
            const auto pixels = qint64(size.width()) * size.height();
            if (size.isValid() && pixels <= pixelsLeft) {
                pixelsLeft -= pixels;
                if (auto animation = textureAnimation(library, texture, path)) {
                    const auto &entries = animation->animationState().entries;
                    image = animation->frameComposite(entries[qMax(0, animationEntry) % entries.size()].frame);
                }
                if (image.isNull())
                    image = cachedPng(path, true);
            }
        }
        if (result.fallback.isNull() && !image.isNull())
            result.fallback = image;
        result.materials.insert(texture, image);
    };
    for (const auto &instance : instances) {
        const auto model = library.modelConst(instance.model);
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
// Recipes finished on worker threads wait here for their OpenGL capture, which must
// run on the GUI thread. Rows being painted promote their entry to the front so the
// visible page fills first while a large rebuild works through the backlog.
struct AssetModel::IconQueue {
    struct Entry {
        QString path;
        int generation = 0, revision = 0;
        bool fromModel = false;
        IconRecipe recipe;
    };
    std::deque<Entry> ready;
    bool drainScheduled = false;
    void push(Entry entry) { ready.push_back(std::move(entry)); }
    void promote(const QString &path) {
        for (int i = int(ready.size()) - 1; i >= 0; --i)
            if (ready[size_t(i)].path == path) {
                auto entry = std::move(ready[size_t(i)]);
                ready.erase(ready.begin() + i);
                ready.push_front(std::move(entry));
                return;
            }
    }
    void forget(const QString &path) {
        ready.erase(std::remove_if(ready.begin(), ready.end(),
                                   [&](const Entry &entry) { return entry.path == path; }),
                    ready.end());
    }
};
int inventoryIconAnimationLength(const mc::Library &library, const QString &asset) {
    int length = 1;
    for (const auto &texture : buildIcon(library, asset).materials.keys()) {
        const auto path = library.assetPath(texture);
        if (!path.isEmpty())
            if (auto animation = textureAnimation(library, texture, path))
                length = qMax(length, int(animation->animationState().entries.size()));
    }
    return length;
}
QImage renderInventoryIcon(const mc::Library &library, const QString &asset, int scale, int animationEntry) {
    const auto recipe = buildIcon(library, asset, animationEntry);
    if (recipe.mesh->faces.isEmpty())
        return {};
    // One hidden renderer serves every call: creating an OpenGL context per icon dominated large
    // comparisons. It ends with the application.
    static Metrics metrics;
    static QPointer<Cube> cube;
    if (!cube) {
        cube = new Cube(&metrics);
        cube->setAttribute(Qt::WA_DontShowOnScreen);
        cube->setAttribute(Qt::WA_ShowWithoutActivating);
        cube->resize(200, 150);
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, cube, &QObject::deleteLater);
    }
    cube->setPreviewContext("gui", false, recipe.blockId);
    cube->setModel(recipe.mesh, {}, recipe.materials);
    if (!cube->isValid())
        cube->grabFramebuffer(); // creates the context
    return cube->captureInventoryIcon(scale);
}
void AssetModel::describe(Node &node) {
    node.name = node.path.mid(node.path.lastIndexOf('/') + 1);
    node.modelIcon = !node.directory && isModelIcon(node.path);
}
void AssetModel::emitChanged(Node *parent, const std::function<bool(const Node &)> &affected,
                             const QList<int> &roles) {
    const int count = int(parent->children.size());
    for (int row = 0, start = -1; row <= count; ++row) {
        const bool hit = row < count && affected(*parent->children[size_t(row)]);
        if (hit && start < 0)
            start = row;
        if (!hit && start >= 0) {
            emit dataChanged(createIndex(start, 0, parent->children[size_t(start)].get()),
                             createIndex(row - 1, 0, parent->children[size_t(row - 1)].get()), roles);
            start = -1;
        }
        if (row < count && !parent->children[size_t(row)]->children.empty())
            emitChanged(parent->children[size_t(row)].get(), affected, roles);
    }
}
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
    describe(*n);
    nodes[path] = n.get();
    if (n->directory) {
        auto children = idx.directories.value(path);
        if (path.isEmpty() && !category.isEmpty()) {
            children.clear();
            for (auto it = idx.records.cbegin(); it != idx.records.cend(); ++it)
                if (it->category == category)
                    children.append(it.key());
        }
        // Folders first, then by path; each child is classified once, not per comparison.
        QStringList folders, files;
        for (const auto &child : children)
            (idx.directories.contains(child) ? folders : files).append(child);
        folders.sort();
        files.sort();
        int r = 0;
        for (const auto &p : folders)
            n->children.push_back(build(idx, p, n.get(), r++));
        for (const auto &p : files)
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
            const auto slash = path.lastIndexOf('/');
            const auto parentPath = slash < 0 ? QString() : path.left(slash);
            auto p = nodes.value(parentPath, nullptr);
            if (!p)
                continue;
            bool directory = idx.directories.contains(path);
            // Siblings stay ordered (directories first, then by path), so the slot is a
            // binary search rather than a walk of the whole folder.
            const int row = int(std::partition_point(p->children.begin(), p->children.end(),
                                                     [&](const std::unique_ptr<Node> &sibling) {
                                                         return !(directory != sibling->directory
                                                                      ? directory
                                                                      : path < sibling->path);
                                                     }) -
                                p->children.begin());
            beginInsertRows(indexFor(parentPath), row, row);
            auto n = std::make_unique<Node>();
            n->path = path;
            n->directory = directory;
            describe(*n);
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
    const bool everything = exact.contains("");
    QStringList directoryPrefixes;
    for (const auto &path : changed)
        if (idx.directories.contains(path))
            directoryPrefixes.append(path + '/');
    auto invalidated = [&](const QString &path) {
        if (everything || exact.contains(path)) return true;
        for (const auto &prefix : directoryPrefixes)
            if (path.startsWith(prefix)) return true;
        return false;
    };
    if (everything) {
        cache.clear();
        failed.clear();
    } else {
        // setCategory() retains the bounded cache for quick navigation. Entries
        // outside the visible node set still need content invalidation.
        for (const auto &path : cache.keys())
            if (invalidated(path)) cache.remove(path);
        failed.removeIf(invalidated);
    }
    // Rows that changed: the changed nodes and, for a changed folder, its whole subtree,
    // found through the tree instead of testing every node's path against every folder.
    QSet<const Node *> affected;
    if (!everything) {
        std::function<void(const Node *)> subtree = [&](const Node *n) {
            affected.insert(n);
            for (const auto &child : n->children)
                subtree(child.get());
        };
        for (const auto &path : changed)
            if (const auto *n = nodes.value(path, nullptr)) {
                if (idx.directories.contains(path))
                    subtree(n);
                else
                    affected.insert(n);
            }
    }
    emitChanged(root.get(),
                [&](const Node &n) { return everything || affected.contains(&n); },
                {Qt::DecorationRole, Qt::ForegroundRole, Qt::ToolTipRole, OriginRole});
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
    // A whole-library change (project switch, version change, resource-stack reload)
    // flushes every model icon. A scoped update drops only icons whose recipe reads a
    // changed asset: the asset itself plus everything reaching it over reverse
    // dependency edges (children, parents' other users, item and blockstate definitions).
    QSet<QString> stale;
    // A library without any models is fresh (project open/close hands one over before
    // the first index update lands); flush fully as the previous behavior did.
    const bool full = !modelSources || !modelSources->hasModels() ||
                      modelSources->lastUpdateChanged().contains(QString());
    if (!full)
        for (const auto &path : modelSources->lastUpdateChanged()) {
            stale.insert(path);
            const auto users = modelSources->dependents(path);
            stale.unite(QSet<QString>(users.cbegin(), users.cend()));
        }
    if (full) {
        for (const auto &path : cache.keys())
            if (isModelIcon(path)) {
                cache.remove(path);
                ++iconCounters.invalidated;
            }
        pending.removeIf(isModelIcon);
        failed.removeIf(isModelIcon);
        if (iconQueue)
            iconQueue->ready.clear();
    } else
        for (const auto &path : stale)
            if (isModelIcon(path)) {
                if (cache.remove(path))
                    ++iconCounters.invalidated;
                pending.remove(path);
                failed.remove(path);
                if (iconQueue)
                    iconQueue->forget(path);
            }
    emitChanged(root.get(),
                [&](const Node &n) { return n.modelIcon && (full || stale.contains(n.path)); },
                {Qt::DecorationRole});
}
void AssetModel::resetIconStats() const { iconCounters = {}; }
void AssetModel::scheduleIconDrain() const {
    if (iconQueue->drainScheduled)
        return;
    iconQueue->drainScheduled = true;
    QMetaObject::invokeMethod(const_cast<AssetModel *>(this), [this] { drainIconQueue(); },
                              Qt::QueuedConnection);
}
// Capture finished recipes in bounded turns: at most a handful of icons, or roughly the
// time budget once at least one icon was captured, then the event loop gets control
// back and the next turn continues. A turn always processes at least one entry so the
// queue keeps draining even when a single capture is slow (first context creation).
void AssetModel::drainIconQueue() const {
    if (!iconQueue)
        return;
    iconQueue->drainScheduled = false;
    QElapsedTimer turn;
    turn.start();
    int finished = 0;
    while (!iconQueue->ready.empty()) {
        if (finished >= 8 || (finished > 0 && turn.elapsed() >= 6))
            break;
        auto entry = std::move(iconQueue->ready.front());
        iconQueue->ready.pop_front();
        ++finished;
        if (entry.generation != generation || (entry.fromModel && entry.revision != modelRevision))
            continue; // superseded by a flush; a newer request may already be running
        pending.remove(entry.path);
        auto im = entry.recipe.fallback;
        if (entry.fromModel && !thumbnailContextFailed && !entry.recipe.mesh->faces.isEmpty()) {
            QElapsedTimer capture;
            capture.start();
            if (!thumbnailCube) {
                thumbnailMetrics = std::make_unique<Metrics>();
                thumbnailCube = std::make_unique<Cube>(thumbnailMetrics.get());
                thumbnailCube->setAttribute(Qt::WA_DontShowOnScreen);
                thumbnailCube->setAttribute(Qt::WA_ShowWithoutActivating);
                thumbnailCube->resize(200, 150);
            }
            thumbnailCube->setPreviewContext("gui", false, entry.recipe.blockId);
            thumbnailCube->setModel(entry.recipe.mesh, {}, entry.recipe.materials);
            if (!thumbnailCube->isValid())
                thumbnailCube->grabFramebuffer();
            thumbnailContextFailed = !thumbnailCube->isValid();
            const auto rendered = thumbnailCube->captureInventoryIcon(4);
            const auto ns = capture.nsecsElapsed();
            iconCounters.captureNs += ns;
            iconCounters.longestCaptureNs = qMax(iconCounters.longestCaptureNs, ns);
            ++iconCounters.captured;
            if (!rendered.isNull())
                im = rendered;
        }
        if (im.isNull()) {
            if (failed.size() < 512)
                failed.insert(entry.path);
            continue;
        }
        cache.insert(entry.path, new QIcon(QPixmap::fromImage(im)), 64 * 64 * 4);
        const auto index = indexFor(entry.path);
        emit const_cast<AssetModel *>(this)->dataChanged(index, index, {Qt::DecorationRole});
    }
    iconCounters.longestStallNs = qMax(iconCounters.longestStallNs, turn.nsecsElapsed());
    if (!iconQueue->ready.empty())
        scheduleIconDrain();
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
        return n->name;
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
    // The row is being painted; its finished recipe should be captured before the rest.
    if (iconQueue && pending.contains(n->path))
        iconQueue->promote(n->path);
    const auto sources = n->modelIcon ? modelSources : nullptr;
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
                    if (!iconQueue)
                        iconQueue = std::make_unique<IconQueue>();
                    iconQueue->push(IconQueue::Entry{path, gen, revision, fromModel, std::move(recipe)});
                    scheduleIconDrain();
                },
                Qt::QueuedConnection);
        }));
    }
    return standardIcon(false);
}
} // namespace nova
