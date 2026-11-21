// SPDX-License-Identifier: GPL-3.0-or-later
#include "agent_client.h"
#include "build_identity.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopeGuard>
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
constexpr int StartupTimeout = 30000, AuthenticateTimeout = 180000, CancelTimeout = 8000;
constexpr int RememberInterval = 250; // at most one process snapshot per this many ms of output
constexpr int ListLimit = 64;         // modes, config options and commands kept from one report
QString text(const QJsonObject &error) {
    return error["message"].toString().left(300);
}
QJsonArray bounded(const QJsonArray &list) {
    QJsonArray kept;
    for (qsizetype i = 0; i < qMin<qsizetype>(list.size(), ListLimit); ++i)
        if (list.at(i).isObject())
            kept.append(list.at(i));
    return kept;
}
} // namespace

QString findAgentExecutable(const QString &name) {
    for (const auto *suffix : {".exe", ".cmd"}) {
        const auto found = QStandardPaths::findExecutable(name + suffix);
        if (!resolveAgentProgram(found, {}).executable.isEmpty())
            return QDir::toNativeSeparators(found);
    }
    return {};
}

AgentProgram resolveAgentProgram(const QString &path, const QStringList &arguments, QString *problem) {
    const QFileInfo file(path);
    if (file.suffix().compare("cmd", Qt::CaseInsensitive) != 0)
        return acceptableExecutable(path, problem) ? AgentProgram{path, arguments} : AgentProgram{};
    auto fail = [problem](const QString &message) -> AgentProgram {
        if (problem)
            *problem = message;
        return {};
    };
    if (!file.isAbsolute() || !file.isFile())
        return fail("Choose an existing agent program with its full path.");
    QFile shim(path);
    if (!shim.open(QIODevice::ReadOnly) || shim.size() > 16384)
        return fail("Nova could not read the npm launcher.");
    // npm's current and older cmd-shim forms. Extract only a quoted script under the
    // shim's node_modules directory. No batch statement or expansion is executed.
    constexpr auto insensitive = QRegularExpression::CaseInsensitiveOption;
    static const QRegularExpression invocation(
        R"rx(^\s*(?:endLocal\s*&\s*goto\s+[^\r\n]+?\s*\|\|\s*title\s+%COMSPEC%\s*&\s*)?)rx"
        R"rx(@?(?<program>"%_prog%"|node(?:\.exe)?|"%~dp0\\node\.exe")\s+)rx"
        R"rx("(?:%dp0%|%~dp0)\\(?<script>node_modules\\[^"%\r\n<>:|?*]+\.(?:js|cjs|mjs))"\s+%\*\s*$)rx", insensitive);
    static const QRegularExpression programAssignment(R"rx(^\s*@?SET\s+"_prog=([^"]+)"\s*$)rx", insensitive);
    bool hasNodeAssignment = false, usesProgramVariable = false;
    QString target;
    for (const auto &line : QString::fromUtf8(shim.read(16384)).split('\n')) {
        if (const auto assignment = programAssignment.match(line); assignment.hasMatch()) {
            const auto program = assignment.captured(1).toLower();
            if (program != "node" && program != "node.exe" && program != "%dp0%\\node.exe")
                return fail("The npm launcher does not use Node.js.");
            hasNodeAssignment = true;
        }
        const auto match = invocation.match(line);
        if (!match.hasMatch())
            continue;
        usesProgramVariable |= match.captured("program").compare("\"%_prog%\"", Qt::CaseInsensitive) == 0;
        const auto relative = QDir::fromNativeSeparators(match.captured("script"));
        if (relative != QDir::cleanPath(relative) || (!target.isEmpty() && target != relative))
            return fail("The npm launcher has an unsupported script path.");
        for (const auto &component : relative.split('/'))
            if (component.endsWith('.') || component.endsWith(' '))
                return fail("The npm launcher has an unsupported script path.");
        target = relative;
    }
    if (target.isEmpty() || (usesProgramVariable && !hasNodeAssignment))
        return fail("Choose a native .exe or a standard npm .cmd launcher.");
    const auto script = file.absoluteDir().absoluteFilePath(target);
    if (!QFileInfo(script).isFile())
        return fail("The npm package script is missing. Reinstall the agent adapter.");
    auto node = file.absoluteDir().absoluteFilePath("node.exe");
    if (!acceptableExecutable(node, nullptr))
        node = QStandardPaths::findExecutable("node.exe");
    if (!acceptableExecutable(node, nullptr))
        return fail("Install Node.js or add node.exe to PATH, then reopen Nova Studio.");
    return {QDir::toNativeSeparators(node), QStringList{QDir::toNativeSeparators(script)} + arguments};
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
        return refuse("Choose a native .exe or a standard npm .cmd launcher.");
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
    connect(&process, &QProcess::readyReadStandardError, this, &Session::readError);
    connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readError();
        finish((status == QProcess::NormalExit ? QString("The agent exited (code %1).").arg(code)
                                              : QString("The agent stopped unexpectedly.")) +
               " Open Show agent output, correct the reported problem, then start again.");
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish("Windows could not start the agent. Check the program path and reinstall its adapter or runtime.");
    });
    // The handshake begins once Windows reports the process running, so starting never blocks the window.
    connect(&process, &QProcess::started, this, &Session::initialize);
    shutdownTimer.setSingleShot(true);
    connect(&shutdownTimer, &QTimer::timeout, this, &Session::endTree);
    replyTimer.setSingleShot(true);
    connect(&replyTimer, &QTimer::timeout, this, [this] { stop(timeoutReason); });
    // Remember the processes the agent starts (such as Nova's bridge), so stopping ends them too
    // even after the agent itself has exited.
    treeTimer.setInterval(500);
    connect(&treeTimer, &QTimer::timeout, this, &Session::remember);
    rememberSoon.setSingleShot(true);
    connect(&rememberSoon, &QTimer::timeout, this, &Session::remember);
}

Session::~Session() {
    disconnect(&process, nullptr, this, nullptr);
    endTree();
    if (process.state() != QProcess::NotRunning)
        process.waitForFinished(2000);
}

bool Session::start(const AgentProgram &program, const QString &workingDirectory, const QJsonArray &mcpServers,
                    const QProcessEnvironment &environment, QString *problem, const QString &resumeSessionId) {
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
    resumeId = resumeSessionId;
    process.setProgram(program.executable);
    process.setArguments(program.arguments);
    process.setWorkingDirectory(workingDirectory);
    process.setProcessEnvironment(environment);
    setState(State::Starting);
    // Waits for neither the process nor the agent: started, errorOccurred and the reply timer continue.
    expectReply(StartupTimeout, "The agent program did not start. Check the program path and runtime, then retry.");
    process.start();
    if (current == State::Ended) {
        if (problem)
            *problem = reason;
        return false;
    }
    return true;
}

void Session::initialize() {
    if (current != State::Starting)
        return;
    identity = testing::identifyProcess(process.processId());
    sinceRemember.start();
    treeTimer.start();
    expectReply(StartupTimeout, "The agent did not answer the ACP handshake. Check the adapter and its launch arguments.");
    request("initialize", {{"protocolVersion", ProtocolVersion},
                           {"clientCapabilities", QJsonObject{{"fs", QJsonObject{{"readTextFile", false}, {"writeTextFile", false}}},
                                                              {"terminal", false}}},
                           {"clientInfo", QJsonObject{{"name", "nova-studio"}, {"title", "Nova Studio"}, {"version", AppVersion}}}},
            [this](const QJsonObject &reply) {
        if (current != State::Starting)
            return;
        replyTimer.stop();
        if (reply.contains("error"))
            return stop("The agent refused to start a session: " + text(reply["error"].toObject()));
        const auto result = reply["result"].toObject();
        if (result["protocolVersion"].toInt() != ProtocolVersion)
            return stop("The agent does not support ACP protocol version 1.");
        const auto info = result["agentInfo"].toObject();
        agent = (info["title"].toString(info["name"].toString("Agent")) + ' ' + info["version"].toString()).trimmed().left(120);
        const auto capabilities = result["agentCapabilities"].toObject();
        canClose = capabilities["sessionCapabilities"].toObject().contains("close");
        loadable = capabilities["loadSession"].toBool();
        promptAbilities = capabilities["promptCapabilities"].toObject();
        for (const auto &value : result["authMethods"].toArray()) {
            const auto method = value.toObject();
            if (!method["id"].toString().isEmpty() && authMethods.size() < 32)
                authMethods.append(method);
        }
        createSession();
    });
}

void Session::createSession() {
    if (current != State::Starting)
        return;
    const bool load = loadable && !resumeId.isEmpty();
    expectReply(StartupTimeout, load ? "The agent did not reopen the session. Check agent output, then start again."
                                     : "The agent did not open a session. Check sign-in and agent output, then retry.");
    QJsonObject params{{"cwd", directory}, {"mcpServers", servers}};
    if (load) {
        params["sessionId"] = resumeId;
        session = resumeId; // the replayed history arrives as session/update before the reply
        replaying = true;
    }
    request(load ? "session/load" : "session/new", params, [this, load](const QJsonObject &reply) {
        if (current != State::Starting)
            return;
        replyTimer.stop();
        replaying = false;
        if (reply.contains("error")) {
            const auto error = reply["error"].toObject();
            if (load && error["code"].toInt() != -32000) {
                // The agent could not reopen it: continue in a new session rather than not at all.
                resumeId.clear();
                session.clear();
                emit problem("The agent could not continue the earlier conversation, so a new session started: " +
                             text(error));
                return createSession();
            }
            if (error["code"].toInt() != -32000)
                return stop("The agent could not start a session: " + text(error));
            waitingForAuth = !authMethods.isEmpty();
            emit authenticationRequired(authMethods);
            if (!waitingForAuth)
                stop("Sign-in required.");
            return;
        }
        const auto result = reply["result"].toObject();
        if (!load)
            session = result["sessionId"].toString();
        if (session.isEmpty())
            return stop("The agent did not return a session.");
        wasResumed = load;
        opened(result);
    });
}

void Session::opened(const QJsonObject &result) {
    readModes(result);
    if (result.contains("configOptions")) {
        options = bounded(result["configOptions"].toArray());
        emit configOptionsChanged();
    }
    setState(State::Ready);
}

void Session::readModes(const QJsonObject &result) {
    if (!result.contains("modes"))
        return;
    const auto state = result["modes"].toObject();
    modeState = {{"currentModeId", state["currentModeId"].toString()},
                 {"availableModes", bounded(state["availableModes"].toArray())}};
    emit modesChanged();
}

bool Session::setMode(const QString &modeId) {
    if ((current != State::Ready && current != State::Prompting) || modeId.isEmpty())
        return false;
    request("session/set_mode", {{"sessionId", session}, {"modeId", modeId}}, [this, modeId](const QJsonObject &reply) {
        if (current != State::Ready && current != State::Prompting)
            return;
        if (reply.contains("error"))
            emit problem("The agent did not change its mode: " + text(reply["error"].toObject()));
        else
            modeState["currentModeId"] = modeId;
        emit modesChanged(); // a refusal puts the picker back
    });
    return true;
}

bool Session::setConfigOption(const QString &configId, const QString &value) {
    if ((current != State::Ready && current != State::Prompting) || configId.isEmpty())
        return false;
    request("session/set_config_option", {{"sessionId", session}, {"configId", configId}, {"value", value}},
            [this](const QJsonObject &reply) {
        if (current != State::Ready && current != State::Prompting)
            return;
        if (reply.contains("error"))
            emit problem("The agent did not change that setting: " + text(reply["error"].toObject()));
        else if (reply["result"].toObject()["configOptions"].isArray())
            options = bounded(reply["result"].toObject()["configOptions"].toArray());
        emit configOptionsChanged();
    });
    return true;
}

bool Session::authenticate(const QString &methodId) {
    if (current != State::Starting || !waitingForAuth)
        return false;
    for (const auto &value : authMethods) {
        if (value.toObject()["id"].toString() != methodId)
            continue;
        waitingForAuth = false;
        expectReply(AuthenticateTimeout, "The agent did not finish signing in. Complete sign-in in its own tool, then retry.");
        request("authenticate", {{"methodId", methodId}}, [this](const QJsonObject &reply) {
            if (current != State::Starting)
                return;
            replyTimer.stop();
            if (reply.contains("error")) {
                waitingForAuth = true;
                emit authenticationRequired(authMethods);
                emit problem("Sign-in failed: " + text(reply["error"].toObject()));
                return;
            }
            createSession();
        });
        return true;
    }
    return false;
}

bool Session::prompt(const QString &message, const QJsonArray &attachments) {
    if (current != State::Ready || message.trimmed().isEmpty())
        return false;
    cancellingTurn = false;
    setState(State::Prompting);
    auto blocks = QJsonArray{QJsonObject{{"type", "text"}, {"text", message}}};
    for (const auto &block : attachments)
        blocks.append(block);
    request("session/prompt", {{"sessionId", session}, {"prompt", blocks}},
            [this](const QJsonObject &reply) {
        replyTimer.stop();
        cancelPermissions();
        if (current == State::Prompting)
            setState(State::Ready);
        emit turnEnded(reply.contains("error") ? "error: " + text(reply["error"].toObject())
                                               : reply["result"].toObject()["stopReason"].toString("unknown"));
    });
    return true;
}

void Session::cancel() {
    cancelPermissions();
    if (current == State::Prompting && !cancellingTurn) {
        cancellingTurn = true;
        notify("session/cancel", {{"sessionId", session}});
        expectReply(CancelTimeout, "The agent ignored cancellation. Nova ended the session. Start again to continue.");
    }
}

void Session::cancelPermissions() {
    // Pending permission questions are answered "cancelled", as the protocol requires.
    for (const auto &id : std::exchange(permissions, {}))
        write({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"outcome", QJsonObject{{"outcome", "cancelled"}}}}}});
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
    replyTimer.stop();
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
    sinceRemember.start();
    rememberSoon.stop();
    const auto tree = testing::descendants(identity);
    for (const auto &child : tree)
        if (!started.contains(child))
            started << child;
}

void Session::readOutput() {
    // Output follows the agent starting a process (such as Nova's bridge). Streamed chunks take a
    // snapshot of every process at most once per interval, with one more after it; replies and
    // requests (rare, and what a turn ends with) always take one.
    if (!sinceRemember.isValid() || sinceRemember.elapsed() >= RememberInterval)
        remember();
    else if (!rememberSoon.isActive())
        rememberSoon.start(int(RememberInterval - sinceRemember.elapsed()));
    buffer += process.readAllStandardOutput();
    if (reading)
        return; // reached from a nested event loop; the outer call reads on through the appended data
    reading = true;
    const auto done = qScopeGuard([this] { reading = false; });
    // Lines are read in place and the consumed prefix removed once, so a large write stays linear.
    qsizetype start = 0, end;
    while ((end = buffer.indexOf('\n', start)) >= 0) {
        if (end - start > LineLimit) {
            buffer.clear();
            stop("The agent sent an oversized message.");
            return;
        }
        const auto line = QByteArrayView(buffer).sliced(start, end - start).trimmed().toByteArray();
        start = end + 1;
        if (line.isEmpty())
            continue;
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            emit problem("The agent sent a message Nova could not read; it was ignored.");
            continue;
        }
        if (document.object().contains("id") && sinceRemember.elapsed() > 0)
            remember();
        handle(document.object());
        if (current == State::Ended) {
            buffer.clear();
            return;
        }
    }
    buffer.remove(0, start);
    if (buffer.size() > LineLimit)
        stop("The agent sent an oversized message.");
}

void Session::readError() {
    const auto chunk = process.readAllStandardError();
    if (chunk.isEmpty())
        return;
    if (chunk.size() >= OutputLimit)
        stderrTail = chunk.last(OutputLimit);
    else {
        stderrTail += chunk;
        if (stderrTail.size() > OutputLimit)
            stderrTail.remove(0, stderrTail.size() - OutputLimit);
    }
    emit agentOutputChanged();
}

void Session::expectReply(int timeoutMs, const QString &failure) {
    timeoutReason = failure;
    replyTimer.start(timeoutMs);
}

void Session::handle(const QJsonObject &message) {
    const auto method = message["method"].toString();
    if (message.contains("method") && message.contains("id")) {
        if (method == "session/request_permission") {
            const auto id = message["id"];
            const auto params = message["params"].toObject();
            if (permissions.contains(id))
                return;
            if (current != State::Prompting || cancellingTurn || params["sessionId"].toString() != session ||
                permissions.size() >= 32) {
                write({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{
                    {"outcome", QJsonObject{{"outcome", "cancelled"}}}}}});
                if (permissions.size() >= 32)
                    emit problem("The agent sent too many tool permission requests. Additional requests were cancelled.");
                return;
            }
            permissions << id;
            emit permissionRequested(id, params);
            return;
        }
        write({{"jsonrpc", "2.0"}, {"id", message["id"]},
               {"error", QJsonObject{{"code", -32601}, {"message", "Nova Studio does not provide this method."}}}});
        return;
    }
    if (message.contains("method")) {
        if (method == "session/update" && message["params"].toObject()["sessionId"].toString() == session) {
            update(message["params"].toObject());
        } else if (method == "$/cancel_request") {
            // The agent withdrew a question it asked; the original request is answered "cancelled" (-32800).
            const QJsonValue id = message["params"].toObject().value("requestId");
            if (permissions.removeOne(id)) {
                write({{"jsonrpc", "2.0"}, {"id", id},
                       {"error", QJsonObject{{"code", -32800}, {"message", "Request cancelled."}}}});
                emit permissionWithdrawn(id);
            }
        }
        return;
    }
    const auto id = qint64(message["id"].toDouble(-1));
    if (auto done = pending.take(id))
        done(message);
}

void Session::update(const QJsonObject &params) {
    const auto change = params["update"].toObject();
    const auto kind = change["sessionUpdate"].toString();
    if (kind == "current_mode_update") {
        modeState["currentModeId"] = change["currentModeId"].toString();
        emit modesChanged();
    } else if (kind == "config_option_update") {
        options = bounded(change["configOptions"].toArray());
        emit configOptionsChanged();
    } else if (kind == "available_commands_update") {
        availableCommands = bounded(change["availableCommands"].toArray());
        emit commandsChanged();
    } else if (kind == "usage_update") {
        used.used = qint64(change["used"].toDouble(-1));
        used.size = qint64(change["size"].toDouble(-1));
        const auto cost = change["cost"].toObject();
        used.cost = cost["amount"].isDouble() ? cost["amount"].toDouble() : -1;
        used.currency = cost["currency"].toString().left(8);
        emit detailsChanged();
    } else if (kind == "session_info_update") {
        if (change.contains("title")) {
            sessionTitle = change["title"].toString(); // null clears it
            emit detailsChanged();
        }
    } else if (replaying) {
        // session/load replays the conversation, which the window still shows.
    } else if (kind == "agent_message_chunk" || kind == "agent_thought_chunk") {
        const auto content = change["content"].toObject();
        const bool thought = kind == "agent_thought_chunk";
        if (content["type"] != "text")
            emit agentContent(thought, content);
        else if (thought)
            emit agentThought(content["text"].toString());
        else
            emit agentText(content["text"].toString());
    } else if (kind == "tool_call" || kind == "tool_call_update") {
        emit toolCall(change);
    } else if (kind == "plan") {
        emit planChanged(change["entries"].toArray());
    }
    // Other kinds (user_message_chunk echoes, notices, newer protocol additions) are not shown.
}

void Session::finish(const QString &why) {
    if (current == State::Ended)
        return;
    if (reason.isEmpty())
        reason = why;
    shutdownTimer.stop();
    treeTimer.stop();
    rememberSoon.stop();
    replyTimer.stop();
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
