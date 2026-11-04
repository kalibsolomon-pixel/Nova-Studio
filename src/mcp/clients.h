// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Phase 4-G: locating and configuring MCP clients (AI agents) for nova-mcp.exe.
//
// Nothing here grants access to a project. Client configuration only names the bridge;
// access still requires Allow in Nova's MCP Integration window for each session.
// Detection and status are read-only. Changes are made only after the user confirms a Plan:
//   * Claude Desktop: a text-preserving edit of its JSON file (only the "nova-studio" member is
//     inserted or replaced), validated before writing, backed up, and published atomically with
//     a revision check so a concurrent rewrite by the client is never overwritten.
//   * Claude Code and Codex: their own command-line tools ("mcp add"), which own their formats.
//   * Anything ambiguous, malformed or unsupported: copyable manual configuration only.
namespace nova::mcp::clients {
inline constexpr auto ServerName = "nova-studio";

enum class Kind { ClaudeDesktop, ClaudeCode, Codex, Generic };
enum class Method { EditJson, VendorCli, Manual };

// Where to look. Tests point every location into a temporary folder.
struct Environment {
    QString localAppData, appData, userProfile, codexHome, claudeConfigDir, path, backupDirectory;
    bool ignoreRunning = false; // tests only (NOVA_MCP_TEST_IGNORE_RUNNING): skip the running-client check
    static Environment system();
};

struct Bridge {
    QString path;
    bool exists = false;
    bool developmentBuild = false; // next to a CMake build tree rather than an installed copy
};
Bridge locateBridge(const QString &applicationDirectory);

struct Client {
    Kind kind = Kind::Generic;
    QString id, name;
    bool installed = false;
    QString evidence;          // what detection found, for the user
    QString configPath;        // the file Nova or the vendor tool changes
    QStringList candidates;    // several plausible files: ambiguous, so manual only
    QString cliPath;           // vendor tool used for one-click setup
    Method method = Method::Manual;
    QString manualReason;      // why one-click setup is unavailable
    bool running = false;      // the client application is running now
};
QList<Client> detect(const Environment &environment);

struct Status {
    enum State { NotDetected, NotConfigured, Configured, DifferentPath, Invalid, Unknown } state = Unknown;
    QString detail, configuredName, configuredCommand;
};
// Read-only. Never treats the presence of a configuration as a connected session.
Status inspect(const Client &client, const QString &bridgePath);

QString manualConfiguration(const Client &client, const QString &bridgePath);
QString instructions(const Client &client);
// Quotes a path for a command line the user pastes into PowerShell or Command Prompt.
QString shellArgument(const QString &path);
QString tomlString(const QString &text);
QString jsonString(const QString &text);

struct Plan {
    bool possible = false;
    QString problem;            // why the change cannot be made safely
    QString target, summary, preview;
    bool replaces = false;      // an existing Nova entry with another path is replaced
    // EditJson
    bool existed = false;
    QByteArray original, updated;
    // VendorCli: run in order; each is program + arguments
    QList<QStringList> commands;
    bool restartRequired = false;
};
Plan plan(const Client &client, const QString &bridgePath);

// Applies an EditJson plan: backup, revision-checked atomic write, re-read and verify.
// VendorCli plans are run by the caller (asynchronously) and verified with inspect().
bool applyJson(const Plan &plan, const Environment &environment, QString *backupPath, QString *error);
QString backupFile(const QString &path, const QString &clientId, const Environment &environment, QString *error);

// Text-preserving JSON edit: sets object["mcpServers"][name] = entry, leaving every other byte
// of the document unchanged. Returns an empty array and an error for invalid input.
QByteArray setJsonServer(const QByteArray &document, const QString &name, const QByteArray &entryJson,
                         QString *error);
}
