// SPDX-License-Identifier: GPL-3.0-or-later
// ACP prototype: the native Agent Session window starts a scripted ACP agent (fake_acp_agent), which
// starts Nova's real build-tree bridge from session/new. Consent, read-only enforcement, the
// process-bound grant, revoke, project switch, crash, stop, cancel, permission requests, editing
// after separate consent, and secret handling. Synthetic packs only.
#include "window.h"
#include "search.h"
#include "service.h"
#include "agent_client.h"
#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <qt_windows.h>

using namespace nova::mcp;

namespace {
QString bridge() { return QString::fromUtf8(NOVA_MCP_TEST_BRIDGE); }
QString agent() { return QDir::toNativeSeparators(QString::fromUtf8(NOVA_FAKE_ACP_AGENT)); }
void write(const QString &file, const QByteArray &data) {
    QVERIFY(QDir().mkpath(QFileInfo(file).path()));
    QFile output(file);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write(data), data.size());
}
QMap<QString, QByteArray> fingerprint(const QString &root) {
    QMap<QString, QByteArray> hashes;
    QDirIterator files(root, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto file = files.next();
        if (QDir(root).relativeFilePath(file).startsWith(".nova/"))
            continue;
        QFile input(file);
        hashes.insert(QDir(root).relativeFilePath(file),
                      input.open(QIODevice::ReadOnly) ? QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256)
                                                        : QByteArray("unreadable"));
    }
    return hashes;
}
bool alive(qint64 pid) {
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!handle)
        return false;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return running;
}
// Answers the next message box with a button and records its title and text.
struct Answer {
    QString title, text;
    QTimer timer;
    explicit Answer(QMessageBox::StandardButton button) {
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, [this, button] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                title = box->windowTitle();
                text = box->text();
                timer.stop();
                box->button(button)->click();
            }
        });
        timer.start();
    }
};
} // namespace

class AcpSessionTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString root, other;
    std::unique_ptr<nova::Window> window;
    QDialog *dialog = nullptr;

    Service *service() { return window->findChild<Service *>(); }
    nova::acp::Session *session() { return dialog->findChild<nova::acp::Session *>(); }
    QString transcript() { return dialog->findChild<QTextEdit *>("acpConversation")->toPlainText(); }
    QString status() { return dialog->findChild<QLabel *>("acpStatus")->text(); }
    QString notice() { return dialog->findChild<QLabel *>("acpNotice")->text(); }
    // Names any modal dialog that is open, for failure messages (an open modal makes Nova refuse edits).
    static QString modal() {
        auto widget = QApplication::activeModalWidget();
        if (!widget)
            return "no modal dialog";
        auto box = qobject_cast<QMessageBox *>(widget);
        return QString("modal %1 \"%2\" %3").arg(widget->metaObject()->className(), widget->windowTitle(),
                                                 box ? box->text() : widget->objectName());
    }
    QPushButton *button(const char *name) { return dialog->findChild<QPushButton *>(name); }
    void open() {
        QElapsedTimer timer;
        timer.start();
        window = std::make_unique<nova::Window>(root, QString{}, timer);
        window->show();
        window->findChild<QAction *>("acpSession")->trigger();
        dialog = window->findChild<QDialog *>("acpSessionDialog");
        QVERIFY(dialog);
        dialog->findChild<QLineEdit *>("acpAgentPath")->setText(agent());
    }
    // Starts a read-only session and waits until the agent's bridge holds the grant.
    void startReadOnly() {
        Answer yes(QMessageBox::Yes);
        button("acpStart")->click();
        QTRY_VERIFY_WITH_TIMEOUT(status().startsWith("Connected"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(service()->clientConnected(), 15000);
        QCOMPARE(service()->permission(), "Read-only");
    }
    void ask(const QString &prompt, const QString &expect) {
        const auto before = transcript().size();
        dialog->findChild<QLineEdit *>("acpPrompt")->setText(prompt);
        button("acpSend")->click();
        QTRY_VERIFY2_WITH_TIMEOUT(transcript().mid(before).contains(expect),
                                  qPrintable(modal() + " | " + transcript().right(1500)), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(status().startsWith("Connected") || status().startsWith("Ended"), 15000);
    }
    void expectTreeGone(const QList<nova::testing::ProcessIdentity> &tree) {
        for (const auto &process : tree)
            QTRY_VERIFY2_WITH_TIMEOUT(!alive(process.pid), qPrintable(QString::number(process.pid)), 10000);
    }

  private slots:
    void initTestCase() {
        QVERIFY(dir.isValid());
        qApp->setProperty("mcpBridgePath", bridge());
    }
    void init() {
        const auto base = dir.path() + '/' + QTest::currentTestFunction();
        root = base + "/Synthetic Pack";
        other = base + "/Other Pack";
        for (const auto &pack : {root, other}) {
            write(pack + "/pack.mcmeta", "{\"pack\":{\"min_format\":[75,0],\"max_format\":[75,0],\"description\":\"ACP\"}}");
            QImage image(16, 16, QImage::Format_RGBA8888);
            image.fill(Qt::gray);
            QVERIFY(QDir().mkpath(pack + "/assets/nova/textures/block"));
            QVERIFY(image.save(pack + "/assets/nova/textures/block/plain.png"));
        }
        qApp->setProperty("settingsFile", base + "/settings/studio.ini");
        // Agent Session is experimental and off by default; every other slot turns it on.
        if (QTest::currentTestFunction() != QByteArray("hiddenUntilEnabled"))
            QSettings(base + "/settings/studio.ini", QSettings::IniFormat).setValue("experimental/agentSession", true);
        qunsetenv("FAKE_ACP_SKIP_MCP");
        const auto evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        if (!evidence.isEmpty())
            qputenv("FAKE_ACP_DIAGNOSTICS", (evidence + "/fake-acp-" + QTest::currentTestFunction() + ".jsonl").toUtf8());
        else
            qunsetenv("FAKE_ACP_DIAGNOSTICS");
    }
    void cleanup() {
        dialog = nullptr;
        window.reset();
    }

    // Off by default: the command is hidden, cannot be triggered and is not offered by Search until
    // Preferences › Experimental turns it on.
    void hiddenUntilEnabled() {
        QElapsedTimer timer;
        timer.start();
        window = std::make_unique<nova::Window>(root, QString{}, timer);
        window->show();
        auto command = window->findChild<QAction *>("acpSession");
        QVERIFY(command);
        QVERIFY(!command->isVisible());
        QVERIFY(!command->isEnabled());
        command->trigger();
        QVERIFY(!window->findChild<QDialog *>("acpSessionDialog"));
        const auto offered = [this] {
            for (const auto &entry : window->searchIndex().entries)
                if (entry.action && entry.action->objectName() == "acpSession")
                    return true;
            return false;
        };
        QVERIFY(!offered());

        QTimer::singleShot(150, window.get(), [] {
            auto preferences = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(preferences && preferences->objectName() == "preferencesDialog");
            auto box = preferences->findChild<QCheckBox *>("preferenceExperimentalAgentSession");
            QVERIFY(box && !box->isChecked());
            box->setChecked(true);
            preferences->accept();
        });
        window->findChild<QAction *>("preferencesCommand")->trigger();
        QVERIFY(command->isVisible());
        QVERIFY(command->isEnabled());
        QVERIFY(offered());
        command->trigger();
        dialog = window->findChild<QDialog *>("acpSessionDialog");
        QVERIFY(dialog);
        QVERIFY(dialog->windowTitle().contains("Experimental"));
        QVERIFY(!session());
    }

    // Nothing starts and nothing is granted until the user agrees; the question names the
    // project, the program and the access.
    void consentRequired() {
        open();
        {
            Answer cancel(QMessageBox::Cancel);
            button("acpStart")->click();
            QTRY_VERIFY(!cancel.title.isEmpty());
            QCOMPARE(cancel.title, "Start agent session?");
            QVERIFY(cancel.text.contains(agent()));
            QVERIFY(cancel.text.contains("“Synthetic Pack”"));
            QVERIFY(cancel.text.contains("read-only"));
        }
        QCOMPARE(service()->status(), "Disabled");
        QVERIFY(service()->connectionCode().isEmpty());
        QVERIFY(!session());
        QVERIFY(!button("acpEditing")->isEnabled());
    }

    // Read-only tools work; editing and saving are refused; nothing in the pack changes.
    void readOnlySession() {
        open();
        const auto before = fingerprint(root);
        startReadOnly();
        ask("call nova.assets.list {}", "RESULT ok");
        // The window is a conversation: the user, the agent's reply and its reasoning; no tool activity.
        const auto shown = transcript();
        QVERIFY2(shown.contains("You\ncall nova.assets.list {}") && shown.contains("Agent\nRESULT ok") &&
                 shown.contains("Agent · thinking\nThinking about: call nova.assets.list"), qPrintable(shown.left(600)));
        for (const auto hidden : {"[agent tool]", "in_progress", "completed", "[Nova]", "tool_call", "Session started"})
            QVERIFY2(!shown.contains(hidden), hidden);
        ask("call nova.drafts.save_all {}", "RESULT error");
        QVERIFY2(transcript().contains("permission_denied"), qPrintable(transcript().right(800)));
        ask("call nova.asset.create {\"namespace\":\"nova\",\"path\":\"block/x\",\"conflictPolicy\":\"reject\",\"parameters\":{\"command\":\"texture\",\"width\":16,\"height\":16,\"layerName\":\"Base\",\"fill\":\"transparent\",\"pixelFormat\":\"rgba8888\"}}",
            "RESULT error");
        QCOMPARE(fingerprint(root), before);
    }

    // A natural exchange renders as a conversation: speakers, reasoning in lighter text, formatted reply.
    // With NOVA_EVIDENCE_DIR set, the window as shown on the screen (frame included) is saved.
    void conversationScreenshot() {
        open();
        dialog->resize(820, 600);
        startReadOnly();
        ask("Please check which textures in this pack still need a 16×16 version.", "Nothing needs changing.");
        const auto shown = transcript();
        QVERIFY2(shown.contains("You\nPlease check") && shown.contains("Agent · thinking") &&
                 shown.contains("Agent\nI checked the textures in Synthetic Pack"), qPrintable(shown));
        QVERIFY(!shown.contains("**")); // Markdown is rendered, not shown raw
        // With no permission request pending, no permission controls are shown.
        for (const auto name : {"acpPermissionAllow", "acpPermissionReject"})
            QVERIFY2(!button(name)->isVisible(), name);
        QVERIFY(!dialog->findChild<QLabel *>("acpPermission")->isVisible());
        const auto evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        if (evidence.isEmpty())
            return;
        dialog->raise();
        dialog->activateWindow();
        QTest::qWait(500);
        const auto screen = dialog->screen();
        const auto frame = dialog->frameGeometry();
        const auto origin = frame.topLeft() - screen->geometry().topLeft();
        const auto pixels = screen->grabWindow(0, origin.x(), origin.y(), frame.width(), frame.height());
        QVERIFY(!pixels.isNull());
        QVERIFY(pixels.save(evidence + "/acp-conversation-window.png"));
    }

    // The grant accepts only a bridge started by the Nova-launched agent; others do not consume it.
    void foreignClientRefused() {
        qputenv("FAKE_ACP_SKIP_MCP", "1");
        open();
        Answer yes(QMessageBox::Yes);
        button("acpStart")->click();
        QTRY_VERIFY_WITH_TIMEOUT(status().startsWith("Connected"), 15000);
        const auto code = service()->connectionCode();
        QVERIFY(!code.isEmpty());
        QProcess outsider;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("NOVA_MCP_CODE", code);
        outsider.setProcessEnvironment(env);
        outsider.start(bridge());
        QVERIFY(outsider.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(service()->error().contains("outside this agent session"), 10000);
        QVERIFY(!service()->clientConnected());
        QCOMPARE(service()->connectionCode(), code); // still offered to the agent's own bridge
        outsider.closeWriteChannel();
        QVERIFY(outsider.waitForFinished(5000));
    }

    // Revoke ends Nova access but not the agent; a project switch ends the session and its processes.
    void revokeAndProjectSwitch() {
        open();
        startReadOnly();
        auto tree = session()->processTree();
        QVERIFY(!tree.isEmpty()); // the bridge
        button("acpRevoke")->click();
        QCOMPARE(service()->status(), "Disabled");
        QTRY_VERIFY(dialog->findChild<QLabel *>("acpAccess")->text().startsWith("Revoked"));
        ask("call nova.assets.list {}", "RESULT error");
        QVERIFY(status().startsWith("Connected")); // the agent keeps running without Nova's tools
        button("acpStop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(status().startsWith("Ended"), 10000);
        expectTreeGone(tree);

        startReadOnly();
        tree = session()->processTree();
        auto path = window->findChild<QLineEdit *>("projectPath");
        QVERIFY(path);
        path->setText(other);
        QTest::keyClick(path, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(status().contains("The project changed."), 10000);
        QCOMPARE(service()->status(), "Disabled");
        expectTreeGone(tree);
    }

    // A crashing agent loses the grant, and Nova ends the processes it left behind.
    void crashCleansUp() {
        open();
        startReadOnly();
        ask("spawn", "SPAWNED ");
        const auto pid = transcript().section("SPAWNED ", 1).section('\n', 0, 0).trimmed().toLongLong();
        QVERIFY(alive(pid));
        QTRY_VERIFY(session()->processTree().size() >= 2);
        const auto tree = session()->processTree();
        dialog->findChild<QLineEdit *>("acpPrompt")->setText("crash");
        button("acpSend")->click();
        QTRY_VERIFY_WITH_TIMEOUT(status().contains("code 3"), 10000);
        QCOMPARE(service()->status(), "Disabled");
        QVERIFY(service()->connectionCode().isEmpty());
        expectTreeGone(tree);
        QVERIFY(!alive(pid));
    }

    // Cancel ends the turn; the agent's own permission requests are shown and answered by the user.
    void cancelAndPermission() {
        open();
        startReadOnly();
        dialog->findChild<QLineEdit *>("acpPrompt")->setText("wait");
        button("acpSend")->click();
        QTRY_VERIFY(button("acpCancel")->isEnabled());
        button("acpCancel")->click();
        QTRY_COMPARE_WITH_TIMEOUT(notice(), QString("You cancelled the agent's turn."), 10000);
        QVERIFY(dialog->findChild<QLabel *>("acpNotice")->isVisible());
        dialog->findChild<QLineEdit *>("acpPrompt")->setText("ask");
        button("acpSend")->click();
        QTRY_VERIFY(button("acpPermissionReject")->isVisible());
        QVERIFY(dialog->findChild<QLabel *>("acpPermission")->text().contains("not a Nova tool"));
        button("acpPermissionReject")->click();
        QTRY_VERIFY_WITH_TIMEOUT(transcript().contains("PERMISSION selected no"), 10000);
    }

    // Stop ends the agent and everything it started, and revokes the grant.
    void stopEndsTree() {
        open();
        startReadOnly();
        const auto tree = session()->processTree();
        button("acpStop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(status().startsWith("Ended"), 10000);
        QVERIFY(status().contains("Stopped by you."));
        QCOMPARE(service()->status(), "Disabled");
        expectTreeGone(tree);
    }

    // Editing needs its own consent after a connected read-only session, and writes only into the
    // authorized synthetic pack.
    void editingAfterSeparateConsent() {
        open();
        QVERIFY(!button("acpEditing")->isEnabled());
        startReadOnly();
        QTRY_VERIFY(button("acpEditing")->isEnabled());
        const auto outside = fingerprint(other);
        {
            Answer yes(QMessageBox::Yes);
            button("acpEditing")->click();
            QTRY_VERIFY(!yes.title.isEmpty());
            QCOMPARE(yes.title, "Allow agent editing?");
            QVERIFY(yes.text.contains("Project: Synthetic Pack"));
            QVERIFY(yes.text.contains("save"));
        }
        QTRY_VERIFY_WITH_TIMEOUT(service()->clientConnected() && service()->permission() == "Editing allowed", 20000);
        QTRY_VERIFY_WITH_TIMEOUT(status().startsWith("Connected"), 15000);
        ask("call nova.project.info {}", "RESULT ok");
        const auto revision = transcript().section("\"indexRevision\":\"", 1).section('"', 0, 0);
        QVERIFY(!revision.isEmpty());
        ask(QString("call nova.asset.create {\"indexRevision\":\"%1\",\"namespace\":\"nova\",\"path\":\"block/agent_made\",\"conflictPolicy\":\"reject\",\"parameters\":{\"command\":\"texture\",\"width\":16,\"height\":16,\"layerName\":\"Base\",\"fill\":\"#336699\",\"pixelFormat\":\"rgba8888\"}}").arg(revision),
            "RESULT ok");
        ask("call nova.drafts.save_all {}", "RESULT ok");
        QTRY_VERIFY(QFileInfo::exists(root + "/assets/nova/textures/block/agent_made.png"));
        QCOMPARE(fingerprint(other), outside);
    }

    // Connection codes never reach the agent: not in its environment, the session request or the window.
    void secretsStayInNova() {
        qputenv("NOVA_MCP_CODE", "secret-code-that-must-not-leak");
        open();
        startReadOnly();
        ask("env", "ENV NOVA_MCP_CODE=absent");
        QVERIFY(!transcript().contains("secret-code-that-must-not-leak"));
        qunsetenv("NOVA_MCP_CODE");
        const auto server = nova::acp::stdioServer("nova", bridge());
        QCOMPARE(server["env"].toArray().size(), 0);
        QVERIFY(!server.contains("type"));
        QVERIFY(QFileInfo(server["command"].toString()).isAbsolute());
        QString problem;
        QVERIFY(!nova::acp::acceptableExecutable("C:/Windows/System32/cmd.cmd", &problem));
        QVERIFY(!nova::acp::acceptableExecutable("opencode.exe", &problem));
    }
};

QTEST_MAIN(AcpSessionTest)
#include "acp_session_test.moc"
