// SPDX-License-Identifier: GPL-3.0-or-later
// Manual probe (never run by CTest): starts a real ACP agent such as "opencode acp" through Nova's ACP
// client with a read-only grant on a synthetic pack, attaches Nova's bridge through session/new, waits
// for the agent's bridge to connect, then stops the session. It sends no prompt, so no model is used.
// The agent gets an allow-list environment pointing every profile, cache and temp folder at --scratch;
// no variable from the caller's environment that could carry a credential is passed on.
//   nova-acp-live-probe --agent <agent.exe> [--agent-arg acp] --scratch <empty folder> --report <file.json>
// Console only: no window.
#include "agent_client.h"
#include "service.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cstdio>

using namespace nova;

namespace {
QString argument(const QStringList &args, const QString &name) {
    const auto at = args.indexOf(name);
    return at >= 0 && at + 1 < args.size() ? args[at + 1] : QString{};
}
bool waitFor(const std::function<bool()> &condition, int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return condition();
}
// Only what a Windows program needs to run, plus scratch locations for everything per-user.
QProcessEnvironment isolated(const QString &scratch, const QString &agentDir) {
    const auto system = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment env;
    for (const auto name : {"SystemRoot", "windir", "SystemDrive", "ComSpec", "PATHEXT", "NUMBER_OF_PROCESSORS",
                            "PROCESSOR_ARCHITECTURE", "OS"})
        if (system.contains(name))
            env.insert(name, system.value(name));
    const auto windows = system.value("SystemRoot", "C:\\Windows");
    env.insert("PATH", QDir::toNativeSeparators(agentDir) + ";" + windows + "\\System32;" + windows);
    for (const auto &[name, folder] : QList<QPair<QString, QString>>{
             {"USERPROFILE", "/home"}, {"HOME", "/home"}, {"OPENCODE_TEST_HOME", "/home"},
             {"APPDATA", "/home/AppData/Roaming"}, {"LOCALAPPDATA", "/home/AppData/Local"},
             {"TEMP", "/temp"}, {"TMP", "/temp"}, {"XDG_CONFIG_HOME", "/xdg/config"},
             {"XDG_DATA_HOME", "/xdg/data"}, {"XDG_CACHE_HOME", "/xdg/cache"}, {"XDG_STATE_HOME", "/xdg/state"}}) {
        QDir().mkpath(scratch + folder);
        env.insert(name, QDir::toNativeSeparators(scratch + folder));
    }
    env.insert("OPENCODE_DISABLE_AUTOUPDATE", "1");
    env.insert("OPENCODE_DISABLE_PROJECT_CONFIG", "1");
    env.insert("OPENCODE_DISABLE_MODELS_FETCH", "1");
    return env;
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const auto agent = argument(args, "--agent");
    const auto scratch = QDir::cleanPath(argument(args, "--scratch"));
    const auto reportPath = argument(args, "--report");
    QStringList agentArgs;
    for (int i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--agent-arg")
            agentArgs << args[i + 1];
    if (agent.isEmpty() || scratch.isEmpty() || reportPath.isEmpty() || !QDir(scratch).isEmpty()) {
        std::fputs("usage: nova-acp-live-probe --agent <exe> [--agent-arg a] --scratch <new empty folder> --report <json>\n", stderr);
        return 2;
    }
    QJsonObject report{{"agentProgram", QDir::toNativeSeparators(agent)}, {"agentArguments", QJsonArray::fromStringList(agentArgs)},
                       {"promptSent", false}};
    QElapsedTimer clock;
    clock.start();
    // A synthetic pack and the agent's empty working folder, both inside the scratch folder.
    const auto project = scratch + "/Synthetic Pack";
    const auto work = scratch + "/work";
    const auto handoff = scratch + "/handoff";
    QDir().mkpath(project + "/assets/nova/textures/block");
    QDir().mkpath(work);
    QDir().mkpath(handoff);
    QFile meta(project + "/pack.mcmeta");
    meta.open(QIODevice::WriteOnly);
    meta.write(R"({"pack":{"min_format":[75,0],"max_format":[75,0],"description":"ACP live probe"}})");
    meta.close();
    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(Qt::gray);
    image.save(project + "/assets/nova/textures/block/plain.png");
    qputenv("NOVA_MCP_HANDOFF_DIR", QDir::toNativeSeparators(handoff).toUtf8()); // never the user's profile

    mcp::Service service([](mcp::Snapshot &, const QString &) { return false; }, &app);
    if (!service.authorize(QDir(project).absolutePath(), false)) {
        report["error"] = "authorize failed: " + service.error();
    } else {
        acp::Session session;
        QJsonArray filtered;
        service.setClientFilter([&](qint64 pid) {
            const bool owned = session.ownsProcess(pid);
            filtered.append(QJsonObject{{"pid", pid}, {"accepted", owned}});
            return owned;
        });
        // The bridge reads the offer from the probe's handoff folder; that location is passed in the
        // standard mcpServers env (a folder path, not a secret).
        auto server = acp::stdioServer("nova", QString::fromUtf8(NOVA_MCP_TEST_BRIDGE));
        server["env"] = QJsonArray{QJsonObject{{"name", "NOVA_MCP_HANDOFF_DIR"}, {"value", QDir::toNativeSeparators(handoff)}}};
        QStringList problems;
        QObject::connect(&session, &acp::Session::problem, [&](const QString &message) { problems << message; });
        QString problem;
        const bool started = session.start({QDir::toNativeSeparators(agent), agentArgs}, work, QJsonArray{server},
                                           isolated(scratch, QFileInfo(agent).absolutePath()), &problem);
        report["started"] = started;
        if (!started)
            report["startProblem"] = problem;
        const bool settled = waitFor([&] { return session.state() != acp::Session::State::Starting; }, 240000);
        report["sessionReadyMs"] = clock.elapsed();
        report["sessionState"] = session.state() == acp::Session::State::Ready ? "ready" : session.state() == acp::Session::State::Ended ? "ended" : settled ? "other" : "timeout";
        report["agentInfo"] = session.agentName();
        report["sessionIdReturned"] = !session.sessionId().isEmpty();
        if (session.state() == acp::Session::State::Ended)
            report["endReason"] = session.endReason();
        if (session.state() == acp::Session::State::Ready) {
            const bool connected = waitFor([&] { return service.clientConnected(); }, 120000);
            report["bridgeConnected"] = connected;
            report["bridgeConnectedMs"] = clock.elapsed();
            report["mcpClientName"] = service.clientName();
            report["serviceStatus"] = service.status();
            report["permission"] = service.permission();
            QJsonArray tree;
            for (const auto &process : session.processTree())
                tree.append(QJsonObject{{"pid", process.pid}});
            report["processTreeWhileRunning"] = tree;
        }
        report["clientFilterDecisions"] = filtered;
        const auto tree = session.processTree();
        session.stop("Probe finished.");
        waitFor([&] { return session.state() == acp::Session::State::Ended; }, 15000);
        report["endState"] = session.state() == acp::Session::State::Ended ? "ended" : "not ended";
        report["endReasonAfterStop"] = session.endReason();
        QThread::msleep(500);
        int alive = 0;
        for (const auto &process : tree)
            alive += testing::processAlive(process);
        report["processesAliveAfterStop"] = alive;
        report["agentProblems"] = QJsonArray::fromStringList(problems);
        service.revoke();
        report["grantAfterRevoke"] = service.permission();
    }
    report["elapsedMs"] = clock.elapsed();
    QFile out(reportPath);
    out.open(QIODevice::WriteOnly);
    out.write(QJsonDocument(report).toJson());
    std::fputs(QJsonDocument(report).toJson().constData(), stdout);
    return 0;
}
