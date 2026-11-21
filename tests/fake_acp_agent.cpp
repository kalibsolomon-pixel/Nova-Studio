// SPDX-License-Identifier: GPL-3.0-or-later
// A scripted ACP (protocol version 1) agent for tests. It starts every stdio MCP server it receives in
// session/new, as real agents do, and obeys prompts:
//   call <tool> <json>  MCP tools/call (generation/projectId added from nova.project.info); first
//                       reports any failed MCP initialize/tools/list from session start
//   ask                 session/request_permission (an edit), then reports the outcome
//   ask-queue           two edit permission requests at once
//   ask-command         an execute permission request
//   wait                waits for session/cancel and ends the turn "cancelled"
//   spawn               starts a long-lived child process (this program with --sleep)
//   crash               exits at once with code 3, leaving its children behind
//   env                 reports whether NOVA_MCP_CODE is in its environment
//   burst               2000 one-character message chunks
//   flood               5000 message chunks in a single write
//   mode                reports the current session mode and model setting
//   remember <text>     keeps <text> for this session (in FAKE_ACP_STATE, so session/load restores it)
//   recall              reports what the session remembers
//   cwd                 reports the session's working directory
//   usage               sends usage_update and session_info_update
//   extras              resource_link and image chunks, an unknown update kind, and a tool call with
//                       a kind and locations
//   ask-withdraw        session/request_permission, then withdraws it with $/cancel_request
//   late                starts a tool call and updates it after the turn ended
//   attached            reports the prompt's content blocks after the first
//   /review             a slash command (advertised with available_commands_update)
// It advertises loadSession (not with --no-load), session modes (ask, code, bypassPermissions) and a
// "model" select setting. FAKE_ACP_SKIP_MCP=1 makes it ignore mcpServers. No network, no files outside
// the given folders.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QThread>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
void send(const QJsonObject &message) {
    std::cout << QJsonDocument(message).toJson(QJsonDocument::Compact).toStdString() << '\n' << std::flush;
}
void reply(const QJsonValue &id, const QJsonObject &result) { send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}); }
void update(const QString &session, const QJsonObject &change) {
    send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", QJsonObject{{"sessionId", session}, {"update", change}}}});
}
void say(const QString &session, const QString &text) {
    update(session, {{"sessionUpdate", "agent_message_chunk"}, {"content", QJsonObject{{"type", "text"}, {"text", text}}}});
}
QJsonObject readMessage(bool *eof) {
    std::string line;
    if (!std::getline(std::cin, line)) {
        *eof = true;
        return {};
    }
    return QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
}

struct Server {
    std::unique_ptr<QProcess> process;
    int next = 1;
    QMap<QString, QJsonObject> properties; // input schema properties per tool
    QString setupProblem; // why initialize or tools/list gave no usable answer, with timings
    QJsonObject call(const QString &method, const QJsonObject &params) {
        const int id = ++next;
        process->write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}})
                           .toJson(QJsonDocument::Compact) + '\n');
        while (process->state() == QProcess::Running || process->canReadLine()) {
            if (!process->canReadLine() && !process->waitForReadyRead(10000)) {
                if (process->state() == QProcess::Running)
                    return {{"error", QJsonObject{{"code", -32000}, {"message", "No reply within 10 s."}}}};
                break;
            }
            while (process->canReadLine()) {
                const auto message = QJsonDocument::fromJson(process->readLine()).object();
                if (message["id"].toInt() == id)
                    return message;
            }
        }
        return {{"error", QJsonObject{{"code", -32000}, {"message", "The MCP server disconnected."}}}};
    }
    // Records a setup call that gave no usable result, so a later failure names its cause.
    QJsonObject setupCall(const QString &method, const QJsonObject &params) {
        QElapsedTimer timer;
        timer.start();
        const auto result = call(method, params);
        if (!result.contains("result"))
            setupProblem += QString("%1 failed after %2 ms: %3\n").arg(method).arg(timer.elapsed())
                                .arg(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)).left(400));
        return result;
    }
    QJsonObject projectScope() {
        // Index updates are a documented retryable response, not a project scope. Never turn
        // an error into a malformed dependent request by copying undefined JSON values.
        for (int attempt = 1; attempt <= 5; ++attempt) {
            const auto response = call("tools/call", {{"name", "nova.project.info"}, {"arguments", QJsonObject{}}});
            const auto result = response["result"].toObject();
            const auto scope = result["structuredContent"].toObject();
            if (!response.contains("error") && !result["isError"].toBool() &&
                !scope["generation"].toString().isEmpty() && !scope["projectId"].toString().isEmpty())
                return scope;
            const auto content = result["content"].toArray();
            const auto error = content.isEmpty() ? QJsonObject{} :
                QJsonDocument::fromJson(content.first().toObject()["text"].toString().toUtf8())
                    .object()["error"].toObject();
            std::cerr << "MCP SCOPE attempt " << attempt << ": "
                      << QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString() << '\n' << std::flush;
            // Nova deliberately discards agent stderr. Qualification keeps this synthetic
            // fixture's failed scope responses in its evidence folder as well.
            const auto evidence = qEnvironmentVariable("FAKE_ACP_DIAGNOSTICS");
            if (!evidence.isEmpty()) {
                QFile log(evidence);
                if (log.open(QIODevice::WriteOnly | QIODevice::Append))
                    log.write(QJsonDocument(QJsonObject{{"attempt", attempt}, {"response", response}})
                                  .toJson(QJsonDocument::Compact) + '\n');
            }
            if (error["code"].toString() != "validation_unavailable" || attempt == 5)
                return {};
            QThread::msleep(50);
        }
        return {};
    }
};

// Starts every stdio MCP server in a session/new or session/load request, as real agents do.
void startServers(const QJsonObject &params, std::vector<Server> &servers) {
    if (qEnvironmentVariable("FAKE_ACP_SKIP_MCP") == "1")
        return;
    for (const auto &value : params["mcpServers"].toArray()) {
        const auto entry = value.toObject();
        if (entry.contains("type"))
            continue;
        Server server;
        server.process = std::make_unique<QProcess>();
        auto env = QProcessEnvironment::systemEnvironment();
        for (const auto &pair : entry["env"].toArray())
            env.insert(pair.toObject()["name"].toString(), pair.toObject()["value"].toString());
        server.process->setProcessEnvironment(env);
        QStringList args;
        for (const auto &arg : entry["args"].toArray())
            args << arg.toString();
        server.process->start(entry["command"].toString(), args);
        if (!server.process->waitForStarted())
            continue;
        server.setupCall("initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}},
                                        {"clientInfo", QJsonObject{{"name", "fake-acp-agent"}, {"version", "1"}}}});
        server.process->write(R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n");
        for (const auto &tool : server.setupCall("tools/list", {})["result"].toObject()["tools"].toArray())
            server.properties.insert(tool.toObject()["name"].toString(),
                                     tool.toObject()["inputSchema"].toObject()["properties"].toObject());
        if (server.properties.isEmpty())
            server.setupProblem += "tools/list returned no tools\n";
        servers.push_back(std::move(server));
    }
}

QString stateFile(const QString &session) {
    const auto folder = qEnvironmentVariable("FAKE_ACP_STATE");
    return folder.isEmpty() ? QString{} : folder + '/' + session + ".json";
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--sleep")) {
        QThread::sleep(120);
        return 0;
    }
    std::vector<Server> servers;
    std::vector<std::unique_ptr<QProcess>> children;
    QString session, workingDirectory, mode = "ask", model = "fast";
    QStringList memory; // remember / recall, kept across session/load through FAKE_ACP_STATE
    const bool requireAuth = app.arguments().contains("--require-auth");
    bool authenticated = false;
    bool eof = false;
    const auto modes = [&mode] {
        return QJsonObject{{"currentModeId", mode}, {"availableModes", QJsonArray{
            QJsonObject{{"id", "ask"}, {"name", "Ask"}, {"description", "Asks before changing anything"}},
            QJsonObject{{"id", "code"}, {"name", "Code"}},
            QJsonObject{{"id", "bypassPermissions"}, {"name", "Bypass permissions"}}}}};
    };
    const auto settings = [&model] {
        return QJsonArray{QJsonObject{{"id", "model"}, {"name", "Model"}, {"category", "model"}, {"type", "select"},
                                      {"currentValue", model}, {"options", QJsonArray{
            QJsonObject{{"value", "fast"}, {"name", "Fast"}},
            QJsonObject{{"value", "deep"}, {"name", "Deep"}, {"description", "Slower, more thorough"}}}}}};
    };
    const auto commands = [&session] {
        update(session, {{"sessionUpdate", "available_commands_update"}, {"availableCommands", QJsonArray{
            QJsonObject{{"name", "review"}, {"description", "Review the pack's textures"}},
            QJsonObject{{"name", "plan"}, {"description", "Make a plan"}, {"input", QJsonObject{{"hint", "what to plan"}}}}}}});
    };
    while (!eof) {
        const auto message = readMessage(&eof);
        const auto method = message["method"].toString();
        const auto params = message["params"].toObject();
        if (method == "initialize") {
            QJsonArray methods;
            if (requireAuth)
                methods.append(QJsonObject{{"id", "fake-sign-in"}, {"name", "Scripted sign-in"}});
            reply(message["id"], {{"protocolVersion", 1}, {"agentInfo", QJsonObject{{"name", "fake-acp-agent"}, {"version", "1"}}},
                                  {"agentCapabilities", QJsonObject{
                                      {"loadSession", !app.arguments().contains("--no-load")},
                                      {"promptCapabilities", QJsonObject{{"image", false}, {"embeddedContext", false}}},
                                      {"sessionCapabilities", QJsonObject{{"close", QJsonObject{}}}}}},
                                  {"authMethods", methods}});
        } else if (method == "authenticate" && params["methodId"] == "fake-sign-in") {
            authenticated = true;
            reply(message["id"], {});
        } else if (method == "session/new" || method == "session/load") {
            if (requireAuth && !authenticated) {
                send({{"jsonrpc", "2.0"}, {"id", message["id"]},
                      {"error", QJsonObject{{"code", -32000}, {"message", "Sign-in needed"}}}});
                continue;
            }
            const bool load = method == "session/load";
            session = load ? params["sessionId"].toString() : "fake-session-1";
            workingDirectory = params["cwd"].toString();
            startServers(params, servers);
            if (load) {
                // Restores what this session remembered and replays it, as agents replay history.
                QFile saved(stateFile(session));
                if (saved.open(QIODevice::ReadOnly))
                    for (const auto &item : QJsonDocument::fromJson(saved.readAll()).array())
                        memory << item.toString();
                for (const auto &item : memory) {
                    update(session, {{"sessionUpdate", "user_message_chunk"},
                                     {"content", QJsonObject{{"type", "text"}, {"text", "remember " + item}}}});
                    say(session, "REMEMBERED " + item);
                }
                reply(message["id"], {{"modes", modes()}, {"configOptions", settings()}});
            } else {
                reply(message["id"], {{"sessionId", session}, {"modes", modes()}, {"configOptions", settings()}});
            }
            commands();
        } else if (method == "session/set_mode") {
            mode = params["modeId"].toString();
            reply(message["id"], {});
            update(session, {{"sessionUpdate", "current_mode_update"}, {"currentModeId", mode}});
        } else if (method == "session/set_config_option") {
            if (params["configId"] == "model")
                model = params["value"].toString();
            reply(message["id"], {{"configOptions", settings()}});
        } else if (method == "session/prompt") {
            const auto blocks = params["prompt"].toArray();
            const auto text = blocks.first().toObject()["text"].toString();
            QString stop = "end_turn";
            bool late = false;
            if (!text.startsWith("Please "))
                update(session, {{"sessionUpdate", "agent_thought_chunk"},
                                 {"content", QJsonObject{{"type", "text"}, {"text", "Thinking about: " + text.left(40)}}}});
            if (text.startsWith("call ") && !servers.empty()) {
                // The request below is built from the schemas read at session start; without them
                // the scope arguments are missing and Nova rightly rejects the call. Say why.
                if (!servers.front().setupProblem.isEmpty())
                    say(session, "MCP SETUP PROBLEM\n\n```\n" + servers.front().setupProblem + "```\n");
                const auto tool = text.section(' ', 1, 1);
                auto args = QJsonDocument::fromJson(text.section(' ', 2).toUtf8()).object();
                const auto scope = servers.front().projectScope();
                if (scope.isEmpty()) {
                    say(session, "RESULT error\nMCP SCOPE PROBLEM: nova.project.info returned no usable scope; see agent stderr.\n");
                    reply(message["id"], {{"stopReason", stop}});
                    continue;
                }
                for (const auto key : {"generation", "projectId"})
                    if (servers.front().properties.value(tool).contains(key))
                        args[key] = scope[key];
                update(session, {{"sessionUpdate", "tool_call"}, {"toolCallId", "c1"}, {"title", "nova " + tool},
                                 {"kind", "other"}, {"status", "in_progress"}});
                const auto result = servers.front().call("tools/call", {{"name", tool}, {"arguments", args}});
                const bool failed = result.contains("error") || result["result"].toObject()["isError"].toBool();
                update(session, {{"sessionUpdate", "tool_call_update"}, {"toolCallId", "c1"},
                                 {"status", failed ? "failed" : "completed"}});
                // A code block, so the window's Markdown rendering keeps the JSON as written.
                say(session, QString("RESULT %1\n\n```\n%2\n```\n").arg(failed ? "error" : "ok",
                    QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)).left(4000)));
            } else if (text == "ask" || text == "ask-queue" || text == "ask-command" ||
                       text == "Review local notes before updating the draft.") {
                const int count = text == "ask-queue" || text.startsWith("Review") ? 2 : 1;
                const QJsonArray options{
                    QJsonObject{{"optionId", "yes"}, {"name", "Allow"}, {"kind", "allow_once"}},
                    QJsonObject{{"optionId", "no"}, {"name", "Reject"}, {"kind", "reject_once"}},
                    QJsonObject{{"optionId", "yes-always"}, {"name", "Always allow"}, {"kind", "allow_always"}},
                    QJsonObject{{"optionId", "no-always"}, {"name", "Always reject"}, {"kind", "reject_always"}}};
                QStringList unanswered;
                for (int i = 1; i <= count; ++i) {
                    const auto id = QString("perm-%1").arg(i);
                    unanswered.append(id);
                    const QString path = i == 1 ? "notes.txt" : "draft.txt";
                    const bool command = text == "ask-command";
                    send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "session/request_permission"},
                          {"params", QJsonObject{{"sessionId", session},
                              {"toolCall", QJsonObject{{"toolCallId", id}, {"title", command ? "Run build" : "Write " + path},
                                                       {"kind", command ? "execute" : "edit"},
                                                       {"rawInput", command ? QJsonObject{{"command", "build"}}
                                                                            : QJsonObject{{"path", path}}}}},
                              {"options", options}}}});
                }
                while (!unanswered.isEmpty()) {
                    const auto answer = readMessage(&eof);
                    if (eof)
                        break;
                    if (unanswered.removeOne(answer["id"].toString())) {
                        const auto outcome = answer["result"].toObject()["outcome"].toObject();
                        say(session, "PERMISSION " + outcome["outcome"].toString() + " " + outcome["optionId"].toString() +
                                     " " + answer["id"].toString() + "\n");
                    }
                }
            } else if (text == "activity" || text == "Show your plan and texture checks.") {
                update(session, {{"sessionUpdate", "plan"}, {"entries", QJsonArray{
                    QJsonObject{{"content", "Inspect pack assets"}, {"priority", "high"}, {"status", "completed"}},
                    QJsonObject{{"content", "Review texture sizes"}, {"priority", "medium"}, {"status", "in_progress"}}}}});
                update(session, {{"sessionUpdate", "tool_call"}, {"toolCallId", "activity-1"},
                                 {"title", "nova.assets.list"}, {"status", "in_progress"}});
                update(session, {{"sessionUpdate", "tool_call_update"}, {"toolCallId", "activity-1"},
                                 {"status", "completed"}, {"rawOutput", QJsonObject{{"textures", 1}}}});
                update(session, {{"sessionUpdate", "tool_call"}, {"toolCallId", "activity-2"},
                                 {"title", "Review notes"}, {"status", "failed"}, {"content", QJsonArray{
                    QJsonObject{{"type", "diff"}, {"path", "notes.txt"}, {"oldText", "Before text"}, {"newText", "After text"}}}}});
                say(session, "Review finished.\n\n```text\nplain.png: 16x16\n```\n");
            } else if (text == "turn-error") {
                send({{"jsonrpc", "2.0"}, {"id", message["id"]},
                      {"error", QJsonObject{{"code", -32001}, {"message", "Scripted upstream failure."}}}});
                continue;
            } else if (text == "stream") {
                say(session, "First chunk");
                QThread::msleep(450);
                say(session, " and the next chunk.");
            } else if (text == "wait") {
                for (;;) {
                    const auto next = readMessage(&eof);
                    if (eof || next["method"] == "session/cancel")
                        break;
                }
                stop = "cancelled";
            } else if (text == "spawn") {
                auto child = std::make_unique<QProcess>();
                child->start(QCoreApplication::applicationFilePath(), {"--sleep"});
                child->waitForStarted();
                say(session, QString("SPAWNED %1").arg(child->processId()));
                children.push_back(std::move(child));
            } else if (text == "crash") {
                std::fputs("discarded startup message\n", stderr);
                const QByteArray filler(20 * 1024, 'x');
                std::fwrite(filler.constData(), 1, size_t(filler.size()), stderr);
                std::fputs("\n\x1b[31mScripted failure: missing dependency.\x1b[0m\n", stderr);
                std::fflush(stderr);
                std::fflush(stdout);
                std::_Exit(3);
            } else if (text.startsWith("Please ")) {
                // A natural exchange for the conversation screenshot: reasoning, then a Markdown reply.
                update(session, {{"sessionUpdate", "agent_thought_chunk"},
                                 {"content", QJsonObject{{"type", "text"}, {"text",
                                     "I'll list the pack's textures with Nova's read-only tools and compare their sizes."}}}});
                say(session, "I checked the textures in **Synthetic Pack**:\n\n"
                             "- `assets/nova/textures/block/plain.png` is already 16×16.\n"
                             "- No other textures need a 16×16 version.\n\n"
                             "Nothing needs changing. Tell me if you'd like me to look at the models next.");
            } else if (text == "env") {
                say(session, QString("ENV NOVA_MCP_CODE=%1").arg(qEnvironmentVariableIsSet("NOVA_MCP_CODE") ? "present" : "absent"));
            } else if (text == "args") {
                say(session, "ARGUMENTS " + app.arguments().mid(1).join(" | "));
            } else if (text == "burst") {
                for (int i = 0; i < 2000; ++i)
                    say(session, QString(QChar('a' + i % 26)));
                say(session, " BURST DONE");
            } else if (text == "flood") {
                QByteArray lines;
                for (int i = 0; i < 5000; ++i)
                    lines += QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", QJsonObject{
                        {"sessionId", session}, {"update", QJsonObject{{"sessionUpdate", "agent_message_chunk"},
                            {"content", QJsonObject{{"type", "text"}, {"text", "f"}}}}}}}}).toJson(QJsonDocument::Compact) + '\n';
                std::cout.write(lines.constData(), lines.size());
                say(session, " FLOOD DONE");
            } else if (text == "mode") {
                say(session, "MODE " + mode + " MODEL " + model);
            } else if (text.startsWith("remember ")) {
                memory << text.mid(9);
                QFile saved(stateFile(session));
                if (!stateFile(session).isEmpty() && QDir().mkpath(QFileInfo(saved).path()) && saved.open(QIODevice::WriteOnly))
                    saved.write(QJsonDocument(QJsonArray::fromStringList(memory)).toJson(QJsonDocument::Compact));
                say(session, "REMEMBERED " + text.mid(9));
            } else if (text == "recall") {
                say(session, "RECALL " + memory.join(','));
            } else if (text == "cwd") {
                say(session, "CWD " + workingDirectory);
            } else if (text == "usage") {
                update(session, {{"sessionUpdate", "usage_update"}, {"used", 12345}, {"size", 200000},
                                 {"cost", QJsonObject{{"amount", 0.42}, {"currency", "USD"}}}});
                update(session, {{"sessionUpdate", "session_info_update"}, {"title", "Texture review"}});
                say(session, "USAGE SENT");
            } else if (text == "extras") {
                update(session, {{"sessionUpdate", "agent_message_chunk"}, {"content", QJsonObject{
                    {"type", "resource_link"}, {"name", "Pack guide"}, {"uri", "https://example.invalid/guide [1]"}}}});
                update(session, {{"sessionUpdate", "agent_message_chunk"}, {"content", QJsonObject{
                    {"type", "image"}, {"mimeType", "image/png"}, {"data", QString(4000, 'A')}}}});
                update(session, {{"sessionUpdate", "mystery_update"}, {"payload", QJsonObject{{"x", 1}}}});
                update(session, {{"sessionUpdate", "tool_call"}, {"toolCallId", "extras-1"}, {"title", "Read notes"},
                                 {"kind", "read"}, {"status", "completed"}, {"locations", QJsonArray{
                    QJsonObject{{"path", "C:/pack/notes.txt"}, {"line", 12}}}}});
                say(session, "EXTRAS DONE");
            } else if (text == "ask-withdraw") {
                send({{"jsonrpc", "2.0"}, {"id", "perm-w"}, {"method", "session/request_permission"},
                      {"params", QJsonObject{{"sessionId", session},
                          {"toolCall", QJsonObject{{"toolCallId", "perm-w"}, {"title", "Write withdrawn.txt"}, {"kind", "edit"}}},
                          {"options", QJsonArray{QJsonObject{{"optionId", "yes"}, {"name", "Allow"}, {"kind", "allow_once"}}}}}}});
                QThread::msleep(300);
                send({{"jsonrpc", "2.0"}, {"method", "$/cancel_request"}, {"params", QJsonObject{{"requestId", "perm-w"}}}});
                for (;;) {
                    const auto answer = readMessage(&eof);
                    if (eof || answer["id"] == "perm-w") {
                        say(session, QString("WITHDRAWN %1").arg(answer["error"].toObject()["code"].toInt()));
                        break;
                    }
                }
            } else if (text == "late") {
                update(session, {{"sessionUpdate", "tool_call"}, {"toolCallId", "late-1"}, {"title", "Search textures"},
                                 {"kind", "search"}, {"status", "in_progress"}});
                late = true;
            } else if (text == "attached") {
                QStringList extra;
                for (qsizetype i = 1; i < blocks.size(); ++i)
                    extra << blocks.at(i).toObject()["type"].toString() + ": " + blocks.at(i).toObject()["text"].toString();
                say(session, QString("BLOCKS %1 %2").arg(blocks.size()).arg(extra.join(" | ")));
            } else if (text.startsWith("/review")) {
                say(session, "COMMAND review");
            }
            reply(message["id"], {{"stopReason", stop}});
            if (late) {
                QThread::msleep(200); // the update arrives after the turn ended
                update(session, {{"sessionUpdate", "tool_call_update"}, {"toolCallId", "late-1"}, {"status", "completed"}});
            }
        } else if (method == "session/close") {
            reply(message["id"], {});
        } else if (message.contains("id") && message.contains("method")) {
            send({{"jsonrpc", "2.0"}, {"id", message["id"]}, {"error", QJsonObject{{"code", -32601}, {"message", "unknown"}}}});
        }
    }
    for (auto &server : servers)
        server.process->closeWriteChannel();
    for (auto &server : servers)
        server.process->waitForFinished(3000);
    return 0;
}
