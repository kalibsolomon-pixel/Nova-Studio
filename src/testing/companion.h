// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Nova's side of the companion mod in its managed test client
// (assets/fabric-runtime/companion). The channel is a folder in Nova's own test
// instance, <game>/nova-companion: the mod announces itself in companion.json,
// Nova drops request-<id>.json, the mod deletes it when taken and answers in
// response-<id>.json. Every file names the launch id Nova passed to the game, so
// nothing from an earlier launch is trusted. There is no network listener.
#include <QJsonObject>
#include <QObject>
#include <QStringList>

class QTimer;
namespace nova::testing {

inline constexpr int companionProtocol = 1;
inline constexpr auto companionFolder = "nova-companion";

QString newLaunchId(); // 32 lowercase hex digits

class CompanionLink : public QObject {
    Q_OBJECT
  public:
    explicit CompanionLink(QObject *parent = nullptr);
    // Follows the companion of one launch; an empty launch id detaches.
    void attach(const QString &gameDirectory, const QString &launchId);
    // Removes stale channel files before a new launch (the folder is Nova's alone).
    static void reset(const QString &gameDirectory);
    QString launchId() const { return launch; }
    // The running game loaded the companion for this launch.
    bool available() const;
    bool reloading() const { return !pending.isEmpty() && pendingType == "reload"; }
    bool busy() const { return !pending.isEmpty(); }
    // Asks the game to reload its resource packs (F3+T). False with a reason when it cannot.
    bool requestReload(QString *error = nullptr);
    // Asks the game to draw these items' inventory icons (1 to 64 ids such as minecraft:stone) in
    // slots at its current GUI scale and save one PNG per item.
    bool requestIcons(const QStringList &items, QString *error = nullptr);
    void setTimeout(int milliseconds) { timeoutMs = milliseconds; }
    void poll(); // public for deterministic tests

  signals:
    void reloadStarted();
    void reloadFinished(bool ok, qint64 durationMs, const QString &error);
    // result: guiScale, background (#rrggbb), missing (unknown ids), files (absolute PNG path per
    // requested item, in order; empty for a missing one).
    void iconsFinished(bool ok, const QJsonObject &result, const QString &error);

  private:
    bool send(const QString &type, QJsonObject request, int timeout, QString *error);
    void finish(bool ok, qint64 durationMs, const QString &error, QJsonObject result = {});
    QString path(const QString &name) const;
    QTimer *timer;
    QString game, launch, pending, pendingType;
    int pendingItems = 0, pendingTimeout = 0;
    bool taken = false;
    qint64 sentMs = 0;
    int timeoutMs = 120000;
};

} // namespace nova::testing
