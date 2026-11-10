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
    QString resource; // a project resource read (resources/read) that the service must answer
};
class Protocol {
    enum class State { Initial, AwaitingNotification, Ready };
    State state = State::Initial;
    QString client;
  public:
    QString clientName() const { return client; }
    Request receive(const QByteArray &line);
};
// Tool errors are {code, message}; details carry data only where it helps the caller (a JSON
// pointer for invalid arguments, per-document save outcomes, supported versions or topics).
QJsonObject toolError(const QString &code, const QString &message, const QJsonObject &details = {});
QJsonObject toolReply(const QJsonValue &id, const QJsonObject &payload);
QJsonObject rpcError(const QJsonValue &id, int code, const QString &message, const QJsonValue &data = {});
QJsonObject resourceReply(const QJsonValue &id, const QString &uri, const QString &mimeType, const QString &text);
// The first schema violation as a JSON pointer and a field-specific message; empty when valid.
struct SchemaError {
    QString pointer, message;
    bool isEmpty() const { return message.isEmpty(); }
};
SchemaError checkArguments(const QJsonValue &value, const QJsonObject &schema);
const QJsonArray &toolDefinitions();
// The native command behind a compact texture tool (nova.texture.put_pixels -> put_pixels); empty
// for every other tool. These tools are mutations and need editing permission.
QString compactTextureCommand(const QString &tool);
QStringList categories();
QString randomToken();
}
