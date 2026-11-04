#pragma once
#include "core.h"
#include <QAbstractItemModel>
#include <QCache>
#include <QIcon>
#include <QSet>
#include <QThreadPool>
#include <memory>
namespace nova {
class AssetModel : public QAbstractItemModel {
    struct Node {
        QString path;
        bool directory;
        Node *parent = nullptr;
        int row = 0;
        std::vector<std::unique_ptr<Node>> children;
    };
    std::unique_ptr<Node> root;
    QMap<QString, Node *> nodes;
    mutable QCache<QString, QIcon> cache{256};
    mutable QSet<QString> pending, failed;
    mutable QThreadPool pool;
    QString project;
    int generation = 0;
    Node *node(const QModelIndex &index) const;
    std::unique_ptr<Node> build(const Index &index, QString path, Node *parent, int row);

  public:
    explicit AssetModel(QObject *parent = nullptr);
    ~AssetModel() override;
    void setIndex(const Index &index, const QString &rootPath);
    QModelIndex indexFor(const QString &path) const;
    QString pathFor(const QModelIndex &index) const;
    bool isDirectory(const QModelIndex &index) const;
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex & = {}) const override { return 1; }
    QVariant data(const QModelIndex &index, int role) const override;
};
} // namespace nova
