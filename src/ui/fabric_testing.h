// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// "Test in Minecraft" (Phase 3-D.1): Nova's own Fabric development client.
#include "fabric_runtime.h"
#include "gamelog.h"
#include <QDateTime>
#include <QDialog>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QWidget;
namespace nova {
struct Preferences;
namespace mc {
class Library;
}

class FabricTestDialog : public QDialog {
    Q_OBJECT
  public:
    struct Context {
        std::function<QString()> projectRoot, packName;
        std::function<QStringList()> unsavedDocuments;
        std::function<void(const QString &)> activity;
        Preferences *preferences = nullptr;
        std::function<std::shared_ptr<mc::Library>()> modelLibrary; // for Compare Icons
        testing::FabricRuntime::Options runtime; // base options (tests override)
        bool interactive = true;
    };
    explicit FabricTestDialog(Context context, QWidget *parent = nullptr);

    // Ctrl+Shift+M: prepare on first use, then sync and launch; when the game
    // already runs, sync only.
    void testInMinecraft();
    // Ctrl+Alt+M: export and sync without launching. Returns success.
    bool syncPack();
    // Asks the running game to reload its resources through Nova's companion mod (F3+T).
    bool reloadInGame(QString *error = nullptr);
    testing::FabricRuntime *runtime() const { return engine; }
    testing::GameLogMonitor *monitor() const { return log; }
    QString packFolder() const;
    static QString testWorldName() { return "Nova Test World"; }
    bool testWorldExists() const;
    // Nova shutdown: false = the user cancelled closing Nova.
    bool confirmShutdown();

  signals:
    void reloadReported(const QString &message); // for the main window's status bar

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    void rebuildRuntime();
    void refresh();
    void continueAfterPreparation(bool ok);
    void launchAfterSync();
    void enablePackOnce();
    bool ask(const QString &title, const QString &text);
    void inform(const QString &title, const QString &text, bool warning = false);
    void updateMonitoring();
    void fitToScreen();
    void companionReloadFinished(bool ok, qint64 durationMs, const QString &error);

    Context context;
    testing::FabricRuntime *engine = nullptr;
    testing::GameLogMonitor *log;
    bool pendingLaunch = false;
    QDateTime lastSync;
    QString reloadNote; // Nova's own reload request, shown before the log's account
    QLabel *versionValue, *runtimeValue, *rrlsValue, *gameValue, *syncValue, *reloadValue,
        *guidance;
    QProgressBar *progress;
    QListWidget *findings;
    QPushButton *testButton, *syncButton, *reloadButton, *compareButton, *stopButton, *folderButton, *logsButton, *advancedButton,
        *repairButton, *javaButton, *locationButton;
    QWidget *advanced;
    QComboBox *versionBox;
    QLabel *javaValue, *locationValue;
    QCheckBox *detailedLogs, *openWorld, *autoReload;
};

} // namespace nova
