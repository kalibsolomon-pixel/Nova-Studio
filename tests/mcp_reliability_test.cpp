// SPDX-License-Identifier: GPL-3.0-or-later
// MCP reliability: asset creation distinguishes invalid, occupied, missing-folder and unsafe
// destinations, and a stale index entry is reconciled by re-indexing instead of blocking creation.
// Parallel calls queue in order; long edits report progress; agent editing (selection, active layer,
// registry operations, multi-step history, open and close) behaves as documented.
// Synthetic packs only; nothing outside the temporary project is written.
#include "window.h"
#include "service.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QProcess>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent>
#include <QtTest>
#include <qt_windows.h>

using namespace nova::mcp;

namespace nova {
class EditorTestAccess {
  public:
    static bool indexReady(Window &window) {
        return !window.indexBusy && !window.indexDebounce->isActive() && window.pendingChanges.isEmpty();
    }
    // Simulates a missed folder-monitor event: the index keeps a record of a deleted file.
    static void stopMonitor(Window &window) { window.monitor.stop(); }
    static bool indexed(Window &window, const QString &relative) { return window.collection.records.contains(relative); }
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
QMap<QString, QByteArray> fingerprint(const QString &root) {
    QMap<QString, QByteArray> hashes;
    QDirIterator files(root, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto file = files.next();
        if (QDir(root).relativeFilePath(file).startsWith(".nova/"))
            continue;
        QFile input(file);
        hashes.insert(QDir(root).relativeFilePath(file), input.open(QIODevice::ReadOnly)
            ? QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256) : QByteArray("unreadable"));
    }
    return hashes;
}
QByteArray request(int id, const QString &method, const QJsonObject &params = {}) {
    QJsonObject data{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (id >= 0)
        data["id"] = id;
    return QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n';
}
QJsonObject receive(QProcess &process, int timeout = 10000) {
    if (!QTest::qWaitFor([&] { return process.canReadLine() || process.state() == QProcess::NotRunning; }, timeout))
        return {};
    return QJsonDocument::fromJson(process.readLine()).object();
}
QJsonObject payload(const QJsonObject &response) {
    const auto result = response["result"].toObject();
    if (!result["isError"].toBool())
        return result["structuredContent"].toObject();
    if (result.contains("structuredContent"))
        return {{"unexpected", "structured content on an error"}};
    return QJsonDocument::fromJson(result["content"].toArray().first().toObject()["text"].toString().toUtf8()).object();
}
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
std::wstring native(const QString &path) { return QDir::toNativeSeparators(path).toStdWString(); }
// Holds the only worker thread, so a read stays running until the gate is released.
class WorkerGate {
    QThreadPool *pool = QThreadPool::globalInstance();
    int previous = pool->maxThreadCount();
    std::shared_ptr<QSemaphore> release = std::make_shared<QSemaphore>();
  public:
    WorkerGate() {
        pool->setMaxThreadCount(1);
        auto semaphore = release;
        [[maybe_unused]] auto blocked = QtConcurrent::run([semaphore] { semaphore->acquire(); });
    }
    ~WorkerGate() {
        release->release();
        pool->setMaxThreadCount(previous);
    }
};
QByteArray toolCall(int id, const QString &tool, const QJsonObject &args, const QJsonValue &progress = {}) {
    QJsonObject params{{"name", tool}, {"arguments", args}};
    if (!progress.isNull())
        params["_meta"] = QJsonObject{{"progressToken", progress}};
    return request(id, "tools/call", params);
}
} // namespace

class McpReliabilityTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString root, outside;
    int next = 10;

    QProcess client;
    QJsonObject scope;
    void connectClient(nova::Window &window, bool editing) {
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("NOVA_MCP_CODE", authorize(window, editing));
        client.setProcessEnvironment(env);
        client.start(bridge());
        QVERIFY(client.waitForStarted());
        client.write(request(1, "initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "Nova reliability tests"}, {"version", "1"}}}}));
        QVERIFY(receive(client)["result"].toObject().contains("serverInfo"));
        client.write(request(-1, "notifications/initialized"));
    }
    QJsonObject call(const QString &tool, const QJsonObject &args = {}) {
        client.write(request(++next, "tools/call", {{"name", tool}, {"arguments", args}}));
        const auto reply = receive(client);
        if (reply.isEmpty())
            return {{"error", QJsonObject{{"code", "no_reply"}}}};
        return payload(reply);
    }
    QJsonObject info() {
        QJsonObject result;
        for (int attempt = 0; attempt < 100; ++attempt) {
            result = call("nova.project.info");
            if (!result.contains("error"))
                break;
            QTest::qWait(50);
        }
        scope = {{"generation", result["generation"]}, {"projectId", result["projectId"]}};
        return result;
    }
    QJsonObject preview(const QString &path, bool model = false, const QString &tool = "nova.asset.create_preview") {
        QJsonObject result;
        // A late index update (the fixture's links are reported after indexReady) may move the
        // revision between nova.project.info and the call, or still be running when the call
        // arrives. Only that plain mismatch and the documented retryable validation_unavailable
        // (docs/release/MCP_ASSET_TOOLS.md) are retried; info() waits for the index again first.
        for (int attempt = 0; attempt < 5; ++attempt) {
            const QJsonValue revision = info()["indexRevision"];
            auto args = scope;
            args["indexRevision"] = revision;
            args["namespace"] = "nova";
            args["path"] = path;
            args["conflictPolicy"] = "reject";
            args["parameters"] = model ? QJsonObject{{"command", "model"}, {"cubeTexture", "nova:block/plain"}}
                                       : QJsonObject{{"command", "texture"}, {"width", 16}, {"height", 16},
                                                     {"layerName", "Base"}, {"fill", "transparent"}, {"pixelFormat", "rgba8888"}};
            result = call(tool, args);
            const bool updating = code(result) == "validation_unavailable";
            if (!updating && (code(result) != "stale_index" || result["error"].toObject().contains("details")))
                break;
            qInfo("%s %s: retrying after %s", qPrintable(tool), qPrintable(path), qPrintable(code(result)));
            QTest::qWait(200);
        }
        return result;
    }
    static QString code(const QJsonObject &result) { return result["error"].toObject()["code"].toString(); }
    static QString text(const QJsonObject &result) { return QString::fromUtf8(QJsonDocument(result).toJson()); }
    // Reads messages up to the reply to `id`; notifications on the way are kept in `seen`.
    QJsonObject replyTo(int id, QList<QJsonObject> *seen = nullptr) {
        for (int i = 0; i < 64; ++i) {
            const auto message = receive(client);
            if (message.isEmpty() || message["id"] == id)
                return message;
            if (seen)
                seen->append(message);
        }
        return {};
    }
    QMap<QString, QString> assetIds() {
        QMap<QString, QString> ids;
        const auto rows = call("nova.assets.list", {{"generation", scope["generation"]}, {"limit", 100}})["assets"].toArray();
        for (const QJsonValue &row : rows)
            ids.insert(row["path"].toString(), row["assetId"].toString());
        return ids;
    }
    static std::shared_ptr<nova::Document> openDocument(nova::Window &window, const QString &name) {
        for (const auto &doc : window.documents().all())
            if (doc->path.endsWith('/' + name))
                return doc;
        return {};
    }
    QJsonObject target(const QString &asset, const QJsonValue &revision) {
        auto args = scope;
        args["assetId"] = asset;
        args["expectedRevision"] = revision;
        return args;
    }

  private slots:
    void initTestCase() { QVERIFY(dir.isValid()); }
    void init() {
        const auto base = dir.path() + '/' + QTest::currentTestFunction();
        root = base + "/pack";
        outside = base + "/outside";
        QVERIFY(QDir().mkpath(outside));
        write(root + "/pack.mcmeta", R"({"pack":{"min_format":[75,0],"max_format":[75,0],"description":"Reliability"}})");
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::gray);
        QVERIFY(QDir().mkpath(root + "/assets/nova/textures/block"));
        QVERIFY(QDir().mkpath(root + "/assets/nova/models/block"));
        QVERIFY(image.save(root + "/assets/nova/textures/block/plain.png"));
        qApp->setProperty("settingsFile", base + "/settings/studio.ini");
    }
    void cleanup() {
        if (client.state() != QProcess::NotRunning) {
            client.closeWriteChannel();
            if (!client.waitForFinished(3000))
                client.kill();
            client.waitForFinished(3000);
        }
    }

    // Each kind of unusable destination gets its own code and a message naming the actual problem.
    void creationDestinations() {
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        // An occupied path that is not indexed: a hard link Nova will not open.
        write(outside + "/secret.png", "outside");
        QVERIFY(CreateHardLinkW(native(root + "/assets/nova/textures/block/hard.png").c_str(),
                                native(outside + "/secret.png").c_str(), nullptr));
        QProcess cmd;
        cmd.setProgram("cmd.exe");
        cmd.setNativeArguments("/d /c mklink /J \"" + QDir::toNativeSeparators(root + "/assets/nova/textures/linked") +
                               "\" \"" + QDir::toNativeSeparators(outside) + "\"");
        cmd.start();
        QVERIFY(cmd.waitForFinished());
        QCOMPARE(cmd.exitCode(), 0);
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        const auto before = fingerprint(root);
        const auto outsideBefore = fingerprint(outside);
        connectClient(window, false);

        QCOMPARE(code(preview("block/new")), QString()); // the control: an ordinary vacant path works
        QCOMPARE(code(preview("../escape")), "invalid_asset_path");
        QCOMPARE(code(preview("block/plain")), "asset_conflict");

        const auto missing = preview("block/sub/deeper/new");
        QCOMPARE(code(missing), "missing_directory");
        QCOMPARE(missing["error"].toObject()["details"].toObject()["folder"].toString(), "assets/nova/textures/block/sub");
        QVERIFY2(!text(missing).contains("must already exist"), qPrintable(text(missing)));
        const auto missingModel = preview("block/sub/new", true);
        QCOMPARE(code(missingModel), "missing_directory"); // models and textures agree
        QCOMPARE(missingModel["error"].toObject()["details"].toObject()["folder"].toString(), "assets/nova/models/block/sub");

        const auto occupied = preview("block/hard");
        QCOMPARE(code(occupied), "asset_conflict");
        QVERIFY2(!text(occupied).contains("directory must"), qPrintable(text(occupied)));

        const auto linked = preview("linked/new");
        QCOMPARE(code(linked), "unsafe_asset");
        QCOMPARE(linked["error"].toObject()["details"].toObject()["path"].toString(), "assets/nova/textures/linked");
        QVERIFY2(text(linked).contains("link"), qPrintable(text(linked)));

        // Errors stay protocol-shaped (text-only tool errors) and never reveal absolute paths.
        for (const auto &result : {missing, missingModel, occupied, linked}) {
            QVERIFY(!result.contains("unexpected"));
            QVERIFY2(!text(result).contains(QDir::toNativeSeparators(dir.path())) && !text(result).contains(dir.path()),
                     qPrintable(text(result)));
        }
        QCOMPARE(fingerprint(outside), outsideBefore);
        QCOMPARE(fingerprint(root), before);
        QVERIFY(RemoveDirectoryW(native(root + "/assets/nova/textures/linked").c_str()));
    }

    // A deleted file that the index still lists no longer blocks creation forever: Nova reports the
    // stale entry, re-indexes, and the next attempt succeeds. No user file is touched.
    void staleIndexEntry() {
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        const QString gone = "assets/nova/textures/block/gone.png";
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        QVERIFY(image.save(root + '/' + gone));
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window) && nova::EditorTestAccess::indexed(window, gone));
        nova::EditorTestAccess::stopMonitor(window);
        QVERIFY(QFile::remove(root + '/' + gone));
        QTest::qWait(200);
        QVERIFY(nova::EditorTestAccess::indexed(window, gone)); // the deletion was missed
        const auto before = fingerprint(root);
        connectClient(window, true);
        const auto stale = preview("block/gone");
        QCOMPARE(code(stale), "stale_index");
        QCOMPARE(stale["error"].toObject()["details"].toObject()["reason"].toString(), "stale_index_entry");
        QCOMPARE(stale["error"].toObject()["details"].toObject()["path"].toString(), gone);
        QTRY_VERIFY_WITH_TIMEOUT(!nova::EditorTestAccess::indexed(window, gone) && nova::EditorTestAccess::indexReady(window), 10000);
        const auto created = preview("block/gone", false, "nova.asset.create");
        QVERIFY2(created["provisional"].toBool(), qPrintable(text(created)));
        QCOMPARE(fingerprint(root), before); // creation is an unsaved draft; nothing written to the pack
    }

    // Parallel calls wait in order instead of being refused. A canceled waiting call is dropped
    // without a reply, a fifth waiting call is refused as busy, and a duplicate waiting ID ends the
    // connection like a duplicate running one.
    void queuedCalls() {
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        connectClient(window, false);
        info();
        const QJsonObject list{{"generation", scope["generation"]}};
        {
            WorkerGate gate; // keeps call 1000 running
            auto burst = toolCall(1000, "nova.assets.list", list);
            for (int id = 1001; id <= 1005; ++id)
                burst += toolCall(id, "nova.project.info", {});
            burst += request(-1, "notifications/cancelled", {{"requestId", 1003}});
            burst += toolCall(1006, "nova.project.info", {});
            client.write(burst);
            const auto refused = receive(client);
            QCOMPARE(refused["id"].toInt(), 1005);
            QCOMPARE(code(payload(refused)), "busy");
            QTest::qWait(200);
            QVERIFY(!client.canReadLine());
        }
        QList<int> order;
        for (int i = 0; i < 5; ++i) {
            const auto reply = receive(client);
            order << reply["id"].toInt();
            QVERIFY2(!payload(reply).contains("error"), qPrintable(text(reply)));
        }
        QCOMPARE(order, (QList<int>{1000, 1001, 1002, 1004, 1006}));
        QTest::qWait(200);
        QVERIFY(!client.canReadLine()); // the canceled call never ran
        QCOMPARE(info()["generation"], scope["generation"]);
        {
            WorkerGate gate;
            client.write(toolCall(1010, "nova.assets.list", list) + toolCall(1011, "nova.project.info", {}) +
                         toolCall(1011, "nova.project.info", {}));
            QTRY_COMPARE_WITH_TIMEOUT(client.state(), QProcess::NotRunning, 10000);
        }
        QCOMPARE(window.findChild<Service *>()->status(), "Disabled");
    }

    // What agents use to edit precisely: progress for bulk work, the selection that clips pixel
    // commands, choosing the active layer, registry operations, multi-step undo, opening models and
    // closing clean documents, and a bulk edit refused before it could run out of document tabs.
    void agentEditing() {
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::gray);
        for (const auto name : {"second", "third"})
            QVERIFY(image.save(root + "/assets/nova/textures/block/" + name + ".png"));
        QVERIFY(QDir().mkpath(root + "/assets/nova/textures/many"));
        for (int i = 0; i < 34; ++i)
            QVERIFY(image.save(root + QString("/assets/nova/textures/many/t%1.png").arg(i)));
        const QJsonObject face{{"texture", "#all"}};
        const QJsonObject element{{"from", QJsonArray{0, 0, 0}}, {"to", QJsonArray{16, 16, 16}},
                                  {"faces", QJsonObject{{"north", face}}}};
        write(root + "/assets/nova/models/block/cube.json", QJsonDocument(QJsonObject{
            {"textures", QJsonObject{{"all", "nova:block/plain"}}}, {"elements", QJsonArray{element}}}).toJson());
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        connectClient(window, true);
        info();
        const auto ids = assetIds();
        const auto plain = ids["assets/nova/textures/block/plain.png"];
        const auto second = ids["assets/nova/textures/block/second.png"];
        QVERIFY(!plain.isEmpty() && !second.isEmpty());

        // A bulk edit reports each committed target through notifications/progress.
        auto bulk = scope;
        bulk["targets"] = QJsonArray{QJsonObject{{"assetId", plain}, {"expectedRevision", "current"}},
                                     QJsonObject{{"assetId", second}, {"expectedRevision", "current"}}};
        bulk["operations"] = QJsonArray{QJsonObject{{"command", "invert"}}};
        client.write(toolCall(2000, "nova.texture.adjust_bulk", bulk, "bulk-progress"));
        QList<QJsonObject> notes;
        const auto adjusted = payload(replyTo(2000, &notes));
        QVERIFY2(adjusted["committed"].toInt() == 2, qPrintable(text(adjusted)));
        QCOMPARE(notes.size(), 2);
        for (int i = 0; i < notes.size(); ++i) {
            QCOMPARE(notes[i]["method"].toString(), "notifications/progress");
            const auto params = notes[i]["params"].toObject();
            QCOMPARE(params["progressToken"].toString(), "bulk-progress");
            QCOMPARE(params["progress"].toInt(), i + 1);
            QCOMPARE(params["total"].toInt(), 2);
        }
        const auto doc = openDocument(window, "plain.png");
        QVERIFY(doc);

        // The selection is part of the document state and clips the fill.
        const auto document = call("nova.edit.capabilities", {{"generation", scope["generation"]}, {"assetId", plain}})["document"].toObject();
        QCOMPARE(document["selection"].toObject()["present"].toBool(), false);
        const auto base = document["activeLayer"].toString();
        auto batch = target(plain, document["revision"]);
        batch["operations"] = QJsonArray{
            QJsonObject{{"command", "select_rect"}, {"x", 0}, {"y", 0}, {"width", 4}, {"height", 4}},
            QJsonObject{{"command", "fill_rect"}, {"layer", base}, {"x", 0}, {"y", 0}, {"width", 16}, {"height", 16},
                        {"color", "#ff0000"}, {"opacity", 255}}};
        const auto selected = call("nova.edit.batch", batch);
        QVERIFY2(!selected.contains("error"), qPrintable(text(selected)));
        QCOMPARE(selected["selection"].toObject()["present"].toBool(), true);
        QCOMPARE(selected["selection"].toObject()["bounds"].toObject(),
                 (QJsonObject{{"x", 0}, {"y", 0}, {"width", 4}, {"height", 4}}));
        QCOMPARE(doc->image.pixelColor(1, 1), QColor(Qt::red));
        QVERIFY(doc->image.pixelColor(8, 8) != QColor(Qt::red));
        auto single = [&](const QString &tool, const QJsonValue &revision, const QJsonObject &operation) {
            auto args = target(plain, revision);
            args["operation"] = operation;
            return call(tool, args);
        };
        const auto cleared = single("nova.texture.edit", selected["revision"], {{"command", "deselect"}});
        QCOMPARE(cleared["selection"].toObject()["present"].toBool(), false);

        // Drawing paints the active layer; layer_select chooses it, on its own or in a batch.
        const auto added = single("nova.layer.edit", cleared["revision"], {{"command", "layer_add"}, {"layer", base}, {"name", "Ink"}});
        QVERIFY2(!added.contains("error"), qPrintable(text(added)));
        QString other;
        for (const QJsonValue &layer : added["layers"].toArray())
            if (layer["id"].toString() != added["activeLayer"].toString())
                other = layer["id"].toString();
        QVERIFY(!other.isEmpty());
        const auto chosen = single("nova.layer.edit", added["revision"], {{"command", "layer_select"}, {"layer", other}});
        QVERIFY2(chosen["changed"].toBool(), qPrintable(text(chosen)));
        QCOMPARE(chosen["activeLayer"].toString(), other);
        QCOMPARE(doc->activeLayer(), QUuid(other));

        // Registry operations, including ones that change the canvas size.
        const auto flipped = single("nova.texture.edit", chosen["revision"],
                                    {{"command", "flip"}, {"params", QJsonObject{{"direction", "Horizontal"}}}});
        QVERIFY2(!flipped.contains("error"), qPrintable(text(flipped)));
        const auto turned = single("nova.texture.edit", flipped["revision"],
            {{"command", "image_rotate"}, {"params", QJsonObject{{"turns", QString("180") + QChar(0x00B0)}}}});
        QVERIFY2(!turned.contains("error"), qPrintable(text(turned)));
        const auto cropped = single("nova.texture.edit", turned["revision"],
            {{"command", "crop"}, {"params", QJsonObject{{"x", 0}, {"y", 0}, {"width", 8}, {"height", 8}}}});
        QVERIFY2(cropped["width"].toInt() == 8, qPrintable(text(cropped)));
        // Validation checks only the form named by the command, and still says what is wrong with it.
        const auto incomplete = single("nova.texture.edit", cropped["revision"], {{"command", "fill"}});
        QCOMPARE(code(incomplete), "invalid_arguments");
        QVERIFY2(incomplete["error"].toObject()["details"].toObject()["pointer"].toString().startsWith("/operation/"),
                 qPrintable(text(incomplete)));
        const auto unknown = single("nova.texture.edit", cropped["revision"], {{"command", "nope"}});
        QVERIFY2(unknown["error"].toObject()["message"].toString().contains("unknown command \"nope\""),
                 qPrintable(text(unknown)));

        // Several history steps in one call; redo stops at the end of what can be redone.
        auto history = target(plain, cropped["revision"]);
        history["steps"] = 3;
        const auto undone = call("nova.edit.undo", history);
        QCOMPARE(undone["stepsApplied"].toInt(), 3);
        QCOMPARE(undone["width"].toInt(), 16);
        history["expectedRevision"] = undone["revision"];
        history["steps"] = 32;
        const auto redone = call("nova.edit.redo", history);
        QCOMPARE(redone["stepsApplied"].toInt(), 3);
        QCOMPARE(redone["width"].toInt(), 8);

        // Closing frees a tab only for a document without unsaved changes.
        auto open = scope;
        open["assetId"] = plain;
        QCOMPARE(code(call("nova.asset.close", open)), "document_dirty");
        QVERIFY(openDocument(window, "plain.png"));
        open["assetId"] = ids["assets/nova/textures/block/third.png"];
        QVERIFY(call("nova.asset.open", open)["opened"].toBool());
        QVERIFY(openDocument(window, "third.png"));
        const auto closed = call("nova.asset.close", open);
        QVERIFY2(closed["closed"].toBool(), qPrintable(text(closed)));
        QVERIFY(!openDocument(window, "third.png"));
        QCOMPARE(code(call("nova.asset.close", open)), "document_not_open");

        // Models open in a model tab and close the same way.
        open["assetId"] = ids["assets/nova/models/block/cube.json"];
        const auto model = call("nova.asset.open", open);
        QVERIFY2(model["opened"].toBool(), qPrintable(text(model)));
        QCOMPARE(model["document"].toObject()["elementCount"].toInt(), 1);
        QVERIFY2(call("nova.asset.close", open)["closed"].toBool(), "model close");

        // A bulk edit that would need more tabs than are free is refused before any commit.
        const auto before = window.documents().all().size();
        QJsonArray many;
        for (int i = 0; i < 34; ++i)
            many.append(QJsonObject{{"assetId", ids[QString("assets/nova/textures/many/t%1.png").arg(i)]},
                                    {"expectedRevision", "current"}});
        bulk["targets"] = many;
        const auto refused = call("nova.texture.adjust_bulk", bulk);
        QCOMPARE(code(refused), "open_limit");
        QCOMPARE(refused["error"].toObject()["details"].toObject()["closedTargets"].toInt(), 34);
        QCOMPARE(window.documents().all().size(), before);
    }
};

QTEST_MAIN(McpReliabilityTest)
#include "mcp_reliability_test.moc"
