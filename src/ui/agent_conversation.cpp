// SPDX-License-Identifier: GPL-3.0-or-later
#include "agent_conversation.h"
#include "theme.h"
#include <QAbstractTextDocumentLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace nova {
QString agentDisplayText(QString text, int limit) {
    static const QRegularExpression control(
        R"((?:\x1b\[[0-?]*[ -/]*[@-~])|[\x00-\x08\x0b-\x1f\x7f\x{200b}-\x{200f}\x{202a}-\x{202e}\x{2066}-\x{2069}])");
    text.remove(control);
    return text.size() > limit ? text.left(limit) + " …" : text;
}

namespace {
constexpr int MessageLimit = 200000;
constexpr int RowLimit = 500;

class MessageBody final : public QTextEdit {
  public:
    explicit MessageBody(QWidget *parent) : QTextEdit(parent) {
        setProperty("role", "transcriptBody");
        setReadOnly(true);
        setFocusPolicy(Qt::ClickFocus);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document()->setDocumentMargin(0);
        const auto fitHeight = [this](const QSizeF &size) { setFixedHeight(int(std::ceil(size.height()))); };
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, fitHeight);
        fitHeight(document()->size());
        connect(this, &QTextEdit::selectionChanged, this, [this] {
            if (formatPending && !textCursor().hasSelection())
                finishMarkdown();
        });
    }

    void appendText(const QString &text) {
        QTextCursor cursor(document());
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(text);
    }

    void finishMarkdown() {
        formatPending = textCursor().hasSelection();
        if (formatPending)
            return;
        // Prevent agent-controlled HTML and images from reading local or remote resources.
        const auto source = toPlainText();
        static const QRegularExpression image(R"(!\[([^\]]*)\])");
        QString markdown = source;
        markdown.replace(image, "[\\1]");
        document()->setMarkdown(markdown, QTextDocument::MarkdownFeatures::fromInt(
            QTextDocument::MarkdownDialectGitHub | QTextDocument::MarkdownNoHTML));
        themeCode();
    }

  protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
    void changeEvent(QEvent *event) override {
        QTextEdit::changeEvent(event);
        if (event->type() == QEvent::PaletteChange)
            themeCode();
    }

  private:
    bool formatPending = false;
    void themeCode() {
        for (auto block = document()->begin(); block.isValid(); block = block.next()) {
            auto format = block.blockFormat();
            if (!format.hasProperty(QTextFormat::BlockCodeLanguage) && !format.hasProperty(QTextFormat::BlockCodeFence))
                continue;
            format.setBackground(ui::current().raised);
            format.setLeftMargin(ui::SpaceL);
            format.setRightMargin(ui::SpaceL);
            QTextCursor(block).setBlockFormat(format);
        }
    }
};

QLabel *plainLabel(const QString &text, QWidget *parent, const char *role = nullptr) {
    auto label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    if (role)
        label->setProperty("role", role);
    return label;
}

QToolButton *disclosure(const QString &text, QWidget *parent, QWidget *details) {
    auto button = new QToolButton(parent);
    button->setText(text);
    button->setProperty("role", "disclosure");
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setArrowType(Qt::RightArrow);
    button->setCheckable(true);
    button->setFocusPolicy(Qt::StrongFocus);
    details->hide();
    QObject::connect(button, &QToolButton::toggled, details, [button, details](bool expanded) {
        button->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        details->setVisible(expanded);
    });
    return button;
}

class ActivityIcon final : public QLabel {
  public:
    explicit ActivityIcon(QWidget *parent) : QLabel(parent) { setFixedSize(ui::IconSize, ui::IconSize); }
    void setStatus(const QString &state) {
        status = state;
        refresh();
    }
  protected:
    void changeEvent(QEvent *event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::PaletteChange)
            refresh();
    }
  private:
    QString status;
    void refresh() {
        const auto &t = ui::current();
        const auto icon = status == "completed" ? ui::lineIcon("check", t.success) :
                          status == "failed" ? ui::lineIcon("error", t.danger) :
                          status == "in_progress" ? ui::lineIcon("in-progress", t.info) :
                          ui::lineIcon("pending", t.muted);
        setPixmap(icon.pixmap(size(), devicePixelRatioF()));
    }
};

QString statusWord(const QString &state) {
    if (state == "completed")
        return "Completed";
    if (state == "failed")
        return "Failed";
    if (state == "in_progress")
        return "In progress";
    return "Pending";
}

// ACP ToolKind as a word; "other" and unknown kinds add nothing.
QString kindWord(const QString &kind) {
    static const QHash<QString, QString> words{
        {"read", "Read"}, {"edit", "Edit"}, {"delete", "Delete"}, {"move", "Move"}, {"search", "Search"},
        {"execute", "Run"}, {"think", "Think"}, {"fetch", "Fetch"}, {"switch_mode", "Mode change"}};
    return words.value(kind);
}

QString byteSize(qint64 bytes) {
    return bytes >= 1024 * 1024 ? QString("%1 MB").arg(double(bytes) / (1024 * 1024), 0, 'f', 1)
                                : QString("%1 KB").arg(qMax<qint64>(1, (bytes + 1023) / 1024));
}

// Markdown punctuation in agent-supplied names stays literal.
QString markdownLiteral(QString text) {
    static const QRegularExpression special(R"(([\\`*_{}\[\]()<>#+!|~-]))");
    return text.replace(special, "\\\\1");
}

// One plain line for a non-text content block. Nothing is fetched, decoded or played.
QString contentSummary(const QJsonObject &content) {
    const auto type = content["type"].toString();
    const auto mime = agentDisplayText(content["mimeType"].toString(), 60).simplified();
    const auto bytes = qint64(content["data"].toString().size()) * 3 / 4; // base64
    if (type == "resource_link") {
        const auto name = agentDisplayText(content["title"].toString(content["name"].toString()), 160).simplified();
        const auto uri = agentDisplayText(content["uri"].toString(), 400).simplified();
        return name.isEmpty() || name == uri ? "Link: " + uri : "Link: " + name + " (" + uri + ')';
    }
    if (type == "image")
        return QString("Image (%1, %2) not shown").arg(mime.isEmpty() ? "unknown type" : mime, byteSize(bytes));
    if (type == "audio")
        return QString("Audio (%1, %2) not played").arg(mime.isEmpty() ? "unknown type" : mime, byteSize(bytes));
    if (type == "resource")
        return "Attached resource: " +
               agentDisplayText(content["resource"].toObject()["uri"].toString(), 400).simplified();
    return {};
}

QString jsonText(const QJsonValue &value) {
    if (value.isObject() || value.isArray()) {
        const auto doc = value.isObject() ? QJsonDocument(value.toObject()) : QJsonDocument(value.toArray());
        return agentDisplayText(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)), 1200);
    }
    return agentDisplayText(value.toVariant().toString(), 1200);
}
} // namespace

class AgentConversation::Message final : public QWidget {
  public:
    Speaker speaker;
    MessageBody *body;
    int characters = 0;

    Message(Speaker who, QWidget *parent) : QWidget(parent), speaker(who), body(new MessageBody(this)) {
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(ui::SpaceS);
        if (speaker == Speaker::Thinking) {
            auto toggle = disclosure("Agent reasoning", this, body);
            toggle->setObjectName("acpReasoning");
            layout->addWidget(toggle, 0, Qt::AlignLeft);
            body->setAccessibleName("Agent reasoning");
        } else {
            const QString name = speaker == Speaker::You ? "You" : "Agent";
            layout->addWidget(plainLabel(name, this, "strong"));
            body->setAccessibleName(name);
        }
        layout->addWidget(body);
    }
};

class AgentConversation::Tool final : public QFrame {
  public:
    int turn;
    Tool(int started, QWidget *parent) : QFrame(parent), turn(started) {
        setObjectName("acpToolCard");
        setProperty("role", "card");
        auto outer = new QVBoxLayout(this);
        outer->setContentsMargins(ui::SpaceL, ui::SpaceS, ui::SpaceL, ui::SpaceS);
        outer->setSpacing(ui::SpaceS);
        auto header = new QHBoxLayout;
        header->setSpacing(ui::SpaceM);
        icon = new ActivityIcon(this);
        header->addWidget(icon);
        details = plainLabel({}, this, "hint");
        details->setObjectName("acpToolContent");
        details->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        toggle = disclosure({}, this, details);
        toggle->setObjectName("acpToolDetails");
        header->addWidget(toggle);
        header->addStretch();
        stateLabel = plainLabel({}, this, "hint");
        header->addWidget(stateLabel);
        outer->addLayout(header);
        outer->addWidget(details);
    }

    void apply(const QJsonObject &update) {
        if (update["title"].isString())
            title = agentDisplayText(update["title"].toString(), 44).simplified();
        if (update["status"].isString())
            state = update["status"].toString().left(32);
        if (update["kind"].isString())
            kind = kindWord(update["kind"].toString());
        // ACP v1 has no structured MCP-server field. This label describes the agent's
        // reported call and plays no part in authorization.
        static const QRegularExpression novaTool(R"(\bnova\.[a-z_][a-z0-9_]*\.[a-z0-9_.]+\b)");
        const auto source = novaTool.match(title).hasMatch() ? "Nova tool" : "Agent tool";
        toggle->setText(QString("%1 · %2").arg(source, title));
        icon->setStatus(state);
        stateLabel->setText(kind.isEmpty() ? statusWord(state) : kind + " · " + statusWord(state));
        bool changed = false;
        if (update.contains("rawInput") && !update["rawInput"].isNull()) {
            input = "Input\n" + jsonText(update["rawInput"]);
            changed = true;
        }
        if (update.contains("rawOutput") && !update["rawOutput"].isNull()) {
            output = "Output\n" + jsonText(update["rawOutput"]);
            changed = true;
        }
        if (update["content"].isArray()) {
            readContent(update["content"].toArray());
            changed = true;
        }
        if (update["locations"].isArray()) {
            readLocations(update["locations"].toArray());
            changed = true;
        }
        // A status-only update leaves the details as they are.
        if (!changed && !details->text().isEmpty())
            return;
        QStringList lines;
        if (!places.isEmpty())
            lines << places;
        if (!input.isEmpty())
            lines << input;
        if (!output.isEmpty())
            lines << output;
        lines += contentLines;
        details->setText(lines.isEmpty() ? "The agent provided no details." : lines.join("\n\n"));
    }

  private:
    QString title = "Tool call", state, kind, input, output, places;
    QStringList contentLines;

    void readLocations(const QJsonArray &items) {
        QStringList paths;
        for (qsizetype i = 0; i < qMin<qsizetype>(items.size(), 8); ++i) {
            const auto item = items.at(i).toObject();
            auto path = agentDisplayText(item["path"].toString(), 200).simplified();
            if (path.isEmpty())
                continue;
            if (item["line"].isDouble())
                path += ':' + QString::number(qint64(item["line"].toDouble()));
            paths << path;
        }
        places = paths.isEmpty() ? QString{} : (paths.size() == 1 ? "Location\n" : "Locations\n") + paths.join('\n');
    }
    ActivityIcon *icon;
    QLabel *details, *stateLabel;
    QToolButton *toggle;

    void readContent(const QJsonArray &items) {
        contentLines.clear();
        for (qsizetype i = 0; i < qMin<qsizetype>(items.size(), 8); ++i) {
            const auto item = items.at(i).toObject();
            const auto content = item["content"].toObject();
            if (item["type"] == "content" && content["type"] == "text")
                contentLines << agentDisplayText(content["text"].toString(), 1200);
            else if (const auto summary = contentSummary(content); item["type"] == "content" && !summary.isEmpty())
                contentLines << summary;
            else if (item["type"] == "diff")
                contentLines << "Changes to " + agentDisplayText(item["path"].toString(), 200) +
                                "\nBefore\n" + agentDisplayText(item["oldText"].toString(), 800) +
                                "\nAfter\n" + agentDisplayText(item["newText"].toString(), 800);
        }
    }
};

AgentConversation::AgentConversation(QWidget *parent) : QScrollArea(parent) {
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setProperty("role", "transcript");
    setAccessibleName("Conversation with the agent");
    auto content = new QWidget;
    content->setProperty("role", "transcriptContent");
    rows = new QVBoxLayout(content);
    rows->setContentsMargins(ui::PanelPadding, ui::PanelPadding, ui::PanelPadding, ui::PanelPadding);
    rows->setSpacing(ui::SpaceL);
    rows->addStretch();
    setWidget(content);
    auto bar = verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, [this, bar](int value) {
        follow = value >= bar->maximum() - ui::SpaceS;
    });
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar](int, int end) {
        if (follow)
            bar->setValue(end);
    });
    flushTimer.setSingleShot(true);
    flushTimer.setInterval(FlushInterval);
    connect(&flushTimer, &QTimer::timeout, this, &AgentConversation::flush);
}

void AgentConversation::append(QWidget *row) {
    rows->insertWidget(rows->count() - 1, row);
    if (rows->count() > RowLimit + 1)
        evictOldest();
}

void AgentConversation::evictOldest() {
    const auto oldest = rows->takeAt(0);
    auto widget = oldest->widget();
    if (widget == plan)
        plan = nullptr;
    if (widget == active)
        active = nullptr;
    if (auto tool = dynamic_cast<Tool *>(widget); tool && tools.value(tools.key(tool)) == tool)
        tools.remove(tools.key(tool));
    if (auto message = dynamic_cast<Message *>(widget))
        characters -= message->characters;
    delete widget;
    delete oldest;
}

void AgentConversation::add(Speaker speaker, const QString &text) {
    if (text.isEmpty())
        return;
    if (speaker == Speaker::You) {
        finishMessage(); // your message always starts its own row, at once
        pendingSpeaker = speaker;
        pending = text;
        return flush();
    }
    if (!pending.isEmpty() && pendingSpeaker != speaker)
        flush();
    pendingSpeaker = speaker;
    pending += text;
    // One insert and one relayout per frame, however many chunks the agent sent.
    if (pending.size() >= 64 * 1024)
        flush();
    else if (!flushTimer.isActive())
        flushTimer.start();
}

void AgentConversation::addContent(Speaker speaker, const QJsonObject &content) {
    if (content["type"] == "resource_link" && speaker == Speaker::Agent) {
        // Shown as a link; Nova does not open or fetch it (MessageBody loads no resources).
        auto uri = agentDisplayText(content["uri"].toString(), 400).simplified();
        uri.replace(' ', "%20").replace('<', "%3C").replace('>', "%3E");
        const auto name = agentDisplayText(content["title"].toString(content["name"].toString()), 160).simplified();
        if (uri.isEmpty())
            return;
        const auto label = name.isEmpty() ? uri : name;
        auto link = QString("[%1](<%2>)").arg(markdownLiteral(label), uri);
        if (label != uri)
            link += " (" + markdownLiteral(uri) + ')';
        return add(speaker, "\n\n" + link + "\n\n");
    }
    const auto summary = contentSummary(content);
    if (!summary.isEmpty())
        add(speaker, "\n\n" + (speaker == Speaker::Agent ? '*' + markdownLiteral(summary) + '*' : summary) + "\n\n");
}

void AgentConversation::flush() {
    flushTimer.stop();
    if (pending.isEmpty())
        return;
    const auto text = std::exchange(pending, {});
    if (!active || active->speaker != pendingSpeaker) {
        finishMessage();
        active = new Message(pendingSpeaker, widget());
        append(active);
    }
    const auto remaining = MessageLimit - active->characters;
    if (remaining <= 0)
        return;
    const auto chunk = agentDisplayText(text, remaining);
    active->characters += int(chunk.size());
    characters += chunk.size();
    active->body->appendText(chunk);
    while (characters > CharacterBudget && rows->count() > 2)
        evictOldest();
}

void AgentConversation::finishMessage() {
    flush();
    if (active && active->speaker == Speaker::Agent)
        active->body->finishMarkdown();
    active = nullptr;
}

void AgentConversation::showTool(const QJsonObject &update) {
    const auto id = update["toolCallId"].toString();
    if (id.isEmpty())
        return;
    auto tool = tools.value(id);
    // A tool_call starts a call; an id an agent reuses in a later turn gets a new card. A late
    // tool_call_update after the turn ended still finds the card it belongs to.
    if (!tool || (update["sessionUpdate"] == "tool_call" && tool->turn != turn)) {
        finishMessage();
        tool = new Tool(turn, widget());
        tools.insert(id, tool);
        append(tool);
    }
    tool->apply(update);
}

void AgentConversation::showPlan(const QJsonArray &entries) {
    if (entries.isEmpty() && !plan)
        return;
    if (!plan) {
        finishMessage();
        auto card = new QFrame(widget());
        card->setObjectName("acpPlan");
        card->setProperty("role", "card");
        auto checklist = new QVBoxLayout(card);
        checklist->setContentsMargins(ui::SpaceL, ui::SpaceM, ui::SpaceL, ui::SpaceM);
        checklist->setSpacing(ui::SpaceS);
        plan = card;
        append(plan);
    }
    auto checklist = plan->layout();
    while (auto item = checklist->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    checklist->addWidget(plainLabel("Plan", plan, "strong"));
    for (qsizetype i = 0; i < qMin<qsizetype>(entries.size(), 24); ++i) {
        const auto entry = entries.at(i).toObject();
        auto line = new QWidget(plan);
        auto row = new QHBoxLayout(line);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(ui::SpaceM);
        const auto status = entry["status"].toString();
        auto icon = new ActivityIcon(line);
        icon->setStatus(status);
        row->addWidget(icon);
        row->addWidget(plainLabel(agentDisplayText(entry["content"].toString(), 160), line), 1);
        row->addWidget(plainLabel(statusWord(status), line, "hint"));
        checklist->addWidget(line);
    }
    plan->setVisible(!entries.isEmpty());
}

void AgentConversation::finishTurn() {
    finishMessage();
    ++turn;
    plan = nullptr;
}
} // namespace nova
