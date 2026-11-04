// SPDX-License-Identifier: GPL-3.0-or-later
#include "clients.h"
#include "revision.h"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <optional>
#include <qt_windows.h>
#include <tlhelp32.h>

namespace nova::mcp::clients {
namespace {
constexpr qint64 ConfigLimit = 64LL * 1024 * 1024;
constexpr auto ClaudePackage = "Claude_pzs8sxrjxfjjc";

QString clean(const QString &path) { return path.isEmpty() ? QString{} : QDir::cleanPath(path); }
QString native(const QString &path) { return QDir::toNativeSeparators(path); }
bool samePath(const QString &a, const QString &b) {
    return !a.isEmpty() && QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(
                               QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive) == 0;
}
bool isBridgeCommand(const QString &command) {
    return QFileInfo(QDir::fromNativeSeparators(command)).fileName().compare("nova-mcp.exe", Qt::CaseInsensitive) == 0;
}
// Newest file matching a pattern below a directory, at a fixed depth.
QStringList globFiles(const QString &base, const QStringList &segments) {
    QStringList current{base};
    for (const auto &segment : segments) {
        QStringList next;
        for (const auto &dir : current) {
            const bool last = &segment == &segments.last();
            const auto filters = last ? QDir::Files : QDir::Dirs | QDir::NoDotAndDotDot;
            for (const auto &entry : QDir(dir).entryInfoList({segment}, filters, QDir::Time))
                next << entry.absoluteFilePath();
        }
        current = next;
    }
    return current;
}
QString newest(const QStringList &files) {
    QString best;
    QDateTime when;
    for (const auto &file : files) {
        const auto modified = QFileInfo(file).lastModified();
        if (best.isEmpty() || modified > when) {
            best = file;
            when = modified;
        }
    }
    return best;
}
QStringList runningImages() {
    QStringList images;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return images;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (bool more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"claude.exe") != 0)
            continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        wchar_t path[MAX_PATH * 2];
        DWORD size = DWORD(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size))
            images << QString::fromWCharArray(path, int(size));
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return images;
}
// Only a native executable is run; batch wrappers are never started with arguments.
QString findExecutable(const QString &name, const QString &searchPath, const QStringList &extra) {
    for (const auto &dir : searchPath.split(';', Qt::SkipEmptyParts)) {
        const auto candidate = QDir(dir.trimmed()).filePath(name + ".exe");
        if (QDir::isAbsolutePath(dir.trimmed()) && QFileInfo(candidate).isFile())
            return clean(candidate);
    }
    for (const auto &candidate : extra)
        if (QFileInfo(candidate).isFile())
            return clean(candidate);
    return {};
}

std::optional<QJsonObject> readJsonObject(const QString &path, QString *error) {
    QString readError;
    auto bytes = readFileBytes(path, ConfigLimit, &readError);
    if (!readError.isEmpty()) {
        *error = "The configuration file could not be read: " + readError;
        return std::nullopt;
    }
    if (bytes.startsWith("\xEF\xBB\xBF"))
        bytes.remove(0, 3);
    if (bytes.trimmed().isEmpty())
        return QJsonObject{};
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        *error = parse.error != QJsonParseError::NoError
            ? QString("The configuration file is not valid JSON (%1 at byte %2).").arg(parse.errorString()).arg(parse.offset)
            : QString("The configuration file does not contain a JSON object.");
        return std::nullopt;
    }
    return document.object();
}
Status jsonStatus(const QString &path, const QString &bridgePath) {
    Status status;
    if (!QFileInfo::exists(path)) {
        status.state = Status::NotConfigured;
        status.detail = "No configuration file yet. Nova Studio can create it.";
        return status;
    }
    QString error;
    const auto object = readJsonObject(path, &error);
    if (!object) {
        status.state = Status::Invalid;
        status.detail = error + " Nova Studio will not change it.";
        return status;
    }
    const auto servers = object->value("mcpServers");
    if (!servers.isUndefined() && !servers.isObject()) {
        status.state = Status::Invalid;
        status.detail = "\"mcpServers\" is not an object. Nova Studio will not change it.";
        return status;
    }
    status.state = Status::NotConfigured;
    status.detail = "Nova Studio is not in this configuration.";
    const auto entries = servers.toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const auto command = it.value().toObject()["command"].toString();
        const bool ours = it.key() == ServerName;
        if (!ours && !isBridgeCommand(command))
            continue;
        status.configuredName = it.key();
        status.configuredCommand = command;
        if (samePath(command, bridgePath)) {
            status.state = Status::Configured;
            status.detail = QString("Configured as \"%1\" with this copy of nova-mcp.exe.").arg(it.key());
            return status;
        }
        status.state = Status::DifferentPath;
        status.detail = QString("\"%1\" starts a different nova-mcp.exe: %2").arg(it.key(), native(command));
        if (ours)
            return status;
    }
    return status;
}
// Line-based reading of Codex's TOML: only the [mcp_servers.<name>] tables matter.
Status tomlStatus(const QString &path, const QString &bridgePath) {
    Status status;
    if (!QFileInfo::exists(path)) {
        status.state = Status::NotConfigured;
        status.detail = "No Codex configuration file yet. Codex creates it when Nova Studio is added.";
        return status;
    }
    QString error;
    const auto bytes = readFileBytes(path, ConfigLimit, &error);
    if (!error.isEmpty()) {
        status.state = Status::Invalid;
        status.detail = "The Codex configuration could not be read: " + error;
        return status;
    }
    static const QRegularExpression header(R"re(^\s*\[\s*mcp_servers\s*\.\s*("([^"]+)"|'([^']+)'|([A-Za-z0-9_-]+))\s*\]\s*(#.*)?$)re");
    static const QRegularExpression anyHeader(R"re(^\s*\[)re");
    static const QRegularExpression command(R"re(^\s*command\s*=\s*("((?:[^"\\]|\\.)*)"|'([^']*)')\s*(#.*)?$)re");
    status.state = Status::NotConfigured;
    status.detail = "Nova Studio is not in the Codex configuration.";
    QString table;
    for (const auto &raw : QString::fromUtf8(bytes).split('\n')) {
        const auto line = raw.trimmed();
        if (anyHeader.match(line).hasMatch()) {
            const auto match = header.match(line);
            table = match.hasMatch() ? match.captured(2) + match.captured(3) + match.captured(4) : QString{};
            continue;
        }
        if (table.isEmpty())
            continue;
        const auto match = command.match(line);
        if (!match.hasMatch())
            continue;
        auto value = match.captured(3);
        if (!match.captured(2).isEmpty()) {
            value = match.captured(2);
            value.replace(R"(\\)", "\x01").replace(R"(\")", "\"").replace('\x01', '\\');
        }
        if (table != ServerName && !isBridgeCommand(value))
            continue;
        status.configuredName = table;
        status.configuredCommand = value;
        if (samePath(value, bridgePath)) {
            status.state = Status::Configured;
            status.detail = QString("Configured as \"%1\" with this copy of nova-mcp.exe.").arg(table);
            return status;
        }
        status.state = Status::DifferentPath;
        status.detail = QString("\"%1\" starts a different nova-mcp.exe: %2").arg(table, native(value));
        if (table == ServerName)
            return status;
    }
    return status;
}

// ---- minimal JSON scanner for text-preserving edits
struct Scanner {
    const QByteArray &text;
    qsizetype ws(qsizetype i) const {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
            ++i;
        return i;
    }
    qsizetype string(qsizetype i) const {
        if (i >= text.size() || text[i] != '"')
            return -1;
        for (++i; i < text.size(); ++i) {
            if (text[i] == '\\')
                ++i;
            else if (text[i] == '"')
                return i + 1;
        }
        return -1;
    }
    qsizetype value(qsizetype i) const {
        i = ws(i);
        if (i >= text.size())
            return -1;
        if (text[i] == '"')
            return string(i);
        if (text[i] == '{' || text[i] == '[') {
            const char close = text[i] == '{' ? '}' : ']';
            i = ws(i + 1);
            if (i < text.size() && text[i] == close)
                return i + 1;
            while (i < text.size()) {
                if (close == '}') {
                    i = string(ws(i));
                    if (i < 0)
                        return -1;
                    i = ws(i);
                    if (i >= text.size() || text[i] != ':')
                        return -1;
                    ++i;
                }
                i = value(i);
                if (i < 0)
                    return -1;
                i = ws(i);
                if (i < text.size() && text[i] == ',') {
                    ++i;
                    continue;
                }
                return i < text.size() && text[i] == close ? i + 1 : -1;
            }
            return -1;
        }
        while (i < text.size() && !QByteArrayView(",}] \t\r\n").contains(text[i]))
            ++i;
        return i;
    }
    struct Member {
        QString key;
        qsizetype keyBegin, valueBegin, valueEnd;
    };
    // Members of the object starting at `open` ('{'); `close` receives the '}' index.
    std::optional<QList<Member>> members(qsizetype open, qsizetype *close) const {
        QList<Member> result;
        qsizetype i = ws(open + 1);
        if (i < text.size() && text[i] == '}') {
            *close = i;
            return result;
        }
        while (i < text.size()) {
            i = ws(i);
            const auto keyEnd = string(i);
            if (keyEnd < 0)
                return std::nullopt;
            const auto key = QJsonDocument::fromJson("[" + text.mid(i, keyEnd - i) + "]").array().at(0).toString();
            qsizetype colon = ws(keyEnd);
            if (colon >= text.size() || text[colon] != ':')
                return std::nullopt;
            const auto begin = ws(colon + 1);
            const auto end = value(begin);
            if (end < 0)
                return std::nullopt;
            result.append({key, i, begin, end});
            i = ws(end);
            if (i < text.size() && text[i] == ',') {
                ++i;
                continue;
            }
            if (i < text.size() && text[i] == '}') {
                *close = i;
                return result;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }
    QByteArray indentBefore(qsizetype position) const {
        qsizetype start = position;
        while (start > 0 && text[start - 1] != '\n')
            --start;
        QByteArray indent;
        for (qsizetype i = start; i < position && (text[i] == ' ' || text[i] == '\t'); ++i)
            indent += text[i];
        return indent;
    }
};
QByteArray indentEntry(const QByteArray &entry, const QByteArray &indent) {
    auto lines = entry.split('\n');
    for (qsizetype i = 1; i < lines.size(); ++i)
        lines[i] = indent + lines[i];
    return lines.join('\n');
}
QByteArray entryFor(const QString &bridgePath) {
    return QJsonDocument(QJsonObject{{"command", native(bridgePath)}, {"args", QJsonArray{}}})
        .toJson(QJsonDocument::Indented).trimmed();
}
} // namespace

Environment Environment::system() {
    Environment environment;
    environment.localAppData = clean(qEnvironmentVariable("LOCALAPPDATA"));
    environment.appData = clean(qEnvironmentVariable("APPDATA"));
    environment.userProfile = clean(qEnvironmentVariable("USERPROFILE"));
    environment.codexHome = clean(qEnvironmentVariable("CODEX_HOME"));
    environment.claudeConfigDir = clean(qEnvironmentVariable("CLAUDE_CONFIG_DIR"));
    environment.path = qEnvironmentVariable("PATH");
    environment.ignoreRunning = qEnvironmentVariableIsSet("NOVA_MCP_TEST_IGNORE_RUNNING");
    environment.backupDirectory = environment.localAppData.isEmpty() ? QString{}
        : environment.localAppData + "/Nova Studio/Nova Studio/mcp/client-backups";
    return environment;
}

Bridge locateBridge(const QString &applicationDirectory) {
    Bridge bridge;
    bridge.path = clean(QDir(applicationDirectory).filePath("nova-mcp.exe"));
    bridge.exists = QFileInfo(bridge.path).isFile();
    for (auto dir = QDir(applicationDirectory); ; ) {
        if (QFileInfo::exists(dir.filePath("CMakeCache.txt"))) {
            bridge.developmentBuild = true;
            break;
        }
        if (!dir.cdUp() || dir.isRoot())
            break;
    }
    return bridge;
}

QList<Client> detect(const Environment &env) {
    QList<Client> result;
    const auto images = env.ignoreRunning ? QStringList{} : runningImages();
    // Claude Desktop: three known per-user locations. A single existing file is unambiguous.
    {
        Client client{Kind::ClaudeDesktop, "claude-desktop", "Claude Desktop"};
        QStringList evidence;
        const bool msix = !env.localAppData.isEmpty() &&
            (QFileInfo(env.localAppData + "/Packages/" + ClaudePackage).isDir() ||
             !globFiles(env.localAppData + "/Packages", {"Claude_*"}).isEmpty());
        const bool classic = !env.localAppData.isEmpty() && QFileInfo(env.localAppData + "/AnthropicClaude").isDir();
        if (msix)
            evidence << "Microsoft Store / MSIX package";
        if (classic)
            evidence << "installer in AppData\\Local\\AnthropicClaude";
        QStringList existing;
        if (!env.localAppData.isEmpty()) {
            existing << globFiles(env.localAppData + "/Claude-Data", {"claude_*", "claude_desktop_config.json"});
            existing << globFiles(env.localAppData + "/Packages",
                                  {"Claude_*", "LocalCache", "Roaming", "Claude", "claude_desktop_config.json"});
        }
        const auto roaming = env.appData.isEmpty() ? QString{} : env.appData + "/Claude/claude_desktop_config.json";
        if (!roaming.isEmpty() && QFileInfo(roaming).isFile())
            existing << roaming;
        existing.removeDuplicates();
        client.installed = msix || classic || !existing.isEmpty() ||
                           (!env.appData.isEmpty() && QFileInfo(env.appData + "/Claude").isDir());
        for (const auto &image : images)
            if (image.contains("\\WindowsApps\\Claude_", Qt::CaseInsensitive) ||
                image.contains("\\AnthropicClaude\\", Qt::CaseInsensitive))
                client.running = true;
        if (existing.size() == 1) {
            client.configPath = clean(existing.first());
            client.method = Method::EditJson;
        } else if (existing.size() > 1) {
            client.candidates = existing;
            client.manualReason = "Claude Desktop has more than one configuration file on this PC, so Nova Studio "
                                  "cannot tell which one it reads. Add the configuration manually from Claude "
                                  "Desktop › Settings › Developer › Edit Config.";
        } else if (classic || (!msix && client.installed)) {
            client.configPath = roaming;
            client.method = Method::EditJson;
        } else if (msix) {
            client.manualReason = "Claude Desktop has not created its configuration file yet. Open Claude Desktop › "
                                  "Settings › Developer › Edit Config once, close Claude Desktop, then check again.";
        } else {
            client.manualReason = "Claude Desktop was not found for this Windows user.";
        }
        if (!client.configPath.isEmpty())
            evidence << "configuration " + native(client.configPath);
        client.evidence = evidence.join("; ");
        result << client;
    }
    // Claude Code: its own CLI changes user-scoped configuration.
    {
        Client client{Kind::ClaudeCode, "claude-code", "Claude Code"};
        QStringList extra;
        if (!env.userProfile.isEmpty())
            extra << env.userProfile + "/.local/bin/claude.exe";
        if (!env.localAppData.isEmpty())
            extra << newest(globFiles(env.localAppData + "/Claude-Data", {"claude_*", "claude-code", "*", "*", "claude.exe"}));
        extra.removeAll(QString{});
        client.cliPath = findExecutable("claude", env.path, extra);
        const auto base = env.claudeConfigDir.isEmpty() ? env.userProfile : env.claudeConfigDir;
        client.configPath = base.isEmpty() ? QString{} : base + "/.claude.json";
        client.installed = !client.cliPath.isEmpty() || QFileInfo(client.configPath).isFile();
        client.evidence = client.cliPath.isEmpty() ? QString("configuration %1").arg(native(client.configPath))
                                                   : "command-line tool " + native(client.cliPath);
        if (!client.cliPath.isEmpty())
            client.method = Method::VendorCli;
        else
            client.manualReason = "The Claude Code command-line tool (claude.exe) was not found. Run the command "
                                  "below in a terminal where Claude Code is available.";
        result << client;
    }
    // Codex: its own CLI changes config.toml (CODEX_HOME or %USERPROFILE%\.codex).
    {
        Client client{Kind::Codex, "codex", "Codex"};
        QStringList extra;
        if (!env.localAppData.isEmpty())
            extra << newest(globFiles(env.localAppData + "/OpenAI/Codex/bin", {"*", "codex.exe"}));
        extra.removeAll(QString{});
        client.cliPath = findExecutable("codex", env.path, extra);
        const auto home = env.codexHome.isEmpty() ? (env.userProfile.isEmpty() ? QString{} : env.userProfile + "/.codex")
                                                  : env.codexHome;
        client.configPath = home.isEmpty() ? QString{} : home + "/config.toml";
        client.installed = !client.cliPath.isEmpty() || QFileInfo(home).isDir();
        client.evidence = client.cliPath.isEmpty() ? QString("configuration folder %1").arg(native(home))
                                                   : "command-line tool " + native(client.cliPath);
        if (!client.cliPath.isEmpty())
            client.method = Method::VendorCli;
        else
            client.manualReason = "The Codex command-line tool (codex.exe) was not found. Run the command below "
                                  "where Codex is available, or add the TOML table to config.toml.";
        result << client;
    }
    result << Client{Kind::Generic, "other", "Other MCP client (stdio)", true, "Any client that starts a local "
                     "stdio MCP server", {}, {}, {}, Method::Manual,
                     "Each client stores MCP servers differently. Use the command and JSON below."};
    return result;
}

Status inspect(const Client &client, const QString &bridgePath) {
    Status status;
    switch (client.kind) {
    case Kind::ClaudeDesktop:
        if (!client.installed) {
            status.state = Status::NotDetected;
            status.detail = "Claude Desktop was not found.";
        } else if (!client.candidates.isEmpty()) {
            status.state = Status::Unknown;
            status.detail = "Several configuration files: " + QStringList(client.candidates).replaceInStrings("/", "\\").join("; ");
        } else if (client.configPath.isEmpty()) {
            status.state = Status::Unknown;
            status.detail = client.manualReason;
        } else
            status = jsonStatus(client.configPath, bridgePath);
        break;
    case Kind::ClaudeCode:
        if (!client.installed) {
            status.state = Status::NotDetected;
            status.detail = "Claude Code was not found.";
        } else if (!QFileInfo::exists(client.configPath)) {
            status.state = Status::NotConfigured;
            status.detail = "Claude Code has no user configuration yet.";
        } else
            status = jsonStatus(client.configPath, bridgePath);
        break;
    case Kind::Codex:
        if (!client.installed) {
            status.state = Status::NotDetected;
            status.detail = "Codex was not found.";
        } else
            status = tomlStatus(client.configPath, bridgePath);
        break;
    case Kind::Generic:
        status.state = Status::Unknown;
        status.detail = "Nova Studio cannot read other clients' configuration. Use the manual setup below.";
        break;
    }
    return status;
}

QString shellArgument(const QString &path) {
    const auto text = native(path);
    if (text.contains(QRegularExpression("[$`%\"']")))
        return "'" + QString(text).replace("'", "''") + "'"; // PowerShell literal string
    return "\"" + text + "\"";
}
QString tomlString(const QString &text) {
    QString out = "\"";
    for (const auto ch : text) {
        if (ch == '\\' || ch == '"')
            out += '\\' + QString(ch);
        else if (ch.unicode() < 0x20 || ch.unicode() == 0x7f)
            out += QString("\\u%1").arg(uint(ch.unicode()), 4, 16, QChar('0'));
        else
            out += ch;
    }
    return out + "\"";
}
QString jsonString(const QString &text) {
    const auto array = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}

QString manualConfiguration(const Client &client, const QString &bridgePath) {
    const auto path = native(bridgePath);
    const auto json = QString("{\n  \"mcpServers\": {\n    \"%1\": {\n      \"command\": %2,\n      \"args\": []\n    }\n  }\n}")
                          .arg(ServerName, jsonString(path));
    switch (client.kind) {
    case Kind::ClaudeDesktop:
        return json;
    case Kind::ClaudeCode:
        return QString("claude mcp add --scope user %1 -- %2").arg(ServerName, shellArgument(path));
    case Kind::Codex:
        return QString("codex mcp add %1 -- %2\n\n# or add to config.toml:\n[mcp_servers.%1]\ncommand = %3")
            .arg(ServerName, shellArgument(path), tomlString(path));
    case Kind::Generic:
        return QString("Transport: stdio\nCommand: %1\nArguments: none\nEnvironment: none\n\n%2").arg(path, json);
    }
    return {};
}

QString instructions(const Client &client) {
    const QString allow = "Then, each time you start a session, choose Allow read-only (or Allow editing) on the "
                          "Access tab first. Your agent has five minutes to connect.";
    switch (client.kind) {
    case Kind::ClaudeDesktop:
        return "1. Quit Claude Desktop (it can rewrite its configuration while running).\n"
               "2. Add Nova Studio, or paste the JSON into claude_desktop_config.json.\n"
               "3. Choose Allow on the Access tab, then start Claude Desktop.\n" + allow +
               "\nIf the server fails, open Claude Desktop › Settings › Developer to see its log.";
    case Kind::ClaudeCode:
        return "1. Add Nova Studio (runs \"claude mcp add\" for your user), or run the command yourself.\n"
               "2. Choose Allow on the Access tab, then start Claude Code, or run /mcp and reconnect nova-studio.\n" +
               allow + "\nCheck with: claude mcp list";
    case Kind::Codex:
        return "1. Add Nova Studio (runs \"codex mcp add\"), or run the command yourself.\n"
               "2. Choose Allow on the Access tab, then start a new Codex session.\n" + allow +
               "\nCheck with: codex mcp list";
    case Kind::Generic:
        return "Add a stdio MCP server that runs the command below with no arguments. Choose Allow on the Access "
               "tab, then start the client within five minutes. Advanced: a client can instead pass the code from "
               "Copy connection code in the NOVA_MCP_CODE environment variable.";
    }
    return {};
}

QByteArray setJsonServer(const QByteArray &document, const QString &name, const QByteArray &entryJson,
                         QString *error) {
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return QByteArray{};
    };
    const bool bom = document.startsWith("\xEF\xBB\xBF");
    const QByteArray text = bom ? document.mid(3) : document;
    QJsonParseError parse;
    const auto before = QJsonDocument::fromJson(text.trimmed().isEmpty() ? QByteArray("{}") : text, &parse);
    if (parse.error != QJsonParseError::NoError || !before.isObject())
        return fail("The configuration is not a valid JSON object.");
    const auto entryValue = QJsonDocument::fromJson(entryJson).object();
    if (entryValue.isEmpty())
        return fail("The new entry is not a JSON object.");
    const auto key = jsonString(name).toUtf8();
    QByteArray updated;
    if (text.trimmed().isEmpty()) {
        updated = "{\n  \"mcpServers\": {\n    " + key + ": " + indentEntry(entryJson, "    ") + "\n  }\n}\n";
    } else {
        Scanner scan{text};
        const auto rootOpen = scan.ws(0);
        qsizetype rootClose = -1;
        const auto root = scan.members(rootOpen, &rootClose);
        if (!root || scan.ws(rootClose + 1) != text.size())
            return fail("The configuration could not be read for a safe edit.");
        const Scanner::Member *servers = nullptr;
        for (const auto &member : *root)
            if (member.key == "mcpServers")
                servers = &member;
        if (servers && text[servers->valueBegin] != '{')
            return fail("\"mcpServers\" is not an object.");
        if (!servers) {
            // Insert "mcpServers" as the first member of the root object.
            const auto indent = root->isEmpty() ? QByteArray("  ") : scan.indentBefore(root->first().keyBegin);
            const auto inner = indent + indent;
            const auto block = "\"mcpServers\": {\n" + inner + key + ": " + indentEntry(entryJson, inner) + "\n" +
                               indent + "}";
            if (root->isEmpty())
                updated = text.left(rootOpen + 1) + "\n" + indent + block + "\n" + text.mid(rootClose);
            else
                updated = text.left(root->first().keyBegin) + block + ",\n" + indent + text.mid(root->first().keyBegin);
        } else {
            qsizetype close = -1;
            const auto entries = scan.members(servers->valueBegin, &close);
            if (!entries)
                return fail("\"mcpServers\" could not be read for a safe edit.");
            const Scanner::Member *existing = nullptr;
            for (const auto &member : *entries)
                if (member.key == name)
                    existing = &member;
            if (existing) {
                const auto indent = scan.indentBefore(existing->keyBegin);
                updated = text.left(existing->valueBegin) + indentEntry(entryJson, indent) + text.mid(existing->valueEnd);
            } else if (entries->isEmpty()) {
                const auto outer = scan.indentBefore(servers->keyBegin);
                const auto indent = outer + "  ";
                updated = text.left(servers->valueBegin + 1) + "\n" + indent + key + ": " +
                          indentEntry(entryJson, indent) + "\n" + outer + text.mid(close);
            } else {
                const auto indent = scan.indentBefore(entries->first().keyBegin);
                updated = text.left(entries->first().keyBegin) + key + ": " + indentEntry(entryJson, indent) + ",\n" +
                          indent + text.mid(entries->first().keyBegin);
            }
        }
    }
    // Verify: valid JSON, the entry is exactly what was intended, and nothing else changed.
    const auto after = QJsonDocument::fromJson(updated, &parse);
    if (parse.error != QJsonParseError::NoError || !after.isObject())
        return fail("The edited configuration did not validate; nothing was written.");
    auto expected = before.object();
    auto servers = expected["mcpServers"].toObject();
    servers[name] = entryValue;
    expected["mcpServers"] = servers;
    if (after.object() != expected)
        return fail("The edited configuration changed more than the Nova Studio entry; nothing was written.");
    return bom ? "\xEF\xBB\xBF" + updated : updated;
}

Plan plan(const Client &client, const QString &bridgePath) {
    Plan result;
    const auto status = inspect(client, bridgePath);
    result.target = native(client.configPath);
    if (!QFileInfo(bridgePath).isFile()) {
        result.problem = "nova-mcp.exe was not found next to Nova Studio. Reinstall Nova Studio.";
        return result;
    }
    if (status.state == Status::Configured) {
        result.problem = "Already configured. Nothing needs to change.";
        return result;
    }
    if (status.state == Status::Invalid) {
        result.problem = status.detail;
        return result;
    }
    if (status.state == Status::DifferentPath && status.configuredName != ServerName) {
        result.problem = QString("The entry \"%1\" already starts nova-mcp.exe from another folder. Update or remove "
                                 "it in the client to avoid two Nova Studio entries.").arg(status.configuredName);
        return result;
    }
    result.replaces = status.state == Status::DifferentPath;
    if (client.method == Method::EditJson) {
        if (client.running) {
            result.problem = "Claude Desktop is running. Quit it first (including from the notification area) so it "
                             "does not overwrite the change, then check again.";
            return result;
        }
        QString error;
        result.existed = QFileInfo::exists(client.configPath);
        if (result.existed) {
            result.original = readFileBytes(client.configPath, ConfigLimit, &error);
            if (!error.isEmpty()) {
                result.problem = "The configuration could not be read: " + error;
                return result;
            }
        }
        result.updated = setJsonServer(result.original, ServerName, entryFor(bridgePath), &error);
        if (result.updated.isEmpty()) {
            result.problem = error + " Use Copy configuration instead.";
            return result;
        }
        result.possible = true;
        result.restartRequired = true;
        result.summary = QString("%1 the \"%2\" server in %3. Every other setting stays exactly as it is.")
                             .arg(result.replaces ? "Replace" : "Add", ServerName, result.target);
        result.preview = QString("\"%1\": %2").arg(ServerName, QString::fromUtf8(entryFor(bridgePath)));
        return result;
    }
    if (client.method == Method::VendorCli) {
        const auto bridge = native(bridgePath);
        if (client.kind == Kind::ClaudeCode) {
            if (result.replaces)
                result.commands << QStringList{client.cliPath, "mcp", "remove", "--scope", "user", ServerName};
            result.commands << QStringList{client.cliPath, "mcp", "add", "--scope", "user", ServerName, "--", bridge};
        } else {
            if (result.replaces)
                result.commands << QStringList{client.cliPath, "mcp", "remove", ServerName};
            result.commands << QStringList{client.cliPath, "mcp", "add", ServerName, "--", bridge};
        }
        QStringList shown;
        for (const auto &command : std::as_const(result.commands)) {
            QStringList parts{shellArgument(command.first())};
            for (const auto &argument : command.mid(1))
                parts << (argument.contains(' ') || argument.contains('\\') ? shellArgument(argument) : argument);
            shown << parts.join(' ');
        }
        result.possible = true;
        result.summary = QString("Run %1's own tool to %2 the \"%3\" server for your Windows user.")
                             .arg(client.name, result.replaces ? "replace" : "add", ServerName);
        result.preview = shown.join('\n');
        return result;
    }
    result.problem = client.manualReason.isEmpty() ? "Automatic setup is not available for this client." : client.manualReason;
    return result;
}

QString backupFile(const QString &path, const QString &clientId, const Environment &environment, QString *error) {
    if (!QFileInfo(path).isFile())
        return {};
    if (environment.backupDirectory.isEmpty() || !QDir().mkpath(environment.backupDirectory)) {
        *error = "No folder is available for the configuration backup.";
        return {};
    }
    const auto stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz");
    const auto target = QDir(environment.backupDirectory).filePath(
        QString("%1-%2-%3").arg(clientId, stamp, QFileInfo(path).fileName()));
    if (!QFile::copy(path, target)) {
        *error = "The configuration backup could not be written.";
        return {};
    }
    return target;
}

bool applyJson(const Plan &plan, const Environment &environment, QString *backupPath, QString *error) {
    if (!plan.possible || plan.updated.isEmpty()) {
        *error = plan.problem.isEmpty() ? "Nothing to apply." : plan.problem;
        return false;
    }
    const auto path = QDir::fromNativeSeparators(plan.target);
    // The file must still be exactly what the plan was built from.
    const auto current = Revision::read(path);
    if (current.exists != plan.existed ||
        (current.exists && readFileBytes(path, ConfigLimit) != plan.original)) {
        *error = "The configuration changed after it was checked. Nothing was written; check again.";
        return false;
    }
    if (current.exists) {
        *backupPath = backupFile(path, "claude-desktop", environment, error);
        if (backupPath->isEmpty())
            return false;
    } else if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = "The configuration folder could not be created.";
        return false;
    }
    if (!atomicWrite(path, plan.updated, current, error))
        return false;
    if (readFileBytes(path, ConfigLimit) != plan.updated) {
        *error = "The configuration was written but did not read back as expected.";
        return false;
    }
    return true;
}
} // namespace nova::mcp::clients
