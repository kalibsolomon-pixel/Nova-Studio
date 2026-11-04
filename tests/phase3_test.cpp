// SPDX-License-Identifier: GPL-3.0-or-later
#include "creation.h"
#include "diagnostics.h"
#include "modelpanel.h"
#include "recovery.h"
#include "window.h"
#include <QtTest>
#include <QtWidgets>

namespace nova {
class EditorTestAccess {
  public:
    static ModelPanel *panel(Window &w) { return w.modelPanel; }
    static bool busy(Window &w) { return w.indexBusy; }
    static QString project(Window &w) { return w.project; }
    static bool confirm(Window &w) { return w.confirmClose(w.current()); }
    static void recover(Window &w) { w.recoverSession(); }
    static void save(Window &w) { w.action("save"); }
};
}
using namespace nova;
namespace {
QImage image(QColor color = Qt::darkCyan) {
    QImage result(16, 16, QImage::Format_RGBA8888);
    result.fill(color);
    return result;
}
bool fixture(const QString &root) {
    const auto path = root + "/assets/nova/";
    QDir().mkpath(path + "textures/block");
    QDir().mkpath(path + "models/block");
    QFile model(path + "models/block/a.json");
    return image().save(path + "textures/block/a.png") && model.open(QIODevice::WriteOnly) &&
           model.write(QJsonDocument(mc::modelTemplate(mc::ModelTemplate::MultiElement,
                                                       "nova:block/a")).toJson()) > 0;
}
class Answer : public QTimer {
  public:
    int count = 0;
    Answer(QMessageBox::StandardButton choice) {
        setInterval(15);
        connect(this, &QTimer::timeout, this, [this, choice] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                if (auto button = box->button(choice)) {
                    ++count;
                    button->click();
                }
        });
        start();
    }
};
void capture(QWidget &widget, const QString &name) {
    auto directory = qEnvironmentVariable("NOVA_PHASE3_EVIDENCE_DIR");
    if (!directory.isEmpty()) {
        QDir().mkpath(directory);
        widget.grab().save(directory + '/' + name + ".png");
    }
}
}
class Phase3Tests : public QObject {
    Q_OBJECT
  private slots:
    void closeProjectHonorsUnsavedModelCancel() {
        QTemporaryDir tmp;
        QVERIFY(fixture(tmp.path()));
        qApp->setProperty("settingsFile", tmp.path() + "/settings.ini");
        QElapsedTimer timer; timer.start();
        Window window(tmp.path(), {}, timer); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.activeCanvas(), 10000);
        auto panel = EditorTestAccess::panel(window);
        QTRY_VERIFY_WITH_TIMEOUT(panel->catalog()->modelIds().contains("nova:block/a"), 10000);
        auto dock = window.findChild<QDockWidget *>("modelsUv");
        dock->show(); dock->raise();
        panel->openAsset("assets/nova/models/block/a.json");
        panel->findChild<QToolButton *>("displayToggle")->click();
        QTRY_VERIFY(panel->findChild<QWidget *>("displayEditor")->isVisible());
        panel->findChild<QDoubleSpinBox *>("displayRotationY")->setValue(37);
        panel->findChild<QPushButton *>("displayApply")->click();
        QVERIFY(!panel->documentSources().isEmpty());
        {
            Answer answer(QMessageBox::Cancel);
            window.findChild<QAction *>("closeProjectCommand")->trigger();
            QCOMPARE(answer.count, 1);
        }
        QCOMPARE(EditorTestAccess::project(window), QDir(tmp.path()).absolutePath());
        QVERIFY(!panel->documentSources().isEmpty());
        QVERIFY(window.activeCanvas());
        {
            Answer answer(QMessageBox::Discard);
            window.findChild<QAction *>("closeProjectCommand")->trigger();
            QCOMPARE(answer.count, 1);
        }
        QVERIFY(EditorTestAccess::project(window).isEmpty());
        QVERIFY(panel->documentSources().isEmpty());
        QVERIFY(!panel->modelHistory(false)); // no history from the closed project remains routable
        QVERIFY(!EditorTestAccess::busy(window));
        QVERIFY(!window.findChild<QTimer *>("assetReconciliation")->isActive());
        QVERIFY(window.close());
    }
    void switchImmediatelyInvalidatesModelCatalog() {
        QTemporaryDir first, second;
        QVERIFY(fixture(first.path())); QVERIFY(fixture(second.path()));
        qApp->setProperty("settingsFile", first.path() + "/settings.ini");
        QElapsedTimer timer; timer.start();
        Window window(first.path(), {}, timer); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.activeCanvas(), 10000);
        auto panel = EditorTestAccess::panel(window);
        QTRY_VERIFY_WITH_TIMEOUT(panel->catalog()->modelIds().contains("nova:block/a"), 10000);
        panel->openAsset("assets/nova/models/block/a.json");
        auto previous = panel->catalog();
        auto path = window.findChild<QLineEdit *>("projectPath");
        path->setText(second.path()); emit path->returnPressed();
        QVERIFY(panel->catalog() != previous);
        QVERIFY(panel->modelAsset().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(window.activeCanvas(), 10000);
        QVERIFY(window.activeCanvas()->document->path.startsWith(second.path()));
        QVERIFY(window.close());
    }
    void pendingPasteParticipatesInCloseAndSave() {
        QTemporaryDir tmp; QVERIFY(fixture(tmp.path()));
        qApp->setProperty("settingsFile", tmp.path() + "/settings.ini");
        QElapsedTimer timer; timer.start();
        Window window(tmp.path(), {}, timer); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.activeCanvas(), 10000);
        auto canvas = window.activeCanvas();
        QVERIFY(!canvas->document->modified());
        canvas->beginFloating(image(Qt::red).copy(0, 0, 2, 2), {2, 2}, {}, "Paste");
        QVERIFY(canvas->hasFloating());
        {
            Answer answer(QMessageBox::Cancel);
            QVERIFY(!EditorTestAccess::confirm(window));
            QCOMPARE(answer.count, 1);
        }
        QVERIFY(canvas->document->modified());
        QVERIFY(!canvas->hasFloating());
        QCOMPARE(canvas->document->image.pixelColor(2, 2), QColor(Qt::red));
        canvas->setFocus();
        QTest::keyClick(canvas, Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY(!canvas->document->modified());
        QCOMPARE(loadPng(canvas->document->path).pixelColor(2, 2), QColor(Qt::red));
        QVERIFY(window.close());
    }
    void failedFloatingCommitRetainsPreview() {
        auto doc = std::make_shared<Document>(QString(), image());
        Metrics metrics; DocumentSession session;
        Canvas canvas(doc, &metrics); canvas.session = &session;
        canvas.beginFloating(image(Qt::red).copy(0, 0, 2, 2), {2, 2}, {}, "Paste");
        const auto before = canvas.displayed();
        QVERIFY(!canvas.commitFloating()); // injected failure: document not in this session
        QVERIFY(canvas.hasFloating());
        QCOMPARE(canvas.displayed(), before);
        QVERIFY(!doc->modified());
        QVERIFY(session.adopt(doc));
        QVERIFY(canvas.commitFloating());
        QVERIFY(!canvas.hasFloating());
        QCOMPARE(doc->image.pixelColor(2, 2), QColor(Qt::red));
    }
    void recoveredCanvasUsesWorkspaceTools_data() {
        QTest::addColumn<bool>("native");
        QTest::newRow("plain") << false;
        QTest::newRow("layered") << true;
    }
    void recoveredCanvasUsesWorkspaceTools() {
        QFETCH(bool, native);
        QTemporaryDir tmp;
        const auto path = tmp.path() + "/a.png";
        QVERIFY(image().save(path));
        auto doc = Document::open(path); QVERIFY(doc);
        if (native) {
            QVERIFY(doc->addLayerWithPixels("Overlay", image(Qt::transparent)));
            QVERIFY(doc->saveNative(tmp.path()));
        }
        doc->draw({1, 1}, {3, 1}, qRgba(240, 30, 60, 255)); doc->finish();
        const auto expected = doc->image;
        RecoveryStore store(tmp.path() + "/recovery");
        QVERIFY(store.writeDocument(*doc, tmp.path()));
        qApp->setProperty("settingsFile", tmp.path() + "/settings.ini");
        QElapsedTimer timer; timer.start();
        Window window({}, {}, timer); window.show();
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box && box->objectName() == "recoveryDialog")
                for (auto button : box->buttons())
                    if (button->text() == "Restore") { button->click(); break; }
        });
        answer.start(15);
        EditorTestAccess::recover(window);
        QTRY_VERIFY_WITH_TIMEOUT(window.activeCanvas(), 5000);
        answer.stop();
        auto canvas = window.activeCanvas();
        QCOMPARE(canvas->tools, &window.tools());
        QCOMPARE(canvas->session, &window.documents());
        QVERIFY(bool(canvas->contextMenu)); QVERIFY(bool(canvas->transformLayers));
        QCOMPARE(canvas->document->image, expected);
        QCOMPARE(canvas->document->nativeDocument(), native);
        Answer discard(QMessageBox::Discard);
        QVERIFY(window.close());
    }
    void compactAndNormalWorkspaceGallery_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("dark") << true;
        QTest::newRow("light") << false;
    }
    void compactAndNormalWorkspaceGallery() {
        QFETCH(bool, dark);
        QTemporaryDir tmp;
        qApp->setProperty("settingsFile", tmp.path() + "/settings.ini");
        Preferences preferences; preferences.dark = dark;
        QVERIFY(Settings(tmp.path() + "/settings.ini").save(preferences));
        QElapsedTimer timer; timer.start();
        Window window({}, {}, timer); window.show();
        QTest::qWait(100);
        QVERIFY(!window.windowIcon().pixmap(32, 32).isNull());
        for (const auto name : {"welcomeLogo", "toolbarLogo"}) {
            const auto logo = window.findChild<QLabel *>(name);
            QVERIFY(logo);
            QVERIFY(!logo->pixmap().isNull());
            QCOMPARE(logo->accessibleName(), "Nova Studio logo");
        }
        QTRY_COMPARE(window.focusWidget(), window.findChild<QPushButton *>("welcomeCreate"));
        capture(window, dark ? "welcome-normal-dark" : "welcome-normal-light");
        window.resize(800, 550); QTest::qWait(60);
        capture(window, dark ? "welcome-compact-dark" : "welcome-compact-light");
        auto welcome = window.findChild<QScrollArea *>("welcomeActions");
        QVERIFY(welcome);
        QCOMPARE(welcome->horizontalScrollBar()->maximum(), 0);
        for (auto name : {"welcomeCreate", "welcomeOpen", "welcomeRecent"}) {
            auto button = window.findChild<QPushButton *>(name);
            QVERIFY(button);
            welcome->ensureWidgetVisible(button);
            QVERIFY(button->height() >= button->sizeHint().height());
            QVERIFY(welcome->viewport()->rect().contains(
                QRect(button->mapTo(welcome->viewport(), QPoint()), button->size())));
        }
        QVERIFY(window.close());
    }
    void helpAndAccessibleAssetControls() {
        QTemporaryDir tmp;
        qApp->setProperty("settingsFile", tmp.path() + "/settings.ini");
        QElapsedTimer timer; timer.start();
        Window window({}, {}, timer); window.show();
        QCOMPARE(window.findChild<QLineEdit *>("assetSearch")->accessibleName(),
                 QString("Search project assets"));
        QCOMPARE(window.findChild<QTreeView *>("assetTree")->accessibleName(), QString("Project assets"));
        auto help = window.findChild<QAction *>("gettingStartedCommand");
        QVERIFY(help); QCOMPARE(help->shortcut(), QKeySequence::HelpContents);
        help->trigger();
        auto dialog = window.findChild<QDialog *>("gettingStartedDialog");
        QVERIFY(dialog && dialog->isVisible());
        QVERIFY(!dialog->isModal());
        QVERIFY(dialog->findChild<QTextBrowser *>()->toPlainText().contains("Ctrl+S"));
        capture(*dialog, "getting-started");
        dialog->close();
        QVERIFY(window.close());
    }
    void diagnosticExportAndFailure() {
        QTemporaryDir tmp;
        const auto path = tmp.path() + "/diagnostics.json";
        QString error;
        QVERIFY(diagnostics::exportReport(path, &error));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        QVERIFY(bytes.size() < 96 * 1024);
        const auto json = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(json["format"].toString(), QString("nova-local-diagnostics"));
        QCOMPARE(json["application_version"].toString(), QString("0.1.0-preview.1"));
        QVERIFY(!json["source_revision"].toString().isEmpty());
        QVERIFY(!json["automatic_upload"].toBool(true));
        QVERIFY(!json.contains("environment")); QVERIFY(!json.contains("project"));
        QVERIFY(!diagnostics::exportReport(tmp.path() + "/missing/diagnostics.json", &error));
        QVERIFY(!error.isEmpty());
    }
    void diagnosticLogBudgetAndRotation() {
        QTemporaryDir tmp;
        struct RestoreHandler {
            QtMessageHandler previous = qInstallMessageHandler(nullptr);
            ~RestoreHandler() { qInstallMessageHandler(previous); }
        } restore;
        QVERIFY(diagnostics::start(tmp.path()));
        QFile file(diagnostics::logPath());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
        const qint64 limit = 1024 * 1024;
        const auto padding = QByteArray(int(limit - 128 - file.size()), 'x');
        QCOMPARE(file.write(padding), qint64(padding.size()));
        file.close();
        const auto before = QFileInfo(file).size();
        qWarning().noquote() << QString(1024, 'y');
        QCOMPARE(QFileInfo(file).size(), before); // oversized message is refused
        qWarning("small test message");
        QVERIFY(QFileInfo(file).size() <= limit);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto expected = file.readAll(); file.close();
        QVERIFY(diagnostics::start(tmp.path()));
        QFile previous(tmp.path() + "/studio-previous.log");
        QVERIFY(previous.open(QIODevice::ReadOnly));
        QCOMPARE(previous.readAll(), expected);
        QVERIFY(QFileInfo(diagnostics::logPath()).size() < 4096);
    }
    void nativeExceptionContextIsLocalAndBounded() {
        QTemporaryDir tmp;
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationDirPath() + "/nova-diagnostics-crash-probe.exe", {tmp.path()});
        QVERIFY(child.waitForFinished(10000));
        QVERIFY(child.exitCode() != 0);
        QFile context(tmp.path() + "/native-crash-context.txt");
        QVERIFY(context.open(QIODevice::ReadOnly));
        const auto bytes = context.readAll();
        QVERIFY(bytes.size() < 512);
        QVERIFY(bytes.contains("Nova Studio 0.1.0-preview.1 source="));
        QVERIFY(bytes.contains("native exception=0xE0424E56 address=0x"));
    }
};
QTEST_MAIN(Phase3Tests)
#include "phase3_test.moc"
