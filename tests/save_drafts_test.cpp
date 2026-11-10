// SPDX-License-Identifier: GPL-3.0-or-later
// MCP Phase 1: Save writes into the pack, Save All reports partial failures, the unsaved count,
// style notes, and the MCP draft list / adoption / save tools with their authorization rules.
#include "window.h"
#include "modelpanel.h"
#include "projectdialogs.h"
#include "service.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QtTest>

using namespace nova::mcp;

namespace nova {
class EditorTestAccess {
  public:
    static void open(Window &window, const QString &file) { window.openTexture(file); }
    static void action(Window &window, const QString &name) { window.action(name); }
    static bool dirty(Window &window, const std::shared_ptr<Document> &doc) { return window.documentDirty(*doc); }
    static QList<SaveOutcome> saveAll(Window &window) { return window.saveAllDocuments(true); }
    static QStringList unsaved(Window &window) { return window.unsavedDocuments(false); }
    static QLabel *unsavedLabel(Window &window) { return window.unsavedLabel; }
    static void checkpoint(Window &window) { window.backgroundSafety(); }
    static bool checkpointBusy(Window &window) { return window.ioBusy; }
    static QList<QUuid> recovered(Window &window) {
        QList<QUuid> ids;
        for (const auto &entry : window.recovery->entries())
            ids << entry.id;
        return ids;
    }
    static QString documentId(Window &window, const std::shared_ptr<Document> &doc) {
        return window.documentIdentifier(doc->instanceId());
    }
    static QString tabText(Window &window, const std::shared_ptr<Document> &doc) {
        for (int i = 0; i < window.tabs->count(); ++i)
            if (auto canvas = dynamic_cast<Canvas *>(window.tabs->widget(i)); canvas && canvas->document == doc)
                return window.tabs->tabText(i);
        return {};
    }
    static std::shared_ptr<Document> active(Window &window) {
        return window.current() ? window.current()->document : nullptr;
    }
    static bool indexReady(Window &window) {
        return !window.indexBusy && !window.indexDebounce->isActive() && window.pendingChanges.isEmpty();
    }
    // The window looks for recoverable snapshots once, 800 ms after it opens, in the background, and offers
    // this project's earlier snapshots. Slots wait until the check has run, so it cannot interleave with
    // their own dialogs. The scan can finish between polls; past 2.5 s with no scan pending, it has run.
    static void waitForRecoveryCheck(Window &window, const QElapsedTimer &opened) {
        bool seen = false;
        auto checked = [&] {
            bool running = false;
            for (auto watcher : window.findChildren<QFutureWatcherBase *>())
                if (dynamic_cast<QFutureWatcher<QList<RecoveryEntry>> *>(watcher)) {
                    seen = true;
                    running |= !watcher->isFinished();
                }
            return opened.elapsed() >= 800 && !running && (seen || opened.elapsed() >= 2500);
        };
        QTRY_VERIFY_WITH_TIMEOUT(checked(), 10000);
    }
};
} // namespace nova

namespace {
QString bridge() { return QString::fromUtf8(NOVA_MCP_TEST_BRIDGE); }
void write(const QString &file, const QByteArray &data) {
    QVERIFY(QDir().mkpath(QFileInfo(file).path()));
    QFile output(file);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write(data), data.size());
}
QByteArray bytes(const QString &file) {
    QFile input(file);
    return input.open(QIODevice::ReadOnly) ? input.readAll() : QByteArray();
}
QMap<QString, QByteArray> fingerprint(const QString &root) {
    QMap<QString, QByteArray> hashes;
    QDirIterator files(root, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto file = files.next();
        if (QDir(root).relativeFilePath(file).startsWith(".nova/"))
            continue; // editor recovery checkpoints are not pack content
        hashes.insert(QDir(root).relativeFilePath(file), QCryptographicHash::hash(bytes(file), QCryptographicHash::Sha256));
    }
    return hashes;
}
QImage png(int width, int height, QColor color) {
    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(color);
    return image;
}
QImage disk(const QString &file) { return QImage(file).convertToFormat(QImage::Format_RGBA8888); }
QByteArray request(int id, const QString &method, const QJsonObject &params = {}) {
    QJsonObject data{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (id >= 0)
        data["id"] = id;
    return QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n';
}
QByteArray call(int id, const QString &tool, const QJsonObject &args = {}) {
    return request(id, "tools/call", {{"name", tool}, {"arguments", args}});
}
void start(QProcess &process, const QString &code, const QString &program = bridge(), const QStringList &arguments = {}) {
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("NOVA_MCP_CODE", code);
    process.setProcessEnvironment(env);
    process.start(program, arguments);
    QVERIFY(process.waitForStarted());
}
QJsonObject receive(QProcess &process, int timeout = 10000) {
    if (!QTest::qWaitFor([&] { return process.canReadLine() || process.state() == QProcess::NotRunning; }, timeout))
        return {};
    const auto line = process.readLine();
    if (const auto evidence = qEnvironmentVariable("NOVA_MCP_EVIDENCE"); !evidence.isEmpty()) {
        QFile file(evidence + "/phase1-protocol.jsonl");
        if (file.open(QIODevice::Append))
            file.write(line);
    }
    return QJsonDocument::fromJson(line).object();
}
QJsonObject payload(const QJsonObject &response) {
    const auto result = response["result"].toObject();
    if (!result["isError"].toBool())
        return result["structuredContent"].toObject();
    if (result.contains("structuredContent"))
        return {{"unexpected", "structured content on an error"}};
    return QJsonDocument::fromJson(result["content"].toArray().first().toObject()["text"].toString().toUtf8()).object();
}
QString error(const QJsonObject &response) { return payload(response)["error"].toObject()["code"].toString(); }
QJsonObject initialize(QProcess &process) {
    process.write(request(1, "initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}},
        {"clientInfo", QJsonObject{{"name", "Nova phase 1 tests"}, {"version", "1"}}}}));
    const auto reply = receive(process);
    process.write(request(-1, "notifications/initialized"));
    return reply["result"].toObject();
}
// Grants access in the MCP dialog; editing asks for confirmation.
QString authorize(QWidget &window, bool editing) {
    auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
    if (!dialog) {
        window.findChild<QAction *>("mcpAccess")->trigger();
        dialog = window.findChild<QDialog *>("mcpAccessDialog");
    }
    if (editing)
        QTimer::singleShot(0, &window, [] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box && box->button(QMessageBox::Yes))
                box->button(QMessageBox::Yes)->click();
        });
    dialog->findChild<QPushButton *>(editing ? "mcpAuthorizeEditing" : "mcpAuthorize")->click();
    return window.findChild<Service *>()->connectionCode();
}
QJsonObject projectInfo(QProcess &client) {
    QJsonObject info;
    for (int attempt = 0; attempt < 100; ++attempt) {
        client.write(call(90, "nova.project.info"));
        info = payload(receive(client));
        if (!info.contains("error"))
            break;
        QTest::qWait(50);
    }
    return info;
}
} // namespace

class SaveDraftsTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString root, other;
    const QString plain = "assets/nova/textures/block/plain.png";
    const QString layered = "assets/nova/textures/block/layered.png";
    const QString animated = "assets/nova/textures/block/animated.png";
    const QString second = "assets/nova/textures/block/second.png";
    const QString model = "assets/nova/models/block/plain.json";

    void makePack(const QString &pack) {
        write(pack + "/pack.mcmeta", R"({"pack":{"min_format":[75,0],"max_format":[75,0],"description":"Phase 1"}})");
        for (const auto &file : {plain, layered, second}) {
            QVERIFY(QDir().mkpath(QFileInfo(pack + '/' + file).path()));
            QVERIFY(png(16, 16, Qt::gray).save(pack + '/' + file));
        }
        QVERIFY(png(16, 32, Qt::darkGreen).save(pack + '/' + animated));
        write(pack + '/' + animated + ".mcmeta", R"({"animation":{"frametime":2}})");
        write(pack + '/' + model, R"({"parent":"minecraft:block/cube_all","textures":{"all":"nova:block/plain"}})");
    }
    static void paint(nova::Window &window, const std::shared_ptr<nova::Document> &doc, QColor color, QPoint at = {1, 1}) {
        QVERIFY(window.documents().paint(doc, doc->activeLayer(), at, at, color.rgba(), true));
    }

    // Fails the slot (instead of hanging it) if an unexpected modal dialog appears; tests that
    // expect a dialog close it themselves within the same event-loop turn.
    QTimer sentinel;
    QStringList unexpectedModals;
    void writeStyleNotes() {
        auto config = nova::ProjectConfig::load(root);
        nova::StyleNotes notes;
        notes.theme = "Cozy medieval";
        config.setStyleNotes(notes);
        QString failure;
        QVERIFY2(config.save(root, &failure), qPrintable(failure));
    }

  private slots:
    void initTestCase() { QVERIFY(dir.isValid()); }
    // Each slot gets its own packs, settings and recovery folder, so one failure cannot cascade.
    void cleanup() {
        sentinel.stop();
        // Cleared before asserting: a failed check returns early, and must not carry into the next slot.
        const auto modals = std::exchange(unexpectedModals, {});
        QVERIFY2(modals.isEmpty(), qPrintable(modals.join(" | ")));
    }
    void init() {
        sentinel.setInterval(1500);
        QObject::disconnect(&sentinel, nullptr, nullptr, nullptr);
        connect(&sentinel, &QTimer::timeout, this, [this] {
            auto modal = QApplication::activeModalWidget();
            if (!modal)
                return;
            auto box = qobject_cast<QMessageBox *>(modal);
            unexpectedModals << modal->objectName() + ": " + modal->windowTitle() + (box ? " · " + box->text() : QString());
            modal->close();
        });
        sentinel.start();
        const auto base = dir.path() + '/' + QTest::currentTestFunction();
        root = base + "/phase1-pack";
        other = base + "/other-pack";
        makePack(root);
        makePack(other);
        qApp->setProperty("settingsFile", base + "/settings/studio.ini");
    }

    // Save writes every texture kind into the pack and leaves it reported saved; history is kept.
    void saveWritesThePack() {
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        nova::EditorTestAccess::waitForRecoveryCheck(window, timer);
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        auto saveAction = window.findChild<QAction *>("saveCommand");
        auto saveAll = window.findChild<QAction *>("saveAllCommand");
        QVERIFY(saveAction && saveAll);
        QCOMPARE(saveAll->shortcut(), QKeySequence("Ctrl+Alt+Shift+S"));
        QCOMPARE(saveAction->shortcut(), QKeySequence("Ctrl+S")); // unchanged
        QCOMPARE(window.findChild<QAction *>("saveWorkspaceCommand")->shortcut(), QKeySequence("Ctrl+Alt+S"));

        // Plain PNG: one edit → one unsaved document; Save writes the PNG and clears it.
        nova::EditorTestAccess::open(window, root + '/' + plain);
        auto doc = nova::EditorTestAccess::active(window);
        QVERIFY(doc && !doc->nativeDocument() && !doc->animated());
        QVERIFY(nova::EditorTestAccess::unsavedLabel(window));
        QVERIFY(!nova::EditorTestAccess::unsavedLabel(window)->isVisible());
        paint(window, doc, Qt::red);
        QVERIFY(nova::EditorTestAccess::dirty(window, doc));
        QCOMPARE(nova::EditorTestAccess::unsavedLabel(window)->text(), "1 unsaved document");
        QVERIFY(nova::EditorTestAccess::unsavedLabel(window)->isVisible());
        QVERIFY(nova::EditorTestAccess::tabText(window, doc).endsWith(" ●"));
        nova::EditorTestAccess::checkpoint(window);
        QTRY_VERIFY(!nova::EditorTestAccess::checkpointBusy(window));
        QVERIFY(nova::EditorTestAccess::recovered(window).contains(doc->id()));
        const auto cursor = doc->historyCursor();
        const auto history = doc->historyLabels();
        saveAction->trigger();
        QCOMPARE(disk(root + '/' + plain), doc->image.convertToFormat(QImage::Format_RGBA8888));
        QVERIFY(!nova::EditorTestAccess::dirty(window, doc));
        QVERIFY(!nova::EditorTestAccess::unsavedLabel(window)->isVisible());
        QVERIFY(!nova::EditorTestAccess::tabText(window, doc).contains("●"));
        QVERIFY(window.statusBar()->currentMessage().startsWith("Saved to the pack · " + plain));
        QVERIFY(!nova::EditorTestAccess::recovered(window).contains(doc->id())); // clean: checkpoint removed
        // Saving does not rewrite history; undo/redo move across the saved point.
        QCOMPARE(doc->historyCursor(), cursor);
        QCOMPARE(doc->historyLabels(), history);
        QVERIFY(doc->canUndo());
        window.documents().dispatch(doc, nova::Command::Undo);
        QVERIFY(nova::EditorTestAccess::dirty(window, doc));
        QCOMPARE(nova::EditorTestAccess::unsaved(window).size(), 1);
        window.documents().dispatch(doc, nova::Command::Redo);
        QVERIFY(!nova::EditorTestAccess::dirty(window, doc));

        // Layered document: Save writes the composite PNG into the pack and the editable layers.
        nova::EditorTestAccess::open(window, root + '/' + layered);
        auto layers = nova::EditorTestAccess::active(window);
        QVERIFY(window.documents().addLayer(layers, "Overlay", png(16, 16, Qt::transparent)));
        QVERIFY(window.documents().dispatch(layers, nova::Command::SaveNative));
        QVERIFY(layers->nativeDocument());
        const auto editable = layers->editablePath();
        QVERIFY(QFileInfo::exists(editable));
        const auto editableBefore = bytes(editable);
        paint(window, layers, Qt::blue, {3, 3});
        QVERIFY(layers->minecraftOutputPending() && nova::EditorTestAccess::dirty(window, layers));
        saveAction->trigger();
        QCOMPARE(disk(root + '/' + layered), layers->image.convertToFormat(QImage::Format_RGBA8888));
        QCOMPARE(disk(root + '/' + layered).pixelColor(3, 3), QColor(Qt::blue));
        QVERIFY(bytes(editable) != editableBefore);
        QVERIFY(!layers->modified() && !layers->minecraftOutputPending());
        QVERIFY(!nova::EditorTestAccess::dirty(window, layers));
        QVERIFY(window.statusBar()->currentMessage().contains("editable source saved"));
        QVERIFY(!nova::EditorTestAccess::tabText(window, layers).contains("↑PNG"));

        // Animated texture: Save writes the PNG + .png.mcmeta pair and the editable animation.
        nova::EditorTestAccess::open(window, root + '/' + animated);
        auto animation = nova::EditorTestAccess::active(window);
        QVERIFY(animation && animation->animated());
        const auto sheetBefore = bytes(root + '/' + animated);
        paint(window, animation, Qt::yellow, {2, 2});
        QVERIFY(nova::EditorTestAccess::dirty(window, animation));
        saveAction->trigger();
        QTRY_VERIFY(!nova::EditorTestAccess::dirty(window, animation));
        QVERIFY(bytes(root + '/' + animated) != sheetBefore);
        QCOMPARE(disk(root + '/' + animated).size(), QSize(16, 32));
        QCOMPARE(QJsonDocument::fromJson(bytes(root + '/' + animated + ".mcmeta")).object()["animation"]
                     .toObject()["frametime"].toInt(), 2);
        QVERIFY(QFileInfo::exists(animation->editablePath()));
        QVERIFY(nova::EditorTestAccess::unsaved(window).isEmpty());
        for (const auto &open : window.documents().all())
            QVERIFY(!open->modified());
    }

    // Save All saves what it can, names what it could not, and never loses a draft.
    void saveAllReportsPartialFailure() {
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        nova::EditorTestAccess::waitForRecoveryCheck(window, timer);
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        nova::EditorTestAccess::open(window, root + '/' + plain);
        auto good = nova::EditorTestAccess::active(window);
        nova::EditorTestAccess::open(window, root + '/' + second);
        auto changed = nova::EditorTestAccess::active(window);
        paint(window, good, Qt::green);
        paint(window, changed, Qt::magenta);
        // A never-saved document whose folder disappears, and an external change on disk.
        QVERIFY(QDir().mkpath(root + "/assets/nova/textures/gone"));
        const auto fresh = nova::DocumentSession::newRaster(root + "/assets/nova/textures/gone/new.png",
                                                            png(16, 16, Qt::cyan), "Layer", 1, 1, false, nullptr);
        QVERIFY(fresh && window.documents().adopt(fresh));
        QVERIFY(nova::EditorTestAccess::dirty(window, fresh));
        QVERIFY(QDir(root + "/assets/nova/textures/gone").removeRecursively());
        const auto external = png(16, 16, Qt::black);
        QVERIFY(external.save(root + '/' + second));
        const auto externalBytes = bytes(root + '/' + second);
        QCOMPARE(nova::EditorTestAccess::unsaved(window).size(), 3);
        QCOMPARE(window.findChild<QLabel *>("unsavedStatus")->text(), "3 unsaved documents");
        nova::EditorTestAccess::checkpoint(window);
        QTRY_VERIFY(!nova::EditorTestAccess::checkpointBusy(window));
        const auto changedPixels = changed->image;

        const auto outcomes = nova::EditorTestAccess::saveAll(window);
        QCOMPARE(outcomes.size(), 3);
        QMap<QString, nova::SaveOutcome> byPath;
        for (const auto &outcome : outcomes)
            byPath.insert(outcome.path, outcome);
        QVERIFY(byPath[plain].ok());
        QCOMPARE(byPath[plain].written, QStringList{"pack"});
        QCOMPARE(byPath[second].code, "external_change");
        QVERIFY(byPath[second].written.isEmpty());
        QCOMPARE(byPath["assets/nova/textures/gone/new.png"].code, "missing_directory");
        for (const auto &outcome : outcomes)
            QVERIFY2(!outcome.message.contains(dir.path()) && !outcome.message.contains(":/"), qPrintable(outcome.message));
        // Saved: on disk and clean. Not saved: disk untouched, draft and recovery checkpoint kept.
        QCOMPARE(disk(root + '/' + plain), good->image.convertToFormat(QImage::Format_RGBA8888));
        QVERIFY(!nova::EditorTestAccess::dirty(window, good));
        QCOMPARE(bytes(root + '/' + second), externalBytes);
        QVERIFY(nova::EditorTestAccess::dirty(window, changed) && changed->image == changedPixels);
        QVERIFY(nova::EditorTestAccess::dirty(window, fresh));
        QVERIFY(!QFileInfo::exists(root + "/assets/nova/textures/gone"));
        const auto recovered = nova::EditorTestAccess::recovered(window);
        QVERIFY(recovered.contains(changed->id()) && !recovered.contains(good->id()));
        QCOMPARE(window.findChild<QLabel *>("unsavedStatus")->text(), "2 unsaved documents");

        // The UI action shows which documents were not saved and why.
        auto report = std::make_shared<QString>();
        QTimer::singleShot(0, &window, [report] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                *report = box->objectName() + '\n' + box->text() + '\n' + box->informativeText();
                box->accept();
            }
        });
        window.findChild<QAction *>("saveAllCommand")->trigger();
        QVERIFY2(report->startsWith("saveAllReport\n2 of 2 documents were not saved"), qPrintable(*report));
        QVERIFY(report->contains(second + " — The file changed on disk"));
        QVERIFY(report->contains("gone/new.png — The folder for this resource path no longer exists"));
        QVERIFY(!report->contains("Saved:"));
        // Discard the leftover drafts so the window closes without prompts.
        for (const auto &doc : {changed, fresh})
            window.documents().close(doc);
        QVERIFY(png(16, 16, Qt::gray).save(root + '/' + second));
    }

    void styleNotes() {
        QVERIFY(nova::ProjectConfig::load(root).styleNotes().empty());
        QWidget parent;
        QTimer::singleShot(0, &parent, [] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog && dialog->objectName() == "styleNotesDialog");
            dialog->findChild<QLineEdit *>("styleTheme")->setText("  Cozy medieval  ");
            dialog->findChild<QLineEdit *>("styleAudience")->setText("Survival players");
            dialog->findChild<QPlainTextEdit *>("stylePalette")->setPlainText("Warm browns; no neon");
            dialog->findChild<QPlainTextEdit *>("stylePreserve")->setPlainText("Vanilla ore shapes");
            dialog->accept(); // not the Save button: nothing is written
        });
        QVERIFY(!nova::styleNotesDialog(&parent, root));
        QVERIFY(!QFileInfo::exists(root + "/.nova/project.json"));
        QTimer::singleShot(0, &parent, [] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            dialog->findChild<QLineEdit *>("styleTheme")->setText("  Cozy medieval  ");
            dialog->findChild<QLineEdit *>("styleAudience")->setText("Survival players");
            dialog->findChild<QPlainTextEdit *>("stylePalette")->setPlainText("Warm browns; no neon");
            dialog->findChild<QPlainTextEdit *>("stylePreserve")->setPlainText("Vanilla ore shapes");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        QVERIFY(nova::styleNotesDialog(&parent, root));
        const auto notes = nova::ProjectConfig::load(root).styleNotes();
        QCOMPARE(notes.theme, "Cozy medieval");
        QCOMPARE(notes.audience, "Survival players");
        QCOMPARE(notes.palette, "Warm browns; no neon");
        QCOMPARE(notes.preserve, "Vanilla ore shapes");
        QVERIFY(notes.notes.isEmpty());
        const auto config = QJsonDocument::fromJson(bytes(root + "/.nova/project.json")).object();
        QCOMPARE(config["schema"].toInt(), 1);
        QVERIFY(!config["styleNotes"].toObject().contains("notes"));
        QCOMPARE(nova::styleNotesSummary(notes).left(20), "Theme: Cozy medieval");
        QVERIFY(!bytes(root + "/pack.mcmeta").contains("Cozy")); // pack metadata is not modified
    }

    // Draft listing, adoption and saving over MCP, with authorization, conflicts and validation.
    void mcpDrafts() {
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        nova::EditorTestAccess::waitForRecoveryCheck(window, timer);
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        writeStyleNotes();
        auto service = [&] { return window.findChild<Service *>(); };
        nova::EditorTestAccess::open(window, root + '/' + plain);
        auto doc = nova::EditorTestAccess::active(window);
        nova::EditorTestAccess::open(window, root + '/' + second);
        auto secondDoc = nova::EditorTestAccess::active(window);
        auto foreign = window.documents().open(other + '/' + plain); // another project's document
        QVERIFY(foreign);
        paint(window, doc, Qt::red);
        const auto before = fingerprint(root);

        // Read-only connection: listing works; adoption and saving are refused.
        {
            QProcess client;
            start(client, authorize(window, false));
            const auto init = initialize(client);
            QVERIFY(init["instructions"].toString().contains("not proof"));
            QVERIFY(init["capabilities"].toObject()["resources"].isObject());
            const auto info = projectInfo(client);
            QCOMPARE(info["unsavedCount"].toInt(), 1);
            QCOMPARE(info["styleNotes"].toObject()["present"].toBool(), true);
            QCOMPARE(info["styleNotes"].toObject()["kind"].toString(), "user_content");
            const QJsonObject scope{{"generation", info["generation"]}, {"projectId", info["projectId"]}};
            client.write(call(2, "nova.drafts.list", scope));
            const auto list = payload(receive(client));
            const auto rows = list["drafts"].toArray();
            QCOMPARE(list["unsavedCount"].toInt(), 1);
            // Both open textures are listed (models the Models panel opened may be listed too); the
            // other project's open document is not.
            int textures = 0;
            for (const auto &value : rows) {
                textures += value.toObject()["type"] == "texture";
                QVERIFY(value.toObject()["documentId"] != nova::EditorTestAccess::documentId(window, foreign));
                QVERIFY(value.toObject()["type"] == "texture" || !value.toObject()["dirty"].toBool());
            }
            QCOMPARE(textures, 2);
            const auto text = QJsonDocument(list).toJson();
            QVERIFY2(!text.contains(QDir::toNativeSeparators(dir.path()).toUtf8()) && !text.contains(dir.path().toUtf8()) &&
                     !text.contains(":/") && !text.contains(":\\\\"), text.constData());
            QJsonObject row;
            for (const auto &value : rows)
                if (value.toObject()["path"] == plain)
                    row = value.toObject();
            QCOMPARE(row["documentId"].toString(), nova::EditorTestAccess::documentId(window, doc));
            QVERIFY(row["dirty"].toBool() && !row["packCurrent"].toBool() && row["saveSupported"].toBool());
            QCOMPARE(row["type"].toString(), "texture");
            client.write(call(3, "nova.drafts.adopt", {{"generation", scope["generation"]}, {"projectId", scope["projectId"]},
                {"documentId", row["documentId"]}, {"path", plain}, {"draftRevision", row["draftRevision"]}}));
            QCOMPARE(error(receive(client)), "permission_denied");
            client.write(call(4, "nova.drafts.save", {{"generation", scope["generation"]}, {"projectId", scope["projectId"]},
                {"documentId", row["documentId"]}, {"expectedRevision", row["revision"]}}));
            QCOMPARE(error(receive(client)), "permission_denied");
            client.write(call(5, "nova.drafts.save_all", scope));
            QCOMPARE(error(receive(client)), "permission_denied");
            // Style notes resource: read-only user content.
            client.write(request(6, "resources/read", {{"uri", "nova://project/style-notes"}}));
            const auto notes = QJsonDocument::fromJson(receive(client)["result"].toObject()["contents"].toArray().first()
                                                           .toObject()["text"].toString().toUtf8()).object();
            QCOMPARE(notes["theme"].toString(), "Cozy medieval");
            QVERIFY(notes["notice"].toString().contains("never grant permissions"));
            client.closeWriteChannel();
            QTRY_COMPARE(client.state(), QProcess::NotRunning);
        }
        QCOMPARE(fingerprint(root), before);
        QVERIFY(nova::EditorTestAccess::dirty(window, doc));

        // A later editing connection reconnects to the still-open draft and saves it.
        QProcess client;
        QTRY_COMPARE(service()->status(), "Disabled");
        start(client, authorize(window, true));
        initialize(client);
        auto info = projectInfo(client);
        QJsonObject scope{{"generation", info["generation"]}, {"projectId", info["projectId"]}};
        auto listDrafts = [&](int id) {
            client.write(call(id, "nova.drafts.list", scope));
            QMap<QString, QJsonObject> rows;
            for (const auto &value : payload(receive(client))["drafts"].toArray())
                rows.insert(value.toObject()["path"].toString(), value.toObject());
            return rows;
        };
        auto rows = listDrafts(10);
        auto row = rows[plain];
        auto adopt = [&](int id, const QJsonValue &documentId, const QJsonValue &path, const QJsonValue &revision) {
            client.write(call(id, "nova.drafts.adopt", {{"generation", scope["generation"]}, {"projectId", scope["projectId"]},
                {"documentId", documentId}, {"path", path}, {"draftRevision", revision}}));
            return receive(client);
        };
        QCOMPARE(error(adopt(11, row["documentId"], plain, QString(32, 'f'))), "stale_draft");
        QCOMPARE(error(adopt(12, row["documentId"], second, row["draftRevision"])), "stale_draft");
        QCOMPARE(error(adopt(13, QString(32, 'a'), plain, row["draftRevision"])), "document_not_open");
        QCOMPARE(error(adopt(14, nova::EditorTestAccess::documentId(window, foreign), plain, row["draftRevision"])),
                 "document_not_open"); // cross-project adoption is impossible
        client.write(call(15, "nova.drafts.adopt", {{"generation", scope["generation"]}, {"projectId", randomToken()},
            {"documentId", row["documentId"]}, {"path", plain}, {"draftRevision", row["draftRevision"]}}));
        QCOMPARE(error(receive(client)), "stale_project");
        const auto adopted = payload(adopt(16, row["documentId"], plain, row["draftRevision"]));
        QVERIFY(adopted["adopted"].toBool());
        QCOMPARE(adopted["revision"], row["revision"]);
        QCOMPARE(adopted["undoAvailable"].toBool(), false); // the earlier edit belongs to the user
        auto save = [&](int id, const QJsonValue &documentId, const QJsonValue &revision) {
            client.write(call(id, "nova.drafts.save", {{"generation", scope["generation"]}, {"projectId", scope["projectId"]},
                {"documentId", documentId}, {"expectedRevision", revision}}));
            return receive(client);
        };
        QCOMPARE(error(save(17, row["documentId"], randomToken())), "revision_conflict");
        QCOMPARE(fingerprint(root), before);
        const auto saved = payload(save(18, row["documentId"], adopted["revision"]));
        QCOMPARE(saved["status"].toString(), "saved");
        QCOMPARE(saved["saved"].toArray().first().toObject()["path"].toString(), plain);
        QCOMPARE(saved["saved"].toArray().first().toObject()["written"].toArray(), QJsonArray{"pack"});
        QCOMPARE(saved["unsavedCount"].toInt(), 0);
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window)); // the save's own write is re-indexed
        QCOMPARE(disk(root + '/' + plain), doc->image.convertToFormat(QImage::Format_RGBA8888));
        QVERIFY(!nova::EditorTestAccess::dirty(window, doc));
        QVERIFY(!window.findChild<QLabel *>("unsavedStatus")->isVisible());

        // Agent edits, undo and redo across the saved point.
        client.write(call(19, "nova.edit.capabilities", {{"generation", scope["generation"]}, {"assetId", row["assetId"]}}));
        auto state = payload(receive(client))["document"].toObject();
        client.write(call(20, "nova.texture.edit", {{"generation", scope["generation"]}, {"projectId", scope["projectId"]},
            {"assetId", row["assetId"]}, {"expectedRevision", state["revision"]}, {"operation", QJsonObject{
                {"command", "fill_rect"}, {"layer", state["layers"].toArray().first().toObject()["id"]}, {"x", 0}, {"y", 0},
                {"width", 4}, {"height", 4}, {"color", "#00ff00"}, {"opacity", 255}}}}));
        auto edited = payload(receive(client));
        QVERIFY(edited["dirty"].toBool() && edited["changed"].toBool());
        QCOMPARE(listDrafts(21)[plain]["dirty"].toBool(), true);
        QVERIFY(QImage(root + '/' + plain).pixelColor(0, 0) != QColor(Qt::green)); // an edit alone is not a save
        rows = listDrafts(22);
        QCOMPARE(payload(save(23, rows[plain]["documentId"], rows[plain]["revision"]))["status"].toString(), "saved");
        QCOMPARE(QImage(root + '/' + plain).pixelColor(0, 0), QColor(Qt::green));
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        client.write(call(24, "nova.edit.undo", {{"generation", scope["generation"]}, {"projectId", scope["projectId"]},
            {"assetId", row["assetId"]}, {"expectedRevision", listDrafts(25)[plain]["revision"]}}));
        const auto undone = payload(receive(client));
        QVERIFY2(!undone.contains("error") && undone["dirty"].toBool(), QJsonDocument(undone).toJson().constData());
        QVERIFY(nova::EditorTestAccess::dirty(window, doc));
        client.write(call(26, "nova.edit.redo", {{"generation", scope["generation"]}, {"projectId", scope["projectId"]},
            {"assetId", row["assetId"]}, {"expectedRevision", listDrafts(27)[plain]["revision"]}}));
        const auto redone = payload(receive(client));
        QVERIFY2(!redone.contains("error") && !redone["dirty"].toBool(), QJsonDocument(redone).toJson().constData());
        QVERIFY(!nova::EditorTestAccess::dirty(window, doc));

        // Draft-aware validation: an open draft is checked live and labelled; an unreadable one is reported.
        paint(window, secondDoc, Qt::blue);
        client.write(call(28, "nova.pack.validate", {{"generation", scope["generation"]}}));
        auto validation = payload(receive(client, 60000));
        auto drafts = validation["drafts"].toObject();
        QCOMPARE(drafts["complete"].toBool(), true);
        QCOMPARE(drafts["included"].toArray().size(), 1);
        QCOMPARE(drafts["included"].toArray().first().toObject()["path"].toString(), second);
        bool live = false;
        for (const auto &value : validation["diagnostics"].toArray()) {
            const auto finding = value.toObject();
            QVERIFY(finding["source"] == "disk" || finding["source"] == "open_draft");
            if (finding["code"] == "live_texture_checked") {
                live = true;
                QCOMPARE(finding["source"].toString(), "open_draft");
                QCOMPARE(finding["path"].toString(), second);
                QCOMPARE(finding["revision"].toString(), listDrafts(29)[second]["revision"].toString());
            }
        }
        QVERIFY(live);
        QVERIFY(window.documents().paint(secondDoc, secondDoc->activeLayer(), {5, 5}, {6, 6}, qRgba(0, 0, 0, 255), false));
        QVERIFY(secondDoc->strokeOpen());
        client.write(call(30, "nova.pack.validate", {{"generation", scope["generation"]}}));
        validation = payload(receive(client, 60000));
        drafts = validation["drafts"].toObject();
        QCOMPARE(drafts["complete"].toBool(), false);
        QCOMPARE(drafts["unavailable"].toArray().first().toObject()["path"].toString(), second);
        bool reported = false;
        for (const auto &value : validation["diagnostics"].toArray())
            if (value.toObject()["code"] == "draft_unavailable") {
                reported = true;
                QCOMPARE(value.toObject()["severity"].toString(), "Warning");
                QCOMPARE(value.toObject()["source"].toString(), "open_draft");
            }
        QVERIFY(reported);
        window.documents().finishStroke(secondDoc);

        // External change: the save is refused and the draft is kept; Save All reports each document.
        const auto secondBytes = bytes(root + '/' + second);
        QVERIFY(png(16, 16, Qt::white).save(root + '/' + second));
        const auto externalBytes = bytes(root + '/' + second);
        QVERIFY(externalBytes != secondBytes);
        QTest::qWait(100);
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        rows = listDrafts(31);
        const auto refused = payload(save(32, rows[second]["documentId"], rows[second]["revision"]))["error"].toObject();
        QCOMPARE(refused["code"].toString(), "external_change");
        QCOMPARE(refused["details"].toObject()["failed"].toArray().first().toObject()["draftPreserved"].toBool(), true);
        QCOMPARE(bytes(root + '/' + second), externalBytes);
        QVERIFY(nova::EditorTestAccess::dirty(window, secondDoc));
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        paint(window, doc, Qt::darkRed);
        client.write(call(33, "nova.drafts.save_all", scope));
        const auto partial = payload(receive(client))["error"].toObject();
        QCOMPARE(partial["code"].toString(), "save_incomplete");
        QCOMPARE(partial["details"].toObject()["saved"].toArray().first().toObject()["path"].toString(), plain);
        QCOMPARE(partial["details"].toObject()["failed"].toArray().first().toObject()["path"].toString(), second);
        QCOMPARE(partial["details"].toObject()["atomic"].toBool(), false);
        QCOMPARE(partial["details"].toObject()["unsavedCount"].toInt(), 1);
        QCOMPARE(disk(root + '/' + plain).pixelColor(1, 1), QColor(Qt::darkRed));
        QCOMPARE(bytes(root + '/' + second), externalBytes);
        // Once the external change is undone, the preserved draft saves normally.
        write(root + '/' + second, secondBytes);
        QTest::qWait(100);
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        rows = listDrafts(34);
        QCOMPARE(payload(save(35, rows[second]["documentId"], rows[second]["revision"]))["status"].toString(), "saved");
        QCOMPARE(disk(root + '/' + second), secondDoc->image.convertToFormat(QImage::Format_RGBA8888));
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));

        // Revocation ends the connection; nothing more can be written.
        paint(window, doc, Qt::darkBlue);
        const auto beforeRevoke = fingerprint(root);
        window.findChild<QDialog *>("mcpAccessDialog")->findChild<QPushButton *>("mcpRevoke")->click();
        QTRY_COMPARE(client.state(), QProcess::NotRunning);
        QCOMPARE(fingerprint(root), beforeRevoke);
        QVERIFY(nova::EditorTestAccess::dirty(window, doc));
        QVERIFY(window.documents().dispatch(doc, nova::Command::Save));

        // Project switch: the connection becomes stale and cannot save.
        QProcess switched;
        start(switched, authorize(window, true));
        initialize(switched);
        info = projectInfo(switched);
        scope = {{"generation", info["generation"]}, {"projectId", info["projectId"]}};
        switched.write(call(40, "nova.drafts.list", scope));
        const auto stillOpen = payload(receive(switched))["drafts"].toArray().first().toObject();
        window.documents().close(foreign);
        auto path = window.findChild<QLineEdit *>("projectPath");
        path->setText(other);
        QTest::keyClick(path, Qt::Key_Return);
        QTRY_COMPARE(service()->status(), "Project changed");
        const auto otherBefore = fingerprint(other);
        switched.write(call(41, "nova.drafts.save_all", scope));
        QCOMPARE(error(receive(switched)), "stale_project");
        switched.write(call(42, "nova.drafts.save", {{"generation", scope["generation"]}, {"projectId", scope["projectId"]},
            {"documentId", stillOpen["documentId"]}, {"expectedRevision", stillOpen["revision"]}}));
        QCOMPARE(error(receive(switched)), "stale_project");
        switched.write(request(43, "resources/read", {{"uri", "nova://project/style-notes"}}));
        QCOMPARE(receive(switched)["error"].toObject()["code"].toInt(), -32002);
        QCOMPARE(fingerprint(other), otherBefore);
        switched.closeWriteChannel();
        QTRY_COMPARE(switched.state(), QProcess::NotRunning);
    }

    // Owner-review captures of the save workflow (NOVA_EVIDENCE_DIR; run once per QT_SCALE_FACTOR).
    void visualEvidence() {
        const auto evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        if (evidence.isEmpty())
            QSKIP("NOVA_EVIDENCE_DIR is not set");
        QVERIFY(QDir().mkpath(evidence));
        const auto scale = qEnvironmentVariable("QT_SCALE_FACTOR", "1");
        auto grab = [&](QWidget *widget, const QString &theme, const QString &name) {
            QVERIFY(widget->grab().save(QString("%1/scale%2-%3-%4.png").arg(evidence, scale, theme, name)));
        };
        writeStyleNotes();
        for (const auto &theme : {QString("dark"), QString("light")}) {
            QElapsedTimer timer;
            timer.start();
            nova::Window window(root, {}, timer);
            window.resize(1280, 800);
            window.show();
            nova::EditorTestAccess::waitForRecoveryCheck(window, timer);
            QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
            window.findChild<QAction *>("darkThemeCommand")->setChecked(theme == "dark");
            nova::EditorTestAccess::open(window, root + '/' + layered);
            auto layers = nova::EditorTestAccess::active(window);
            nova::EditorTestAccess::open(window, root + '/' + plain);
            auto doc = nova::EditorTestAccess::active(window);
            const bool dark = theme == "dark";
            paint(window, layers, dark ? Qt::blue : Qt::darkYellow, {2, 2});
            paint(window, doc, dark ? Qt::red : Qt::magenta, {4, 4});
            QTest::qWait(100);
            grab(&window, theme, "01-two-unsaved");
            auto file = window.findChild<QMenu *>("menu-File");
            file->popup(window.mapToGlobal(QPoint(20, 40)));
            QTest::qWait(100);
            grab(file, theme, "02-file-menu");
            file->hide();
            window.findChild<QAction *>("saveCommand")->trigger();
            QTest::qWait(100);
            grab(&window, theme, "03-saved-active-one-unsaved");
            // Save All with one document changed on disk: the report names it and keeps the draft.
            const auto original = bytes(root + '/' + layered);
            QVERIFY(png(16, 16, Qt::black).save(root + '/' + layered));
            paint(window, doc, dark ? Qt::green : Qt::cyan, {5, 5});
            QTimer::singleShot(0, &window, [&] {
                if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    grab(box, theme, "04-save-all-report");
                    box->accept();
                }
            });
            window.findChild<QAction *>("saveAllCommand")->trigger();
            QTest::qWait(100);
            grab(&window, theme, "05-after-save-all");
            QTimer::singleShot(0, &window, [&] {
                if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                    QTimer::singleShot(0, dialog, [&, dialog] {
                        if (auto notes = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                            notes && notes != dialog) {
                            grab(notes, theme, "07-style-notes-dialog");
                            notes->reject();
                        }
                        grab(dialog, theme, "06-project-properties");
                        dialog->reject();
                    });
                    dialog->findChild<QPushButton *>("editStyleNotes")->click();
                }
            });
            window.findChild<QAction *>("projectPropertiesCommand")->trigger();
            // Undo the external change; the kept draft then saves normally.
            write(root + '/' + layered, original);
            QTest::qWait(100);
            QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
            window.findChild<QAction *>("saveAllCommand")->trigger();
            QVERIFY(nova::EditorTestAccess::unsaved(window).isEmpty());
        }
    }

    // The official MCP Python SDK: instructions, resources, guide parity, drafts and saving, with
    // structured results validated against the declared output schemas.
    void officialSdk() {
        const auto python = qEnvironmentVariable("NOVA_MCP_SDK_PYTHON");
        if (python.isEmpty())
            QSKIP("NOVA_MCP_SDK_PYTHON is not set");
        writeStyleNotes();
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        nova::EditorTestAccess::waitForRecoveryCheck(window, timer);
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        nova::EditorTestAccess::open(window, root + '/' + plain);
        auto doc = nova::EditorTestAccess::active(window);
        paint(window, doc, Qt::darkCyan, {7, 7});
        QProcess client;
        start(client, authorize(window, true), python, {QString::fromUtf8(NOVA_MCP_TEST_SDK), bridge(), "phase1"});
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QProcess::NotRunning, 30000);
        const auto output = client.readAllStandardOutput();
        qInfo().noquote() << "SDK phase1" << output;
        QVERIFY2(client.exitCode() == 0, client.readAllStandardError().constData());
        const auto summary = QJsonDocument::fromJson(output.trimmed().split('\n').last()).object();
        QCOMPARE(summary["saved"].toString(), plain);
        QCOMPARE(summary["topics"].toInt(), 7);
        QCOMPARE(disk(root + '/' + plain).pixelColor(7, 7), QColor(Qt::darkCyan));
        QVERIFY(!nova::EditorTestAccess::dirty(window, doc));
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    SaveDraftsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "save_drafts_test.moc"
