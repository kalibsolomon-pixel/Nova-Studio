// SPDX-License-Identifier: GPL-3.0-or-later
#include "agent_client.h"
#include "build_identity.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

namespace nova::testing {
// Same process: id and creation time (ids are recycled).
static bool operator==(const ProcessIdentity &a, const ProcessIdentity &b) {
    return a.pid == b.pid && a.created == b.created;
}
} // namespace nova::testing

namespace nova::acp {
namespace {
constexpr int ProtocolVersion = 1;
QString text(const QJsonObject &error) {
    return error["message"].toString().left(300);
}
} // namespace

QString findAgentExecutable(const QString &name) {
    const auto found = QStandardPaths::findExecutable(name + ".exe");
    return acceptableExecutable(found, nullptr) ? QDir::toNativeSeparators(found) : QString{};
}

bool acceptableExecutable(const QString &path, QString *problem) {
    const QFileInfo file(path);
    auto refuse = [problem](const QString &message) {
        if (problem)
            *problem = message;
        return false;
    };
    if (path.isEmpty() || !file.isAbsolute())
        return refuse("Choose the agent program with its full path.");
    if (file.suffix().compare("exe", Qt::CaseInsensitive) != 0)
        return refuse("Choose the agent's .exe program, not a script or .cmd/.bat shim.");
    if (!file.isFile() || !file.isExecutable())
        return refuse("The agent program does not exist.");
    return true;
}

QProcessEnvironment agentEnvironment(QProcessEnvironment base) {
    // A connection code in the environment would be visible to the agent and everything it starts.
    base.remove("NOVA_MCP_CODE");
    return base;
}

QJsonObject stdioServer(const QString &name, const QString &command) {
    return {{"name", name}, {"command", QDir::toNativeSeparators(QFileInfo(command).absoluteFilePath())},
            {"args", QJsonArray{}}, {"env", QJsonArray{}}};
}

Session::Session(QObject *parent) : QObject(parent) {
    process.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process, &QProcess::readyReadStandardOutput, this, &Session::readOutput);
    // The agent's diagnostics may contain credentials or prompts: drained, never stored or shown.
    connect(&process, &QProcess::readyReadStandardError, this, [this] { process.readAllStandardError(); });
    connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit ? QString("The agent exited (code %1).").arg(code)
                                              : QString("The agent stopped unexpectedly."));
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish("The agent program could not be started.");
    });
    shutdownTimer.setSingleShot(true);
    connect(&shutdownTimer, &QTimer::timeout, this, &Session::endTree);
    // Remember the processes the agent starts (such as Nova's bridge), so stopping ends them too
    // even after the agent itself has exited.
    treeTimer.setInterval(500);
    connect(&treeTimer, &QTimer::timeout, this, &Session::remember);
}

Session::~Session() {
    disconnect(&process, nullptr, this, nullptr);
    endTree();
    if (process.state() != QProcess::NotRunning)
        process.waitForFinished(2000);
}

bool Session::start(const AgentProgram &program, const QString &workingDirectory, const QJsonArray &mcpServers,
                    const QProcessEnvironment &environment, QString *problem) {
    if (current != State::Idle) {
        if (problem)
            *problem = "This session has already been used.";
        return false;
    }
    if (!acceptableExecutable(program.executable, problem))
        return false;
    if (!QFileInfo(workingDirectory).isAbsolute() || !QFileInfo(workingDirectory).isDir()) {
        if (problem)
            *problem = "The agent's working folder is unavailable.";
        return false;
    }
    directory = QDir::toNativeSeparators(workingDirectory);
    servers = mcpServers;
    process.setProgram(program.executable);
    process.setArguments(program.arguments);
    process.setWorkingDirectory(workingDirectory);
    process.setProcessEnvironment(environment);
    setState(State::Starting);
    process.start();
    if (!process.waitForStarted(10000)) {
        finish("The agent program could not be started.");
        if (problem)
            *problem = reason;
        return false;
    }
    identity = testing::identifyProcess(process.processId());
    treeTimer.start();
    request("initialize", {{"protocolVersion", ProtocolVersion},
                           {"clientCapabilities", QJsonObject{{"fs", QJsonObject{{"readTextFile", false}, {"writeTextFile", false}}},
                                                              {"terminal", false}}},
                           {"clientInfo", QJsonObject{{"name", "nova-studio"}, {"title", "Nova Studio"}, {"version", AppVersion}}}},
            [this](const QJsonObject &reply) {
        if (reply.contains("error"))
            return stop("The agent refused to start a session: " + text(reply["error"].toObject()));
        const auto result = reply["result"].toObject();
        if (result["protocolVersion"].toInt() != ProtocolVersion)
            return stop("The agent does not support ACP protocol version 1.");
        const auto info = result["agentInfo"].toObject();
        agent = (info["title"].toString(info["name"].toString("Agent")) + ' ' + info["version"].toString()).trimmed().left(120);
        canClose = result["agentCapabilities"].toObject()["sessionCapabilities"].toObject().contains("close");
        request("session/new", {{"cwd", directory}, {"mcpServers", servers}}, [this](const QJsonObject &reply) {
            if (reply.contains("error")) {
                const auto error = reply["error"].toObject();
                return stop(error["code"].toInt() == -32000
                    ? "The agent needs you to sign in with its own tool first."
                    : "The agent could not start a session: " + text(error));
            }
            session = reply["result"].toObject()["sessionId"].toString();
            if (session.isEmpty())
                return stop("The agent did not return a session.");
            setState(State::Ready);
        });
    });
    return true;
}

bool Session::prompt(const QString &message) {
    if (current != State::Ready || message.trimmed().isEmpty())
        return false;
    setState(State::Prompting);
    request("session/prompt", {{"sessionId", session}, {"prompt", QJsonArray{QJsonObject{{"type", "text"}, {"text", message}}}}},
            [this](const QJsonObject &reply) {
        if (current == State::Prompting)
            setState(State::Ready);
        emit turnEnded(reply.contains("error") ? "error: " + text(reply["error"].toObject())
                                               : reply["result"].toObject()["stopReason"].toString("unknown"));
    });
    return true;
}

void Session::cancel() {
    // Pending permission questions are answered "cancelled", as the protocol requires.
    for (const auto &id : std::exchange(permissions, {}))
        write({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"outcome", QJsonObject{{"outcome", "cancelled"}}}}}});
    if (current == State::Prompting)
        notify("session/cancel", {{"sessionId", session}});
}

void Session::answerPermission(const QJsonValue &requestId, const QString &optionId) {
    if (!permissions.removeOne(requestId))
        return;
    const auto outcome = optionId.isEmpty() ? QJsonObject{{"outcome", "cancelled"}}
                                            : QJsonObject{{"outcome", "selected"}, {"optionId", optionId}};
    write({{"jsonrpc", "2.0"}, {"id", requestId}, {"result", QJsonObject{{"outcome", outcome}}}});
}

void Session::stop(const QString &why) {
    if (current == State::Idle || current == State::Ended || current == State::Stopping)
        return;
    reason = why;
    cancel();
    setState(State::Stopping);
    if (canClose && !session.isEmpty())
        request("session/close", {{"sessionId", session}}, [](const QJsonObject &) {});
    process.closeWriteChannel();
    shutdownTimer.start(3000);
}

bool Session::ownsProcess(qint64 pid) const {
    const auto candidate = testing::identifyProcess(pid);
    if (!candidate.valid() || !identity.valid())
        return false;
    return candidate == identity || testing::descendants(identity).contains(candidate);
}

QList<testing::ProcessIdentity> Session::processTree() const {
    auto tree = started;
    for (const auto &child : testing::descendants(identity))
        if (!tree.contains(child))
            tree << child;
    return tree;
}

void Session::setState(State next) {
    if (current == next)
        return;
    current = next;
    emit stateChanged();
}

qint64 Session::request(const QString &method, const QJsonObject &params, std::function<void(const QJsonObject &)> done) {
    const auto id = ++nextId;
    pending.insert(id, std::move(done));
    write({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
    return id;
}

void Session::notify(const QString &method, const QJsonObject &params) {
    write({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

void Session::write(const QJsonObject &message) {
    if (process.state() == QProcess::Running)
        process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void Session::remember() {
    const auto tree = testing::descendants(identity);
    for (const auto &child : tree)
        if (!started.contains(child))
            started << child;
}

void Session::readOutput() {
    remember(); // output follows the agent starting a process (such as Nova's bridge)
    buffer += process.readAllStandardOutput();
    qsizetype end;
    while ((end = buffer.indexOf('\n')) >= 0) {
        const auto line = buffer.left(end).trimmed();
        buffer.remove(0, end + 1);
        if (line.isEmpty())
            continue;
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            emit problem("The agent sent a message Nova could not read; it was ignored.");
            continue;
        }
        handle(document.object());
        if (current == State::Ended)
            return;
    }
    if (buffer.size() > LineLimit)
        stop("The agent sent an oversized message.");
}

void Session::handle(const QJsonObject &message) {
    const auto method = message["method"].toString();
    if (message.contains("method") && message.contains("id")) {
        if (method == "session/request_permission" && current != State::Stopping) {
            permissions << message["id"];
            emit permissionRequested(message["id"], message["params"].toObject());
            return;
        }
        write({{"jsonrpc", "2.0"}, {"id", message["id"]},
               {"error", QJsonObject{{"code", -32601}, {"message", "Nova Studio does not provide this method."}}}});
        return;
    }
    if (message.contains("method")) {
        if (method == "session/update" && message["params"].toObject()["sessionId"].toString() == session)
            update(message["params"].toObject());
        return;
    }
    const auto id = qint64(message["id"].toDouble(-1));
    if (auto done = pending.take(id))
        done(message);
}

void Session::update(const QJsonObject &params) {
    const auto change = params["update"].toObject();
    const auto kind = change["sessionUpdate"].toString();
    if (kind == "agent_message_chunk" || kind == "agent_thought_chunk") {
        const auto content = change["content"].toObject();
        if (content["type"] != "text")
            return;
        if (kind == "agent_message_chunk")
            emit agentText(content["text"].toString());
        else
            emit agentThought(content["text"].toString());
    } else if (kind == "tool_call" || kind == "tool_call_update") {
        emit toolCall(change);
    }
}

void Session::finish(const QString &why) {
    if (current == State::Ended)
        return;
    if (reason.isEmpty())
        reason = why;
    shutdownTimer.stop();
    treeTimer.stop();
    endTree();
    pending.clear();
    permissions.clear();
    setState(State::Ended);
}

void Session::endTree() {
    // Only processes this session started: the agent and its descendants, by id and creation time.
    const auto tree = processTree();
    if (identity.valid() && testing::processAlive(identity))
        testing::terminateTree(identity);
    for (const auto &child : tree)
        if (testing::processAlive(child))
            testing::terminateTree(child);
    if (process.state() != QProcess::NotRunning)
        process.kill();
}

} // namespace nova::acp
