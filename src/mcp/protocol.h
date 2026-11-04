// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <functional>

namespace nova::mcp {
constexpr qsizetype RequestLimit = 64 * 1024;
constexpr qsizetype ResponseLimit = 512 * 1024;
struct Request {
    QJsonValue id;
    QString tool;
    QJsonObject arguments;
    QJsonObject response;
    QJsonValue cancelId;
    bool workflowStage = false;
    std::function<bool()> continuePreparation;
};
class Protocol {
    enum class State { Initial, AwaitingNotification, Ready };
    State state = State::Initial;
    QString client;
  public:
    QString clientName() const { return client; }
    Request receive(const QByteArray &line);
};
QJsonObject toolError(const QString &code, const QString &message);
QJsonObject toolReply(const QJsonValue &id, const QJsonObject &payload);
QJsonObject rpcError(const QJsonValue &id, int code, const QString &message);
QStringList categories();
QString randomToken();
}
