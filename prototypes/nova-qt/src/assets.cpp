#include "assets.h"
#include <QApplication>
#include <QFileInfo>
#include <QMetaObject>
#include <QRunnable>
#include <QStyle>
namespace nova {
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
    project = path;
    nodes.clear();
    root = build(idx, "", nullptr, 0);
    cache.clear();
    pending.clear();
    failed.clear();
    endResetModel();
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
QVariant AssetModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid())
        return {};
    auto n = node(i);
    if (role == Qt::DisplayRole)
        return QFileInfo(n->path).fileName();
    if (role == Qt::ToolTipRole || role == Qt::UserRole)
        return n->path;
    if (role != Qt::DecorationRole)
        return {};
    if (n->directory)
        return QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    if (auto icon = cache.object(n->path))
        return *icon;
    if (!pending.contains(n->path) && !failed.contains(n->path) && pending.size() < 128) {
        QString path = n->path, full = project + '/' + path;
        int gen = generation;
        pending.insert(path);
        pool.start(QRunnable::create([this, path, full, gen] {
            auto im = loadPng(full).scaled(64, 64, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            QMetaObject::invokeMethod(
                const_cast<AssetModel *>(this),
                [this, path, gen, im] {
                    if (gen != generation)
                        return;
                    pending.remove(path);
                    if (im.isNull()) {
                        failed.insert(path);
                        return;
                    }
                    cache.insert(path, new QIcon(QPixmap::fromImage(im)));
                    auto idx = indexFor(path);
                    emit const_cast<AssetModel *>(this)->dataChanged(idx, idx,
                                                                     {Qt::DecorationRole});
                },
                Qt::QueuedConnection);
        }));
    }
    return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
}
} // namespace nova
