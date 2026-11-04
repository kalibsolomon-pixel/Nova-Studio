// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-G: Agent Setup page of Resources › MCP Integration.
// Detection, status and one-click setup for AI agents that start nova-mcp.exe. Every file read
// and every external process runs off the GUI thread. Nothing here grants project access.
#include "mcp_setup.h"
#include "build_identity.h"
#include "clients.h"
#include "theme.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QScrollArea>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <memory>

namespace nova {
namespace clients = mcp::clients;
namespace {
struct Detection {
    QList<clients::Client> clients;
    QList<clients::Status> statuses;
};
QString stateText(const clients::Status &status) {
    switch (status.state) {
    case clients::Status::NotDetected: return "Not detected on this PC";
    case clients::Status::NotConfigured: return "Agent configuration required";
    case clients::Status::Configured: return "Agent configured";
    case clients::Status::DifferentPath: return "Configured for a different copy of Nova Studio";
    case clients::Status::Invalid: return "Configuration error";
    case clients::Status::Unknown: return "Manual setup";
    }
    return {};
}
clients::Bridge bridge() { return clients::locateBridge(QCoreApplication::applicationDirPath()); }

// Runs program + arguments without blocking the GUI thread; reports exit code and output.
void runProcess(QObject *owner, const QStringList &command, int timeoutMs,
                std::function<void(int exitCode, bool timedOut, const QString &output)> done) {
    auto process = new QProcess(owner);
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto timer = new QTimer(process);
    timer->setSingleShot(true);
    auto finished = std::make_shared<bool>(false);
    auto finish = [process, finished, done](int code, bool timedOut) {
        if (*finished)
            return;
        *finished = true;
        const auto output = QString::fromUtf8(process->readAll()).trimmed();
        done(code, timedOut, output);
        process->deleteLater();
    };
    QObject::connect(timer, &QTimer::timeout, process, [process, finish] {
        process->kill();
        finish(-1, true);
    });
    QObject::connect(process, &QProcess::finished, process,
                     [finish](int code, QProcess::ExitStatus status) { finish(status == QProcess::NormalExit ? code : -1, false); });
    QObject::connect(process, &QProcess::errorOccurred, process, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(-1, false);
    });
    timer->start(timeoutMs);
    process->start(command.first(), command.mid(1));
}
} // namespace

QString mcpBridgeSummary() {
    const auto b = bridge();
    if (!b.exists)
        return "MCP bridge missing: nova-mcp.exe is not next to Nova Studio. Reinstall Nova Studio.";
    return b.developmentBuild ? QString("MCP available (development build)") : QString("MCP available");
}

QWidget *createMcpSetupPage(QWidget *parent, std::function<QString()> connection) {
    // The page scrolls, so the dialog fits at every display scale.
    auto scroll = new QScrollArea(parent);
    scroll->setObjectName("mcpSetupScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto page = new QWidget;
    page->setObjectName("mcpSetupPage");
    scroll->setWidget(page);
    auto layout = new QVBoxLayout(page);
    layout->setSpacing(ui::SpaceL);
    auto intro = ui::hint("Connect an AI agent once. Setup only tells the agent where nova-mcp.exe is; it never "
                          "grants access. You still choose Allow on the Access tab for each session.", page);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto form = new QFormLayout;
    form->setHorizontalSpacing(ui::SpaceXL);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto bridgeRow = new QHBoxLayout;
    auto bridgeLabel = new QLabel(page);
    bridgeLabel->setObjectName("mcpBridgePath");
    bridgeLabel->setTextFormat(Qt::PlainText);
    bridgeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bridgeLabel->setWordWrap(true);
    bridgeLabel->setMinimumWidth(1);
    bridgeRow->addWidget(bridgeLabel, 1);
    auto copyPath = new QPushButton("Copy path", page);
    copyPath->setObjectName("mcpCopyBridgePath");
    bridgeRow->addWidget(copyPath, 0, Qt::AlignTop);
    form->addRow("Bridge", bridgeRow);

    auto agentRow = new QHBoxLayout;
    auto agent = new QComboBox(page);
    agent->setObjectName("mcpAgentChoice");
    agent->setAccessibleName("AI agent");
    agent->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    agent->setMinimumContentsLength(24);
    agentRow->addWidget(agent, 1);
    auto recheck = new QPushButton("Check again", page);
    recheck->setObjectName("mcpAgentRecheck");
    agentRow->addWidget(recheck);
    form->addRow("Agent", agentRow);

    auto status = new QLabel(page);
    status->setObjectName("mcpAgentStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setMinimumWidth(1);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("Status", status);
    layout->addLayout(form);

    auto actions = new QHBoxLayout;
    auto add = new QPushButton("Add Nova Studio…", page);
    add->setObjectName("mcpAgentConfigure");
    actions->addWidget(add);
    auto copyConfig = new QPushButton("Copy configuration", page);
    copyConfig->setObjectName("mcpCopyConfiguration");
    actions->addWidget(copyConfig);
    auto diagnose = new QPushButton("Run diagnostics", page);
    diagnose->setObjectName("mcpRunDiagnostics");
    actions->addWidget(diagnose);
    actions->addStretch();
    layout->addLayout(actions);

    layout->addWidget(ui::sectionTitle("Configuration", page));
    auto configuration = new QPlainTextEdit(page);
    configuration->setObjectName("mcpAgentConfiguration");
    configuration->setAccessibleName("Configuration for the selected agent");
    configuration->setReadOnly(true);
    configuration->setLineWrapMode(QPlainTextEdit::NoWrap);
    configuration->setFixedHeight(configuration->fontMetrics().lineSpacing() * 6 + 16);
    layout->addWidget(configuration);
    auto steps = new QLabel(page);
    steps->setObjectName("mcpAgentInstructions");
    steps->setTextFormat(Qt::PlainText);
    steps->setWordWrap(true);
    steps->setMinimumWidth(1);
    steps->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(steps);
    auto results = new QPlainTextEdit(page);
    results->setObjectName("mcpDiagnosticsResults");
    results->setAccessibleName("Diagnostics results");
    results->setReadOnly(true);
    results->setFixedHeight(results->fontMetrics().lineSpacing() * 7 + 16);
    results->hide();
    layout->addWidget(results);
    layout->addStretch();

    auto detection = std::make_shared<Detection>();
    auto busy = std::make_shared<bool>(false);
    const auto current = [agent, detection]() -> std::pair<clients::Client, clients::Status> {
        const auto i = agent->currentIndex();
        if (i < 0 || i >= detection->clients.size())
            return {};
        return {detection->clients[i], detection->statuses[i]};
    };
    const auto refreshSelection = [=] {
        const auto b = bridge();
        bridgeLabel->setText(b.exists ? QString("%1%2").arg(QDir::toNativeSeparators(b.path),
                                            b.developmentBuild ? "\nThis is a development build. Configure agents "
                                                                 "from an installed copy of Nova Studio." : "")
                                      : QString("MCP bridge missing: nova-mcp.exe is not next to Nova Studio."));
        copyPath->setEnabled(b.exists);
        const auto selection = current();
        const auto client = selection.first;
        const auto state = selection.second;
        if (client.id.isEmpty()) {
            status->setText("Checking agents…");
            add->setEnabled(false);
            copyConfig->setEnabled(false);
            return;
        }
        QStringList lines{stateText(state)};
        if (!state.detail.isEmpty())
            lines << state.detail;
        if (!client.evidence.isEmpty() && client.kind != clients::Kind::Generic)
            lines << "Found · " + client.evidence;
        if (client.running && client.kind == clients::Kind::ClaudeDesktop)
            lines << "Claude Desktop is running. Quit it before adding Nova Studio.";
        if (client.method == clients::Method::Manual && !client.manualReason.isEmpty())
            lines << client.manualReason;
        status->setText(lines.join('\n'));
        configuration->setPlainText(clients::manualConfiguration(client, b.path));
        steps->setText(clients::instructions(client));
        const bool automatic = client.method != clients::Method::Manual && client.installed &&
                               state.state != clients::Status::Invalid;
        add->setVisible(client.kind != clients::Kind::Generic);
        add->setEnabled(!*busy && b.exists && automatic && state.state != clients::Status::Configured);
        add->setText(state.state == clients::Status::DifferentPath ? "Update Nova Studio…" : "Add Nova Studio…");
        copyConfig->setEnabled(b.exists);
        diagnose->setEnabled(!*busy && b.exists);
    };
    // Detection reads client files (some are several MB) on a worker thread.
    auto watcher = new QFutureWatcher<Detection>(page);
    QObject::connect(watcher, &QFutureWatcher<Detection>::finished, page, [=] {
        const auto selected = agent->currentData().toString();
        *detection = watcher->result();
        const QSignalBlocker block(agent);
        agent->clear();
        for (int i = 0; i < detection->clients.size(); ++i) {
            const auto &client = detection->clients[i];
            agent->addItem(client.name + (client.kind == clients::Kind::Generic ? QString{}
                                          : " — " + stateText(detection->statuses[i])), client.id);
        }
        int index = agent->findData(selected);
        if (index < 0)
            for (int i = 0; i < detection->clients.size() && index < 0; ++i)
                if (detection->clients[i].installed && detection->clients[i].kind != clients::Kind::Generic)
                    index = i;
        agent->setCurrentIndex(std::max(0, index));
        refreshSelection();
    });
    const auto detect = [=] {
        if (watcher->isRunning())
            return;
        status->setText("Checking agents…");
        const auto bridgePath = bridge().path;
        watcher->setFuture(QtConcurrent::run([bridgePath] {
            Detection result;
            result.clients = clients::detect(clients::Environment::system());
            for (const auto &client : std::as_const(result.clients))
                result.statuses << clients::inspect(client, bridgePath);
            return result;
        }));
    };
    QObject::connect(agent, &QComboBox::currentIndexChanged, page, [=] {
        results->clear();
        results->hide();
        refreshSelection();
    });
    QObject::connect(recheck, &QPushButton::clicked, page, detect);
    QObject::connect(copyPath, &QPushButton::clicked, page, [] {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(bridge().path));
    });
    QObject::connect(copyConfig, &QPushButton::clicked, page, [=] {
        QApplication::clipboard()->setText(configuration->toPlainText());
        status->setText(status->text() + "\nConfiguration copied.");
    });

    const auto report = [results](const QString &text) {
        results->show();
        results->appendPlainText(text);
    };
    QObject::connect(add, &QPushButton::clicked, page, [=] {
        const auto selection = current();
        const auto client = selection.first;
        const auto state = selection.second;
        const auto b = bridge();
        auto plan = clients::plan(client, b.path);
        if (!plan.possible) {
            QMessageBox::information(page, "Add Nova Studio", plan.problem);
            return;
        }
        QMessageBox confirm(QMessageBox::Question, QString("Add Nova Studio to %1?").arg(client.name),
            plan.summary + "\n\nThis does not grant access to any project.", QMessageBox::Cancel, page);
        confirm.setObjectName("mcpConfigureConfirm");
        auto apply = confirm.addButton(plan.replaces ? "Update" : "Add", QMessageBox::AcceptRole);
        confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.setDetailedText((client.method == clients::Method::EditJson ? "Entry:\n" : "Commands:\n") + plan.preview +
                                (plan.target.isEmpty() ? QString{} : "\n\nFile: " + plan.target));
        confirm.exec();
        if (confirm.clickedButton() != apply)
            return;
        *busy = true;
        refreshSelection();
        results->clear();
        const auto environment = clients::Environment::system();
        const auto finishWith = [=](const QString &message) {
            *busy = false;
            report(message);
            detect();
        };
        if (client.method == clients::Method::EditJson) {
            auto applied = new QFutureWatcher<QString>(page);
            QObject::connect(applied, &QFutureWatcher<QString>::finished, page, [=] {
                finishWith(applied->result());
                applied->deleteLater();
            });
            applied->setFuture(QtConcurrent::run([plan, environment] {
                QString backup, error;
                if (!clients::applyJson(plan, environment, &backup, &error))
                    return "Not changed: " + error;
                return QString("Added Nova Studio to %1.%2\nStart Claude Desktop after choosing Allow on the Access tab.")
                    .arg(plan.target, backup.isEmpty() ? QString{} : "\nBackup of the previous file: " +
                                                                    QDir::toNativeSeparators(backup));
            }));
            return;
        }
        // Vendor tool: back up the file it edits, then run its commands one after another.
        QString backupError;
        const auto backup = clients::backupFile(client.configPath, client.id, environment, &backupError);
        if (!backupError.isEmpty()) {
            finishWith("Not changed: " + backupError);
            return;
        }
        if (!backup.isEmpty())
            report("Backup of " + QDir::toNativeSeparators(client.configPath) + ": " + QDir::toNativeSeparators(backup));
        auto remaining = std::make_shared<QList<QStringList>>(plan.commands);
        auto next = std::make_shared<std::function<void()>>();
        *next = [=] {
            if (remaining->isEmpty()) {
                finishWith(QString("Done. Check the status above, then follow the steps to connect %1.").arg(client.name));
                *next = nullptr;
                return;
            }
            const auto command = remaining->takeFirst();
            report("> " + command.mid(1).join(' '));
            runProcess(page, command, 60000, [=](int code, bool timedOut, const QString &output) {
                if (!output.isEmpty())
                    report(output.left(4000));
                if (code != 0) {
                    finishWith(timedOut ? "Stopped: the command did not finish within 60 seconds."
                                        : QString("Stopped: the command failed (exit code %1).%2").arg(code)
                                              .arg(backup.isEmpty() ? QString{} : " The backup above is unchanged."));
                    *next = nullptr;
                    return;
                }
                (*next)();
            });
        };
        (*next)();
    });

    QObject::connect(diagnose, &QPushButton::clicked, page, [=] {
        *busy = true;
        refreshSelection();
        results->clear();
        const auto b = bridge();
        const auto selection = current();
        const auto client = selection.first;
        const auto state = selection.second;
        report(QString("%1 · Bridge file: %2").arg(b.exists ? "Pass" : "Fail", QDir::toNativeSeparators(b.path)));
        report(b.developmentBuild ? "Note · Location: development build folder, not an installed copy"
                                  : "Pass · Location: installed beside nova-studio.exe");
        const auto done = [=] {
            if (client.kind != clients::Kind::Generic) {
                const bool ok = state.state == clients::Status::Configured;
                report(QString("%1 · %2 configuration: %3").arg(ok ? "Pass" : "Info", client.name, state.detail));
                if (!state.configuredCommand.isEmpty())
                    report(QString("%1 · Configured command resolves: %2")
                               .arg(QFileInfo(QDir::fromNativeSeparators(state.configuredCommand)).isFile() ? "Pass" : "Fail",
                                    QDir::toNativeSeparators(state.configuredCommand)));
            }
            const auto live = connection();
            report(live.isEmpty() ? "Info · Connected agent: none. These checks are local; they do not grant access "
                                    "or prove that an agent has connected."
                                  : "Pass · Connected agent: " + live);
            *busy = false;
            refreshSelection();
        };
        if (!b.exists) {
            done();
            return;
        }
        runProcess(page, {b.path, "--version"}, 10000, [=](int code, bool timedOut, const QString &output) {
            const bool matches = code == 0 && output.contains(QString(" ") + AppVersion + " ");
            report(QString("%1 · Bridge starts: %2").arg(matches ? "Pass" : "Fail",
                timedOut ? QString("no answer within 10 seconds") : output.isEmpty() ? QString("exit code %1").arg(code) : output));
            runProcess(page, {b.path, "--self-test"}, 10000, [=](int code, bool, const QString &output) {
                const auto json = QJsonDocument::fromJson(output.toUtf8()).object();
                report(QString("%1 · Protocol self-test: MCP %2, %3 tools declared (no connection, no project access)")
                           .arg(code == 0 && json["tools"].toInt() > 0 ? "Pass" : "Fail",
                                json["protocolVersion"].toString("unknown")).arg(json["tools"].toInt()));
                done();
            });
        });
    });
    refreshSelection();
    QTimer::singleShot(0, page, detect);
    return scroll;
}
} // namespace nova
