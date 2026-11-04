// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "inspection.h"
#include "protocol.h"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <functional>

namespace nova::mcp {
struct ReviewStage {
    QString path, assetId, state = "Pending", revision;
    QStringList operations;
    bool changed = false;
};
struct WorkflowReview {
    QString id, project, client, tool, state, error;
    QList<ReviewStage> stages;
};
class Service : public QObject {
    Q_OBJECT
    QLocalServer server;
    QLocalSocket *socket = nullptr;
    QTimer handshake, grantExpiry, deadline;
    Protocol protocol;
    QString grant, root, generation, projectId, state = "Disabled", lastError;
    QString offered, offerFailure; // the code currently offered to agents through the handoff file
    void withdrawOffer();
    QByteArray rootIdentity, idKey;
    bool editing = false;
    QString lastEdit, lastValidation;
    QStringList validationFindings;
    WorkflowReview review;
    void beginReview(const Request &request);
    void updateReviewStage(qsizetype index, const QString &state, const QJsonObject &result = {});
    void stopReview(const QString &code, bool rejected = false);
    std::shared_ptr<RelationshipCache> relationships;
    bool pendingEdit = false, reviewingEdit = false;
    quint64 editSequence = 0;
    quint64 publishingEdit = 0;
  public:
    using Commit = std::function<QJsonObject()>;
  private:
    using EditHandler = std::function<QJsonObject(const Snapshot &, const Request &, bool, Commit &)>;
    EditHandler editHandler;
    bool authenticated = false, stale = false, shuttingDown = false;
    QFutureWatcher<QJsonObject> *worker = nullptr;
    std::shared_ptr<std::atomic_bool> cancel;
    QJsonValue pendingId;
    bool suppress = false;
    std::function<bool(Snapshot &, const QString &)> snapshot;
    void accept();
    void receive();
    void send(const QJsonObject &message);
    void closeSocket();
    void fail(const QString &message);
    void dispatch(const Request &request);
    struct Workflow {
        QJsonArray results;
        int total = 0;
        QJsonObject stopReason;
    };
    std::shared_ptr<Workflow> workflow;
    QJsonObject workflowResult(const QString &status, const QJsonObject &failure = {}) const;
    void publishWorkflow(quint64 sequence, const std::shared_ptr<Workflow> &state,
                         const std::shared_ptr<QList<Commit>> &commits);
  public:
    explicit Service(std::function<bool(Snapshot &, const QString &)> snapshot, QObject *parent);
    ~Service() override;
    bool authorize(const QString &project, bool allowEditing = false);
    void setEditHandler(EditHandler handler) { editHandler = std::move(handler); }
    QString permission() const {
        return root.isEmpty() || stale ? "Disabled" : editing ? "Editing allowed" : "Read-only";
    }
    void revoke();
    void projectChanged();
    void shutdown();
    QString connectionCode() const { return grant; }
    // True while the waiting grant is offered to the next agent that starts nova-mcp.exe.
    bool offeredToAgents() const { return !offered.isEmpty(); }
    QString offerProblem() const { return offerFailure; }
    QString authorizedProject() const { return root; }
    QString status() const { return state; }
    QString clientName() const { return authenticated ? protocol.clientName() : QString{}; }
    const WorkflowReview &workflowReview() const { return review; }
    QString editActivity() const { return lastEdit; }
    QStringList reviewValidationFindings() const { return validationFindings; }
    QString validationActivity() const { return lastValidation; }
    QString error() const { return lastError; }
  signals:
    void changed();
    void editPrepared();
    void preparationCheckpoint();
    void requestParsed(const QElapsedTimer &timer);
    void responseQueued(const QElapsedTimer &timer);
    void editSnapshotTaken(const QElapsedTimer &timer);
};
}
