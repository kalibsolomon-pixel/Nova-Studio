// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Updates from the Nova Studio GitHub releases. Nova checks for a newer release, downloads its
// installer and checks it against the release's SHA256SUMS.txt, then asks before installing. The
// installer runs after Nova closes and starts Nova again. A copy that Setup did not install (the
// portable ZIP or a build) only offers the release page.
#include "updates.h"
#include <QObject>
#include <QPointer>

class QMainWindow;
class QNetworkAccessManager;
namespace nova {
class Updater : public QObject {
    Q_OBJECT
  public:
    Updater(QMainWindow *window, QString settingsFile);
    // Reports how the last update went, then checks unless the user turned checks off.
    void startup(bool checkEnabled);
    // A check the user asked for also reports "up to date" and errors.
    void check(bool manual);

  private:
    void finishPending();
    void downloaded(const updates::Release &release, bool manual);
    void offer(const updates::Release &release, const QString &installer);
    void install(const updates::Release &release, const QString &installer, bool silent);
    void tell(bool manual, const QString &message, bool problem = false);
    QString folder() const;
    QMainWindow *window;
    QString settingsFile;
    QNetworkAccessManager *network = nullptr;
    bool busy = false;
};
} // namespace nova
