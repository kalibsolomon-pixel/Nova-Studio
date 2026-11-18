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
namespace mc {
class Library;
}
class Cube;
struct Metrics;
// Nova's inventory icon for an item definition, model or blockstate asset: the GUI display
// context at 16 x scale pixels, transparent around the model (the Explorer thumbnail recipe).
// Null when the asset has no geometry. GUI thread only (it renders with OpenGL). animationEntry
// selects the playback entry of animated textures (0 = first, as Explorer shows).
QImage renderInventoryIcon(const mc::Library &library, const QString &asset, int scale, int animationEntry = 0);
// The longest playback (in entries) among the textures this asset's icon uses; 1 when none animates.
int inventoryIconAnimationLength(const mc::Library &library, const QString &asset);
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
    mutable bool thumbnailContextFailed = false;
    // Standard icons come from the platform shell and are rescaled per paint unless
    // the same QIcon is reused. Keep one pair for the current application style.
    mutable const QObject *iconStyle = nullptr;
    mutable QIcon directoryIcon, fileIcon;
    const QIcon &standardIcon(bool directory) const;
    mutable QThreadPool pool;
    std::shared_ptr<const mc::Library> modelSources;
    // Recipes finished on workers wait for their OpenGL capture, which must run on the
    // GUI thread; the drain is bounded per event-loop turn so bursts cannot stall input.
    struct IconQueue;
    mutable std::unique_ptr<IconQueue> iconQueue;
    void scheduleIconDrain() const;
    void drainIconQueue() const;
    mutable std::unique_ptr<Metrics> thumbnailMetrics;
    mutable std::unique_ptr<Cube> thumbnailCube;
    int modelRevision = 0;
    QString project;
    Index sourceIndex;
    QString category;
    // Vanilla baseline overlay: empty for legacy projects (no extra roles, unchanged rows).
    QString baselineVersion;
    std::function<QVariant(int origin)> originForeground;
    // Thumbnail pipeline telemetry for benchmarks: model icons dropped by invalidation,
    // OpenGL captures performed, GUI-thread nanoseconds spent capturing, the longest
    // single capture and the longest consecutive GUI-thread icon stall.
    struct IconStats {
        int invalidated = 0, captured = 0;
        qint64 captureNs = 0, longestCaptureNs = 0, longestStallNs = 0;
    };
    mutable IconStats iconCounters;
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
    void setModelSources(const std::shared_ptr<mc::Library> &library);
    void setCategory(const QString &value);
    int cachedBytes() const { return cache.totalCost(); }
    int pendingCount() const { return pending.size(); }
    IconStats iconStats() const { return iconCounters; }
    void resetIconStats() const;
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
