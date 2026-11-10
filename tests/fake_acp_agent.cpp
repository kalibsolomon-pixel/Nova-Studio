// SPDX-License-Identifier: GPL-3.0-or-later
// A scripted ACP (protocol version 1) agent for tests. It starts every stdio MCP server it receives in
// session/new, as real agents do, and obeys prompts:
//   call <tool> <json>  MCP tools/call (generation/projectId added from nova.project.info); first
//                       reports any failed MCP initialize/tools/list from session start
//   ask                 session/request_permission, then reports the outcome
//   wait                waits for session/cancel and ends the turn "cancelled"
//   spawn               starts a long-lived child process (this program with --sleep)
//   crash               exits at once with code 3, leaving its children behind
//   env                 reports whether NOVA_MCP_CODE is in its environment
// FAKE_ACP_SKIP_MCP=1 makes it ignore mcpServers. No network, no files outside the given folders.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
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
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--sleep")) {
        QThread::sleep(120);
        return 0;
    }
    std::vector<Server> servers;
    std::vector<std::unique_ptr<QProcess>> children;
    QString session;
    bool eof = false;
    while (!eof) {
        const auto message = readMessage(&eof);
        const auto method = message["method"].toString();
        const auto params = message["params"].toObject();
        if (method == "initialize") {
            reply(message["id"], {{"protocolVersion", 1}, {"agentInfo", QJsonObject{{"name", "fake-acp-agent"}, {"version", "1"}}},
                                  {"agentCapabilities", QJsonObject{{"sessionCapabilities", QJsonObject{{"close", QJsonObject{}}}}}},
                                  {"authMethods", QJsonArray{}}});
        } else if (method == "session/new") {
            session = "fake-session-1";
            if (qEnvironmentVariable("FAKE_ACP_SKIP_MCP") != "1")
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
            reply(message["id"], {{"sessionId", session}});
        } else if (method == "session/prompt") {
            const auto text = params["prompt"].toArray().first().toObject()["text"].toString();
            QString stop = "end_turn";
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
            } else if (text == "ask") {
                send({{"jsonrpc", "2.0"}, {"id", "perm-1"}, {"method", "session/request_permission"},
                      {"params", QJsonObject{{"sessionId", session},
                          {"toolCall", QJsonObject{{"toolCallId", "c2"}, {"title", "write notes.txt"}, {"kind", "edit"},
                                                   {"rawInput", QJsonObject{{"path", "notes.txt"}}}}},
                          {"options", QJsonArray{QJsonObject{{"optionId", "yes"}, {"name", "Allow"}, {"kind", "allow_once"}},
                                                 QJsonObject{{"optionId", "no"}, {"name", "Reject"}, {"kind", "reject_once"}}}}}}});
                for (;;) {
                    const auto answer = readMessage(&eof);
                    if (eof || answer["id"] == "perm-1") {
                        const auto outcome = answer["result"].toObject()["outcome"].toObject();
                        say(session, "PERMISSION " + outcome["outcome"].toString() + " " + outcome["optionId"].toString());
                        break;
                    }
                }
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
            }
            reply(message["id"], {{"stopReason", stop}});
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
