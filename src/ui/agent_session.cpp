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
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
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
#include <QStackedWidget>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace nova {
namespace clients = mcp::clients;
namespace {
struct AgentChoice {
    QString id, name, command;
    QStringList arguments;
    QString install, signIn;
};
const QList<AgentChoice> agentChoices{
    {"opencode", "OpenCode", "opencode", {"acp"}, "npm install -g opencode-ai", "opencode auth login"},
    {"claude", "Claude Code", "claude-code-acp", {}, "npm install -g @zed-industries/claude-code-acp", "claude, then /login"},
    {"codex", "Codex", "codex-acp", {}, "npm install -g @zed-industries/codex-acp", "codex login"},
    {"gemini", "Gemini CLI", "gemini", {"--experimental-acp"}, "npm install -g @google/gemini-cli", "gemini"},
    {"custom", "Custom", {}, {}, {}, {}},
};
QString bridgePath() {
    const auto configured = qApp->property("mcpBridgePath").toString(); // tests supply the build's bridge
    return configured.isEmpty() ? clients::locateBridge(QCoreApplication::applicationDirPath()).path : configured;
}

class Composer final : public QPlainTextEdit {
  public:
    explicit Composer(QWidget *parent) : QPlainTextEdit(parent) {
        setTabChangesFocus(true);
        setAccessibleName("Message to the agent");
        setAccessibleDescription("Enter sends. Shift+Enter inserts a line break. Up recalls earlier messages.");
        setFixedHeight(3 * fontMetrics().lineSpacing() + 2 * ui::SpaceL);
    }
    std::function<void()> submit;
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
    QStringList history;
    QString draft;
    int position = -1;
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

class AgentSessionDialog : public QDialog {
  public:
    QPointer<acp::Session> session;
    QString sessionProject, workFolder;
    quint64 grant = 0;
    bool editing = false;
    QLabel *status, *statusDetail, *access, *notice, *permissionText;
    QLineEdit *program, *arguments;
    Composer *prompt;
    QComboBox *agentPicker, *authMethod;
    QWidget *authRow;
    acp::AgentProgram activeProgram;
    AgentConversation *conversation;
    QPushButton *start, *edit, *revoke, *send, *cancel, *stop, *allow, *reject;
    QJsonValue permissionId = QJsonValue::Undefined; // undefined: no request pending (null is a valid id)
    QString allowOption, rejectOption;
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
    auto experimental = new QLabel("Experimental", dialog);
    experimental->setObjectName("acpExperimental");
    experimental->setProperty("role", "badge");
    auto form = new QFormLayout;
    form->setHorizontalSpacing(ui::SpaceL);
    form->setVerticalSpacing(ui::SpaceM);
    dialog->agentPicker = new QComboBox(dialog);
    dialog->agentPicker->setObjectName("acpAgentPicker");
    QStringList detected;
    for (const auto &choice : agentChoices) {
        const auto path = choice.command.isEmpty() ? QString{} : acp::findAgentExecutable(choice.command);
        detected << path;
        dialog->agentPicker->addItem(choice.name + (choice.command.isEmpty() ? QString{} :
                                      path.isEmpty() ? " · Not found" : " · Detected"), choice.id);
    }
    dialog->agentPicker->setCurrentIndex(qMax(0, dialog->agentPicker->findData(preferences.acpAgent)));
    auto pickerRow = new QHBoxLayout;
    pickerRow->setSpacing(ui::SpaceL);
    pickerRow->addWidget(dialog->agentPicker, 1);
    pickerRow->addWidget(experimental);
    form->addRow("Agent type", pickerRow);
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
    auto copyInstall = new QPushButton("Copy install command", dialog);
    copyInstall->setObjectName("acpCopyInstall");
    auto setupRow = new QHBoxLayout;
    setupRow->setSpacing(ui::SpaceM);
    setupRow->addWidget(setupHint, 1);
    setupRow->addWidget(copyInstall, 0, Qt::AlignTop);
    form->addRow(setupRow);
    const auto choice = [dialog] { return agentChoices.at(dialog->agentPicker->currentIndex()); };
    const auto configure = [this, dialog, form, choice, detected, programRow, setupRow, choose, setupHint] {
        const auto selected = choice();
        const auto path = detected.at(dialog->agentPicker->currentIndex());
        const bool custom = selected.command.isEmpty();
        dialog->program->setText(custom ? preferences.acpProgram : path);
        dialog->program->setReadOnly(!custom);
        form->setRowVisible(programRow, custom || !path.isEmpty());
        choose->setVisible(custom);
        form->setRowVisible(dialog->arguments, custom);
        form->setRowVisible(setupRow, path.isEmpty() && !custom);
        setupHint->setText(QString("Requires Node.js and npm. Install with %1. To sign in, run %2.")
                               .arg(selected.install, selected.signIn));
    };
    connect(dialog->agentPicker, &QComboBox::currentIndexChanged, dialog, [this, choice, configure] {
        preferences.acpAgent = choice().id;
        configure();
    });
    connect(dialog->program, &QLineEdit::textChanged, dialog, [this, choice](const QString &path) {
        if (choice().command.isEmpty())
            preferences.acpProgram = path;
    });
    connect(dialog->arguments, &QLineEdit::textChanged, dialog, [this](const QString &args) {
        preferences.acpArguments = args;
    });
    connect(copyInstall, &QPushButton::clicked, dialog, [choice] { QApplication::clipboard()->setText(choice().install); });
    configure();
    agentSection->addLayout(form);
    auto sessionSection = section(layout, "Session");
    auto statusRow = new QHBoxLayout;
    statusRow->setSpacing(ui::SpaceL);
    dialog->status = new QLabel("Not running", dialog);
    dialog->status->setObjectName("acpStatus");
    dialog->status->setProperty("role", "statusPill");
    statusRow->addWidget(dialog->status, 0, Qt::AlignTop);
    dialog->statusDetail = ui::hint({}, dialog);
    dialog->statusDetail->setObjectName("acpStatusDetail");
    dialog->statusDetail->setWordWrap(true);
    dialog->statusDetail->setTextFormat(Qt::PlainText);
    statusRow->addWidget(dialog->statusDetail, 1, Qt::AlignVCenter);
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
    transcript->setMinimumHeight(140);
    auto empty = ui::emptyState("Messages will appear here once you send a prompt.", transcript);
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

    dialog->permissionText = new QLabel(dialog);
    dialog->permissionText->setObjectName("acpPermission");
    dialog->permissionText->setWordWrap(true);
    dialog->permissionText->setTextFormat(Qt::PlainText);
    dialog->allow = new QPushButton("Allow once", dialog);
    dialog->allow->setObjectName("acpPermissionAllow");
    dialog->reject = new QPushButton("Reject", dialog);
    dialog->reject->setObjectName("acpPermissionReject");
    auto permissionRow = new QHBoxLayout;
    permissionRow->setSpacing(ui::SpaceM);
    permissionRow->addWidget(dialog->permissionText, 1);
    permissionRow->addWidget(dialog->allow);
    permissionRow->addWidget(dialog->reject);
    conversationSection->addLayout(permissionRow);

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
    conversationSection->addWidget(ui::hint("Enter to send · Shift+Enter for a new line · Up for history", dialog));
    auto note = ui::hint("The agent runs under your Windows account, in an empty folder outside the pack. "
                         "Its own file and shell tools follow its configuration. Nova manages only Nova tools.", dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    dialog->resize(760, 740);

    auto ours = [this, dialog] {
        return mcpService && dialog->grant && mcpService->grantSerial() == dialog->grant &&
               !mcpService->authorizedProject().isEmpty();
    };
    auto refresh = [this, dialog, ours, choose, projectHint] {
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
        QString detail;
        if (dialog->session && !dialog->session->agentName().isEmpty())
            detail = agentDisplayText(dialog->session->agentName(), 120);
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
        choose->setEnabled(configuring);
        dialog->edit->setEnabled(live && ours() && !dialog->editing && mcpService->clientConnected());
        dialog->revoke->setEnabled(ours());
        dialog->stop->setEnabled(live);
        dialog->send->setEnabled(state == acp::Session::State::Ready && !dialog->prompt->toPlainText().trimmed().isEmpty());
        dialog->prompt->setEnabled(state == acp::Session::State::Ready || state == acp::Session::State::Prompting);
        dialog->cancel->setEnabled(state == acp::Session::State::Prompting);
        const bool asking = !dialog->permissionId.isUndefined();
        dialog->permissionText->setVisible(asking);
        dialog->allow->setVisible(asking && !dialog->allowOption.isEmpty());
        dialog->reject->setVisible(asking);
    };
    // Ends Nova's grant when the session that owned it ends; never touches another grant.
    auto release = [this, dialog, ours] {
        if (ours())
            mcpService->revoke();
        dialog->grant = 0;
        if (!dialog->workFolder.isEmpty())
            QDir().rmdir(dialog->workFolder); // only if the agent left it empty
    };
    auto begin = [this, dialog, refresh, release, choice, transcript](bool editing, const acp::AgentProgram &program) {
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
        dialog->workFolder = QDir::tempPath() + "/nova-agent-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QDir().mkpath(dialog->workFolder))
            return dialog->tell("The agent’s working folder could not be created.", "error");
        if (!mcpService->authorize(selected, editing)) {
            release();
            return dialog->tell(mcpService->error(), "error");
        }
        dialog->grant = mcpService->grantSerial();
        auto session = new acp::Session(dialog);
        dialog->session = session;
        // Only a bridge started by this agent (or its children) may take the grant.
        mcpService->setClientFilter([session = QPointer<acp::Session>(session)](qint64 pid) {
            return session && session->ownsProcess(pid);
        });
        connect(session, &acp::Session::stateChanged, dialog, [dialog, session, refresh, release] {
            if (session->state() == acp::Session::State::Ended) {
                dialog->permissionId = QJsonValue::Undefined;
                dialog->conversation->finishTurn();
                dialog->authRow->hide();
                release();
            } else if (session->state() == acp::Session::State::Ready && dialog->authMethod->count()) {
                dialog->authMethod->clear();
                dialog->authRow->hide();
                dialog->tell({});
            }
            refresh();
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
        connect(session, &acp::Session::permissionRequested, dialog,
                [dialog, refresh](const QJsonValue &id, const QJsonObject &params) {
            if (!dialog->permissionId.isUndefined())
                dialog->session->answerPermission(dialog->permissionId, dialog->rejectOption);
            dialog->permissionId = id;
            dialog->allowOption.clear();
            dialog->rejectOption.clear();
            for (const auto &value : params["options"].toArray()) {
                const auto option = value.toObject();
                if (option["kind"] == "allow_once" && dialog->allowOption.isEmpty())
                    dialog->allowOption = option["optionId"].toString();
                if (option["kind"] == "reject_once" && dialog->rejectOption.isEmpty())
                    dialog->rejectOption = option["optionId"].toString();
            }
            const auto call = params["toolCall"].toObject();
            dialog->permissionText->setText("The agent asks to use its own tool (not a Nova tool): " +
                agentDisplayText(call["title"].toString(), 200) + "\n" +
                agentDisplayText(QString::fromUtf8(QJsonDocument(call["rawInput"].toObject()).toJson(QJsonDocument::Compact)), 400));
            refresh();
        });
        dialog->tell({});
        dialog->authMethod->clear();
        if (!session->start(program, dialog->workFolder,
                            QJsonArray{acp::stdioServer("nova", bridge)}, acp::agentEnvironment(), &problem)) {
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
        const auto selected = choice();
        const auto args = selected.command.isEmpty() ? QProcess::splitCommand(dialog->arguments->text()) : selected.arguments;
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
            begin(false, program);
    });
    connect(dialog->edit, &QPushButton::clicked, dialog, [this, dialog, begin] {
        if (QMessageBox::question(dialog, "Allow agent editing?",
                "Nova will end this read-only session and start a new one in which the agent can create unsaved "
                "textures and animations, modify open documents, remove layers, frames and model elements, and save "
                "open documents into the pack’s files on disk.\n\nProject: " + QFileInfo(dialog->sessionProject).fileName(),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes ||
            project != dialog->sessionProject || !dialog->session)
            return;
        auto old = dialog->session;
        const auto authorizedProject = dialog->sessionProject;
        const auto restart = [this, dialog, old, begin, authorizedProject, program = dialog->activeProgram] {
            if (old && dialog->session == old && old->state() == acp::Session::State::Ended &&
                project == authorizedProject)
                begin(true, program);
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
    connect(dialog->cancel, &QPushButton::clicked, dialog, [dialog, refresh] {
        if (dialog->session)
            dialog->session->cancel();
        dialog->permissionId = QJsonValue::Undefined;
        refresh();
    });
    auto submit = [dialog, transcript] {
        const auto text = dialog->prompt->toPlainText().trimmed();
        if (dialog->session && dialog->session->prompt(text)) {
            transcript->setCurrentWidget(dialog->conversation);
            dialog->conversation->add(AgentConversation::Speaker::You, text);
            dialog->tell({});
            dialog->prompt->remember(text);
        }
    };
    connect(dialog->send, &QPushButton::clicked, dialog, submit);
    connect(dialog->prompt, &QPlainTextEdit::textChanged, dialog, refresh);
    dialog->prompt->submit = submit;
    auto answer = [dialog, refresh](bool allowed) {
        if (dialog->session && !dialog->permissionId.isUndefined())
            dialog->session->answerPermission(dialog->permissionId, allowed ? dialog->allowOption : dialog->rejectOption);
        dialog->permissionId = QJsonValue::Undefined;
        refresh();
    };
    connect(dialog->allow, &QPushButton::clicked, dialog, [answer] { answer(true); });
    connect(dialog->reject, &QPushButton::clicked, dialog, [answer] { answer(false); });
    connect(mcpService, &mcp::Service::changed, dialog, refresh);
    connect(this, &QWidget::windowTitleChanged, dialog, refresh);
    // Closing the window ends the agent and the grant it held.
    connect(dialog, &QDialog::finished, dialog, [this, dialog, release] {
        if (dialog->session)
            dialog->session->stop("The session window was closed.");
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
