// SPDX-License-Identifier: GPL-3.0-or-later
// ACP prototype: Nova starts one agent (OpenCode's "opencode acp", or another ACP agent .exe), attaches
// Nova's MCP bridge to the agent session, and keeps the grant under Nova's control. Read-only by
// default; editing needs a separate confirmation after a read-only session has connected. The grant
// accepts only a bridge started by this agent, and ends with the session, a revoke or a project switch.
#include "window.h"
#include "clients.h"
#include "service.h"
#include "agent_client.h"
#include "theme.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
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
// Agent text is untrusted: no control characters (terminal escapes) and a bounded length.
QString displayable(QString text, int limit = 4000) {
    static const QRegularExpression control(R"([\x00-\x08\x0b-\x1f\x7f\x{200b}-\x{200f}\x{202a}-\x{202e}])");
    text.remove(control);
    return text.size() > limit ? text.left(limit) + " …" : text;
}
QString bridgePath() {
    const auto configured = qApp->property("mcpBridgePath").toString(); // tests supply the build's bridge
    return configured.isEmpty() ? clients::locateBridge(QCoreApplication::applicationDirPath()).path : configured;
}

// The conversation: the user's messages, the agent's replies and its reasoning. Tool activity and
// protocol details are not shown; session state, access and decisions live in the labels around it.
class Conversation : public QTextEdit {
  public:
    enum class Speaker { You, Agent, Thinking };
    explicit Conversation(QWidget *parent) : QTextEdit(parent) {
        setReadOnly(true);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        setAccessibleName("Conversation with the agent");
        redraw.setSingleShot(true);
        redraw.setInterval(60); // streamed replies arrive in many small pieces
        connect(&redraw, &QTimer::timeout, this, &Conversation::render);
    }
    // Streamed pieces of the same reply (or reasoning) join the current paragraph.
    void add(Speaker speaker, const QString &text) {
        if (text.isEmpty())
            return;
        if (entries.isEmpty() || entries.last().first != speaker || speaker == Speaker::You)
            entries.append({speaker, QString{}});
        auto &body = entries.last().second;
        if (body.size() < 200000)
            body += displayable(text, 200000 - int(body.size()));
        if (entries.size() > 500)
            entries.removeFirst();
        redraw.start();
    }

  private:
    QList<QPair<Speaker, QString>> entries;
    QTimer redraw;
    void render() {
        auto *bar = verticalScrollBar();
        const bool following = bar->value() >= bar->maximum() - 4;
        clear();
        QTextCursor cursor(document());
        QTextCharFormat name, plain, quiet;
        name.setFontWeight(QFont::DemiBold);
        quiet.setFontItalic(true);
        quiet.setForeground(palette().color(QPalette::PlaceholderText));
        QTextBlockFormat block;
        block.setTopMargin(10);
        bool first = true;
        for (const auto &[speaker, text] : entries) {
            if (!first)
                cursor.insertBlock(block);
            first = false;
            cursor.insertText(speaker == Speaker::You ? "You" : speaker == Speaker::Agent ? "Agent" : "Agent · thinking",
                              speaker == Speaker::Thinking ? quiet : name);
            cursor.insertBlock(QTextBlockFormat{});
            if (speaker == Speaker::Agent) {
                // Replies are Markdown. The agent is untrusted: no raw HTML, and images become their text.
                static const QRegularExpression image(R"(!\[([^\]]*)\]\([^)]*\))");
                cursor.setCharFormat(plain);
                cursor.insertMarkdown(QString(text).replace(image, "\\1"), QTextDocument::MarkdownFeatures::fromInt(
                                          QTextDocument::MarkdownDialectGitHub | QTextDocument::MarkdownNoHTML));
            } else {
                cursor.insertText(text, speaker == Speaker::Thinking ? quiet : plain);
            }
        }
        if (following)
            bar->setValue(bar->maximum());
    }
};

class AgentSessionDialog : public QDialog {
  public:
    QPointer<acp::Session> session;
    QString sessionProject, workFolder;
    quint64 grant = 0;
    bool editing = false;
    QLabel *status, *access, *notice, *permissionText;
    QLineEdit *program, *arguments, *prompt;
    QComboBox *agentPicker, *authMethod;
    QWidget *authRow;
    acp::AgentProgram activeProgram;
    Conversation *conversation;
    QPushButton *start, *edit, *revoke, *send, *cancel, *stop, *allow, *reject;
    QJsonValue permissionId = QJsonValue::Undefined; // undefined: no request pending (null is a valid id)
    QString allowOption, rejectOption;
    explicit AgentSessionDialog(QWidget *parent) : QDialog(parent) {}
    // One line for things the user should know that are not part of the conversation.
    void tell(const QString &message) { notice->setText(message); notice->setVisible(!message.isEmpty()); }
};
} // namespace

void Window::updateExperimentalActions() {
    if (auto a = findChild<QAction *>("acpSession")) {
        a->setVisible(preferences.experimentalAgentSession);
        a->setEnabled(preferences.experimentalAgentSession);
    }
}

void Window::showAgentSession() {
    if (!preferences.experimentalAgentSession)
        return;
    if (auto existing = findChild<QDialog *>("acpSessionDialog")) {
        existing->raise();
        existing->activateWindow();
        return;
    }
    ensureMcpService();
    auto dialog = new AgentSessionDialog(this);
    dialog->setObjectName("acpSessionDialog");
    dialog->setWindowTitle("Agent Session (ACP, Experimental)");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto layout = new QVBoxLayout(dialog);
    auto form = new QFormLayout;
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
    form->addRow("Agent", dialog->agentPicker);
    dialog->program = ui::pathField({}, dialog);
    dialog->program->setObjectName("acpAgentPath");
    dialog->program->setPlaceholderText("Full path to a program or npm launcher");
    auto choose = new QPushButton("Choose…", dialog);
    choose->setObjectName("acpChooseAgent");
    auto programRow = new QHBoxLayout;
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
    dialog->status = new QLabel("Not running", dialog);
    dialog->status->setObjectName("acpStatus");
    dialog->status->setWordWrap(true);
    form->addRow("Session", dialog->status);
    dialog->access = new QLabel(dialog);
    dialog->access->setObjectName("acpAccess");
    dialog->access->setWordWrap(true);
    form->addRow("Nova tools", dialog->access);
    dialog->authRow = new QWidget(dialog);
    auto authLayout = new QHBoxLayout(dialog->authRow);
    authLayout->setContentsMargins(0, 0, 0, 0);
    dialog->authMethod = new QComboBox(dialog->authRow);
    dialog->authMethod->setObjectName("acpAuthMethod");
    dialog->authMethod->setAccessibleName("Agent sign-in method");
    auto signIn = new QPushButton("Sign in", dialog->authRow);
    signIn->setObjectName("acpSignIn");
    authLayout->addWidget(dialog->authMethod, 1);
    authLayout->addWidget(signIn);
    form->addRow(dialog->authRow);
    dialog->authRow->hide();
    connect(signIn, &QPushButton::clicked, dialog, [dialog] {
        if (dialog->session && dialog->session->authenticate(dialog->authMethod->currentData().toString())) {
            dialog->authRow->hide();
            dialog->tell("Waiting for the agent to finish signing in…");
        }
    });
    layout->addLayout(form);

    auto controls = new QHBoxLayout;
    dialog->start = new QPushButton("Start read-only session…", dialog);
    dialog->start->setObjectName("acpStart");
    dialog->edit = new QPushButton("Allow editing…", dialog);
    dialog->edit->setObjectName("acpEditing");
    dialog->revoke = new QPushButton("Revoke Nova access", dialog);
    dialog->revoke->setObjectName("acpRevoke");
    dialog->stop = new QPushButton("Stop session", dialog);
    dialog->stop->setObjectName("acpStop");
    for (auto button : {dialog->start, dialog->edit, dialog->revoke, dialog->stop})
        controls->addWidget(button);
    layout->addLayout(controls);

    dialog->conversation = new Conversation(dialog);
    dialog->conversation->setObjectName("acpConversation");
    layout->addWidget(dialog->conversation, 1);
    dialog->notice = new QLabel(dialog);
    dialog->notice->setObjectName("acpNotice");
    dialog->notice->setWordWrap(true);
    dialog->notice->setTextFormat(Qt::PlainText);
    dialog->notice->hide();
    layout->addWidget(dialog->notice);

    dialog->permissionText = new QLabel(dialog);
    dialog->permissionText->setObjectName("acpPermission");
    dialog->permissionText->setWordWrap(true);
    dialog->permissionText->setTextFormat(Qt::PlainText);
    dialog->allow = new QPushButton("Allow once", dialog);
    dialog->allow->setObjectName("acpPermissionAllow");
    dialog->reject = new QPushButton("Reject", dialog);
    dialog->reject->setObjectName("acpPermissionReject");
    auto permissionRow = new QHBoxLayout;
    permissionRow->addWidget(dialog->permissionText, 1);
    permissionRow->addWidget(dialog->allow);
    permissionRow->addWidget(dialog->reject);
    layout->addLayout(permissionRow);

    auto promptRow = new QHBoxLayout;
    dialog->prompt = new QLineEdit(dialog);
    dialog->prompt->setObjectName("acpPrompt");
    dialog->prompt->setPlaceholderText("Message to the agent");
    dialog->send = new QPushButton("Send", dialog);
    dialog->send->setObjectName("acpSend");
    dialog->cancel = new QPushButton("Cancel turn", dialog);
    dialog->cancel->setObjectName("acpCancel");
    promptRow->addWidget(dialog->prompt, 1);
    promptRow->addWidget(dialog->send);
    promptRow->addWidget(dialog->cancel);
    layout->addLayout(promptRow);
    auto note = new QLabel("Nova controls only its own tools. The agent runs with your Windows account and its own "
                           "settings; its built-in file and shell tools, if any, follow the agent's configuration. "
                           "It works in an empty folder outside the project.", dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    dialog->resize(720, 560);

    auto ours = [this, dialog] {
        return mcpService && dialog->grant && mcpService->grantSerial() == dialog->grant &&
               !mcpService->authorizedProject().isEmpty();
    };
    auto refresh = [this, dialog, ours, choose] {
        const auto state = dialog->session ? dialog->session->state() : acp::Session::State::Idle;
        const bool live = state == acp::Session::State::Starting || state == acp::Session::State::Ready ||
                          state == acp::Session::State::Prompting;
        static const QMap<acp::Session::State, QString> names{
            {acp::Session::State::Idle, "Not running"}, {acp::Session::State::Starting, "Starting…"},
            {acp::Session::State::Ready, "Connected"}, {acp::Session::State::Prompting, "Working…"},
            {acp::Session::State::Stopping, "Stopping…"}, {acp::Session::State::Ended, "Ended"}};
        auto text = names.value(state);
        if (dialog->session && !dialog->session->agentName().isEmpty())
            text += " · " + displayable(dialog->session->agentName(), 120);
        if (state == acp::Session::State::Ended && dialog->session)
            text += " · " + dialog->session->endReason();
        if (live)
            text += " · project " + QFileInfo(dialog->sessionProject).fileName();
        dialog->status->setText(text);
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
        const bool configuring = state == acp::Session::State::Idle || state == acp::Session::State::Ended;
        dialog->agentPicker->setEnabled(configuring);
        dialog->program->setEnabled(configuring);
        dialog->arguments->setEnabled(configuring);
        choose->setEnabled(configuring);
        dialog->edit->setEnabled(live && ours() && !dialog->editing && mcpService->clientConnected());
        dialog->revoke->setEnabled(ours());
        dialog->stop->setEnabled(live);
        dialog->send->setEnabled(state == acp::Session::State::Ready);
        dialog->prompt->setEnabled(state == acp::Session::State::Ready);
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
    auto begin = [this, dialog, refresh, release, choice](bool editing, const acp::AgentProgram &program) {
        QString problem;
        if (!acp::acceptableExecutable(program.executable, &problem))
            return dialog->tell(problem);
        const auto bridge = bridgePath();
        if (!QFileInfo(bridge).isFile())
            return (void)QMessageBox::warning(dialog, "Agent Session", "nova-mcp.exe is not next to Nova Studio.");
        if (mcpService->status() != "Disabled")
            return (void)QMessageBox::warning(dialog, "Agent Session",
                "Another MCP connection is offered or active. End it in MCP Integration first.");
        if (dialog->session)
            dialog->session->deleteLater();
        const auto selected = project;
        dialog->sessionProject = selected;
        dialog->editing = editing;
        dialog->activeProgram = program;
        dialog->workFolder = QDir::tempPath() + "/nova-agent-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QDir().mkpath(dialog->workFolder))
            return (void)QMessageBox::warning(dialog, "Agent Session", "The agent's working folder could not be created.");
        if (!mcpService->authorize(selected, editing)) {
            release();
            return (void)QMessageBox::warning(dialog, "Agent Session", mcpService->error());
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
                dialog->authMethod->addItem(displayable(method["name"].toString(id), 120), id);
                dialog->authMethod->setItemData(dialog->authMethod->count() - 1,
                                               displayable(method["description"].toString(), 300), Qt::ToolTipRole);
            }
            dialog->authRow->setVisible(!methods.isEmpty());
            const auto login = choice().signIn;
            dialog->tell(!methods.isEmpty() ? "Choose a sign-in method to continue." : login.isEmpty()
                ? "Sign in using your agent’s own tool, then start a new session."
                : "Run " + login + " in a terminal to sign in, then start a new session.");
        });
        connect(session, &acp::Session::agentText, dialog,
                [dialog](const QString &text) { dialog->conversation->add(Conversation::Speaker::Agent, text); });
        connect(session, &acp::Session::agentThought, dialog,
                [dialog](const QString &text) { dialog->conversation->add(Conversation::Speaker::Thinking, text); });
        // Only how a turn ended unusually is worth a line; a normal reply speaks for itself.
        connect(session, &acp::Session::turnEnded, dialog, [dialog](const QString &reason) {
            static const QMap<QString, QString> told{
                {"end_turn", ""}, {"cancelled", "You cancelled the agent's turn."},
                {"max_tokens", "The agent stopped: it reached its output limit."},
                {"max_turn_requests", "The agent stopped: it reached its limit of steps for one message."},
                {"refusal", "The agent declined to continue."}};
            dialog->tell(told.contains(reason) ? told.value(reason) : "The agent's turn ended with a problem.");
        });
        connect(session, &acp::Session::problem, dialog, [dialog](const QString &message) { dialog->tell(message); });
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
                displayable(call["title"].toString(), 200) + "\n" +
                displayable(QString::fromUtf8(QJsonDocument(call["rawInput"].toObject()).toJson(QJsonDocument::Compact)), 400));
            refresh();
        });
        dialog->tell({});
        dialog->authMethod->clear();
        if (!session->start(program, dialog->workFolder,
                            QJsonArray{acp::stdioServer("nova", bridge)}, acp::agentEnvironment(), &problem)) {
            dialog->tell("The session could not start: " + problem);
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
            return dialog->tell(problem);
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
    auto submit = [dialog] {
        const auto text = dialog->prompt->text().trimmed();
        if (dialog->session && dialog->session->prompt(text)) {
            dialog->conversation->add(Conversation::Speaker::You, text);
            dialog->tell({});
            dialog->prompt->clear();
        }
    };
    connect(dialog->send, &QPushButton::clicked, dialog, submit);
    connect(dialog->prompt, &QLineEdit::returnPressed, dialog, submit);
    auto answer = [dialog, refresh](bool allowed) {
        if (dialog->session && !dialog->permissionId.isUndefined())
            dialog->session->answerPermission(dialog->permissionId, allowed ? dialog->allowOption : dialog->rejectOption);
        dialog->permissionId = QJsonValue::Undefined;
        refresh();
    };
    connect(dialog->allow, &QPushButton::clicked, dialog, [answer] { answer(true); });
    connect(dialog->reject, &QPushButton::clicked, dialog, [answer] { answer(false); });
    connect(mcpService, &mcp::Service::changed, dialog, refresh);
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
