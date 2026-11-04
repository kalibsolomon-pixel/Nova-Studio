// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// "Test in Minecraft": hands the saved resource pack to the official
// Minecraft Launcher or Aurora Launcher, which sign the player in, verify
// Java Edition ownership and start the game. Nova handles no credentials.
#include "discovery.h"
#include "gamelog.h"
#include <QDialog>
#include <functional>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QNetworkAccessManager;
namespace nova {

struct TestTarget {
    testing::LauncherKind kind = testing::LauncherKind::Official;
    QString key; // stable id remembered in preferences
    QString label, gameDirectory, executable, versionId, instanceName;
    bool modpackManaged = false, modded = false;
};

class MinecraftTestDialog : public QDialog {
    Q_OBJECT
  public:
    struct Context {
        std::function<QString()> projectRoot, packName;
        std::function<QStringList()> unsavedDocuments;
        std::function<void(const QString &)> activity;
        QString *rememberedTarget = nullptr;
        testing::Environment environment = testing::Environment::current();
        bool interactive = true; // false in automated tests: no message boxes
    };
    MinecraftTestDialog(Context context, QWidget *parent = nullptr);
    void rescan();
    bool syncPack(); // returns success
    const QList<TestTarget> &targets() const { return available; }
    testing::GameLogMonitor *monitor() const { return log; }
    QString packEntry() const;
    QString versionForPack() const;

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    const TestTarget *selected() const;
    void selectionChanged();
    void refreshPackStatus();
    void refreshGameStatus();
    void openLauncher();
    bool prepareOfficialInstallation(const TestTarget &target);
    void getLauncher(bool aurora);
    bool ask(const QString &title, const QString &text);
    void inform(const QString &title, const QString &text, bool warning = false);

    Context context;
    QList<TestTarget> available;
    QList<testing::Launcher> launchers;
    QComboBox *targetBox;
    QLabel *targetDetail, *packStatus, *gameStatus, *guidance;
    QListWidget *findings;
    QPushButton *syncButton, *launchButton, *folderButton, *getOfficial, *getAurora;
    testing::GameLogMonitor *log;
    QNetworkAccessManager *network = nullptr;
    bool busy = false;
};

} // namespace nova
