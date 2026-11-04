// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
namespace nova {
struct PlaybackPosition { int entry = -1, next = -1; double fraction = 0; bool ended = false; };
class AnimationSequence {
    std::vector<qint64> boundaries;
  public:
    bool set(const AnimationState &state);
    PlaybackPosition at(double milliseconds, bool loop = true) const;
    double entryMilliseconds(int index) const;
    double nextChangeMilliseconds(double milliseconds, bool interpolate) const;
    qint64 ticks() const { return boundaries.empty() ? 0 : boundaries.back(); }
};
class PlaybackController : public QObject {
    Q_OBJECT
    std::shared_ptr<Document> document;
    AnimationState state;
    AnimationSequence sequence;
    QTimer timer;
    QElapsedTimer elapsed;
    double offset = 0, multiplier = 1;
    bool looping = true, running = false, showing = false;
    int lastEntry = -1, lastTick = -1;
    quint64 serial = 0;
    void present();
  public:
    explicit PlaybackController(QObject *parent = nullptr);
    void setDocument(std::shared_ptr<Document> doc);
    void play();
    void pause();
    void stop();
    void seek(int entry);
    void setLoop(bool value) { looping = value; }
    void setSpeed(double value);
    bool playing() const { return running; }
    bool previewing() const { return showing; }
    double positionMilliseconds() const;
    bool timerActive() const { return timer.isActive(); }
  signals:
    void displayed(QImage image, int entry);
    void stopped();
    void statusChanged();
};
} // namespace nova
