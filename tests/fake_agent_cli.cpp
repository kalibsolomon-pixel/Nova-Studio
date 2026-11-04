// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-G test double for an AI agent's command-line tool. Copied as claude.exe or codex.exe.
// Records its arguments and applies "mcp add" / "mcp remove" to the configuration the real tool
// would use (CLAUDE_CONFIG_DIR/.claude.json or CODEX_HOME/config.toml).
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto args = app.arguments();
    const auto tool = QFileInfo(args.takeFirst()).completeBaseName().toLower();
    const auto home = qEnvironmentVariable(tool == "claude" ? "CLAUDE_CONFIG_DIR" : "CODEX_HOME");
    QFile log(home + "/fake-agent-calls.jsonl");
    if (log.open(QIODevice::Append))
        log.write(QJsonDocument(QJsonObject{{"tool", tool}, {"args", QJsonArray::fromStringList(args)}})
                      .toJson(QJsonDocument::Compact) + '\n');
    if (qEnvironmentVariableIsSet("NOVA_FAKE_AGENT_FAIL")) {
        std::fputs("simulated failure\n", stderr);
        return 3;
    }
    if (args.size() < 3 || args[0] != "mcp")
        return 1;
    const bool add = args[1] == "add";
    args = args.mid(2);
    args.removeAll("--scope");
    args.removeAll("user");
    const auto name = args.value(0);
    const auto separator = args.indexOf("--");
    const auto command = separator > 0 ? args.value(separator + 1) : QString{};
    if (tool == "claude") {
        QFile file(home + "/.claude.json");
        QJsonObject root;
        if (file.open(QIODevice::ReadOnly))
            root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        auto servers = root["mcpServers"].toObject();
        if (add) {
            if (servers.contains(name)) {
                std::fputs("MCP server already exists\n", stderr);
                return 1;
            }
            servers[name] = QJsonObject{{"type", "stdio"}, {"command", command}, {"args", QJsonArray{}}, {"env", QJsonObject{}}};
        } else
            servers.remove(name);
        root["mcpServers"] = servers;
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return 1;
        file.write(QJsonDocument(root).toJson());
        std::printf("%s MCP server %s\n", add ? "Added" : "Removed", qPrintable(name));
        return 0;
    }
    QFile file(home + "/config.toml");
    if (!add)
        return 0;
    if (!file.open(QIODevice::Append))
        return 1;
    auto escaped = command;
    escaped.replace("\\", "\\\\");
    file.write(QString("\n[mcp_servers.%1]\ncommand = \"%2\"\n").arg(name, escaped).toUtf8());
    std::printf("Added global MCP server '%s'.\n", qPrintable(name));
    return 0;
}
