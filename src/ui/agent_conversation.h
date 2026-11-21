// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QScrollArea>
#include <QTimer>

class QJsonArray;
class QJsonObject;
class QVBoxLayout;

namespace nova {
QString agentDisplayText(QString text, int limit = 4000);

class AgentConversation final : public QScrollArea {
  public:
    enum class Speaker { You, Agent, Thinking };
    explicit AgentConversation(QWidget *parent);
    // Agent text arrives in many small chunks; it is collected and shown once per frame.
    void add(Speaker speaker, const QString &text);
    // A non-text content block (resource_link, image, audio, resource): a link or a short placeholder.
    void addContent(Speaker speaker, const QJsonObject &content);
    void showTool(const QJsonObject &update);
    void showPlan(const QJsonArray &entries);
    void finishTurn();
    static constexpr int FlushInterval = 33;           // ms between transcript updates while streaming
    static constexpr int CharacterBudget = 2'000'000; // the oldest rows go once messages exceed it

  private:
    class Message;
    class Tool;
    QVBoxLayout *rows;
    Message *active = nullptr;
    QWidget *plan = nullptr;
    QHash<QString, Tool *> tools; // kept across turns, so late updates find their card
    QString pending;
    Speaker pendingSpeaker = Speaker::Agent;
    QTimer flushTimer;
    qsizetype characters = 0; // text held by message rows
    int turn = 0;
    bool follow = true;
    void append(QWidget *row);
    void evictOldest();
    void flush();
    void finishMessage();
};
} // namespace nova
