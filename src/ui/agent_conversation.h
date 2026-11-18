// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QScrollArea>

class QJsonArray;
class QJsonObject;
class QVBoxLayout;

namespace nova {
QString agentDisplayText(QString text, int limit = 4000);

class AgentConversation final : public QScrollArea {
  public:
    enum class Speaker { You, Agent, Thinking };
    explicit AgentConversation(QWidget *parent);
    void add(Speaker speaker, const QString &text);
    void showTool(const QJsonObject &update);
    void showPlan(const QJsonArray &entries);
    void finishTurn();

  private:
    class Message;
    class Tool;
    QVBoxLayout *rows;
    Message *active = nullptr;
    QWidget *plan = nullptr;
    QHash<QString, Tool *> tools;
    bool follow = true;
    void append(QWidget *row);
    void finishMessage();
};
} // namespace nova
