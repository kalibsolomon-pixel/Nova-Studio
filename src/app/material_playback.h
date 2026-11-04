// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "playback.h"
#include <QMap>
namespace nova {
class MaterialPlayback : public QObject {
    Q_OBJECT
    struct Material {
        std::shared_ptr<Document> document;
        AnimationState state;
        AnimationSequence sequence;
        quint64 serial = 0;
        int entry = -1, tick = -1;
        QImage image;
    };
    QMap<QString, Material> materials;
    QTimer timer;
    QElapsedTimer elapsed;
    double offset = 0;
    bool requested = false, visible = false, running = false;
    void present();
    void synchronize(Material &);

  public:
    explicit MaterialPlayback(QObject *parent = nullptr);
    void setMaterials(const QMap<QString, std::shared_ptr<Document>> &documents);
    void updateDocument(const QString &asset, std::shared_ptr<Document> document);
    void setVisible(bool value);
    void play();
    void pause();
    void stop();
    void seek(double milliseconds);
    bool playing() const { return requested; }
    bool timerActive() const { return timer.isActive(); }
    int count() const { return materials.size(); }
    double positionMilliseconds() const;
    QImage image(const QString &asset) const { return materials.value(asset).image; }
  signals:
    void frame(QString asset, QImage image);
    void statusChanged();
};
} // namespace nova
