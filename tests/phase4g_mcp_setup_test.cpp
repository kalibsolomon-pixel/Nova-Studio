// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-G: MCP onboarding. Client configuration edits, detection, the connection offer used
// by nova-mcp.exe, and the Agent Setup page. Every client location is a temporary folder; the
// real agents' configuration on this PC is never read or written by this suite.
#include "build_identity.h"
#include "clients.h"
#include "handoff.h"
#include "service.h"
#include "window.h"
#include <QComboBox>
#include <QDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtTest>
#include <aclapi.h>
#include <qt_windows.h>
#include <sddl.h>

using namespace nova;
namespace clients = nova::mcp::clients;

namespace {
QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
// A Claude Desktop file shaped like the one found on the development PC (values invented).
// (An ordinary string literal: moc misreads raw strings that contain a backslash-quote.)
const QByteArray desktopConfig =
    "{\n"
    "  \"coworkUserFilesPath\": \"C:\\\\Users\\\\someone\\\\Documents\\\\Claude\",\n"
    "  \"preferences\": {\n"
    "    \"sidebarMode\": \"chat\",\n"
    "    \"localAgentModeTrustedFolders\": [\"C:\\\\Projects\\\\One\", \"D:\\\\\xC3\x9Cn\xC3\xAF" "code Folder\"],\n"
    "    \"bigCounter\": 12345678901234567890,\n"
    "    \"nested\": {\"a\": [1, 2, {\"b\": null}], \"escaped\": \"quote \\\" and \\\\ backslash\"}\n"
    "  }\n"
    "}\n";
QJsonObject request(int id, const QString &method, const QJsonObject &params = {}) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}
QJsonObject receive(QProcess &process, int timeout = 8000) {
    if (!QTest::qWaitFor([&] { return process.canReadLine() || process.state() == QProcess::NotRunning; }, timeout))
        return {};
    return QJsonDocument::fromJson(process.readLine()).object();
}
void send(QProcess &process, const QJsonObject &message) {
    process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}
} // namespace

class Phase4gMcpSetupTest : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QString bridge() const { return QCoreApplication::applicationDirPath() + "/nova-mcp.exe"; }
    QString fakeAgent() const { return QCoreApplication::applicationDirPath() + "/nova-fake-agent-cli.exe"; }
    clients::Environment environment(const QString &name) const {
        clients::Environment env;
        const auto base = temporary.filePath(name);
        env.localAppData = base + "/Local";
        env.appData = base + "/Roaming";
        env.userProfile = base + "/Profile";
        env.backupDirectory = base + "/Backups";
        env.ignoreRunning = true;
        QDir().mkpath(env.localAppData);
        QDir().mkpath(env.appData);
        QDir().mkpath(env.userProfile);
        return env;
    }
    static clients::Client find(const QList<clients::Client> &list, clients::Kind kind) {
        for (const auto &client : list)
            if (client.kind == kind)
                return client;
        return {};
    }
    // Starts the bridge the way an agent does: no arguments, no NOVA_MCP_CODE.
    void startAsAgent(QProcess &process) {
        auto env = QProcessEnvironment::systemEnvironment();
        env.remove("NOVA_MCP_CODE");
        process.setProcessEnvironment(env);
        process.start(bridge(), {});
        QVERIFY(process.waitForStarted());
    }

  private slots:
    void initTestCase() {
        QVERIFY(temporary.isValid());
        qputenv("NOVA_MCP_HANDOFF_DIR", temporary.filePath("handoff").toUtf8());
        QVERIFY(QFileInfo(bridge()).isFile());
        QVERIFY(QFileInfo(fakeAgent()).isFile());
    }

    void jsonEditChangesOnlyTheNovaEntry() {
        const auto entry = QByteArray(R"({"command": "C:\\Nova Studio\\nova-mcp.exe", "args": []})");
        QString error;
        const auto updated = clients::setJsonServer(desktopConfig, "nova-studio", entry, &error);
        QVERIFY2(!updated.isEmpty(), qPrintable(error));
        // Every original byte survives in order: the edit is a single insertion.
        const auto at = updated.indexOf("\"mcpServers\"");
        QVERIFY(at > 0);
        const auto inserted = updated.size() - desktopConfig.size();
        QCOMPARE(updated.left(at), desktopConfig.left(at));
        QCOMPARE(updated.mid(at + inserted), desktopConfig.mid(at));
        QVERIFY(updated.contains("12345678901234567890"));
        const auto object = QJsonDocument::fromJson(updated).object();
        QCOMPARE(object["mcpServers"]["nova-studio"]["command"].toString(), QString("C:\\Nova Studio\\nova-mcp.exe"));
        QCOMPARE(object["preferences"]["nested"]["escaped"].toString(), QString("quote \" and \\ backslash"));
        // Repeating the same setup is byte-for-byte idempotent.
        QCOMPARE(clients::setJsonServer(updated, "nova-studio", entry, &error), updated);
        // Another path replaces only the Nova entry; siblings are untouched.
        auto withSibling = clients::setJsonServer(updated, "other-server",
            R"({"command": "C:\\Tools\\other.exe", "args": ["--flag"]})", &error);
        const auto moved = clients::setJsonServer(withSibling, "nova-studio",
            R"({"command": "D:\\Moved\\nova-mcp.exe", "args": []})", &error);
        const auto servers = QJsonDocument::fromJson(moved).object()["mcpServers"].toObject();
        QCOMPARE(servers.size(), 2);
        QCOMPARE(servers["other-server"]["args"][0].toString(), QString("--flag"));
        QCOMPARE(servers["nova-studio"]["command"].toString(), QString("D:\\Moved\\nova-mcp.exe"));
        // An empty mcpServers object, an empty root, an empty file and a byte-order mark.
        for (const QByteArray &source : {QByteArray("{\"mcpServers\": {}}"), QByteArray("{}"), QByteArray(""),
                                         QByteArray("\xEF\xBB\xBF{\"a\": 1}")}) {
            const auto result = clients::setJsonServer(source, "nova-studio", entry, &error);
            QVERIFY2(!result.isEmpty(), qPrintable(error));
            QCOMPARE(result.startsWith("\xEF\xBB\xBF"), source.startsWith("\xEF\xBB\xBF"));
            const auto parsed = QJsonDocument::fromJson(result.startsWith("\xEF\xBB\xBF") ? result.mid(3) : result).object();
            QCOMPARE(parsed["mcpServers"]["nova-studio"]["command"].toString(), QString("C:\\Nova Studio\\nova-mcp.exe"));
        }
    }
    void malformedConfigurationIsNeverRewritten() {
        QString error;
        for (const QByteArray &source : {QByteArray("{\"mcpServers\": {"), QByteArray("{\"a\": 1,}"),
                                         QByteArray("[1, 2]"), QByteArray("{\"mcpServers\": []}"),
                                         QByteArray("// comment\n{}")}) {
            error.clear();
            QVERIFY(clients::setJsonServer(source, "nova-studio", R"({"command": "x", "args": []})", &error).isEmpty());
            QVERIFY(!error.isEmpty());
        }
        // Through detection, planning and applying: the file is reported and left byte-identical.
        auto env = environment("malformed");
        const auto path = env.appData + "/Claude/claude_desktop_config.json";
        const QByteArray broken = "{\"preferences\": {\"x\": 1,}\n";
        QVERIFY(write(path, broken));
        const auto client = find(clients::detect(env), clients::Kind::ClaudeDesktop);
        QCOMPARE(client.method, clients::Method::EditJson);
        QCOMPARE(clients::inspect(client, bridge()).state, clients::Status::Invalid);
        const auto plan = clients::plan(client, bridge());
        QVERIFY(!plan.possible);
        QVERIFY(plan.problem.contains("not valid JSON"));
        QString backup;
        QVERIFY(!clients::applyJson(plan, env, &backup, &error));
        QCOMPARE(readAll(path), broken);
        QVERIFY(!QDir(env.backupDirectory).exists());
    }
    void spacesUnicodeAndQuoting() {
        const QString odd = "C:/Users/Zoë Müller/Nova Studio 测试/nova-mcp.exe";
        QString error;
        const auto entry = QJsonDocument(QJsonObject{{"command", QDir::toNativeSeparators(odd)}, {"args", QJsonArray{}}}).toJson();
        const auto updated = clients::setJsonServer("{}", "nova-studio", entry, &error);
        QCOMPARE(QJsonDocument::fromJson(updated).object().value("mcpServers")["nova-studio"]["command"].toString(),
                 QDir::toNativeSeparators(odd));
        QCOMPARE(clients::shellArgument(odd), QString("\"C:\\Users\\Zoë Müller\\Nova Studio 测试\\nova-mcp.exe\""));
        QCOMPARE(clients::shellArgument("C:/A$b/nova-mcp.exe"), QString("'C:\\A$b\\nova-mcp.exe'"));
        QCOMPARE(clients::shellArgument("C:/It's/nova-mcp.exe"), QString("'C:\\It''s\\nova-mcp.exe'"));
        QCOMPARE(clients::tomlString("C:\\Zoë \"q\"\\x"), QString("\"C:\\\\Zoë \\\"q\\\"\\\\x\""));
        // The Codex reader decodes what the TOML writer produces.
        auto env = environment("toml");
        env.codexHome = env.userProfile + "/codex-home";
        const auto native = QDir::toNativeSeparators(odd);
        QVERIFY(write(env.codexHome + "/config.toml",
                      ("model = \"x\"\n# comment\n[mcp_servers.other]\ncommand = \"other.exe\"\n\n[mcp_servers.\"nova-studio\"]\ncommand = " +
                       clients::tomlString(native) + "  # trailing\nargs = []\n").toUtf8()));
        auto codex = find(clients::detect(env), clients::Kind::Codex);
        QVERIFY(codex.installed);
        const auto status = clients::inspect(codex, odd);
        QCOMPARE(status.state, clients::Status::Configured);
        QCOMPARE(status.configuredCommand, native);
        QCOMPARE(clients::inspect(codex, "C:/Elsewhere/nova-mcp.exe").state, clients::Status::DifferentPath);
        for (auto kind : {clients::Kind::ClaudeDesktop, clients::Kind::ClaudeCode, clients::Kind::Codex, clients::Kind::Generic}) {
            clients::Client client;
            client.kind = kind;
            QVERIFY(clients::manualConfiguration(client, odd).contains(native) ||
                    clients::manualConfiguration(client, odd).contains(clients::jsonString(native)));
            QVERIFY(!clients::instructions(client).isEmpty());
        }
    }
    void detectionIsConservative() {
        // Nothing installed: nothing detected, nothing automatic.
        auto none = environment("none");
        const auto empty = clients::detect(none);
        QCOMPARE(empty.size(), 4);
        QVERIFY(!find(empty, clients::Kind::ClaudeDesktop).installed);
        QCOMPARE(clients::inspect(find(empty, clients::Kind::ClaudeDesktop), bridge()).state, clients::Status::NotDetected);
        QCOMPARE(find(empty, clients::Kind::ClaudeCode).method, clients::Method::Manual);
        QCOMPARE(find(empty, clients::Kind::Codex).method, clients::Method::Manual);
        QCOMPARE(find(empty, clients::Kind::Generic).method, clients::Method::Manual);
        // MSIX Claude Desktop without a configuration file: manual, never a guessed location.
        auto msix = environment("msix");
        QVERIFY(QDir().mkpath(msix.localAppData + "/Packages/Claude_pzs8sxrjxfjjc"));
        auto desktop = find(clients::detect(msix), clients::Kind::ClaudeDesktop);
        QVERIFY(desktop.installed);
        QCOMPARE(desktop.method, clients::Method::Manual);
        QVERIFY(desktop.manualReason.contains("Edit Config"));
        // One existing file in the Claude-Data layout seen on this PC: automatic.
        QVERIFY(write(msix.localAppData + "/Claude-Data/claude_pzs8sxrjxfjjc/claude_desktop_config.json", desktopConfig));
        desktop = find(clients::detect(msix), clients::Kind::ClaudeDesktop);
        QCOMPARE(desktop.method, clients::Method::EditJson);
        QVERIFY(desktop.configPath.endsWith("Claude-Data/claude_pzs8sxrjxfjjc/claude_desktop_config.json"));
        QCOMPARE(clients::inspect(desktop, bridge()).state, clients::Status::NotConfigured);
        // Two plausible files: ambiguous, manual only.
        QVERIFY(write(msix.localAppData + "/Packages/Claude_pzs8sxrjxfjjc/LocalCache/Roaming/Claude/claude_desktop_config.json", "{}"));
        desktop = find(clients::detect(msix), clients::Kind::ClaudeDesktop);
        QCOMPARE(desktop.method, clients::Method::Manual);
        QCOMPARE(desktop.candidates.size(), 2);
        QVERIFY(!clients::plan(desktop, bridge()).possible);
        // Vendor tools: a native claude.exe or codex.exe; a .cmd wrapper is never run.
        auto tools = environment("tools");
        const auto bin = tools.userProfile + "/bin";
        QVERIFY(QDir().mkpath(bin));
        QVERIFY(write(bin + "/claude.cmd", "@echo off\n"));
        tools.path = QDir::toNativeSeparators(bin);
        QCOMPARE(find(clients::detect(tools), clients::Kind::ClaudeCode).method, clients::Method::Manual);
        QVERIFY(QFile::copy(fakeAgent(), bin + "/claude.exe"));
        QVERIFY(QDir().mkpath(tools.localAppData + "/OpenAI/Codex/bin/abc123"));
        QVERIFY(QFile::copy(fakeAgent(), tools.localAppData + "/OpenAI/Codex/bin/abc123/codex.exe"));
        const auto found = clients::detect(tools);
        QCOMPARE(find(found, clients::Kind::ClaudeCode).method, clients::Method::VendorCli);
        QVERIFY(find(found, clients::Kind::ClaudeCode).cliPath.endsWith("bin/claude.exe"));
        QCOMPARE(find(found, clients::Kind::Codex).method, clients::Method::VendorCli);
        QVERIFY(find(found, clients::Kind::Codex).configPath.endsWith("Profile/.codex/config.toml"));
        const auto plan = clients::plan(find(found, clients::Kind::ClaudeCode), bridge());
        QVERIFY(plan.possible);
        QCOMPARE(plan.commands.size(), 1);
        QCOMPARE(plan.commands.first().mid(1),
                 (QStringList{"mcp", "add", "--scope", "user", "nova-studio", "--", QDir::toNativeSeparators(bridge())}));
    }
    void applyBacksUpAndRefusesConcurrentChanges() {
        auto env = environment("apply");
        const auto path = env.localAppData + "/Claude-Data/claude_pzs8sxrjxfjjc/claude_desktop_config.json";
        QVERIFY(write(path, desktopConfig));
        auto client = find(clients::detect(env), clients::Kind::ClaudeDesktop);
        auto plan = clients::plan(client, bridge());
        QVERIFY2(plan.possible, qPrintable(plan.problem));
        // The client rewrites its file after Nova checked it: nothing is written.
        QVERIFY(write(path, desktopConfig + " "));
        QString backup, error;
        QVERIFY(!clients::applyJson(plan, env, &backup, &error));
        QVERIFY(error.contains("changed after it was checked"));
        QCOMPARE(readAll(path), desktopConfig + " ");
        // Fresh plan: backup first, then the atomic, verified write.
        plan = clients::plan(client, bridge());
        QVERIFY2(clients::applyJson(plan, env, &backup, &error), qPrintable(error));
        QCOMPARE(readAll(backup), desktopConfig + " ");
        QVERIFY(backup.startsWith(env.backupDirectory));
        QCOMPARE(clients::inspect(client, bridge()).state, clients::Status::Configured);
        // Repeated setup: nothing to do.
        const auto again = clients::plan(client, bridge());
        QVERIFY(!again.possible);
        QVERIFY(again.problem.startsWith("Already configured"));
        // An entry under another name that already starts nova-mcp.exe counts as configured.
        auto other = environment("other-name");
        const auto otherPath = other.appData + "/Claude/claude_desktop_config.json";
        QVERIFY(write(otherPath, QString(R"({"mcpServers": {"nova": {"command": %1}}})")
                                     .arg(clients::jsonString(QDir::toNativeSeparators(bridge()))).toUtf8()));
        QCOMPARE(clients::inspect(find(clients::detect(other), clients::Kind::ClaudeDesktop), bridge()).state,
                 clients::Status::Configured);
        // ...and one pointing elsewhere blocks a duplicate Nova entry.
        QVERIFY(write(otherPath, R"({"mcpServers": {"nova": {"command": "C:\\Old\\nova-mcp.exe"}}})"));
        const auto blocked = clients::plan(find(clients::detect(other), clients::Kind::ClaudeDesktop), bridge());
        QVERIFY(!blocked.possible);
        QVERIFY(blocked.problem.contains("\"nova\""));
    }
    void offerIsPrivateSingleAndWithdrawn() {
        const auto code = QString(64, 'a'), other = QString(64, 'b');
        QString error;
        QVERIFY2(mcp::handoff::publish({code, "Pack", QDateTime::currentMSecsSinceEpoch() + 60000}, &error), qPrintable(error));
        const auto path = mcp::handoff::filePath();
        QVERIFY(QFileInfo(path).isFile());
        QCOMPARE(mcp::handoff::read()->code, code);
        // Owner-only DACL, inheritance blocked, and a medium label that denies reads from low integrity.
        PSECURITY_DESCRIPTOR sd = nullptr;
        const auto native = QDir::toNativeSeparators(path);
        QCOMPARE(GetNamedSecurityInfoW(reinterpret_cast<LPCWSTR>(native.utf16()), SE_FILE_OBJECT,
                     DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &sd),
                 DWORD(ERROR_SUCCESS));
        LPWSTR text = nullptr;
        QVERIFY(ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1,
                    DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, &text, nullptr));
        const auto sddl = QString::fromWCharArray(text);
        LocalFree(text);
        LocalFree(sd);
        QVERIFY2(sddl.startsWith("D:P(A;;FA;;;S-1-5-21-") && sddl.count("(A;") == 1, qPrintable(sddl));
        const auto label = sddl.mid(sddl.indexOf("S:"));
        QVERIFY2(label.contains("(ML;;") && label.contains("NR") && label.endsWith(";;;ME)"), qPrintable(sddl));
        // Another window's code never removes this offer; the matching code does.
        mcp::handoff::withdraw(other);
        QVERIFY(QFileInfo(path).isFile());
        mcp::handoff::withdraw(code);
        QVERIFY(!QFileInfo(path).exists());
        QVERIFY(!mcp::handoff::read());
        // A planted junction or link in place of the folder is refused.
        QVERIFY(QDir(mcp::handoff::directory()).removeRecursively());
        const auto target = temporary.filePath("elsewhere");
        QVERIFY(QDir().mkpath(target));
        QProcess junction;
        junction.start("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(mcp::handoff::directory()),
                                   QDir::toNativeSeparators(target)});
        QVERIFY(junction.waitForFinished(10000));
        QCOMPARE(junction.exitCode(), 0);
        QVERIFY(!mcp::handoff::publish({code, "Pack", 0}, &error));
        QVERIFY(QDir(target).isEmpty());
        QVERIFY(QDir().rmdir(mcp::handoff::directory())); // removes the junction, not its target
        QVERIFY(QFileInfo(target).isDir());
    }
    void agentConnectsOnlyAfterAllow() {
        const auto root = temporary.filePath("Offer Pack");
        QVERIFY(write(root + "/pack.mcmeta", R"({"pack":{"pack_format":75,"description":"offer"}})"));
        const auto other = temporary.filePath("Other Pack");
        QVERIFY(write(other + "/pack.mcmeta", R"({"pack":{"pack_format":75,"description":"other"}})"));
        qApp->setProperty("settingsFile", temporary.filePath("offer.ini"));
        QElapsedTimer started;
        started.start();
        Window window(root, {}, started);
        window.show();
        window.findChild<QAction *>("mcpAccess")->trigger();
        auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
        QVERIFY(dialog);
        QCOMPARE(dialog->windowTitle(), QString("MCP Integration"));
        auto status = dialog->findChild<QLabel *>("mcpAccessStatus");
        auto overview = dialog->findChild<QLabel *>("mcpOverview");
        QVERIFY(status && overview);
        QVERIFY(overview->text().contains("Access off"));
        // Before Allow: an agent's bridge finds no offer and exits without connecting.
        QProcess early;
        startAsAgent(early);
        QVERIFY(early.waitForFinished(10000));
        QCOMPARE(early.exitCode(), 2);
        QVERIFY(early.readAllStandardError().contains("has not offered a connection"));
        auto service = window.findChild<mcp::Service *>();
        QVERIFY(service);
        QCOMPARE(service->permission(), "Disabled");
        // Allow read-only: the waiting grant is offered, the overview says so.
        dialog->findChild<QPushButton *>("mcpAuthorize")->click();
        QCOMPARE(status->text(), "Waiting for client");
        QVERIFY(service->offeredToAgents());
        QCOMPARE(mcp::handoff::read()->code, service->connectionCode());
        QVERIFY(overview->text().contains("waiting for an agent"));
        QProcess agent;
        startAsAgent(agent);
        send(agent, request(1, "initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}},
                                              {"clientInfo", QJsonObject{{"name", "phase4g-agent"}, {"version", "1"}}}}));
        const auto initialized = receive(agent);
        QCOMPARE(initialized["result"]["serverInfo"]["version"].toString(), QString(AppVersion));
        agent.write(R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n");
        QTRY_VERIFY(status->text().startsWith("Connected"));
        QVERIFY(overview->text().contains("Client connected · phase4g-agent · Read-only"));
        // The offer is single-use: it is withdrawn once the agent authenticates.
        QVERIFY(!QFileInfo::exists(mcp::handoff::filePath()));
        send(agent, request(2, "tools/list"));
        QCOMPARE(receive(agent).value("result")["tools"].toArray().size(), 26);
        QCOMPARE(service->permission(), "Read-only");
        agent.closeWriteChannel();
        QVERIFY(agent.waitForFinished(10000));
        QTRY_COMPARE(status->text().section('\n', 0, 0), "Disabled");
        QProcess late;
        startAsAgent(late);
        QVERIFY(late.waitForFinished(10000));
        QCOMPARE(late.exitCode(), 2);
        // Revoke withdraws a waiting offer.
        dialog->findChild<QPushButton *>("mcpAuthorize")->click();
        QVERIFY(QFileInfo::exists(mcp::handoff::filePath()));
        dialog->findChild<QPushButton *>("mcpRevoke")->click();
        QVERIFY(!QFileInfo::exists(mcp::handoff::filePath()));
        // Switching projects withdraws it too.
        dialog->findChild<QPushButton *>("mcpAuthorize")->click();
        QVERIFY(QFileInfo::exists(mcp::handoff::filePath()));
        auto path = window.findChild<QLineEdit *>("projectPath");
        path->setText(other);
        QTest::keyClick(path, Qt::Key_Return);
        QTRY_VERIFY(!QFileInfo::exists(mcp::handoff::filePath()));
        QCOMPARE(service->permission(), "Disabled");
        dialog->close();
        QVERIFY(window.close());
    }
    void agentSetupPageConfiguresWithoutGrantingAccess() {
        // Point every client location of this process at a temporary profile.
        const auto base = temporary.filePath("ui-profile");
        const auto bin = base + "/bin";
        QVERIFY(QDir().mkpath(bin));
        QVERIFY(QFile::copy(fakeAgent(), bin + "/claude.exe"));
        QVERIFY(QFile::copy(fakeAgent(), bin + "/codex.exe"));
        const auto desktop = base + "/Roaming/Claude/claude_desktop_config.json";
        QVERIFY(write(desktop, desktopConfig));
        QVERIFY(QDir().mkpath(base + "/Local/AnthropicClaude"));
        QVERIFY(write(base + "/codex/config.toml", "# keep this comment\nmodel = \"x\"\n"));
        const auto previousPath = qgetenv("PATH");
        QMap<QByteArray, std::optional<QByteArray>> previous;
        for (auto name : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "CLAUDE_CONFIG_DIR", "CODEX_HOME", "NOVA_MCP_TEST_IGNORE_RUNNING"})
            previous[name] = qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name)) : std::nullopt;
        for (const auto &[name, value] : std::initializer_list<std::pair<const char *, QString>>{
                 {"LOCALAPPDATA", base + "/Local"}, {"APPDATA", base + "/Roaming"}, {"USERPROFILE", base + "/Profile"},
                 {"CLAUDE_CONFIG_DIR", base + "/claude"}, {"CODEX_HOME", base + "/codex"},
                 {"NOVA_MCP_TEST_IGNORE_RUNNING", "1"}})
            qputenv(name, QDir::toNativeSeparators(value).toUtf8());
        qputenv("PATH", QDir::toNativeSeparators(bin).toUtf8() + ';' + previousPath);
        QVERIFY(QDir().mkpath(base + "/claude"));
        const auto root = temporary.filePath("Setup Pack");
        QVERIFY(write(root + "/pack.mcmeta", R"({"pack":{"pack_format":75,"description":"setup"}})"));
        qApp->setProperty("settingsFile", temporary.filePath("setup.ini"));
        QElapsedTimer started;
        started.start();
        {
            Window window(root, {}, started);
            window.show();
            window.findChild<QAction *>("mcpAccess")->trigger();
            auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
            auto tabs = dialog->findChild<QTabWidget *>("mcpTabs");
            QVERIFY(tabs);
            tabs->setCurrentIndex(1);
            auto agent = dialog->findChild<QComboBox *>("mcpAgentChoice");
            auto status = dialog->findChild<QLabel *>("mcpAgentStatus");
            auto add = dialog->findChild<QPushButton *>("mcpAgentConfigure");
            auto configuration = dialog->findChild<QPlainTextEdit *>("mcpAgentConfiguration");
            QVERIFY(agent && status && add && configuration);
            QTRY_COMPARE(agent->count(), 4);
            auto select = [&](const QString &id) {
                agent->setCurrentIndex(agent->findData(id));
                QTRY_VERIFY(!status->text().startsWith("Checking"));
            };
            auto confirmNext = [&] {
                QTimer::singleShot(0, dialog, [] {
                    QTRY_VERIFY(qobject_cast<QMessageBox *>(QApplication::activeModalWidget()));
                    auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                    QCOMPARE(box->objectName(), QString("mcpConfigureConfirm"));
                    for (auto button : box->buttons())
                        if (box->buttonRole(button) == QMessageBox::AcceptRole)
                            button->click();
                });
            };
            const auto bridgeNative = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/nova-mcp.exe");
            // Claude Desktop: one-click JSON edit; the rest of the file is untouched.
            select("claude-desktop");
            QVERIFY(status->text().contains("Agent configuration required"));
            QVERIFY(configuration->toPlainText().contains("\"nova-studio\""));
            QVERIFY(add->isEnabled());
            confirmNext();
            add->click();
            QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith("Agent configured"), 10000);
            const auto edited = readAll(desktop);
            QCOMPARE(QJsonDocument::fromJson(edited).object().value("mcpServers")["nova-studio"]["command"].toString(), bridgeNative);
            const auto firstKey = desktopConfig.indexOf("\"coworkUserFilesPath\"");
            QCOMPARE(edited.left(edited.indexOf("\"mcpServers\"")), desktopConfig.left(firstKey));
            QVERIFY(edited.endsWith(desktopConfig.mid(firstKey)));
            QVERIFY(!add->isEnabled());
            // Claude Code: its own tool, user scope, exact arguments.
            select("claude-code");
            QVERIFY(add->isEnabled());
            confirmNext();
            add->click();
            QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith("Agent configured"), 15000);
            const auto calls = readAll(base + "/claude/fake-agent-calls.jsonl").trimmed().split('\n');
            QCOMPARE(calls.size(), 1);
            QCOMPARE(QJsonDocument::fromJson(calls.first()).object()["args"].toArray().toVariantList(),
                     (QVariantList{"mcp", "add", "--scope", "user", "nova-studio", "--", bridgeNative}));
            // Codex: its own tool; an earlier comment survives (the tool owns the format).
            select("codex");
            confirmNext();
            add->click();
            QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith("Agent configured"), 15000);
            QVERIFY(readAll(base + "/codex/config.toml").startsWith("# keep this comment\n"));
            // A failing tool is reported; the configuration is unchanged.
            QVERIFY(write(base + "/codex/config.toml", "model = \"y\"\n"));
            qputenv("NOVA_FAKE_AGENT_FAIL", "1");
            dialog->findChild<QPushButton *>("mcpAgentRecheck")->click();
            QTRY_VERIFY(status->text().contains("Agent configuration required"));
            confirmNext();
            add->click();
            auto results = dialog->findChild<QPlainTextEdit *>("mcpDiagnosticsResults");
            QTRY_VERIFY_WITH_TIMEOUT(results->toPlainText().contains("Stopped: the command failed (exit code 3)"), 15000);
            QCOMPARE(readAll(base + "/codex/config.toml"), QByteArray("model = \"y\"\n"));
            qunsetenv("NOVA_FAKE_AGENT_FAIL");
            // Diagnostics: local checks pass and say plainly that no agent is connected.
            QTRY_VERIFY(dialog->findChild<QPushButton *>("mcpRunDiagnostics")->isEnabled());
            dialog->findChild<QPushButton *>("mcpRunDiagnostics")->click();
            QTRY_VERIFY_WITH_TIMEOUT(results->toPlainText().contains("Connected agent: none"), 20000);
            const auto report = results->toPlainText();
            QVERIFY2(report.contains("Pass · Bridge starts: nova-mcp " + QString(AppVersion)), qPrintable(report));
            QVERIFY2(report.contains("Pass · Protocol self-test: MCP 2025-11-25, 26 tools declared"), qPrintable(report));
            // None of this granted access or offered a connection.
            QCOMPARE(window.findChild<mcp::Service *>()->permission(), "Disabled");
            QVERIFY(!QFileInfo::exists(mcp::handoff::filePath()));
            const auto evidence = qEnvironmentVariable("NOVA_PHASE4G_EVIDENCE");
            if (!evidence.isEmpty()) {
                QDir().mkpath(evidence);
                select("claude-desktop");
                QVERIFY(dialog->grab().save(evidence + "/mcp-agent-setup.png"));
                QTRY_VERIFY(dialog->findChild<QPushButton *>("mcpRunDiagnostics")->isEnabled());
                dialog->findChild<QPushButton *>("mcpRunDiagnostics")->click();
                QTRY_VERIFY_WITH_TIMEOUT(results->toPlainText().contains("Connected agent"), 20000);
                QVERIFY(dialog->grab().save(evidence + "/mcp-diagnostics.png"));
                tabs->setCurrentIndex(0);
                QVERIFY(dialog->grab().save(evidence + "/mcp-access.png"));
            }
            dialog->close();
            QVERIFY(window.close());
        }
        qputenv("PATH", previousPath);
        for (auto it = previous.begin(); it != previous.end(); ++it)
            if (it.value())
                qputenv(it.key().constData(), *it.value());
            else
                qunsetenv(it.key().constData());
    }
};

QTEST_MAIN(Phase4gMcpSetupTest)
#include "phase4g_mcp_setup_test.moc"
