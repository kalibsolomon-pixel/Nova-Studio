// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Nova's managed Fabric development runtime: provisions pinned components,
// generates and verifies the Loom workspace, prepares it with Gradle, then
// launches and supervises `runClient` (Minecraft in Loom's development
// environment). This client is not an authenticated launcher: it proves
// nothing about Minecraft ownership and is meant for local pack testing.
#include "discovery.h"
#include "fabric_profile.h"
#include "fabric_workspace.h"
#include "installer.h"
#include "java_locator.h"
#include "process_tree.h"
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcessEnvironment>
#include <atomic>
#include <memory>
#include <optional>

class QProcess;
class QLockFile;
class QTimer;
class QWinEventNotifier;
class QNetworkAccessManager;
namespace nova::testing {

DownloadPolicy runtimePolicy();

class FabricRuntime : public QObject {
    Q_OBJECT
  public:
    enum class State {
        NotConfigured,
        Checking,
        Downloading,
        Preparing,
        GeneratingRunConfig,
        Ready,
        Launching,
        Running,
        Stopping,
        Failed
    };
    enum class Game {
        Idle,
        Starting,
        GradlePreparing,
        MinecraftLaunching,
        MinecraftRunning,
        Exited,
        LaunchFailed,
        CancellationRequested
    };
    struct Options {
        QString base; // empty = default under %LOCALAPPDATA%
        QString minecraftVersion = defaultMinecraftVersion();
        QString chosenGameJava; // user-selected JDK home (validated)
        bool detailedLogs = false;
        Environment environment = Environment::current();
        DownloadPolicy policy = runtimePolicy();
        std::optional<FabricProfile> profile; // tests: a custom profile
        // Tests: replaces "java -classpath wrapper GradleWrapperMain".
        QString processProgram;
        QStringList processPrefix;
        int stopGraceMs = 30000;
    };
    struct Components {
        QString gameJava, gradleJava;
        QHash<QString, QString> loadedMods; // from Fabric Loader's own mod list
    };

    explicit FabricRuntime(Options options, QObject *parent = nullptr);
    ~FabricRuntime() override;

    State state() const { return current; }
    Game game() const { return phase; }
    QString status() const { return detail; }
    QString error() const { return failure; }
    // Progress of the current step; total < 0 when the amount of work is unknown.
    qint64 progressDone() const { return done; }
    qint64 progressTotal() const { return total; }
    const FabricProfile &profile() const { return pins; }
    const RuntimeLayout &layout() const { return paths; }
    const Components &components() const { return parts; }
    bool supported() const { return valid; }
    bool prepared() const;
    bool busy() const;
    bool running() const;
    bool readyForInput() const { return gameReady; } // main menu reached (from the game's log)
    bool stopTimedOut() const { return graceExpired; }
    QString runLogPath() const;
    QString prepareLogPath() const;
    QJsonObject metadata() const;
    void recordMetadata(const QString &key, const QJsonValue &value);
    qint64 lastPrepareMs() const { return preparedMs; }

    void prepare(bool repair = false);
    bool launch(const QString &quickPlayWorld, QString *error = nullptr);
    void cancel();
    void requestStop();
    void forceStop();
    // Re-attaches to a game started by an earlier Nova session.
    bool attachExisting();

  signals:
    void changed();
    void preparationFinished(bool ok);
    void gameExited(int exitCode);

  private:
    friend class FabricRuntimeTestAccess;
    void setState(State state, const QString &text = {});
    void fail(const QString &message);
    void runStep();
    void checkStep();
    void downloadNext();
    void finishDownload(const QString &error);
    void extractJdk(const PinnedArtifact &jdk, const QString &zip);
    void workspaceStep();
    void gradleStep(const QString &manifestHash);
    void gradleOutput(const QByteArray &bytes);
    void classifyRunLine(const QString &line);
    void pollRun();
    void processExited();
    void watch(const ProcessIdentity &root);
    void unwatch();
    QProcessEnvironment gradleEnvironment(QString *error = nullptr) const;
    QStringList gradleCommand(const QStringList &tasks, QString *program) const;
    void saveMetadata();

    Options options;
    FabricProfile pins;
    RuntimeLayout paths;
    bool valid = false;
    State current = State::NotConfigured;
    Game phase = Game::Idle;
    QString detail, failure;
    qint64 done = 0, total = -1;
    Components parts;
    QJsonObject meta;
    bool repairing = false;
    QList<PinnedArtifact> pendingDownloads;
    PinnedArtifact downloading;
    QNetworkAccessManager *network = nullptr;
    QPointer<Fetch> fetch;
    QProcess *gradle = nullptr;
    std::shared_ptr<std::atomic_bool> cancelFlag;
    std::shared_ptr<QLockFile> preparationLock;
    QString gradleBuffer;
    QDateTime prepareStarted;
    qint64 preparedMs = -1;
    // Supervision of a running game.
    ProcessIdentity root;
    std::shared_ptr<void> launchGuard;
    QList<ProcessIdentity> knownTree;
    void *processHandle = nullptr;
    QWinEventNotifier *exitNotifier = nullptr;
    QTimer *poll = nullptr, *grace = nullptr;
    qint64 runOffset = 0, errOffset = 0;
    QByteArray runPartial, errPartial;
    bool modList = false, failureSeen = false, gameReady = false, graceExpired = false;
};

QString stateText(FabricRuntime::State state);
QString gameText(FabricRuntime::Game game);
// Classifies one line of Gradle/Minecraft console output. Exposed for tests.
struct RunLine {
    enum Kind {
        None,
        GradleWork,
        RunTask,
        LoaderStart,
        ModListStart,
        ModEntry,
        PlayerSet,
        MenuReady,
        Failure,
        BuildOk
    };
    Kind kind = None;
    QString text, key, value;
};
RunLine classifyRunOutput(const QString &line, bool inModList);

} // namespace nova::testing
