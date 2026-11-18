// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Agent Client Protocol (ACP, protocol version 1) client: Nova starts one agent program, talks
// newline-delimited JSON-RPC over its stdin/stdout, and attaches Nova's MCP bridge through the
// standard session/new "mcpServers" list. Everything the agent sends is untrusted display data;
// authorization stays with Nova's MCP grant. Nova advertises no file-system or terminal capability,
// so the agent cannot read or write files through Nova. Prompts, replies and the agent's stderr are
// kept in memory only and never written to logs.
#include "process_tree.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QTimer>
#include <functional>

namespace nova::acp {

// An absolute native executable and separate arguments. An npm shim resolves to node.exe and its script.
struct AgentProgram {
    QString executable;
    QStringList arguments;
};
QString findAgentExecutable(const QString &name); // a PATH .exe or npm .cmd, or empty
AgentProgram resolveAgentProgram(const QString &path, const QStringList &arguments, QString *problem = nullptr);
bool acceptableExecutable(const QString &path, QString *problem);
// The user's environment without Nova's own variables (connection codes, test hooks).
QProcessEnvironment agentEnvironment(QProcessEnvironment base = QProcessEnvironment::systemEnvironment());
// One stdio MCP server entry in ACP v1 shape: no "type", absolute command, no secrets in env.
QJsonObject stdioServer(const QString &name, const QString &command);

class Session : public QObject {
    Q_OBJECT
  public:
    enum class State { Idle, Starting, Ready, Prompting, Stopping, Ended };
    explicit Session(QObject *parent = nullptr);
    ~Session() override;
    bool start(const AgentProgram &program, const QString &workingDirectory, const QJsonArray &mcpServers,
               const QProcessEnvironment &environment, QString *problem = nullptr);
    bool prompt(const QString &text);
    bool authenticate(const QString &methodId);
    void cancel();                 // session/cancel: ends the current turn
    void stop(const QString &why); // cancel, close, then end the agent's process tree
    // Answers a session/request_permission; an empty option rejects (sent as "cancelled").
    void answerPermission(const QJsonValue &requestId, const QString &optionId);
    State state() const { return current; }
    QString agentName() const { return agent; }
    QString sessionId() const { return session; }
    QString endReason() const { return reason; }
    // True for the agent process and the processes it started (matched by id and creation time).
    bool ownsProcess(qint64 pid) const;
    QList<testing::ProcessIdentity> processTree() const;
    static constexpr int LineLimit = 4 * 1024 * 1024;
  signals:
    void stateChanged();
    void agentText(const QString &text);              // agent_message_chunk text
    void agentThought(const QString &text);           // agent_thought_chunk text (its reasoning)
    void toolCall(const QJsonObject &update);         // tool_call / tool_call_update (untrusted)
    void planChanged(const QJsonArray &entries);
    void permissionRequested(const QJsonValue &id, const QJsonObject &params);
    void authenticationRequired(const QJsonArray &methods); // empty: sign in with the agent's own CLI
    void turnEnded(const QString &stopReason);
    void problem(const QString &message);

  private:
    QProcess process;
    testing::ProcessIdentity identity;
    QList<testing::ProcessIdentity> started; // the tree seen while running, for shutdown
    State current = State::Idle;
    QString agent, session, reason, directory;
    QJsonArray servers, authMethods;
    bool waitingForAuth = false;
    bool canClose = false;
    bool cancellingTurn = false;
    qint64 nextId = 0;
    QMap<qint64, std::function<void(const QJsonObject &)>> pending;
    QList<QJsonValue> permissions;
    QByteArray buffer;
    QTimer shutdownTimer, treeTimer;
    void setState(State next);
    qint64 request(const QString &method, const QJsonObject &params, std::function<void(const QJsonObject &)> done);
    void notify(const QString &method, const QJsonObject &params);
    void write(const QJsonObject &message);
    void readOutput();
    void handle(const QJsonObject &message);
    void update(const QJsonObject &params);
    void createSession();
    void cancelPermissions();
    void finish(const QString &why);
    void endTree();
    void remember();
};

} // namespace nova::acp
