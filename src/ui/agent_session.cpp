// SPDX-License-Identifier: GPL-3.0-or-later
// ACP prototype: Nova starts one agent (OpenCode's "opencode acp", or another ACP agent .exe), attaches
// Nova's MCP bridge to the agent session, and keeps the grant under Nova's control. Read-only by
// default; editing needs a separate confirmation after a read-only session has connected. The grant
// accepts only a bridge started by this agent, and ends with the session, a revoke or a project switch.
#include "window.h"
#include "clients.h"
#include "service.h"
#include "agent_client.h"
#include "agent_conversation.h"
#include "theme.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QToolButton>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace nova {
namespace clients = mcp::clients;
namespace {
struct AgentChoice {
    QString id, name;
    QStringList commands; // the current adapter's command first, then commands of retired packages
    QStringList arguments;
    QString package, install, signIn; // package: what `install` installs, as the user knows it
    QStringList retired; // npm packages replaced by `package`; they no longer receive updates
    QString replaced, replacedName; // a predecessor's command, found only to explain the replacement
};
const QList<AgentChoice> agentChoices{
    {"opencode", "OpenCode", {"opencode"}, {"acp"}, "opencode-ai", "npm install -g opencode-ai", "opencode auth login", {}},
    {"claude", "Claude Code", {"claude-agent-acp", "claude-code-acp"}, {}, "@agentclientprotocol/claude-agent-acp",
     "npm install -g @agentclientprotocol/claude-agent-acp", "claude, then /login",
     {"@zed-industries/claude-agent-acp", "@zed-industries/claude-code-acp"}},
    {"codex", "Codex", {"codex-acp"}, {}, "@agentclientprotocol/codex-acp", "npm install -g @agentclientprotocol/codex-acp",
     "codex login", {"@zed-industries/codex-acp"}},
    // Google replaced Gemini CLI with Antigravity CLI for individual accounts; Gemini CLI now refuses them.
    {"antigravity", "Google Antigravity", {"agy"}, {"acp"}, "Antigravity CLI", "winget install Google.AntigravityCLI",
     "agy acp --login", {}, "gemini", "Gemini CLI"},
    {"custom", "Custom", {}, {}, {}, {}, {}, {}},
};
// Earlier releases saved these agent ids.
QString currentAgentId(const QString &id) { return id == "gemini" ? "antigravity" : id; }
struct Detection {
    QString path, retired; // retired: the replaced npm package this launcher still runs
    QString replaced;      // the predecessor's launcher, when the agent itself is missing
};
Detection detect(const AgentChoice &choice) {
    for (const auto &command : choice.commands) {
        const auto path = acp::findAgentExecutable(command);
        if (path.isEmpty())
            continue;
        const auto script = QDir::fromNativeSeparators(acp::resolveAgentProgram(path, {}).arguments.value(0));
        for (const auto &package : choice.retired)
            if (script.contains("/node_modules/" + package + '/', Qt::CaseInsensitive))
                return {path, package, {}};
        return {path, {}, {}};
    }
    if (!choice.replaced.isEmpty())
        return {{}, {}, acp::findAgentExecutable(choice.replaced)};
    return {};
}
// One command per line, so pasting into cmd or PowerShell runs each. The retired package goes first:
// it may own the same command name, which npm refuses to overwrite.
QString installCommand(const AgentChoice &choice, const Detection &found) {
    return found.retired.isEmpty() ? choice.install : "npm uninstall -g " + found.retired + '\n' + choice.install;
}
QString setupText(const AgentChoice &choice, const Detection &found) {
    const auto commands = installCommand(choice, found);
    if (!found.retired.isEmpty())
        return QString("This launcher runs %1, which no longer receives updates. It has been replaced by %2. "
                       "Run these in a terminal, then choose Check again:\n%3")
            .arg(found.retired, choice.package, commands);
    const auto steps = QString("Run this in a terminal%1:\n%2\nSign in with %3, then choose Check again.")
                           .arg(commands.startsWith("npm ") ? " (requires Node.js and npm)" : QString{}, commands,
                                choice.signIn);
    if (!found.replaced.isEmpty())
        return QString("%1 was found, but Google replaced it with %2 for individual accounts, and %1 no longer "
                       "signs them in. To use Gemini with a paid license, choose Custom instead. %3")
            .arg(choice.replacedName, choice.package, steps);
    return "Not installed. " + steps;
}
QString bridgePath() {
    const auto configured = qApp->property("mcpBridgePath").toString(); // tests supply the build's bridge
    return configured.isEmpty() ? clients::locateBridge(QCoreApplication::applicationDirPath()).path : configured;
}

class Composer final : public QPlainTextEdit {
  public:
    explicit Composer(QWidget *parent)
        : QPlainTextEdit(parent), completer(new QCompleter(this)), choices(new QStandardItemModel(this)) {
        setTabChangesFocus(true);
        setAccessibleName("Message to the agent");
        setAccessibleDescription("Enter sends. Shift+Enter inserts a line break. Up recalls earlier messages. "
                                 "Type / to list the agent's commands.");
        setFixedHeight(3 * fontMetrics().lineSpacing() + 2 * ui::SpaceL);
        // The agent's slash commands: the list shows each name and description; the name is completed.
        completer->setModel(choices);
        completer->setWidget(this);
        completer->setCompletionMode(QCompleter::PopupCompletion);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setCompletionRole(Qt::UserRole);
        completer->popup()->setObjectName("acpCommandList");
        completer->popup()->setAccessibleName("Agent commands");
        connect(completer, qOverload<const QModelIndex &>(&QCompleter::activated), this, [this](const QModelIndex &index) {
            setPlainText(index.data(Qt::UserRole).toString() + ' ');
            moveCursor(QTextCursor::End);
        });
        connect(this, &QPlainTextEdit::textChanged, this, &Composer::suggest);
    }
    std::function<void()> submit;
    // AvailableCommand[] from the agent (untrusted): names and descriptions are display text only.
    void setCommands(const QJsonArray &commands) {
        choices->clear();
        for (const auto &value : commands) {
            const auto command = value.toObject();
            const auto name = agentDisplayText(command["name"].toString(), 64).simplified();
            if (name.isEmpty() || name.contains(' '))
                continue;
            const auto description = agentDisplayText(command["description"].toString(), 160).simplified();
            const auto input = agentDisplayText(command["input"].toObject()["hint"].toString(), 80).simplified();
            auto item = new QStandardItem('/' + name + (description.isEmpty() ? QString{} : "  ·  " + description));
            item->setData('/' + name, Qt::UserRole);
            item->setToolTip(input.isEmpty() ? description : description + "\nInput: " + input);
            item->setEditable(false);
            choices->appendRow(item);
        }
        suggest();
    }
    void remember(const QString &text) {
        if (history.isEmpty() || history.last() != text)
            history.append(text);
        if (history.size() > 30)
            history.removeFirst();
        position = -1;
        draft.clear();
        clear();
    }

  protected:
    void keyPressEvent(QKeyEvent *event) override {
        const auto key = event->key();
        // While the command list is open, it takes Enter, Tab and Escape.
        if (completer->popup()->isVisible() && (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Tab ||
                                                key == Qt::Key_Backtab || key == Qt::Key_Escape)) {
            event->ignore();
            return;
        }
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) &&
            !(event->modifiers() & ~Qt::KeypadModifier)) {
            if (submit)
                submit();
            return;
        }
        const auto cursor = textCursor();
        const auto block = cursor.block();
        const auto line = block.layout()->lineForTextPosition(cursor.positionInBlock());
        const bool firstLine = block.blockNumber() == 0 && (!line.isValid() || line.lineNumber() == 0);
        if (event->modifiers() == Qt::NoModifier && !cursor.hasSelection() && !history.isEmpty() &&
            ((key == Qt::Key_Up && firstLine) || (key == Qt::Key_Down && position >= 0))) {
            if (position < 0) {
                draft = toPlainText();
                position = int(history.size());
            }
            position = qBound(0, position + (key == Qt::Key_Up ? -1 : 1), int(history.size()));
            if (position == history.size()) {
                setPlainText(draft);
                position = -1;
                moveCursor(QTextCursor::End);
            } else {
                setPlainText(history.at(position));
                moveCursor(QTextCursor::Start);
            }
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }

  private:
    QCompleter *completer;
    QStandardItemModel *choices;
    QStringList history;
    QString draft;
    int position = -1;

    void suggest() {
        const auto text = toPlainText();
        static const QRegularExpression space(R"(\s)");
        const auto popup = completer->popup();
        if (!text.startsWith('/') || text.contains(space) || !choices->rowCount()) {
            popup->hide();
            return;
        }
        completer->setCompletionPrefix(text);
        if (!completer->completionCount()) {
            popup->hide();
            return;
        }
        auto area = cursorRect();
        area.setWidth(qMax(width() / 2, popup->sizeHintForColumn(0) + popup->verticalScrollBar()->sizeHint().width()));
        completer->complete(area);
        popup->setCurrentIndex(completer->completionModel()->index(0, 0)); // Enter picks the first match
    }
};

QVBoxLayout *section(QVBoxLayout *parent, const QString &title, int stretch = 0) {
    auto box = new QGroupBox(title);
    box->setProperty("role", "section");
    auto layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0); // the section role supplies the panel padding
    layout->setSpacing(ui::SpaceL);
    parent->addWidget(box, stretch);
    return layout;
}

void appearance(QWidget *widget, const char *property, const QString &value) {
    if (widget->property(property).toString() == value)
        return;
    widget->setProperty(property, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

class PermissionCard final : public QFrame {
  public:
    explicit PermissionCard(QWidget *parent) : QFrame(parent) {
        setObjectName("acpPermissionCard");
        setProperty("role", "notice");
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(ui::PanelPadding, ui::SpaceL, ui::PanelPadding, ui::SpaceL);
        layout->setSpacing(ui::SpaceM);
        auto heading = new QHBoxLayout;
        heading->setSpacing(ui::SpaceL);
        auto title = new QLabel("Tool permission", this);
        title->setProperty("role", "strong");
        heading->addWidget(title);
        heading->addStretch();
        count = ui::hint({}, this);
        count->setObjectName("acpPermissionCount");
        heading->addWidget(count);
        layout->addLayout(heading);
        summary = new QLabel(this);
        summary->setObjectName("acpPermission");
        summary->setWordWrap(true);
        summary->setTextFormat(Qt::PlainText);
        summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(summary);
        layout->addWidget(ui::hint("Your decision goes to the agent. Nova tool access is managed in Session.", this));
        actions = new QHBoxLayout;
        actions->setSpacing(ui::SpaceM);
        layout->addLayout(actions);
        hide();
    }

    std::function<void(const QJsonValue &, const QString &)> answer;

    void enqueue(const QJsonValue &id, const QJsonObject &params) {
        auto tool = params["toolCall"].toObject();
        Request next{id, describe(tool), {}};
        for (const auto &entry : params["options"].toArray()) {
            const auto option = entry.toObject();
            const auto optionId = option["optionId"].toString();
            if (!optionId.isEmpty())
                next.options.append({optionId, option["kind"].toString(), agentDisplayText(option["name"].toString(), 80)});
        }
        queue.append(std::move(next));
        if (queue.size() == 1)
            showFirst();
        else
            updateCount();
    }

    void clear() {
        queue.clear();
        refreshActions();
        hide();
    }

    // The agent no longer needs an answer to this request.
    void withdraw(const QJsonValue &id) {
        for (qsizetype i = 0; i < queue.size(); ++i) {
            if (queue.at(i).id != id)
                continue;
            queue.removeAt(i);
            if (i == 0)
                showFirst();
            else
                updateCount();
            return;
        }
    }

  private:
    struct Option { QString id, kind, name; };
    struct Request { QJsonValue id; QString description; QList<Option> options; };
    QLabel *summary, *count;
    QHBoxLayout *actions;
    QList<Request> queue;

    static QString describe(const QJsonObject &tool) {
        const auto title = agentDisplayText(tool["title"].toString("Tool call"), 160).simplified();
        const auto args = tool["rawInput"].toObject();
        QStringList lines{title};
        const QStringList keys{"path", "file", "command", "arguments", "query", "url"};
        const auto add = [&lines](const QString &key, const QJsonValue &data) {
            if (data.isUndefined() || data.isNull())
                return;
            const auto text = data.isString() ? data.toString() :
                QString::fromUtf8(QJsonDocument(QJsonArray{data}).toJson(QJsonDocument::Compact)).sliced(1).chopped(1);
            lines << agentDisplayText(key, 32) + "  " + agentDisplayText(text, 200).simplified();
        };
        for (const auto &key : keys) {
            if (lines.size() == 4)
                break;
            add(key, args.value(key));
        }
        for (auto it = args.constBegin(); lines.size() < 4 && it != args.constEnd(); ++it)
            if (!keys.contains(it.key()))
                add(it.key(), it.value());
        if (lines.size() == 1) {
            const auto locations = tool["locations"].toArray();
            if (!locations.isEmpty()) {
                const auto path = agentDisplayText(locations.first().toObject()["path"].toString(), 200).simplified();
                if (!path.isEmpty())
                    lines << "Path  " + path;
            }
        }
        return lines.join('\n');
    }

    static QString label(const Option &option, bool distinct) {
        static const QMap<QString, QString> names{
            {"allow_once", "Allow once"}, {"allow_always", "Always allow"},
            {"reject_once", "Reject once"}, {"reject_always", "Always reject"}};
        const auto standard = names.value(option.kind);
        if (standard.isEmpty())
            return option.name.isEmpty() ? "Select" : option.name;
        return distinct && !option.name.isEmpty() ? standard + " · " + option.name : standard;
    }

    void updateCount() { count->setText(queue.size() > 1 ? QString("%1 pending").arg(queue.size()) : QString{}); }

    void choose(const QJsonValue &requestId, const QString &optionId) {
        if (queue.isEmpty() || queue.first().id != requestId)
            return;
        const auto id = queue.takeFirst().id;
        if (answer)
            answer(id, optionId);
        showFirst();
    }

    void refreshActions() {
        while (auto item = actions->takeAt(0)) {
            if (auto button = item->widget()) {
                button->hide();
                button->deleteLater(); // the clicked button may be on the stack
            }
            delete item;
        }
    }

    void showFirst() {
        refreshActions();
        setVisible(!queue.isEmpty());
        if (queue.isEmpty())
            return;
        const auto &request = queue.first();
        summary->setText(request.description);
        updateCount();
        QMap<QString, int> kinds;
        for (const auto &option : request.options)
            ++kinds[option.kind];
        const auto addButton = [this, requestId = request.id](const QString &text, const QString &name, const QString &id) {
            auto button = new QPushButton(text, this);
            button->setObjectName(name);
            button->setAutoDefault(false);
            actions->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, requestId, id] { choose(requestId, id); });
        };
        if (request.options.isEmpty()) {
            addButton("Dismiss request", "acpPermissionDismiss", {});
        } else if (request.options.size() > 4 || kinds.size() != request.options.size() ||
                   kinds.contains(QString{})) {
            auto picker = new QComboBox(this);
            picker->setAccessibleName("Tool permission options");
            for (const auto &option : request.options)
                picker->addItem(label(option, true), option.id);
            picker->setPlaceholderText("Choose a decision");
            picker->setCurrentIndex(-1);
            actions->addWidget(picker, 1);
            auto apply = new QPushButton("Apply decision", this);
            apply->setAutoDefault(false);
            apply->setEnabled(false);
            actions->addWidget(apply);
            connect(picker, &QComboBox::currentIndexChanged, apply, [apply](int index) { apply->setEnabled(index >= 0); });
            connect(apply, &QPushButton::clicked, this, [this, picker, requestId = request.id] {
                choose(requestId, picker->currentData().toString());
            });
            return;
        } else {
            static const QMap<QString, QString> buttonNames{
                {"allow_once", "acpPermissionAllow"}, {"allow_always", "acpPermissionAllowAlways"},
                {"reject_once", "acpPermissionReject"}, {"reject_always", "acpPermissionRejectAlways"}};
            for (const auto &option : request.options)
                addButton(label(option, false), buttonNames.value(option.kind), option.id);
        }
        actions->addStretch();
    }
};

// A mode or setting that stops the agent's own tools asking before they act (by id or name; agents
// name these freely, so this errs towards asking).
bool permissive(const QString &id, const QString &name) {
    static const QRegularExpression risky(
        R"(bypass|full[\s_-]*access|yolo|danger|unrestricted|skip[\s_-]*permission|dont[\s_-]*ask|don't[\s_-]*ask|)"
        R"(never[\s_-]*ask|accept[\s_-]*edits|auto[\s_-]*(?:approve|accept|edit)|^auto$)",
        QRegularExpression::CaseInsensitiveOption);
    return risky.match(id).hasMatch() || risky.match(name.trimmed()).hasMatch();
}

QString tokenCount(qint64 tokens) {
    if (tokens >= 1'000'000)
        return QString::number(double(tokens) / 1'000'000, 'f', 1) + 'M';
    if (tokens >= 1000)
        return QString::number(double(tokens) / 1000, 'f', tokens < 10'000 ? 1 : 0) + 'k';
    return QString::number(tokens);
}

// "12k of 200k tokens · $0.42", or empty before the agent reports usage.
QString usageText(const acp::Session::Usage &usage) {
    QStringList parts;
    if (usage.used >= 0 && usage.size > 0)
        parts << QString("%1 of %2 tokens").arg(tokenCount(usage.used), tokenCount(usage.size));
    if (usage.cost >= 0) {
        const auto amount = QString::number(usage.cost, 'f', 2);
        const auto currency = agentDisplayText(usage.currency, 8).simplified();
        parts << (currency == "USD" ? '$' + amount : (amount + ' ' + currency).trimmed());
    }
    return parts.join(" · ");
}

class AgentSessionDialog : public QDialog {
  public:
    QPointer<acp::Session> session;
    QString sessionProject, workFolder;
    quint64 grant = 0;
    bool editing = false;
    bool keepFolder = false; // the next session continues this one in the same folder
    QHBoxLayout *settingsRow;
    QMap<QString, QPointer<QWidget>> settings; // "mode" or a config option id → label and picker
    QCheckBox *mentionAsset;
    QLabel *status, *statusDetail, *access, *notice;
    QLineEdit *program, *arguments;
    Composer *prompt;
    QComboBox *agentPicker, *authMethod;
    QList<Detection> detected; // one per agentChoices entry
    QWidget *authRow;
    acp::AgentProgram activeProgram;
    AgentConversation *conversation;
    QPushButton *start, *edit, *revoke, *send, *cancel, *stop;
    PermissionCard *permission;
    explicit AgentSessionDialog(QWidget *parent) : QDialog(parent) {}
    // One line for things the user should know that are not part of the conversation.
    void tell(const QString &message, const QString &severity = "info") {
        appearance(notice, "severity", severity);
        notice->setText(agentDisplayText(message, 1000));
        notice->setVisible(!message.isEmpty());
    }
};
} // namespace

void Window::showAgentSession() {
    if (auto existing = findChild<QDialog *>("acpSessionDialog")) {
        existing->raise();
        existing->activateWindow();
        return;
    }
    ensureMcpService();
    auto dialog = new AgentSessionDialog(this);
    dialog->setObjectName("acpSessionDialog");
    dialog->setWindowTitle("Agent Session");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(ui::PanelPadding, ui::PanelPadding, ui::PanelPadding, ui::PanelPadding);
    layout->setSpacing(ui::PanelPadding);
    auto agentSection = section(layout, "Agent");
    auto form = new QFormLayout;
    form->setHorizontalSpacing(ui::SpaceL);
    form->setVerticalSpacing(ui::SpaceM);
    dialog->agentPicker = new QComboBox(dialog);
    dialog->agentPicker->setObjectName("acpAgentPicker");
    for (const auto &choice : agentChoices)
        dialog->agentPicker->addItem(choice.name, choice.id);
    const auto detectAll = [dialog] {
        dialog->detected.clear();
        for (int i = 0; i < agentChoices.size(); ++i) {
            const auto &choice = agentChoices.at(i);
            const auto found = detect(choice);
            dialog->detected << found;
            dialog->agentPicker->setItemText(i, choice.name + (choice.commands.isEmpty() ? QString{} :
                !found.replaced.isEmpty() || !found.retired.isEmpty() ? " · Update needed"
                : found.path.isEmpty() ? " · Not found" : " · Detected"));
        }
    };
    detectAll();
    dialog->agentPicker->setCurrentIndex(qMax(0, dialog->agentPicker->findData(currentAgentId(preferences.acpAgent))));
    form->addRow("Agent type", dialog->agentPicker);
    dialog->program = ui::pathField({}, dialog);
    dialog->program->setObjectName("acpAgentPath");
    dialog->program->setPlaceholderText("Full path to a program or npm launcher");
    auto choose = new QPushButton("Choose…", dialog);
    choose->setObjectName("acpChooseAgent");
    auto programRow = new QHBoxLayout;
    programRow->setSpacing(ui::SpaceM);
    programRow->addWidget(dialog->program, 1);
    programRow->addWidget(choose);
    form->addRow("Agent program", programRow);
    dialog->arguments = new QLineEdit(preferences.acpArguments, dialog);
    dialog->arguments->setObjectName("acpAgentArguments");
    dialog->arguments->setPlaceholderText("Optional arguments, e.g. acp --profile \"My profile\"");
    form->addRow("Arguments", dialog->arguments);
    auto setupHint = ui::hint({}, dialog);
    setupHint->setTextFormat(Qt::PlainText);
    setupHint->setWordWrap(true);
    setupHint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    setupHint->setObjectName("acpSetupHint");
    auto copyInstall = new QPushButton("Copy install command", dialog);
    copyInstall->setObjectName("acpCopyInstall");
    auto recheck = new QPushButton("Check again", dialog);
    recheck->setObjectName("acpCheckAgain");
    recheck->setToolTip("Look for the agent again after installing or updating it");
    auto setupButtons = new QVBoxLayout;
    setupButtons->setSpacing(ui::SpaceM);
    setupButtons->addWidget(copyInstall);
    setupButtons->addWidget(recheck);
    setupButtons->addStretch();
    auto setupRow = new QHBoxLayout;
    setupRow->setSpacing(ui::SpaceM);
    setupRow->addWidget(setupHint, 1);
    setupRow->addLayout(setupButtons);
    form->addRow(setupRow);
    const auto choice = [dialog]() -> const AgentChoice & { return agentChoices.at(dialog->agentPicker->currentIndex()); };
    const auto found = [dialog] { return dialog->detected.at(dialog->agentPicker->currentIndex()); };
    const auto configure = [this, dialog, form, choice, found, programRow, setupRow, choose, setupHint, copyInstall] {
        const auto &selected = choice();
        const auto agent = found();
        const bool custom = selected.commands.isEmpty();
        dialog->program->setText(custom ? preferences.acpProgram : agent.path);
        dialog->program->setReadOnly(!custom);
        form->setRowVisible(programRow, custom || !agent.path.isEmpty());
        choose->setVisible(custom);
        form->setRowVisible(dialog->arguments, custom);
        form->setRowVisible(setupRow, !custom && (agent.path.isEmpty() || !agent.retired.isEmpty()));
        copyInstall->setText(agent.retired.isEmpty() ? "Copy install command" : "Copy update commands");
        setupHint->setText(setupText(selected, agent));
    };
    connect(dialog->agentPicker, &QComboBox::currentIndexChanged, dialog, [this, choice, configure] {
        preferences.acpAgent = choice().id;
        configure();
    });
    connect(dialog->program, &QLineEdit::textChanged, dialog, [this, choice](const QString &path) {
        if (choice().commands.isEmpty())
            preferences.acpProgram = path;
    });
    connect(dialog->arguments, &QLineEdit::textChanged, dialog, [this](const QString &args) {
        preferences.acpArguments = args;
    });
    connect(copyInstall, &QPushButton::clicked, dialog, [choice, found] {
        QApplication::clipboard()->setText(installCommand(choice(), found()));
    });
    connect(recheck, &QPushButton::clicked, dialog, [detectAll, configure] {
        detectAll();
        configure();
    });
    configure();
    agentSection->addLayout(form);
    auto sessionSection = section(layout, "Session");
    auto statusRow = new QHBoxLayout;
    statusRow->setSpacing(ui::SpaceL);
    dialog->status = new QLabel("Not running", dialog);
    dialog->status->setObjectName("acpStatus");
    dialog->status->setProperty("role", "statusPill");
    statusRow->addWidget(dialog->status, 0, Qt::AlignLeft | Qt::AlignTop);
    dialog->statusDetail = ui::hint({}, dialog);
    dialog->statusDetail->setObjectName("acpStatusDetail");
    dialog->statusDetail->setWordWrap(true);
    dialog->statusDetail->setTextFormat(Qt::PlainText);
    statusRow->addWidget(dialog->statusDetail, 1, Qt::AlignVCenter);
    // The agent's session mode and settings (such as its model), when it offers them.
    dialog->settingsRow = new QHBoxLayout;
    dialog->settingsRow->setSpacing(ui::SpaceL);
    statusRow->addLayout(dialog->settingsRow);
    sessionSection->addLayout(statusRow);
    dialog->access = ui::hint({}, dialog);
    dialog->access->setObjectName("acpAccess");
    dialog->access->setWordWrap(true);
    dialog->access->setTextFormat(Qt::PlainText);
    sessionSection->addWidget(dialog->access);
    dialog->authRow = new QWidget(dialog);
    auto authLayout = new QHBoxLayout(dialog->authRow);
    authLayout->setContentsMargins(0, 0, 0, 0);
    authLayout->setSpacing(ui::SpaceM);
    dialog->authMethod = new QComboBox(dialog->authRow);
    dialog->authMethod->setObjectName("acpAuthMethod");
    dialog->authMethod->setAccessibleName("Agent sign-in method");
    auto signIn = new QPushButton("Sign in", dialog->authRow);
    signIn->setObjectName("acpSignIn");
    authLayout->addWidget(dialog->authMethod, 1);
    authLayout->addWidget(signIn);
    sessionSection->addWidget(dialog->authRow);
    dialog->authRow->hide();
    connect(signIn, &QPushButton::clicked, dialog, [dialog] {
        if (dialog->session && dialog->session->authenticate(dialog->authMethod->currentData().toString())) {
            dialog->authRow->hide();
            dialog->tell("Waiting for the agent to finish signing in…");
        }
    });
    auto controls = new QHBoxLayout;
    controls->setSpacing(ui::SpaceM);
    dialog->start = new QPushButton("Start read-only…", dialog);
    dialog->start->setObjectName("acpStart");
    ui::setPrimary(dialog->start);
    dialog->edit = new QPushButton("Allow editing…", dialog);
    dialog->edit->setObjectName("acpEditing");
    dialog->revoke = new QPushButton("Revoke access", dialog);
    dialog->revoke->setObjectName("acpRevoke");
    dialog->stop = new QPushButton("Stop session", dialog);
    dialog->stop->setObjectName("acpStop");
    for (auto button : {dialog->start, dialog->stop, dialog->edit, dialog->revoke}) {
        button->setAutoDefault(false);
        controls->addWidget(button);
    }
    controls->addStretch();
    sessionSection->addLayout(controls);
    auto projectHint = ui::hint("Open a resource pack to start a session.", dialog);
    projectHint->setObjectName("acpProjectHint");
    sessionSection->addWidget(projectHint);

    auto conversationSection = section(layout, "Conversation", 1);
    auto transcript = new QStackedWidget(dialog);
    transcript->setMinimumHeight(8 * ui::ControlHeight);
    auto empty = ui::emptyState("Messages will appear here once you send a prompt.", transcript, "chat");
    dialog->conversation = new AgentConversation(dialog);
    dialog->conversation->setObjectName("acpConversation");
    transcript->addWidget(empty);
    transcript->addWidget(dialog->conversation);
    conversationSection->addWidget(transcript, 1);
    dialog->notice = new QLabel(dialog);
    dialog->notice->setObjectName("acpNotice");
    dialog->notice->setWordWrap(true);
    dialog->notice->setTextFormat(Qt::PlainText);
    dialog->notice->setProperty("role", "notice");
    dialog->notice->hide();
    sessionSection->addWidget(dialog->notice);
    auto outputToggle = new QToolButton(dialog);
    outputToggle->setObjectName("acpOutputToggle");
    outputToggle->setProperty("role", "disclosure");
    outputToggle->setCheckable(true);
    outputToggle->setFocusPolicy(Qt::StrongFocus);
    outputToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    outputToggle->setArrowType(Qt::RightArrow);
    outputToggle->setText("Show agent output");
    sessionSection->addWidget(outputToggle, 0, Qt::AlignLeft);
    auto output = new QPlainTextEdit(dialog);
    output->setObjectName("acpAgentOutput");
    output->setAccessibleName("Agent standard error");
    output->setPlaceholderText("The agent has not written any output.");
    output->setReadOnly(true);
    output->setFixedHeight(5 * output->fontMetrics().lineSpacing() + 2 * ui::SpaceL);
    output->hide();
    sessionSection->addWidget(output);
    const auto refreshOutput = [dialog, output, outputToggle] {
        if (!outputToggle->isChecked() || output->textCursor().hasSelection())
            return;
        const auto text = dialog->session ? agentDisplayText(dialog->session->agentOutput(), acp::Session::OutputLimit)
                                          : QString{};
        if (output->toPlainText() != text) {
            output->setPlainText(text);
            output->moveCursor(QTextCursor::End);
        }
    };
    // A chatty agent writes stderr in many small pieces; the panel follows at most five times a second.
    auto outputTimer = new QTimer(dialog);
    outputTimer->setSingleShot(true);
    outputTimer->setInterval(200);
    connect(outputTimer, &QTimer::timeout, output, refreshOutput);
    const auto outputChanged = [outputTimer, outputToggle] {
        if (outputToggle->isChecked() && !outputTimer->isActive())
            outputTimer->start();
    };
    connect(outputToggle, &QToolButton::toggled, output, [outputToggle, output, refreshOutput](bool show) {
        outputToggle->setText(show ? "Hide agent output" : "Show agent output");
        outputToggle->setArrowType(show ? Qt::DownArrow : Qt::RightArrow);
        output->setVisible(show);
        if (show)
            refreshOutput();
    });
    connect(output, &QPlainTextEdit::selectionChanged, output, refreshOutput);

    dialog->permission = new PermissionCard(dialog);
    conversationSection->addWidget(dialog->permission);
    dialog->permission->answer = [dialog](const QJsonValue &id, const QString &option) {
        if (dialog->session)
            dialog->session->answerPermission(id, option);
    };

    // Rebuilds the mode and setting pickers from what the agent last reported. A picker shows the
    // agent's current value; choosing another asks the agent, which confirms or refuses.
    const auto showSettings = [dialog] {
        const auto session = dialog->session;
        struct Entry { QString key, name, description, current; bool mode; QList<QStringList> values; };
        QList<Entry> entries; // values: {id, name, description, group}
        bool configMode = false;
        if (session) {
            for (const auto &value : session->configOptions()) {
                const auto option = value.toObject();
                if (option["type"] != "select" || option["id"].toString().isEmpty())
                    continue; // boolean options need a capability Nova does not advertise
                Entry entry{option["id"].toString(), agentDisplayText(option["name"].toString(), 40).simplified(),
                            agentDisplayText(option["description"].toString(), 200).simplified(),
                            option["currentValue"].toString(), option["category"] == "mode", {}};
                configMode |= entry.mode;
                // Approval and sandbox settings get the same confirmation as modes.
                static const QRegularExpression approval(R"(mode|approv|permission|sandbox)",
                                                         QRegularExpression::CaseInsensitiveOption);
                entry.mode |= approval.match(entry.key).hasMatch();
                for (const auto &item : option["options"].toArray()) {
                    const auto choice = item.toObject();
                    const auto group = agentDisplayText(choice["name"].toString(), 40).simplified();
                    const auto members = choice.contains("group") ? choice["options"].toArray() : QJsonArray{choice};
                    for (qsizetype i = 0; i < qMin<qsizetype>(members.size(), 64); ++i) {
                        const auto member = members.at(i).toObject();
                        entry.values.append({member["value"].toString(),
                                             agentDisplayText(member["name"].toString(), 60).simplified(),
                                             agentDisplayText(member["description"].toString(), 200).simplified(),
                                             choice.contains("group") ? group : QString{}});
                    }
                }
                if (!entry.values.isEmpty())
                    entries.append(entry);
            }
            // Agents that offer a "mode" setting supersede the older session modes with it.
            const auto modes = session->modes()["availableModes"].toArray();
            if (!configMode && !modes.isEmpty()) {
                Entry entry{"mode", "Mode", {}, session->modes()["currentModeId"].toString(), true, {}};
                for (const auto &item : modes) {
                    const auto mode = item.toObject();
                    entry.values.append({mode["id"].toString(), agentDisplayText(mode["name"].toString(), 60).simplified(),
                                         agentDisplayText(mode["description"].toString(), 200).simplified(), {}});
                }
                entries.prepend(entry);
            }
        }
        QStringList keys;
        for (const auto &entry : entries)
            keys << entry.key;
        for (const auto &key : dialog->settings.keys()) {
            if (keys.contains(key) && dialog->settings.value(key))
                continue;
            if (const auto box = dialog->settings.take(key)) {
                box->hide();
                box->deleteLater(); // its picker may be answering a click
            }
        }
        const auto live = session && (session->state() == acp::Session::State::Ready ||
                                      session->state() == acp::Session::State::Prompting);
        for (const auto &entry : entries) {
            auto &box = dialog->settings[entry.key];
            QComboBox *picker;
            if (!box) {
                box = new QWidget(dialog);
                auto row = new QHBoxLayout(box);
                row->setContentsMargins(0, 0, 0, 0);
                row->setSpacing(ui::SpaceM);
                auto label = ui::hint({}, box);
                label->setTextFormat(Qt::PlainText);
                row->addWidget(label);
                picker = new QComboBox(box);
                picker->setObjectName(entry.key == "mode" ? QString("acpMode") : "acpSetting-" + entry.key);
                picker->setSizeAdjustPolicy(QComboBox::AdjustToContents);
                picker->setMaximumWidth(8 * ui::ControlHeight);
                row->addWidget(picker);
                const bool sessionMode = entry.key == "mode";
                const auto key = entry.key;
                connect(picker, &QComboBox::activated, dialog, [dialog, picker = QPointer<QComboBox>(picker), key,
                                                                 sessionMode](int index) {
                    if (!picker || !dialog->session)
                        return;
                    const auto id = picker->itemData(index).toString();
                    const auto name = picker->itemText(index);
                    const bool risky = picker->property("modeSetting").toBool() && permissive(id, name);
                    // Consent like starting a session: it names the mode and what changes, and defaults to Cancel.
                    if (risky && QMessageBox::question(dialog, "Let the agent act without asking?",
                            "In “" + name + "” the agent's own tools may change files and run commands on this PC "
                            "under your Windows account without asking you first.\n\nNova's access does not change: "
                            "it stays " + QString(dialog->editing ? "editing" : "read-only") + " for project “" +
                            QFileInfo(dialog->sessionProject).fileName() + "”.",
                            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
                        if (picker)
                            picker->setCurrentIndex(picker->findData(picker->property("agentValue")));
                        return;
                    }
                    const bool sent = dialog->session &&
                        (sessionMode ? dialog->session->setMode(id) : dialog->session->setConfigOption(key, id));
                    if (!sent && picker)
                        picker->setCurrentIndex(picker->findData(picker->property("agentValue")));
                });
            } else {
                picker = box->findChild<QComboBox *>();
            }
            box->findChild<QLabel *>()->setText(entry.name.isEmpty() ? "Setting" : entry.name);
            picker->setAccessibleName("Agent " + (entry.name.isEmpty() ? QString("setting") : entry.name.toLower()));
            picker->setToolTip(entry.description);
            picker->setProperty("modeSetting", entry.mode);
            picker->setProperty("agentValue", entry.current);
            const QSignalBlocker quiet(picker);
            picker->clear();
            QString group;
            for (const auto &value : entry.values) {
                if (value.at(3) != group && picker->count())
                    picker->insertSeparator(picker->count());
                group = value.at(3);
                picker->addItem(value.at(1).isEmpty() ? agentDisplayText(value.at(0), 60) : value.at(1), value.at(0));
                picker->setItemData(picker->count() - 1, value.at(2), Qt::ToolTipRole);
            }
            picker->setCurrentIndex(picker->findData(entry.current));
            picker->setEnabled(live);
            dialog->settingsRow->removeWidget(box);
            dialog->settingsRow->addWidget(box);
            box->show();
        }
    };

    auto promptRow = new QHBoxLayout;
    promptRow->setSpacing(ui::SpaceL);
    dialog->prompt = new Composer(dialog);
    dialog->prompt->setObjectName("acpPrompt");
    dialog->prompt->setPlaceholderText("Message to the agent");
    dialog->send = new QPushButton("Send", dialog);
    dialog->send->setObjectName("acpSend");
    dialog->cancel = new QPushButton("Cancel turn", dialog);
    dialog->cancel->setObjectName("acpCancel");
    promptRow->addWidget(dialog->prompt, 1);
    auto turnControls = new QVBoxLayout;
    turnControls->setSpacing(ui::SpaceM);
    for (auto button : {dialog->send, dialog->cancel}) {
        button->setAutoDefault(false);
        turnControls->addWidget(button);
    }
    turnControls->addStretch();
    promptRow->addLayout(turnControls);
    conversationSection->addLayout(promptRow);
    auto composerHints = new QHBoxLayout;
    composerHints->setSpacing(ui::SpaceL);
    composerHints->addWidget(ui::hint("Enter to send · Shift+Enter for a new line · Up for history · / for commands",
                                      dialog), 1);
    // Names the open asset's pack path in the prompt, so the agent can look it up with Nova's tools.
    dialog->mentionAsset = new QCheckBox("Mention the open asset", dialog);
    dialog->mentionAsset->setObjectName("acpMentionAsset");
    composerHints->addWidget(dialog->mentionAsset);
    conversationSection->addLayout(composerHints);
    auto note = ui::hint("The agent runs under your Windows account, in an empty folder outside the pack. "
                         "Its own file and shell tools follow its configuration. Nova manages only Nova tools.", dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    dialog->resize(760, 800);

    auto ours = [this, dialog] {
        return mcpService && dialog->grant && mcpService->grantSerial() == dialog->grant &&
               !mcpService->authorizedProject().isEmpty();
    };
    // The open document's path inside the session's pack, or empty.
    const auto openAsset = [this] {
        const auto path = tabPath(tabs->currentWidget());
        if (path.isEmpty() || project.isEmpty())
            return QString{};
        const auto relative = QDir::fromNativeSeparators(QDir(project).relativeFilePath(path));
        return relative.startsWith("../") || QDir::isAbsolutePath(relative) ? QString{} : relative;
    };
    auto refresh = [this, dialog, ours, choose, recheck, projectHint, outputToggle, form, choice, openAsset] {
        const auto state = dialog->session ? dialog->session->state() : acp::Session::State::Idle;
        const bool live = state == acp::Session::State::Starting || state == acp::Session::State::Ready ||
                          state == acp::Session::State::Prompting;
        static const QMap<acp::Session::State, QString> names{
            {acp::Session::State::Idle, "Not running"}, {acp::Session::State::Starting, "Starting…"},
            {acp::Session::State::Ready, "Connected"}, {acp::Session::State::Prompting, "Working…"},
            {acp::Session::State::Stopping, "Stopping…"}, {acp::Session::State::Ended, "Ended"}};
        dialog->status->setText(names.value(state));
        appearance(dialog->status, "severity",
                   state == acp::Session::State::Ready ? "success" :
                   state == acp::Session::State::Starting || state == acp::Session::State::Prompting ? "info" : "neutral");
        QStringList facts; // agent · conversation title · context use and cost
        if (dialog->session) {
            facts << agentDisplayText(dialog->session->agentName(), 120)
                  << agentDisplayText(dialog->session->title(), 120).simplified() << usageText(dialog->session->usage());
            facts.removeAll(QString{});
        }
        auto detail = facts.join(" · ");
        if (state == acp::Session::State::Ended && dialog->session)
            detail = agentDisplayText(dialog->session->endReason(), 600);
        dialog->statusDetail->setText(detail);
        dialog->statusDetail->setVisible(!detail.isEmpty());
        if (ours()) {
            const auto connected = mcpService->clientConnected();
            dialog->access->setText(QString("%1 for project “%2” · %3")
                .arg(dialog->editing ? "Editing allowed" : "Read-only", QFileInfo(dialog->sessionProject).fileName(),
                     connected ? "the agent's Nova bridge is connected" : "waiting for the agent to start Nova's bridge"));
        } else {
            dialog->access->setText(live && dialog->grant ? "Revoked · the agent can no longer use Nova's tools"
                                                          : "No access");
        }
        const bool canStart = (state == acp::Session::State::Idle || state == acp::Session::State::Ended) &&
                              !project.isEmpty() && !dialog->program->text().trimmed().isEmpty();
        dialog->start->setEnabled(canStart);
        projectHint->setVisible(project.isEmpty());
        const bool configuring = state == acp::Session::State::Idle || state == acp::Session::State::Ended;
        dialog->agentPicker->setEnabled(configuring);
        dialog->program->setEnabled(configuring);
        dialog->arguments->setEnabled(configuring);
        form->setRowVisible(dialog->arguments, configuring && choice().commands.isEmpty());
        choose->setEnabled(configuring);
        recheck->setEnabled(configuring);
        dialog->edit->setEnabled(live && ours() && !dialog->editing && mcpService->clientConnected());
        dialog->revoke->setEnabled(ours());
        dialog->stop->setEnabled(live);
        dialog->send->setEnabled(state == acp::Session::State::Ready && !dialog->prompt->toPlainText().trimmed().isEmpty());
        dialog->prompt->setEnabled(state == acp::Session::State::Ready || state == acp::Session::State::Prompting);
        dialog->cancel->setEnabled(state == acp::Session::State::Prompting);
        outputToggle->setVisible(bool(dialog->session));
        const auto asset = openAsset();
        dialog->mentionAsset->setEnabled(!asset.isEmpty());
        dialog->mentionAsset->setToolTip(asset.isEmpty() ? "Open an asset from this pack to mention it."
                                                         : "Your next message names " + asset + " for the agent.");
    };
    // Ends Nova's grant when the session that owned it ends; never touches another grant.
    auto release = [this, dialog, ours] {
        if (ours())
            mcpService->revoke();
        dialog->grant = 0;
        if (!dialog->workFolder.isEmpty() && !dialog->keepFolder)
            QDir().rmdir(dialog->workFolder); // only if the agent left it empty
    };
    // resume: the session to continue with session/load, in the same folder, when the agent supports it.
    auto begin = [this, dialog, refresh, release, choice, transcript, output, outputChanged, showSettings](
                     bool editing, const acp::AgentProgram &program, const QString &resume) {
        QString problem;
        if (!acp::acceptableExecutable(program.executable, &problem))
            return dialog->tell(problem, "error");
        const auto bridge = bridgePath();
        if (!QFileInfo(bridge).isFile())
            return dialog->tell("nova-mcp.exe is missing. Repair or reinstall Nova Studio.", "error");
        if (mcpService->status() != "Disabled")
            return dialog->tell("Another MCP connection is offered or active. Revoke it in MCP Integration first.", "warning");
        if (dialog->session)
            dialog->session->deleteLater();
        const auto selected = project;
        dialog->sessionProject = selected;
        dialog->editing = editing;
        dialog->activeProgram = program;
        // A continued session keeps its folder: session/load names the same working directory.
        if (resume.isEmpty() || dialog->workFolder.isEmpty())
            dialog->workFolder = QDir::tempPath() + "/nova-agent-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        dialog->keepFolder = false;
        if (!QDir().mkpath(dialog->workFolder))
            return dialog->tell("The agent’s working folder could not be created.", "error");
        if (!mcpService->authorize(selected, editing)) {
            release();
            return dialog->tell(mcpService->error(), "error");
        }
        dialog->grant = mcpService->grantSerial();
        auto session = new acp::Session(dialog);
        dialog->session = session;
        output->clear();
        connect(session, &acp::Session::agentOutputChanged, output, outputChanged);
        connect(session, &acp::Session::modesChanged, dialog, showSettings);
        connect(session, &acp::Session::configOptionsChanged, dialog, showSettings);
        connect(session, &acp::Session::detailsChanged, dialog, refresh);
        connect(session, &acp::Session::commandsChanged, dialog, [dialog, session] {
            dialog->prompt->setCommands(session->commands());
        });
        dialog->prompt->setCommands({});
        showSettings(); // the previous agent's pickers go
        // Only a bridge started by this agent (or its children) may take the grant.
        mcpService->setClientFilter([session = QPointer<acp::Session>(session)](qint64 pid) {
            return session && session->ownsProcess(pid);
        });
        connect(session, &acp::Session::stateChanged, dialog, showSettings); // pickers follow Ready and Ended
        connect(session, &acp::Session::stateChanged, dialog, [dialog, session, refresh, release, connected = false]() mutable {
            if (session->state() == acp::Session::State::Stopping)
                dialog->permission->clear();
            if (session->state() == acp::Session::State::Ended) {
                dialog->permission->clear();
                dialog->conversation->finishTurn();
                dialog->authRow->hide();
                release();
            } else if (session->state() == acp::Session::State::Ready && dialog->authMethod->count()) {
                dialog->authMethod->clear();
                dialog->authRow->hide();
                dialog->tell({});
            }
            refresh();
            if (session->state() == acp::Session::State::Ready && !connected) {
                connected = true;
                dialog->prompt->setFocus(Qt::OtherFocusReason);
            }
        });
        connect(session, &acp::Session::authenticationRequired, dialog, [dialog, choice](const QJsonArray &methods) {
            dialog->authMethod->clear();
            for (const auto &value : methods) {
                const auto method = value.toObject();
                const auto id = method["id"].toString();
                dialog->authMethod->addItem(agentDisplayText(method["name"].toString(id), 120), id);
                dialog->authMethod->setItemData(dialog->authMethod->count() - 1,
                                               agentDisplayText(method["description"].toString(), 300), Qt::ToolTipRole);
            }
            dialog->authRow->setVisible(!methods.isEmpty());
            const auto login = choice().signIn;
            dialog->tell(!methods.isEmpty() ? "Choose a sign-in method to continue." : login.isEmpty()
                ? "Sign in using your agent’s own tool, then start a new session."
                : "Run " + login + " in a terminal to sign in, then start a new session.");
        });
        connect(session, &acp::Session::agentText, dialog,
                [dialog, transcript](const QString &text) {
                    transcript->setCurrentWidget(dialog->conversation);
                    dialog->conversation->add(AgentConversation::Speaker::Agent, text);
                });
        connect(session, &acp::Session::agentThought, dialog,
                [dialog, transcript](const QString &text) {
                    transcript->setCurrentWidget(dialog->conversation);
                    dialog->conversation->add(AgentConversation::Speaker::Thinking, text);
                });
        connect(session, &acp::Session::agentContent, dialog,
                [dialog, transcript](bool thought, const QJsonObject &content) {
                    transcript->setCurrentWidget(dialog->conversation);
                    dialog->conversation->addContent(thought ? AgentConversation::Speaker::Thinking
                                                             : AgentConversation::Speaker::Agent, content);
                });
        connect(session, &acp::Session::toolCall, dialog, [dialog, transcript](const QJsonObject &update) {
            transcript->setCurrentWidget(dialog->conversation);
            dialog->conversation->showTool(update);
        });
        connect(session, &acp::Session::planChanged, dialog, [dialog, transcript](const QJsonArray &entries) {
            transcript->setCurrentWidget(dialog->conversation);
            dialog->conversation->showPlan(entries);
        });
        // Only how a turn ended unusually is worth a line; a normal reply speaks for itself.
        connect(session, &acp::Session::turnEnded, dialog, [dialog](const QString &reason) {
            dialog->permission->clear();
            dialog->conversation->finishTurn();
            static const QMap<QString, QString> told{
                {"end_turn", ""}, {"cancelled", "You cancelled the agent's turn."},
                {"max_tokens", "The agent stopped: it reached its output limit."},
                {"max_turn_requests", "The agent stopped: it reached its limit of steps for one message."},
                {"refusal", "The agent declined to continue."}};
            if (reason.startsWith("error: "))
                dialog->tell("Agent error: " + reason.mid(7), "error");
            else
                dialog->tell(told.value(reason, "The agent ended the turn: " + reason), "info");
        });
        connect(session, &acp::Session::problem, dialog, [dialog](const QString &message) { dialog->tell(message, "warning"); });
        connect(session, &acp::Session::permissionRequested, dialog->permission, &PermissionCard::enqueue);
        connect(session, &acp::Session::permissionWithdrawn, dialog->permission, &PermissionCard::withdraw);
        dialog->tell({});
        dialog->authMethod->clear();
        if (!session->start(program, dialog->workFolder, QJsonArray{acp::stdioServer("nova", bridge)},
                            acp::agentEnvironment(), &problem, resume)) {
            dialog->tell("The session could not start: " + problem, "error");
            release();
        }
        refresh();
    };
    connect(dialog->program, &QLineEdit::textChanged, dialog, refresh);
    connect(dialog->agentPicker, &QComboBox::currentIndexChanged, dialog, refresh);
    connect(choose, &QPushButton::clicked, dialog, [dialog] {
        const auto file = QFileDialog::getOpenFileName(dialog, "Choose the agent program", {}, "Programs (*.exe *.cmd)");
        if (!file.isEmpty())
            dialog->program->setText(QDir::toNativeSeparators(file));
    });
    connect(dialog->start, &QPushButton::clicked, dialog, [this, dialog, begin, choice] {
        const auto executable = QDir::toNativeSeparators(dialog->program->text().trimmed());
        const auto &selected = choice();
        const auto args = selected.commands.isEmpty() ? QProcess::splitCommand(dialog->arguments->text()) : selected.arguments;
        QString problem;
        const auto program = acp::resolveAgentProgram(executable, args, &problem);
        if (program.executable.isEmpty())
            return dialog->tell(problem, "error");
        const auto launcher = executable == program.executable ? executable :
            executable + "\n" + program.executable + " → " + program.arguments.value(0);
        // Consent names the project, the program and the access, and defaults to Cancel.
        if (QMessageBox::question(dialog, "Start agent session?",
                "Nova Studio will start this agent program:\n" + launcher +
                "\n\nIt gets read-only access to Nova's tools for project “" + QFileInfo(project).fileName() +
                "”: it can list, inspect and validate assets, read guides and preview changes. It cannot create, "
                "edit or save anything through Nova.\n\nAccess ends when you stop the session, revoke access, "
                "switch projects, or the agent exits.",
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
            begin(false, program, {});
    });
    connect(dialog->edit, &QPushButton::clicked, dialog, [this, dialog, begin] {
        // An agent that can load sessions continues the conversation; otherwise it starts afresh.
        const bool continues = dialog->session && dialog->session->canLoadSession();
        if (QMessageBox::question(dialog, "Allow agent editing?",
                QString(continues ? "Nova will restart the agent with editing access and continue this conversation. "
                                  : "Nova will end this read-only session and start a new one, which does not remember "
                                    "this conversation. ") +
                "The agent can then create unsaved textures and animations, modify open documents, remove layers, "
                "frames and model elements, and save open documents into the pack’s files on disk.\n\nProject: " +
                QFileInfo(dialog->sessionProject).fileName(),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes ||
            project != dialog->sessionProject || !dialog->session)
            return;
        auto old = dialog->session;
        const auto authorizedProject = dialog->sessionProject;
        const auto resume = old->canLoadSession() ? old->sessionId() : QString{};
        dialog->keepFolder = !resume.isEmpty();
        const auto restart = [this, dialog, old, begin, authorizedProject, resume, program = dialog->activeProgram] {
            if (old && dialog->session == old && old->state() == acp::Session::State::Ended &&
                project == authorizedProject)
                begin(true, program, resume);
        };
        old->stop("Restarted with editing access.");
        // The old agent's tree is ended before the editing grant is offered.
        QTimer::singleShot(0, dialog, [dialog, old, restart] {
            if (old && old->state() != acp::Session::State::Ended)
                return void(QObject::connect(old, &acp::Session::stateChanged, dialog, restart,
                                             Qt::SingleShotConnection));
            restart();
        });
    });
    connect(dialog->revoke, &QPushButton::clicked, dialog, [this, dialog, ours, refresh] {
        if (ours())
            mcpService->revoke();
        refresh(); // the Nova tools line now says the access is revoked
    });
    connect(dialog->stop, &QPushButton::clicked, dialog, [dialog] {
        if (dialog->session)
            dialog->session->stop("Stopped by you.");
    });
    connect(dialog->cancel, &QPushButton::clicked, dialog, [dialog] {
        if (dialog->session)
            dialog->session->cancel();
        dialog->permission->clear();
    });
    auto submit = [this, dialog, transcript, openAsset] {
        const auto text = dialog->prompt->toPlainText().trimmed();
        // A text block every agent accepts; the agent resolves the path with Nova's own tools.
        const auto asset = dialog->mentionAsset->isChecked() ? openAsset() : QString{};
        const auto mention = asset.isEmpty() ? QString{} :
            "The asset open in Nova Studio is " + asset + " in project “" + QFileInfo(project).fileName() +
            "”. Use Nova's tools to inspect it.";
        if (dialog->session &&
            dialog->session->prompt(text, mention.isEmpty() ? QJsonArray{} : QJsonArray{QJsonObject{{"type", "text"},
                                                                                                    {"text", mention}}})) {
            transcript->setCurrentWidget(dialog->conversation);
            dialog->conversation->add(AgentConversation::Speaker::You, asset.isEmpty() ? text : text + "\n\nOpen asset: " + asset);
            dialog->tell({});
            dialog->prompt->remember(text);
        }
    };
    connect(dialog->send, &QPushButton::clicked, dialog, submit);
    // Typing changes only whether Send is available.
    connect(dialog->prompt, &QPlainTextEdit::textChanged, dialog, [dialog] {
        dialog->send->setEnabled(dialog->session && dialog->session->state() == acp::Session::State::Ready &&
                                 !dialog->prompt->toPlainText().trimmed().isEmpty());
    });
    dialog->prompt->submit = submit;
    connect(mcpService, &mcp::Service::changed, dialog, refresh);
    connect(this, &QWidget::windowTitleChanged, dialog, refresh);
    connect(tabs, &QTabWidget::currentChanged, dialog, refresh);
    // Closing the window ends the agent and the grant it held.
    connect(dialog, &QDialog::finished, dialog, [this, dialog, release] {
        if (dialog->session)
            dialog->session->stop("The session window was closed.");
        dialog->keepFolder = false;
        release();
        settings.saveAgentSetup(preferences);
    });
    refresh();
    dialog->show();
}

void Window::endAgentSessionForProjectChange() {
    if (auto dialog = static_cast<AgentSessionDialog *>(findChild<QDialog *>("acpSessionDialog")); dialog && dialog->session)
        dialog->session->stop("The project changed.");
}
} // namespace nova
