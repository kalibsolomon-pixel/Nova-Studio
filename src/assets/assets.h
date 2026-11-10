// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "index.h"
#include <QAbstractItemModel>
#include <QCache>
#include <QIcon>
#include <QSet>
#include <QThreadPool>
#include <functional>
#include <memory>
namespace nova {
class AssetModel : public QAbstractItemModel {
    Q_OBJECT
    struct Node {
        QString path;
        bool directory;
        Node *parent = nullptr;
        int row = 0;
        std::vector<std::unique_ptr<Node>> children;
    };
    std::unique_ptr<Node> root;
    QMap<QString, Node *> nodes;
    mutable QCache<QString, QIcon> cache{4 * 1024 * 1024};
    mutable QSet<QString> pending, failed;
    // Standard icons come from the platform shell and are rescaled per paint unless
    // the same QIcon is reused. Keep one pair for the current application style.
    mutable const QObject *iconStyle = nullptr;
    mutable QIcon directoryIcon, fileIcon;
    const QIcon &standardIcon(bool directory) const;
    mutable QThreadPool pool;
    QString project;
    Index sourceIndex;
    QString category;
    // Vanilla baseline overlay: empty for legacy projects (no extra roles, unchanged rows).
    QString baselineVersion;
    std::function<QVariant(int origin)> originForeground;
    int generation = 0;
    Node *node(const QModelIndex &index) const;
    std::unique_ptr<Node> build(const Index &index, QString path, Node *parent, int row);

  public:
    // OriginRole: RowOrigin as int (Project for every row of a legacy project).
    enum Roles { OriginRole = Qt::UserRole + 1 };
    enum RowOrigin { ProjectRow = 0, OverrideRow = 1, BaselineRow = 2 };
    explicit AssetModel(QObject *parent = nullptr);
    // Version shown in baseline tooltips; empty turns the overlay styling off.
    void setBaselineVersion(const QString &version) { baselineVersion = version; }
    // Paint-time foreground per RowOrigin (the ui layer supplies live theme tokens).
    void setOriginForeground(std::function<QVariant(int origin)> style) { originForeground = std::move(style); }
    RowOrigin origin(const QString &path) const;
    bool hasDirectory(const QString &path) const { return sourceIndex.directories.contains(path); }
    ~AssetModel() override;
    void setIndex(const Index &index, const QString &rootPath);
    void applyIndex(const Index &index, const QStringList &changed);
    void setCategory(const QString &value);
    int cachedBytes() const { return cache.totalCost(); }
    int pendingCount() const { return pending.size(); }
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
