// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.h"
#include "handoff.h"
#include "scope.h"
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QScopedValueRollback>
#include <QtConcurrent>
#include <utility>

namespace nova::mcp {
Service::Service(std::function<bool(Snapshot &, const QString &)> provider, QObject *parent)
    : QObject(parent), snapshot(std::move(provider)) {
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.setListenBacklogSize(1);
    server.setMaxPendingConnections(1);
    connect(&server, &QLocalServer::newConnection, this, &Service::accept);
    for (auto timer : {&handshake, &grantExpiry, &deadline})
        timer->setSingleShot(true);
    connect(&grantExpiry, &QTimer::timeout, this, [this] {
        fail("The unused connection code expired.");
        revoke();
    });
    connect(&handshake, &QTimer::timeout, this, [this] {
        fail("A connection did not authenticate in time.");
        closeSocket();
        emit changed();
    });
    connect(&deadline, &QTimer::timeout, this, [this] {
        cancel->store(true);
        if (!suppress)
            send(toolReply(pendingId, toolError("operation_canceled", "The operation exceeded its deadline.")));
        suppress = true;
    });
}
Service::~Service() { shutdown(); }

bool Service::authorize(const QString &project, bool allowEditing) {
    if (shuttingDown || project.isEmpty())
        return false;
    if (review.project != project) {
        review = {};
        lastValidation.clear();
        validationFindings.clear();
    }
    revoke();
    lastError.clear();
    const Scope scope(project);
    rootIdentity = scope.identity();
    if (rootIdentity.isEmpty()) {
        fail("This project directory cannot be authorized safely.");
        return false;
    }
    relationships = std::make_shared<RelationshipCache>();
    root = project;
    generation = randomToken();
    projectId = randomToken();
    idKey = randomToken().toLatin1();
    grant = randomToken();
    if (!server.listen("nova-mcp-" + grant.left(32))) {
        grant.clear();
        root.clear();
        fail("The local MCP endpoint could not be opened.");
        return false;
    }
    editing = allowEditing;
    state = "Waiting for client";
    grantExpiry.start(5 * 60 * 1000);
    // Offer the waiting grant to the next agent that starts nova-mcp.exe (see handoff.h).
    offerFailure.clear();
    if (handoff::publish({grant, QFileInfo(project).fileName(),
                          QDateTime::currentMSecsSinceEpoch() + 5 * 60 * 1000}, &offerFailure))
        offered = grant;
    emit changed();
    return true;
}
void Service::withdrawOffer() {
    if (!offered.isEmpty())
        handoff::withdraw(std::exchange(offered, QString{}));
}
void Service::revoke() {
    editing = false;
    grantExpiry.stop();
    withdrawOffer();
    server.close();
    grant.clear();
    closeSocket();
    root.clear();
    relationships.reset();
    lastEdit.clear();
    stale = false;
    state = "Disabled";
    emit changed();
}
void Service::shutdown() {
    if (shuttingDown)
        return;
    shuttingDown = true;
    revoke();
}
void Service::projectChanged() {
    review = {};
    lastValidation.clear();
    validationFindings.clear();
    if (root.isEmpty()) {
        emit changed();
        return;
    }
    if (!authenticated) {
        revoke();
        fail("The active project changed. Authorize the new project to connect.");
        return;
    }
    stale = true;
    relationships.reset();
    lastEdit.clear();
    lastValidation.clear();
    editing = false;
    if (pendingEdit) {
        const auto error = toolError("stale_project", "The authorized project has changed.");
        if (workflow && publishingEdit == editSequence) {
            workflow->stopReason = error;
        } else {
            pendingEdit = false;
            ++editSequence;
            send(toolReply(pendingId, workflow ? workflowResult("stopped", error) : error));
            workflow.reset();
        }
    }
    state = "Project changed";
    if (cancel)
        cancel->store(true);
    if (worker && !suppress) {
        send(toolReply(pendingId, toolError("stale_project", "The authorized project has changed.")));
        suppress = true;
    }
    emit changed();
}
void Service::fail(const QString &message) {
    lastError = message;
    emit changed();
}
void Service::closeSocket() {
    handshake.stop();
    if (cancel)
        cancel->store(true);
    suppress = true;
    authenticated = false;
    if (pendingEdit && reviewingEdit)
        stopReview("connection_closed");
    pendingEdit = false;
    workflow.reset();
    ++editSequence;
    if (socket) {
        auto connection = std::exchange(socket, nullptr);
        connection->disconnect(this);
        connection->abort();
        connection->deleteLater();
    }
}
void Service::accept() {
    while (auto incoming = server.nextPendingConnection()) {
        if (socket || grant.isEmpty()) {
            incoming->abort();
            incoming->deleteLater();
            continue;
        }
        socket = incoming;
        socket->setReadBufferSize(RequestLimit + 1);
        protocol = Protocol{};
        handshake.start(5000);
        connect(socket, &QLocalSocket::readyRead, this, &Service::receive);
        connect(socket, &QLocalSocket::disconnected, this, [this] {
            if (authenticated)
                revoke();
            else
                closeSocket();
        });
        receive();
    }
}
void Service::send(const QJsonObject &message) {
    if (!socket)
        return;
    QElapsedTimer queue;
    queue.start();
    auto line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (line.size() > ResponseLimit)
        line = QJsonDocument(toolReply(message["id"],
            toolError("response_limit", "The result exceeded the response budget.")))
            .toJson(QJsonDocument::Compact) + '\n';
    if (socket->bytesToWrite() + line.size() > ResponseLimit * 2 || socket->write(line) < 0)
        revoke();
    emit responseQueued(queue);
}
void Service::receive() {
    // Yield after a small batch so malformed traffic cannot monopolize the GUI.
    for (int i = 0; i < 16 && socket; ++i) {
        if (!socket->canReadLine() && socket->bytesAvailable() <= RequestLimit)
            return;
        const auto line = socket->readLine(RequestLimit + 2);
        if (!authenticated) {
            if (line != grant.mid(32).toLatin1() + '\n') {
                fail("A connection supplied an invalid code.");
                closeSocket();
                return;
            }
            authenticated = true;
            handshake.stop();
            grantExpiry.stop();
            withdrawOffer();
            grant.clear();
            server.close();
            lastError.clear();
            state = "Connected";
            emit changed();
            continue;
        }
        if (line.size() > RequestLimit || !line.endsWith('\n')) {
            send(rpcError(QJsonValue(), -32600, "Request exceeds the framing limit"));
            if (socket)
                socket->disconnectFromServer();
            return;
        }
        const auto previousClient = protocol.clientName();
        QElapsedTimer parsing;
        parsing.start();
        const auto request = protocol.receive(line);
        if (request.tool == "nova.animation.edit" || request.tool == "nova.edit.batch" ||
            request.tool == "nova.edit.undo" || request.tool == "nova.edit.redo" ||
            request.tool == "nova.asset.create" || request.tool == "nova.asset.preview_image" ||
            request.tool == "nova.workflow.execute" || request.tool == "nova.workflow.plan" ||
            request.tool == "nova.model.edit" || request.tool == "nova.uv.edit")
            emit requestParsed(parsing);
        if (protocol.clientName() != previousClient)
            emit changed();
        if (!request.cancelId.isNull()) {
            if (pendingEdit && request.cancelId == pendingId) {
                // Publication is synchronous and must report its actual outcome.
                if (publishingEdit != editSequence) {
                    pendingEdit = false;
                    ++editSequence;
                    const auto error = toolError("operation_canceled", "Publication stopped at the next document boundary.");
                    if (reviewingEdit)
                        stopReview("operation_canceled");
                    send(toolReply(pendingId, workflow ? workflowResult("stopped", error) : error));
                    workflow.reset();
                    emit changed();
                } else if (workflow) {
                    workflow->stopReason = toolError("operation_canceled", "Publication stopped at a document boundary.");
                }
            } else if (worker && request.cancelId == pendingId) {
                cancel->store(true);
                suppress = true;
            }
        } else if (!request.response.isEmpty()) {
            send(request.response);
        } else if (!request.tool.isEmpty()) {
            dispatch(request);
        }
    }
    if (socket && (socket->canReadLine() || socket->bytesAvailable() > RequestLimit))
        QTimer::singleShot(0, this, &Service::receive);
}
void Service::beginReview(const Request &request) {
    review = {randomToken(), root, clientName(), request.tool, "Preparing", {}, {}};
    auto stages = request.arguments["stages"].toArray();
    if (stages.isEmpty())
        stages.append(request.arguments);
    for (const auto &value : stages) {
        const auto args = value.toObject();
        ReviewStage stage;
        stage.assetId = args["assetId"].toString();
        auto operations = args["operations"].toArray();
        if (args.contains("operation"))
            operations.append(args["operation"]);
        if (args.contains("parameters"))
            operations.append(args["parameters"]);
        for (const auto &operation : operations)
            stage.operations << operation.toObject()["command"].toString().left(48);
        if (stage.operations.isEmpty())
            stage.operations << request.tool.section('.', -1);
        review.stages.append(stage);
    }
}
void Service::updateReviewStage(qsizetype index, const QString &stageState, const QJsonObject &result) {
    if (index < 0 || index >= review.stages.size())
        return;
    auto &stage = review.stages[index];
    stage.state = stageState;
    const auto path = result["changeSummary"].toObject()["assetPath"].toString();
    if (!path.isEmpty())
        stage.path = path;
    stage.revision = result["revision"].toString(result["document"].toObject()["revision"].toString());
    stage.changed = result["changed"].toBool();
}
void Service::stopReview(const QString &code, bool rejected) {
    if (review.state != "Preparing" && review.state != "Prepared" && review.state != "Executing")
        return;
    review.error = code;
    // Stages after a rejected stage were never attempted; they were not interrupted.
    const auto stopped = code == "operation_canceled" ? QString("Canceled")
                         : rejected                   ? QString("Not run")
                                                      : QString("Interrupted");
    bool committed = false;
    for (auto &stage : review.stages) {
        if (stage.state == "Committed") {
            committed = true;
        } else if (rejected && (stage.state == "Pending" || stage.state == "Executing")) {
            stage.state = "Rejected";
            rejected = false;
        } else {
            stage.state = stopped;
        }
    }
    const bool hasRejection = std::any_of(review.stages.begin(), review.stages.end(),
                                         [](const auto &stage) { return stage.state == "Rejected"; });
    review.state = committed ? "Partially committed" : hasRejection ? QString("Rejected") : stopped;
}
QJsonObject Service::workflowResult(const QString &status, const QJsonObject &failure) const {
    QJsonObject result{{"status", status}, {"stages", workflow->results}, {"totalStages", workflow->total}};
    const auto code = failure["error"].toObject()["code"].toString();
    if (!failure.isEmpty())
        result["failure"] = failure["error"];
    QJsonArray outcomes;
    for (int i = 0; i < workflow->total; ++i) {
        const auto state = i < workflow->results.size() ? QString("committed") :
            code == "operation_canceled" ? QString("canceled") :
            i == workflow->results.size() && code != "stale_project" ? QString("rejected") :
            QString("interrupted");
        outcomes.append(QJsonObject{{"stage", i}, {"prepared", true}, {"status", state}});
    }
    result["stageOutcomes"] = outcomes;
    return result;
}

void Service::publishWorkflow(quint64 sequence, const std::shared_ptr<Workflow> &progress,
                              const std::shared_ptr<QList<Commit>> &commits) {
    if (socket && socket->canReadLine())
        receive();
    if (!pendingEdit || sequence != editSequence)
        return;
    const auto index = progress->results.size();
    const auto reviewId = review.id;
    if (!reviewId.isEmpty()) {
        review.state = "Executing";
        updateReviewStage(index, "Executing");
        emit changed();
    }
    QJsonObject result;
    try {
        QScopedValueRollback publication(publishingEdit, sequence);
        result = commits->at(index)();
    } catch (...) {
        result = toolError("internal_failure", "The native operation failed.");
    }
    const bool failed = result.contains("error");
    if (!pendingEdit || sequence != editSequence) {
        // A native signal can deliver disconnect after the commit has started.
        if (!failed && !reviewId.isEmpty() && review.id == reviewId) {
            updateReviewStage(index, "Committed", result);
            review.state = index + 1 == progress->total ? "Completed" : "Partially committed";
            if (review.state == "Completed")
                review.error.clear();
            emit changed();
        }
        return;
    }
    if (!failed) {
        progress->results.append(result);
        updateReviewStage(index, "Committed", result);
    }
    lastEdit = QString("Workflow · %1/%2 stages completed").arg(progress->results.size()).arg(progress->total);
    const auto path = result["changeSummary"].toObject()["assetPath"].toString();
    if (!path.isEmpty())
        lastEdit += " · " + path.left(100);
    if (!failed && result["changed"].toBool()) {
        validationFindings.clear();
        lastValidation = "Edited documents have not been validated.";
    }
    const bool complete = progress->results.size() == progress->total;
    if (failed || !progress->stopReason.isEmpty() || complete) {
        if (!complete)
            lastEdit += " · Stopped";
        pendingEdit = false;
        const auto failure = failed ? result : complete ? QJsonObject{} : progress->stopReason;
        if (!review.id.isEmpty()) {
            if (complete)
                review.state = "Completed";
            else
                stopReview(failure["error"].toObject()["code"].toString(), failed);
        }
        const auto response = workflowResult(complete ? "complete" : "stopped", failure);
        workflow.reset();
        send(toolReply(pendingId, response));
    }
    emit changed();
    if (pendingEdit && sequence == editSequence)
        QTimer::singleShot(0, this, [this, sequence, progress, commits] { publishWorkflow(sequence, progress, commits); });
}

void Service::dispatch(const Request &request) {
    auto reject = [&](const QString &code, const QString &message) {
        send(toolReply(request.id, toolError(code, message)));
    };
    if (shuttingDown)
        return reject("application_shutting_down", "Nova Studio is closing.");
    if (stale || (request.tool != "nova.project.info" && request.arguments["generation"] != generation))
        return reject("stale_project", "The authorized project has changed or the generation is invalid.");
    if (pendingEdit || worker) {
        if ((pendingEdit || !suppress) && request.id == pendingId) {
            // A duplicate outstanding ID cannot be answered unambiguously.
            revoke();
            return;
        }
        return reject("busy", "Another MCP operation is still running.");
    }
    const bool capability = request.tool == "nova.edit.capabilities";
    const bool plan = request.tool == "nova.workflow.plan";
    const bool execute = request.tool == "nova.workflow.execute";
    const bool preview = request.tool == "nova.edit.preview" || plan || request.tool == "nova.asset.preview_image" ||
        request.tool == "nova.asset.create_preview";
    const bool prepare = request.tool == "nova.workflow.prepare";
    const bool validate = request.tool == "nova.workflow.validate_changes";
    const bool mutation = execute || request.tool == "nova.asset.create" || request.tool.endsWith(".edit") ||
        (request.tool.startsWith("nova.edit.") && !capability && !preview);
    if (mutation && !editing)
        return reject("permission_denied", "This connection has read-only access.");
    if (request.arguments.contains("projectId") && request.arguments["projectId"] != projectId)
        return reject("stale_project", "The project identifier does not match this connection.");
    Snapshot data;
    data.root = root;
    data.rootIdentity = rootIdentity;
    data.idKey = idKey;
    data.projectId = projectId;
    data.generation = generation;
    data.relationships = relationships;
    if (!mutation && !preview && !snapshot(data, request.tool))
        return reject("validation_unavailable", "The live project index is not ready.");
    if (mutation || capability || preview) {
        if (!editHandler)
            return reject("editing_unavailable", "No editor is available for this project.");
        if (capability) {
            Commit unused;
            send(toolReply(request.id, editHandler(data, request, editing, unused)));
            return;
        }
        pendingEdit = true;
        pendingId = request.id;
        const auto sequence = ++editSequence;
        reviewingEdit = mutation || plan;
        if (reviewingEdit) {
            beginReview(request);
            emit changed();
        }
        QTimer::singleShot(0, this, [this, request, sequence, preview, plan, execute, mutation] {
            if (!pendingEdit || sequence != editSequence)
                return;
            auto finish = [this, request, sequence, mutation, plan](const QJsonObject &result) {
                if (!pendingEdit || sequence != editSequence)
                    return;
                pendingEdit = false;
                const auto replyGeneration = generation;
                if ((mutation || plan) && !review.id.isEmpty()) {
                    const auto error = result["error"].toObject()["code"].toString();
                    if (!error.isEmpty())
                        stopReview(error, error != "operation_canceled" && error != "stale_project");
                    else if (plan)
                        review.state = "Ready";
                    else {
                        updateReviewStage(0, "Committed", result);
                        review.state = "Completed";
                    }
                }
                if (mutation) {
                    const auto error = result.value("error").toObject()["code"].toString();
                    const auto summary = result["changeSummary"].toObject();
                    lastEdit = request.tool + (error.isEmpty()
                        ? QString(" · %1 · %2 · %3").arg(summary["assetPath"].toString().left(100),
                            result["changed"].toBool() ? "Applied" : "No change",
                            result["undoAvailable"].toBool() ? "Undo available at edit" : "No undo at edit")
                        : " · " + error);
                    if (error.isEmpty() && result["changed"].toBool()) {
                        validationFindings.clear();
                        lastValidation = "Edited document has not been validated.";
                    }
                }
                if (mutation || plan)
                    emit changed();
                if (authenticated && generation == replyGeneration)
                    send(toolReply(request.id, result));
            };
            if (stale || (!editing && !preview))
                return finish(toolError("stale_project", "Editing authorization expired."));
            Snapshot current;
            current.root = root;
            current.rootIdentity = rootIdentity;
            current.idKey = idKey;
            current.projectId = projectId;
            current.generation = generation;
            QElapsedTimer dispatchSnapshot;
            dispatchSnapshot.start();
            if (!snapshot(current, request.tool))
                return finish(toolError("validation_unavailable", "The live project index is not ready."));
            emit editSnapshotTaken(dispatchSnapshot);
            if (mutation || plan)
                for (auto &stage : review.stages)
                    stage.path = current.openAssetPaths.value(stage.assetId);
            try {
                if (plan || execute) {
                    const auto stages = request.arguments["stages"].toArray();
                    QSet<QString> targets;
                    QJsonArray previews;
                    auto commits = std::make_shared<QList<Commit>>();
                    const auto continuePreparation = [this, sequence] {
                        emit preparationCheckpoint();
                        // Poll only the transport. Dispatch remains guarded by pendingEdit,
                        // and no nested GUI event loop runs while a draft is being built.
                        if (socket && !socket->canReadLine())
                            socket->waitForReadyRead(0);
                        if (socket && socket->canReadLine())
                            receive();
                        return pendingEdit && sequence == editSequence && !stale;
                    };
                    for (const auto &value : stages) {
                        auto args = value.toObject();
                        const auto asset = args["assetId"].toString();
                        if (targets.contains(asset))
                            return finish(toolError("invalid_arguments", "Use one stage per document."));
                        targets.insert(asset);
                        args["generation"] = current.generation;
                        args["projectId"] = current.projectId;
                        Commit commit;
                        const Request stage{request.id, plan ? "nova.edit.preview" : "nova.edit.batch",
                                            args, {}, {}, execute, continuePreparation};
                        const auto result = editHandler(current, stage, editing, commit);
                        if (!pendingEdit || sequence != editSequence)
                            return;
                        if (result.contains("error"))
                            return finish(result);
                        updateReviewStage(commits->size() + previews.size(), plan ? "Ready" : "Prepared");
                        if (plan)
                            previews.append(result);
                        else if (!commit)
                            return finish(toolError("internal_failure", "A workflow stage could not be prepared."));
                        else
                            commits->append(std::move(commit));
                    }
                    if (plan)
                        return finish({{"status", "ready"}, {"stages", previews}, {"totalStages", stages.size()}});
                    auto state = std::make_shared<Workflow>();
                    state->total = int(stages.size());
                    workflow = state;
                    review.state = "Prepared";
                    lastEdit = QString("Workflow · %1 stages prepared").arg(state->total);
                    emit changed();
                    emit editPrepared();
                    QTimer::singleShot(0, this, [this, sequence, state, commits] {
                        publishWorkflow(sequence, state, commits);
                    });
                    return;
                }
                Commit commit;
                const auto result = editHandler(current, request, editing, commit);
                if (!commit)
                    return finish(result);
                if (mutation) {
                    review.state = "Prepared";
                    updateReviewStage(0, "Prepared");
                    emit changed();
                }
                emit editPrepared();
                // Let Qt deliver disconnect, cancellation, and editor events before publishing.
                QTimer::singleShot(0, this, [this, sequence, finish, mutation, commit = std::move(commit)] {
                    if (socket && socket->canReadLine())
                        receive();
                    if (!pendingEdit || sequence != editSequence)
                        return;
                    try {
                        QScopedValueRollback publication(publishingEdit, sequence);
                        if (mutation && !review.id.isEmpty()) {
                            review.state = "Executing";
                            updateReviewStage(0, "Executing");
                            emit changed();
                        }
                        const auto reviewId = review.id;
                        const auto result = commit();
                        if (mutation && (!pendingEdit || sequence != editSequence) &&
                            !reviewId.isEmpty() && review.id == reviewId && !result.contains("error")) {
                            updateReviewStage(0, "Committed", result);
                            review.state = "Completed";
                            review.error.clear();
                            emit changed();
                        }
                        finish(result);
                    } catch (...) {
                        finish(toolError("internal_failure", "Nova could not complete the operation."));
                    }
                });
            } catch (...) {
                finish(toolError("internal_failure", "Nova could not prepare the operation."));
            }
        });
        return;
    }
    cancel = std::make_shared<std::atomic_bool>(false);
    pendingId = request.id;
    suppress = false;
    worker = new QFutureWatcher<QJsonObject>(this);
    auto active = worker;
    connect(active, &QFutureWatcherBase::finished, this,
            [this, active, prepare, validate, revisions = data.revisions,
             records = validate ? data.diskRecords : QMap<QString, AssetIdentity>{},
             version = data.version, provisional = data.provisional, request] {
        deadline.stop();
        worker = nullptr;
        cancel.reset();
        if (!suppress) {
            QJsonObject result;
            try {
                result = active->result();
            } catch (...) {
                result = toolError("internal_failure", "Nova could not complete the operation.");
            }
            if (prepare) {
                beginReview(request);
                review.stages.clear();
                const auto rows = result["results"].toArray();
                for (const auto &value : rows) {
                    const auto row = value.toObject();
                    ReviewStage stage;
                    stage.path = row["path"].toString();
                    stage.assetId = row["assetId"].toString();
                    stage.state = row["documentOpen"].toBool() ? "Prepared" : "Unavailable";
                    review.stages.append(stage);
                }
                if (result.contains("error"))
                    stopReview(result["error"].toObject()["code"].toString(), true);
                else {
                    result["permission"] = editing ? "editing" : "read_only";
                    review.state = "Prepared";
                }
                emit changed();
            }
            if (validate) {
                if (!result.contains("error")) {
                    Snapshot current;
                    current.root = root;
                    current.idKey = idKey;
                    if (!snapshot(current, "validation_revision_check") || current.version != version ||
                        !current.diskRecords.isSharedWith(records) || current.provisional != provisional ||
                        current.revisions != revisions)
                        result = toolError("revision_conflict", "The editor state changed during validation.");
                }
                const auto error = result.value("error").toObject()["code"].toString();
                const auto counts = result.value("counts").toObject();
                lastValidation = error.isEmpty()
                    ? QString("Last validation · %1 errors · %2 warnings")
                        .arg(counts["Error"].toInt()).arg(counts["Warning"].toInt())
                    : "Validation · " + error;
                validationFindings.clear();
                for (const auto &finding : result["diagnostics"].toArray()) {
                    if (validationFindings.size() >= 32) {
                        validationFindings << "Additional findings were omitted.";
                        break;
                    }
                    const auto item = finding.toObject();
                    validationFindings << QString("%1 · %2 · %3")
                        .arg(item["severity"].toString(), item["code"].toString(),
                             item["message"].toString().left(256));
                }
                emit changed();
            }
            send(toolReply(pendingId, result));
        }
        active->deleteLater();
    });
    deadline.start(validate || request.tool == "nova.pack.validate" ? 60000 : 10000);
    worker->setFuture(QtConcurrent::run(
        [data = std::move(data), tool = request.tool, args = request.arguments, cancellation = cancel] {
            return inspect(data, tool, args, cancellation);
        }));
}
}
