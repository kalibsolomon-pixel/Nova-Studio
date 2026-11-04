// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-F UI Automation responsiveness probe.
//
// Hosts a real Nova Studio main window in this disposable process, prepares a scenario,
// and runs nova-phase4f-uia-client against its own window from a separate process (UI
// Automation clients are always out of process). While the client queries, this process
// measures its GUI heartbeat and samples GUI-thread stacks. A watchdog writes the sampled
// stacks and terminates the process if the GUI thread stops responding for 45 s, so a
// hang can never outlive the run.
//
//   nova-phase4f-uia-probe <work-dir> <output.json> [scenario ...]
#include "phase4f_probe_support.h"
#include "recovery.h"
#include "window.h"
#include <QAbstractButton>
#include <QAction>
#include <QDateTime>
#include <QDialog>
#include <QDockWidget>
#include <QFile>
#include <QJsonDocument>
#include <QListView>
#include <QMessageBox>
#include <QProcess>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QtTest>

namespace nova {
class EditorTestAccess {
  public:
    static bool indexing(const Window &w) { return w.indexBusy; }
    static void openTexture(Window &w, const QString &path) { w.openTexture(path); }
    static void openModel(Window &w, const QString &path) { w.openModelAsset(path); }
    static void setMode(Window &w, bool grid) { w.setMode(grid); }
    static QTreeView *tree(Window &w) { return w.tree; }
    static QListView *grid(Window &w) { return w.grid; }
    static QTabWidget *tabs(Window &w) { return w.tabs; }
    static QDockWidget *modelsDock(Window &w) { return w.modelsDock; }
    static void setDirectory(Window &w, const QString &d) { w.setDirectory(d); }
    static void showReview(Window &w) { w.showMcpAccess(); }
    static void showComparison(Window &w, const QString &path) {
        Window::Comparison c;
        c.path = path;
        c.beforeRevision = "before";
        c.afterRevision = "after";
        c.detail = "Probe comparison";
        c.owner = "probe";
        c.instance = QUuid::createUuid();
        c.before = QImage(64, 64, QImage::Format_RGBA8888);
        c.before.fill(Qt::red);
        c.after = QImage(64, 64, QImage::Format_RGBA8888);
        c.after.fill(Qt::blue);
        w.rememberComparison(c);
        w.showComparison(path, "after");
    }
    static void recover(Window &w, const QString &texture) {
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::green);
        w.recovery->write(QUuid::createUuid(), texture, w.project, Revision::read(texture), image);
        w.recoverSession();
    }
};
} // namespace nova

using namespace nova;
using namespace phase4f;
namespace {
std::atomic<qint64> lastBeat{0};
std::atomic<qint64> scenarioStarted{0}; // a scenario may not run longer than ScenarioLimitMs
constexpr qint64 ScenarioLimitMs = 10 * 60 * 1000;
QElapsedTimer processClock;
QString clientPath() {
    return QCoreApplication::applicationDirPath() + "/nova-phase4f-uia-client.exe";
}
struct Heartbeat {
    QTimer timer;
    QElapsedTimer clock;
    std::vector<double> gaps;
    qint64 previous = 0;
    Heartbeat() {
        timer.setInterval(10);
        timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&timer, &QTimer::timeout, [this] {
            const auto now = clock.nsecsElapsed();
            if (previous)
                gaps.push_back((now - previous) / 1e6);
            previous = now;
            lastBeat = processClock.elapsed();
        });
    }
    void start() {
        gaps.clear();
        previous = 0;
        clock.start();
        timer.start();
    }
    QJsonObject stop() {
        timer.stop();
        auto out = Stats().json(gaps);
        int stalls = 0;
        for (double g : gaps)
            stalls += g >= 250;
        out["stalls_over_250ms"] = stalls;
        return out;
    }
};
qint64 handleCount() {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}
// Runs one client query against `target` while pumping this thread's event loop.
QJsonObject query(QWidget *target, const QString &mode, int budgetMs, const QString &work) {
    const auto out = work + QString("/uia-%1-%2.json").arg(mode).arg(processClock.nsecsElapsed());
    const auto focusBefore = QApplication::focusWidget();
    const auto memBefore = privateBytes();
    const auto handlesBefore = handleCount();
    Heartbeat beat;
    Sampler sampler;
    sampler.start();
    beat.start();
    QProcess client;
    QEventLoop loop;
    QObject::connect(&client, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&client, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    QElapsedTimer wall;
    wall.start();
    client.start(clientPath(), {QString::number(qintptr(target->winId())), mode, QString::number(budgetMs), out});
    // The client bounds itself; this guard bounds the client.
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, [&] {
        client.kill();
        loop.quit();
    });
    guard.start(budgetMs + 60000);
    loop.exec();
    const double wallMs = wall.nsecsElapsed() / 1e6;
    auto heartbeat = beat.stop();
    auto samples = sampler.stop(30);
    QJsonObject result{{"mode", mode}, {"target", target->objectName().isEmpty() ? QString(target->metaObject()->className()) : target->objectName()},
                       {"wall_ms", wallMs}, {"client_exit", client.exitCode()},
                       {"client_killed", guard.isActive() ? false : true}, {"heartbeat", heartbeat},
                       {"focus_unchanged", QApplication::focusWidget() == focusBefore},
                       {"private_bytes_delta", double(privateBytes() - memBefore)},
                       {"handle_delta", double(handleCount() - handlesBefore)}, {"gui_samples", samples}};
    QFile f(out);
    if (f.open(QIODevice::ReadOnly))
        result["client"] = QJsonDocument::fromJson(f.readAll()).object();
    else
        result["client_stderr"] = QString::fromUtf8(client.readAllStandardError());
    return result;
}
QWidget *visibleTop(const QString &objectName, const char *className = nullptr) {
    for (auto w : QApplication::topLevelWidgets())
        if (w->isVisible() && (objectName.isEmpty() || w->objectName() == objectName) &&
            (!className || w->inherits(className)))
            return w;
    return nullptr;
}
} // namespace

int main(int argc, char **argv) {
    processClock.start();
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setSwapInterval(0); // as src/app/main.cpp
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setApplicationName("Nova Studio UIA Probe");
    QFont font = app.font();
    font.setPointSize(9);
    app.setFont(font);
    const auto args = app.arguments();
    if (args.size() < 3)
        return 2;
    const QString work = args[1], output = args[2];
    QStringList scenarios = args.mid(3);
    if (scenarios.isEmpty())
        scenarios = {"empty", "tree-expanded", "dense-grid", "texture", "model", "review", "comparison",
                     "recovery", "close-confirm"};
    const int budget = qEnvironmentVariableIsSet("NOVA_UIA_BUDGET") ? qEnvironmentVariableIntValue("NOVA_UIA_BUDGET") : 60000;
    const QStringList modes = qEnvironmentVariable("NOVA_UIA_MODES", "walk,findall,focus").split(',');
    QDir().mkpath(work);
    const QString emptyRoot = work + "/empty-pack", fullRoot = work + "/many-pack", denseRoot = work + "/dense-pack";
    if (!QFileInfo::exists(emptyRoot + "/pack.mcmeta"))
        makePack(emptyRoot, 0);
    if (!QFileInfo::exists(fullRoot + "/pack.mcmeta"))
        makePack(fullRoot, 2400);
    if (!QFileInfo::exists(denseRoot + "/pack.mcmeta")) {
        makePack(denseRoot, 0);
        for (int i = 0; i < 1500; ++i)
            png(denseRoot + QString("/assets/minecraft/textures/block/b%1.png").arg(i, 4, 10, QChar('0')), 16, i);
    }
    // Watchdog: if the GUI thread misses heartbeats for 45 s, record and terminate.
    lastBeat = processClock.elapsed();
    std::atomic_bool watching = true;
    std::thread watchdog([&] {
        while (watching) {
            Sleep(500);
            const bool unresponsive = processClock.elapsed() - lastBeat > 45000;
            // A heartbeat alone is not progress: a modal loop keeps timers running.
            const bool overdue = scenarioStarted && processClock.elapsed() - scenarioStarted > ScenarioLimitMs;
            if (unresponsive || overdue) {
                QFile f(output + ".hang.txt");
                if (f.open(QIODevice::WriteOnly))
                    f.write(unresponsive ? "GUI thread unresponsive for 45 s; terminating disposable probe\n"
                                         : "Scenario exceeded 10 minutes; terminating disposable probe\n");
                f.close();
                TerminateProcess(GetCurrentProcess(), 99);
            }
        }
    });
    QTimer keepAlive;
    QObject::connect(&keepAlive, &QTimer::timeout, [] { lastBeat = processClock.elapsed(); });
    keepAlive.start(100);
    QJsonArray results;
    for (const auto &scenario : scenarios) {
        scenarioStarted = processClock.elapsed();
        // A fresh profile per run: leftover snapshots would open a startup prompt.
        qApp->setProperty("settingsFile", work + QString("/settings-%1-%2/studio.ini")
                                                     .arg(scenario)
                                                     .arg(QDateTime::currentMSecsSinceEpoch()));
        const bool empty = scenario == "empty", dense = scenario == "dense-grid";
        const QString root = empty ? emptyRoot : dense ? denseRoot : fullRoot;
        QElapsedTimer started;
        started.start();
        Window window(root, {}, started);
        window.setObjectName("novaWorkspaceWindow");
        window.resize(1400, 900);
        window.show();
        QTest::qWaitForWindowExposed(&window);
        QElapsedTimer wait;
        wait.start();
        while ((EditorTestAccess::indexing(window) || wait.elapsed() < 800) && wait.elapsed() < 30000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const QString texture = root + "/assets/nova/textures/block/cube.png";
        QWidget *target = &window;
        if (scenario == "tree-expanded") {
            EditorTestAccess::setMode(window, false);
            EditorTestAccess::setDirectory(window, {});
            EditorTestAccess::tree(window)->setRootIndex({});
            EditorTestAccess::tree(window)->expandAll();
            EditorTestAccess::tree(window)->setFocus();
        }
        if (dense) {
            EditorTestAccess::setMode(window, true);
            EditorTestAccess::setDirectory(window, "assets/minecraft/textures/block");
            EditorTestAccess::grid(window)->setFocus();
        }
        if (scenario == "texture" || scenario == "comparison" || scenario == "close-confirm")
            EditorTestAccess::openTexture(window, texture);
        if (scenario == "model") {
            EditorTestAccess::openModel(window, "assets/nova/models/block/cube.json");
            if (auto dock = EditorTestAccess::modelsDock(window))
                dock->show();
        }
        if (scenario == "review") {
            EditorTestAccess::showReview(window);
            settle(300);
            target = visibleTop("mcpAccessDialog");
        }
        if (scenario == "comparison") {
            EditorTestAccess::showComparison(window, "assets/nova/textures/block/cube.png");
            settle(300);
            target = visibleTop({}, "QDialog");
        }
        settle(500);
        QJsonObject entry{{"scenario", scenario}};
        QJsonArray runs;
        const bool modal = scenario == "recovery" || scenario == "close-confirm";
        if (!modal) {
            if (!target) {
                entry["error"] = "target window not found";
            } else {
                for (const auto &mode : modes)
                    runs.append(query(target, mode, budget, work));
            }
        } else {
            // Modal prompts run nested event loops; measure from a timer inside them.
            bool measured = false;
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, [&] {
                auto box = scenario == "recovery" ? visibleTop("recoveryDialog") : visibleTop({}, "QMessageBox");
                if (!box || measured)
                    return;
                measured = true;
                poll.stop();
                for (const auto &mode : modes)
                    runs.append(query(box, mode, budget, work));
                // Leave the prompt without saving or discarding anything.
                if (auto message = qobject_cast<QMessageBox *>(box)) {
                    if (auto escape = message->escapeButton())
                        escape->click();
                    else
                        message->reject();
                }
            });
            poll.start(50);
            if (scenario == "recovery") {
                EditorTestAccess::recover(window, texture);
            } else {
                auto canvas = EditorTestAccess::tabs(window)->currentWidget();
                Q_UNUSED(canvas);
                const auto &open = window.documents().all();
                if (auto doc = open.empty() ? nullptr : open.front()) {
                    doc->paint(doc->activeLayer(), {1, 1}, {3, 3}, qRgb(1, 2, 3));
                    QMetaObject::invokeMethod(EditorTestAccess::tabs(window), "tabCloseRequested", Qt::QueuedConnection, Q_ARG(int, 0));
                }
            }
            QElapsedTimer modalWait;
            modalWait.start();
            while (!measured && modalWait.elapsed() < 20000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            poll.stop();
            if (!measured)
                entry["error"] = "modal prompt did not appear";
        }
        entry["runs"] = runs;
        results.append(entry);
        for (auto top : QApplication::topLevelWidgets())
            if (top != &window && top->isVisible())
                top->close();
        // Discard the probe's own edits so closing never prompts.
        for (const auto &doc : window.documents().all())
            while (doc->modified() && doc->canUndo())
                doc->undo();
        window.close();
        settle(200);
        fprintf(stderr, "[uia] %s done\n", qPrintable(scenario));
        QFile partial(output + ".partial");
        if (partial.open(QIODevice::WriteOnly))
            partial.write(QJsonDocument(QJsonObject{{"results", results}}).toJson());
    }
    watching = false;
    watchdog.join();
    QFile out(output);
    if (!out.open(QIODevice::WriteOnly))
        return 1;
    out.write(QJsonDocument(QJsonObject{{"results", results}}).toJson());
    return 0;
}
