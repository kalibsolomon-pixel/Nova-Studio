// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "playback.h"
#include "session.h"
#include <QAbstractListModel>
#include <QCache>
#include <QListView>
#include <QWidget>
class QSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;
namespace nova {
struct Metrics;
class TimelineModel : public QAbstractListModel {
    Q_OBJECT
    std::shared_ptr<Document> document;
    std::vector<PlaybackEntry> entries;
    std::vector<std::pair<QUuid, QString>> frames;
    int defaultTicks = 1, thumbnailSize = 56, cursor = -1;
    mutable QCache<QString, QPixmap> thumbnails{8 * 1024 * 1024};
  public:
    Metrics *metrics = nullptr;
    explicit TimelineModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    bool sync(const std::shared_ptr<Document> &doc);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indices) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }
    void setThumbnailSize(int size);
    void setCursor(int index);
    qint64 thumbnailBytes() const { return thumbnails.totalCost(); }
    void setThumbnailBudget(int bytes) { thumbnails.setMaxCost(qBound(0, bytes, 64 * 1024 * 1024)); }
    QUuid frameAt(int row) const;
  signals:
    void moveRequested(int from, int to);
};
class TimelinePanel : public QWidget {
    Q_OBJECT
    DocumentSession *session;
    std::shared_ptr<Document> document;
    quint64 syncedSerial = 0;
    TimelineModel *model;
    QListView *view;
    QComboBox *sources;
    QSpinBox *duration, *defaultTime;
    QCheckBox *interpolate;
    QLabel *timing;
    bool syncing = false;
    std::optional<SourceFrame> clipboard;
    void apply(AnimationState state, const QString &label);
    void selected(const QModelIndex &index);
    void contextMenu(QPoint point);
  protected:
    void hideEvent(QHideEvent *event) override;
  public:
    PlaybackController playback;
    explicit TimelinePanel(DocumentSession *session, QWidget *parent = nullptr);
    void setDocument(std::shared_ptr<Document> doc);
    void frameCommand(FrameCommand command);
    void navigate(int delta, bool absolute = false);
    void moveEntry(int from, int to);
    void duplicateEntry();
    void removeEntries();
    std::vector<QUuid> selectedFrames() const;
    QJsonObject viewSettings() const;
    void setViewSettings(const QJsonObject &settings);
    void setThumbnailBudget(int bytes) { model->setThumbnailBudget(bytes); }
    void setMetrics(Metrics *metrics) { model->metrics = metrics; }
    QListView *timelineView() const { return view; }
  signals:
    void beforeContextChange();
    void failure(QString message);
    void onionChanged(bool enabled, int previous, int next, int opacity, bool tint);
    void propertiesRequested();
    void scopeOperationRequested();
};
} // namespace nova
