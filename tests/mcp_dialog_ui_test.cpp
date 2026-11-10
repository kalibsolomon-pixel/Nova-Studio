// SPDX-License-Identifier: GPL-3.0-or-later
// MCP Integration dialog presentation: bounded sections (connection status, authorization,
// selected client, bridge, setup, diagnostics), a clearly bounded agent selector, full
// (wrapped, never elided) bridge path, accessible names, keyboard navigation, and the
// unchanged consent and grant behaviour. Client detection runs against a temporary
// profile; no external agent, account or provider is contacted. With NOVA_EVIDENCE_DIR
// set, both tabs are captured in the dark and light themes.
#include "service.h"
#include "window.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>
#include <optional>

using namespace nova;
namespace {
bool write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
// Answers the next message box with `choice` and records its title and text.
struct Answer {
    QString title, text;
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton;
    explicit Answer(QMessageBox::StandardButton choice) {
        QTimer::singleShot(0, [this, choice] {
            QTRY_VERIFY(qobject_cast<QMessageBox *>(QApplication::activeModalWidget()));
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            title = box->windowTitle();
            text = box->text();
            defaultButton = box->standardButton(box->defaultButton());
            box->button(choice)->click();
        });
    }
};
} // namespace

class McpDialogUiTests : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QString evidence, root;
    QMap<QByteArray, std::optional<QByteArray>> previous;

    QDialog *openDialog(Window &window, int tab) {
        window.findChild<QAction *>("mcpAccess")->trigger();
        auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
        if (dialog)
            dialog->findChild<QTabWidget *>("mcpTabs")->setCurrentIndex(tab);
        return dialog;
    }
    void capture(QWidget *w, const QString &name) {
        if (evidence.isEmpty())
            return;
        QDir().mkpath(evidence);
        QTest::qWait(80);
        QVERIFY(w->grab().save(evidence + "/mcp-" + name + ".png"));
    }

  private slots:
    void initTestCase() {
        evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        // Every client location of this process points at an empty temporary profile.
        const auto base = temporary.filePath("profile");
        for (const auto &[name, value] : std::initializer_list<std::pair<const char *, QString>>{
                 {"LOCALAPPDATA", base + "/Local"}, {"APPDATA", base + "/Roaming"}, {"USERPROFILE", base + "/Profile"},
                 {"CLAUDE_CONFIG_DIR", base + "/claude"}, {"CODEX_HOME", base + "/codex"}}) {
            previous[name] = qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name)) : std::nullopt;
            QDir().mkpath(value);
            qputenv(name, QDir::toNativeSeparators(value).toUtf8());
        }
        root = temporary.filePath("Dialog Pack");
        QVERIFY(write(root + "/pack.mcmeta", R"({"pack":{"pack_format":75,"description":"dialog"}})"));
        qApp->setProperty("settingsFile", temporary.filePath("dialog.ini"));
    }
    void cleanupTestCase() {
        for (auto it = previous.cbegin(); it != previous.cend(); ++it)
            it.value() ? qputenv(it.key(), *it.value()) : qunsetenv(it.key());
    }
    void sectionsAndBoundedAgentSelector() {
        QElapsedTimer started;
        started.start();
        Window window(root, {}, started);
        window.show();
        auto dialog = openDialog(window, 0);
        QVERIFY(dialog);
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        auto in = [dialog](const char *section, const char *child) {
            auto box = dialog->findChild<QGroupBox *>(section);
            auto widget = dialog->findChild<QWidget *>(child);
            return box && widget && box->isAncestorOf(widget);
        };
        QVERIFY(in("mcpStatusSection", "mcpAccessStatus"));
        QVERIFY(in("mcpStatusSection", "mcpAccessPermission"));
        QVERIFY(in("mcpStatusSection", "mcpProject"));
        QVERIFY(in("mcpAuthorizationSection", "mcpAuthorize"));
        QVERIFY(in("mcpAuthorizationSection", "mcpAuthorizeEditing"));
        QVERIFY(in("mcpAuthorizationSection", "mcpRevoke"));
        QCOMPARE(dialog->findChild<QGroupBox *>("mcpStatusSection")->title(), QString("Connection status"));
        QCOMPARE(dialog->findChild<QGroupBox *>("mcpAuthorizationSection")->title(), QString("Authorization"));
        // Consent wording is unchanged.
        bool wording = false;
        for (auto label : dialog->findChildren<QLabel *>())
            wording |= label->text() ==
                       "MCP lets an AI agent on this PC inspect the open resource pack and, only if you allow editing, "
                       "change it through Nova Studio. Access is off until you choose Allow. It covers the current "
                       "project only and ends when the agent disconnects, the project changes or you revoke it. An "
                       "unused authorization expires after five minutes.";
        QVERIFY(wording);
        capture(dialog, "access-dark");

        dialog->findChild<QTabWidget *>("mcpTabs")->setCurrentIndex(1);
        auto agent = dialog->findChild<QComboBox *>("mcpAgentChoice");
        QTRY_COMPARE(agent->count(), 4);
        QVERIFY(in("mcpAgentSection", "mcpAgentChoice"));
        QVERIFY(in("mcpAgentSection", "mcpAgentRecheck"));
        QVERIFY(in("mcpAgentSection", "mcpAgentStatus"));
        QVERIFY(in("mcpBridgeSection", "mcpBridgePath"));
        QVERIFY(in("mcpBridgeSection", "mcpCopyBridgePath"));
        QVERIFY(in("mcpSetupSection", "mcpAgentConfigure"));
        QVERIFY(in("mcpSetupSection", "mcpAgentConfiguration"));
        QVERIFY(in("mcpDiagnosticsSection", "mcpRunDiagnostics"));
        QVERIFY(in("mcpDiagnosticsSection", "mcpDiagnosticsResults"));
        // The selector's box is visibly framed: it is a section with a title and a border.
        auto agentBox = dialog->findChild<QGroupBox *>("mcpAgentSection");
        QCOMPARE(agentBox->title(), QString("Selected client"));
        QCOMPARE(agentBox->property("role").toString(), QString("section"));
        QVERIFY(agentBox->contentsRect().contains(agentBox->mapFromGlobal(agent->mapToGlobal(agent->rect().center()))));
        QVERIFY(agentBox->contentsRect().top() > 0);
        // Accessible names and the label buddy.
        QCOMPARE(agent->accessibleName(), QString("AI agent"));
        QCOMPARE(dialog->findChild<QPlainTextEdit *>("mcpBridgePath")->accessibleName(), QString("MCP bridge path"));
        QCOMPARE(dialog->findChild<QPushButton *>("mcpAgentRecheck")->accessibleName(), QString("Check agents again"));
        QCOMPARE(dialog->findChild<QPlainTextEdit *>("mcpAgentConfiguration")->accessibleName(),
                 QString("Configuration for the selected agent"));
        QCOMPARE(dialog->findChild<QPlainTextEdit *>("mcpDiagnosticsResults")->accessibleName(), QString("Diagnostics results"));
        bool buddy = false;
        for (auto label : agentBox->findChildren<QLabel *>())
            buddy |= label->buddy() == agent && label->text() == "&Agent";
        QVERIFY(buddy);
        for (auto button : dialog->findChildren<QPushButton *>())
            QVERIFY2(!button->text().isEmpty() || !button->accessibleName().isEmpty(), qPrintable(button->objectName()));
        capture(dialog, "setup-dark");
        dialog->close();
        QVERIFY(window.close());
    }
    void bridgePathIsNeverTruncated() {
        QElapsedTimer started;
        started.start();
        Window window(root, {}, started);
        window.show();
        auto dialog = openDialog(window, 1);
        QVERIFY(dialog);
        auto path = dialog->findChild<QPlainTextEdit *>("mcpBridgePath");
        QTRY_VERIFY(!path->toPlainText().isEmpty());
        const auto bridge = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/nova-mcp.exe");
        // Whatever the state, the text is complete: the full path, or the full missing-bridge message.
        QVERIFY2(path->toPlainText().startsWith(bridge) ||
                     path->toPlainText() == "MCP bridge missing: nova-mcp.exe is not next to Nova Studio.",
                 qPrintable(path->toPlainText()));
        QVERIFY(path->isReadOnly());
        QCOMPARE(path->wordWrapMode(), QTextOption::WrapAtWordBoundaryOrAnywhere);
        QCOMPARE(path->lineWrapMode(), QPlainTextEdit::WidgetWidth);
        // Narrow dialog: the path wraps instead of being cut; no horizontal scrolling.
        dialog->resize(420, dialog->height());
        QTest::qWait(80);
        QCOMPARE(path->horizontalScrollBar()->maximum(), 0);
        capture(dialog, "setup-narrow");
        dialog->close();
        QVERIFY(window.close());
    }
    void keyboardNavigation() {
        QElapsedTimer started;
        started.start();
        Window window(root, {}, started);
        window.show();
        auto dialog = openDialog(window, 1);
        QVERIFY(dialog);
        dialog->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(dialog));
        auto agent = dialog->findChild<QComboBox *>("mcpAgentChoice");
        QTRY_COMPARE(agent->count(), 4);
        QTRY_VERIFY(!dialog->findChild<QLabel *>("mcpAgentStatus")->text().startsWith("Checking"));
        // Alt+A (the label's mnemonic) focuses the selector; arrows change the selection.
        QTest::keyClick(dialog, Qt::Key_A, Qt::AltModifier);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(agent));
        const int before = agent->currentIndex();
        QTest::keyClick(agent, before == agent->count() - 1 ? Qt::Key_Up : Qt::Key_Down);
        QVERIFY(agent->currentIndex() != before);
        // Tab moves through the section controls in reading order.
        QStringList order;
        for (int i = 0; i < 6; ++i) {
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
            order << QApplication::focusWidget()->objectName();
        }
        const QStringList expected = {"mcpAgentRecheck", "mcpBridgePath", "mcpCopyBridgePath"};
        QVERIFY2(order.mid(0, 3) == expected, qPrintable(order.join(", ")));
        QVERIFY2(order.contains("mcpAgentConfiguration") || order.contains("mcpCopyConfiguration"), qPrintable(order.join(", ")));
        // Focus is visible: the focused push button uses the accent border rule.
        QVERIFY(qApp->styleSheet().contains("QPushButton:focus{border-color:"));
        dialog->close();
        QVERIFY(window.close());
    }
    void consentAndGrantBehaviourUnchanged() {
        QElapsedTimer started;
        started.start();
        Window window(root, {}, started);
        window.show();
        auto dialog = openDialog(window, 0);
        QVERIFY(dialog);
        auto service = window.findChild<mcp::Service *>();
        QVERIFY(service);
        auto status = dialog->findChild<QLabel *>("mcpAccessStatus");
        auto overview = dialog->findChild<QLabel *>("mcpOverview");
        QVERIFY(overview->text().contains("Access off"));
        QCOMPARE(service->permission(), QString("Disabled"));
        QVERIFY(!service->offeredToAgents());
        // Editing consent: same question, Cancel is the default and grants nothing.
        {
            Answer answer(QMessageBox::Cancel);
            dialog->findChild<QPushButton *>("mcpAuthorizeEditing")->click();
            QCOMPARE(answer.title, QString("Allow MCP editing?"));
            QVERIFY(answer.text.startsWith("This connection can create unsaved textures and animations"));
            // The consent names what saving does and which project it is for.
            QVERIFY2(answer.text.contains("save open documents into the pack’s files on disk"), qPrintable(answer.text));
            QVERIFY2(answer.text.contains("Project: " + QFileInfo(root).fileName()), qPrintable(answer.text));
            QCOMPARE(answer.defaultButton, QMessageBox::Cancel);
        }
        QCOMPARE(service->permission(), QString("Disabled"));
        QVERIFY(!service->offeredToAgents());
        QCOMPARE(status->text(), QString("Disabled"));
        // Yes offers one editing grant for this project only.
        {
            Answer answer(QMessageBox::Yes);
            dialog->findChild<QPushButton *>("mcpAuthorizeEditing")->click();
        }
        QCOMPARE(status->text(), QString("Waiting for client"));
        QVERIFY(service->offeredToAgents());
        QCOMPARE(service->permission(), QString("Editing allowed"));
        // Revoke withdraws it; read-only Allow offers a read-only grant.
        dialog->findChild<QPushButton *>("mcpRevoke")->click();
        QCOMPARE(service->permission(), QString("Disabled"));
        QVERIFY(!service->offeredToAgents());
        dialog->findChild<QPushButton *>("mcpAuthorize")->click();
        QCOMPARE(service->permission(), QString("Read-only"));
        QVERIFY(overview->text().contains("waiting for an agent"));
        QVERIFY(dialog->findChild<QLabel *>("mcpAccessPermission")->text().endsWith("Read-only"));
        // The setup page never grants access.
        dialog->findChild<QPushButton *>("mcpRevoke")->click();
        dialog->findChild<QTabWidget *>("mcpTabs")->setCurrentIndex(1);
        QTRY_COMPARE(dialog->findChild<QComboBox *>("mcpAgentChoice")->count(), 4);
        QCOMPARE(service->permission(), QString("Disabled"));
        QVERIFY(!service->offeredToAgents());
        // Light theme captures.
        window.findChild<QAction *>("darkThemeCommand")->trigger();
        QTest::qWait(80);
        capture(dialog, "setup-light");
        dialog->findChild<QTabWidget *>("mcpTabs")->setCurrentIndex(0);
        capture(dialog, "access-light");
        window.findChild<QAction *>("darkThemeCommand")->trigger();
        dialog->close();
        QVERIFY(window.close());
    }
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication application(argc, argv);
    application.setStyle("Fusion");
    QFont font = application.font();
    font.setPointSize(9);
    application.setFont(font);
    McpDialogUiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "mcp_dialog_ui_test.moc"
