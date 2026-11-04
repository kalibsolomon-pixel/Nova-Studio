// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Follows Minecraft's own logs/latest.log in a test game directory. This is
// how Nova observes a game it did not start: it never inspects the game
// process (its command line carries the session token). Lines are redacted
// before they are stored or shown.
#include <QDateTime>
#include <QObject>
#include <QStringList>

class QTimer;
namespace nova::testing {

enum class LogEvent {
    None,
    PlayerSet,
    ResourceReload,
    PackActive,
    ResourcesLoaded,
    ReloadFinished, // confirmed by Remove Reloading Screen
    PackWarning,
    Error,
    Stopped
};

struct LogFinding {
    LogEvent event = LogEvent::None;
    QString text;
};

QString redactLogLine(QString line);
// packEntry is Minecraft's identifier for the pack, e.g. "file/Nova Test - X".
LogFinding classifyLogLine(const QString &line, const QString &packEntry);

struct GameState {
    bool running = false, packActive = false, resourcesLoaded = false, crashed = false;
    bool reloadFinished = false; // RRLS reported the end of the latest reload
    QString player, lastReload;
    QStringList warnings; // bounded, redacted
    int reloads = 0;
};

class GameLogMonitor : public QObject {
    Q_OBJECT
  public:
    explicit GameLogMonitor(QObject *parent = nullptr);
    // Reconstructs state from the existing log, then follows appended lines.
    void watch(const QString &gameDirectory, const QString &packEntry);
    void stop();
    bool active() const;
    const GameState &state() const { return current; }
    QString gameDirectory() const { return directory; }
    void poll(); // public for deterministic tests

  signals:
    void changed();

  private:
    void consume(const QString &line);
    void reset();
    QTimer *timer;
    QString directory, entry;
    QByteArray partial; // bytes of an incomplete trailing line
    QByteArray head;    // first bytes of the file being read: detects replacement
    qint64 offset = 0;
    QDateTime birth;
    QStringList knownCrashes;
    GameState current;
};

} // namespace nova::testing
