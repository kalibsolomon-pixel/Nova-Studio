// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "cube.h"
#include "modelpanel.h"
#include "fabric_testing.h"
#include <QListWidget>
#include <QCheckBox>
#include <QMessageBox>
#include <QPushButton>
#include "scope.h"
#include "service.h"
#include <QCryptographicHash>
#include <QDialog>
#include <QDockWidget>
#include <QEventLoop>
#include <QDirIterator>
#include <QJsonDocument>
#include <QProcess>
#include <QSemaphore>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTabWidget>
#include <cstdio>
#include <QtTest>
#include <QtConcurrent>
#include <qt_windows.h>
#include <psapi.h>

using namespace nova::mcp;

namespace nova {
class EditorTestAccess {
  public:
    static void open(Window &window, const QString &file) { window.openTexture(file); }
    static void openModel(Window &window, const QString &asset) { window.openModelAsset(asset); }
    static ModelPanel *models(Window &window) { return window.modelPanel; }
    static void checkpoint(Window &window) { window.backgroundSafety(); }
    static auto documents(Window &window) { return window.session.all(); }
    static bool writeMetrics(Window &window, const QString &path) { return window.metrics.write(path); }
    static const auto &comparisons(Window &window) { return window.comparisons; }
    static QStringList unsavedOutputs(Window &window) { return window.unsavedMinecraftOutputs(); }
    static void relatedAssets(Window &window, const QString &path) { window.showRelatedAssets(path); }
    static bool checkpointBusy(Window &window) { return window.ioBusy; }
    static bool indexReady(Window &window) {
        return !window.indexBusy && !window.indexDebounce->isActive() && window.pendingChanges.isEmpty();
    }
};
}

namespace {
QString bridge() { return QString::fromUtf8(NOVA_MCP_TEST_BRIDGE); }
void write(const QString &file, const QByteArray &data) {
    QVERIFY(QDir().mkpath(QFileInfo(file).path()));
    QFile output(file);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write(data), data.size());
}
QMap<QString, QByteArray> fingerprint(const QString &root, bool sourceOnly = false) {
    QMap<QString, QByteArray> hashes;
    QDirIterator files(root, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (sourceOnly && QDir(root).relativeFilePath(file.fileName()).startsWith(".nova/"))
            continue;
        if (file.open(QIODevice::ReadOnly))
            hashes.insert(QDir(root).relativeFilePath(file.fileName()),
                          QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
    }
    return hashes;
}
QByteArray request(int id, const QString &method, const QJsonObject &params = {}) {
    QJsonObject data{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (id >= 0)
        data["id"] = id;
    return QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n';
}
QByteArray call(int id, const QString &tool, const QJsonObject &args = {}) {
    return request(id, "tools/call", {{"name", tool}, {"arguments", args}});
}
void start(QProcess &process, const QString &code, const QString &program = bridge(),
           const QStringList &arguments = {}) {
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("NOVA_MCP_CODE", code);
    process.setProcessEnvironment(env);
    process.start(program, arguments);
    QVERIFY(process.waitForStarted());
}
void grantEditing(QWidget &window, QDialog *dialog, Service *service) {
    QTimer::singleShot(0, &window, [] {
        auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box && box->button(QMessageBox::Yes))
            box->button(QMessageBox::Yes)->click();
    });
    dialog->findChild<QPushButton *>("mcpAuthorizeEditing")->click();
    QCOMPARE(service->permission(), "Editing allowed");
}
void startSdk(QProcess &client, const QString &code, const QString &mode) {
    start(client, code, qEnvironmentVariable("NOVA_MCP_SDK_PYTHON"),
          {QString::fromUtf8(NOVA_MCP_TEST_SDK), bridge(), mode});
}
void finishSdk(QProcess &client, const QString &mode) {
    QTRY_COMPARE_WITH_TIMEOUT(client.state(), QProcess::NotRunning, 20000);
    qInfo().noquote() << "SDK" << mode << client.readAllStandardOutput();
    QVERIFY2(client.exitCode() == 0, client.readAllStandardError().constData());
}
QJsonObject receive(QProcess &process, int timeout = 5000) {
    if (!QTest::qWaitFor([&] { return process.canReadLine() || process.state() == QProcess::NotRunning; },
                         timeout))
        return {};
    const auto line = process.readLine();
    const auto result = QJsonDocument::fromJson(line).object();
    const auto evidence = qEnvironmentVariable("NOVA_MCP_EVIDENCE");
    if (!evidence.isEmpty() && !result.isEmpty()) {
        QFile file(evidence + "/protocol.jsonl");
        if (file.open(QIODevice::Append))
            file.write(line);
    }
    return result;
}
QJsonObject payload(const QJsonObject &response) {
    const auto result = response["result"].toObject();
    if (!result["isError"].toBool())
        return result["structuredContent"].toObject();
    // Errors must not carry structured content, which clients validate against the output schema.
    const auto content = result["content"].toArray();
    if (result.contains("structuredContent") || content.size() != 1 ||
        content.first().toObject()["type"] != "text")
        return {};
    return QJsonDocument::fromJson(content.first().toObject()["text"].toString().toUtf8()).object();
}
QString error(const QJsonObject &response) { return payload(response)["error"].toObject()["code"].toString(); }
void initialize(QProcess &process, const QString &version = "2025-11-25") {
    process.write(request(1, "initialize", {{"protocolVersion", version}, {"capabilities", QJsonObject{}},
        {"clientInfo", QJsonObject{{"name", "Nova integration tests"}, {"version", "1"}}}}));
    QCOMPARE(receive(process)["result"].toObject()["protocolVersion"].toString(), "2025-11-25");
    process.write(request(-1, "notifications/initialized"));
}
void saveScreenshot(QWidget *dialog, const QString &name) {
    const auto dir = qEnvironmentVariable("NOVA_MCP_EVIDENCE");
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    const auto ratio = qRound(dialog->devicePixelRatioF() * 100);
    QVERIFY(dialog->grab().save(dir + "/" + QString::number(ratio) + "-" + name + ".png"));
}
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
struct ProcessUsage {
    quint64 cpuTicks = 0;
    SIZE_T privateBytes = 0;
};
ProcessUsage usage(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    PROCESS_MEMORY_COUNTERS_EX memory{};
    auto ticks = [](FILETIME time) { return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    GetProcessTimes(process, &created, &exited, &kernel, &user);
    GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory));
    return {ticks(kernel) + ticks(user), memory.PrivateUsage};
}
}

class McpTest : public QObject {
    Q_OBJECT
  private slots:
    void schema() {
        Protocol protocol;
        QCOMPARE(protocol.receive("invalid").response["error"].toObject()["code"].toInt(), -32700);
        QCOMPARE(protocol.receive("[]").response["error"].toObject()["code"].toInt(), -32600);
        QCOMPARE(protocol.receive(request(3, "tools/list")).response["error"].toObject()["code"].toInt(), -32002);
        const auto init = protocol.receive(request(1, "initialize", {{"protocolVersion", "2024-11-05"},
            {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}}));
        QCOMPARE(init.response["result"].toObject()["protocolVersion"].toString(), "2025-11-25");
        protocol.receive(request(-1, "notifications/initialized"));
        const auto generation = randomToken();
        for (const auto &args : QList<QJsonObject>{{}, {{"generation", generation}, {"limit", 101}},
             {{"generation", generation}, {"offset", 1.25}}, {{"generation", generation}, {"category", "bad"}},
             {{"generation", generation}, {"limit", "10"}}, {{"generation", generation}, {"path", "../outside"}}})
            QCOMPARE(error(protocol.receive(call(4, "nova.assets.list", args)).response), "invalid_arguments");
        for (const auto &id : {"../../outside", "C:/Windows/win.ini", "assets/abc:stream"})
            QCOMPARE(error(protocol.receive(call(4, "nova.asset.inspect",
                {{"generation", generation}, {"assetId", id}})).response), "invalid_arguments");

        // Tool errors carry their code and message as text only, for every tool and output schema.
        auto result = [](const QJsonObject &response) { return response["result"].toObject(); };
        auto text = [](const QJsonObject &reply) {
            const auto block = reply["content"].toArray().first().toObject();
            return QJsonDocument::fromJson(block["text"].toString().toUtf8()).object();
        };
        const auto list = result(protocol.receive(request(5, "tools/list")).response)["tools"].toArray();
        QCOMPARE(list.size(), 39);
        int schemas = 0;
        for (const auto &entry : list) {
            const auto tool = entry.toObject();
            schemas += tool.contains("outputSchema");
            const auto rejected = result(protocol.receive(call(6, tool["name"].toString(), {{"unexpected", 1}})).response);
            QVERIFY2(rejected["isError"].toBool() && !rejected.contains("structuredContent"),
                     qPrintable(tool["name"].toString()));
            QCOMPARE(rejected["content"].toArray().size(), 1);
            // Field-specific: the message names the JSON pointer it reports in details.
            const auto failure = text(rejected)["error"].toObject();
            const auto pointer = failure["details"].toObject()["pointer"].toString();
            QCOMPARE(failure["code"].toString(), "invalid_arguments");
            QVERIFY2(pointer == "/unexpected" ? failure["message"].toString().startsWith("Unknown argument /unexpected.")
                                              : failure["message"].toString() == "Missing required argument " + pointer + '.',
                     qPrintable(tool["name"].toString() + ": " + failure["message"].toString()));
            QCOMPARE(failure.keys(), (QStringList{"code", "details", "message"}));
        }
        QCOMPARE(schemas, 34);
        const auto denied = result(toolReply(7, toolError("permission_denied", "Editing is not authorized.")));
        QVERIFY(denied["isError"].toBool() && !denied.contains("structuredContent"));
        QCOMPARE(denied["content"].toArray().first().toObject()["text"].toString(),
                 R"({"error":{"code":"permission_denied","message":"Editing is not authorized."}})");
        const auto ok = result(toolReply(8, {{"revision", "r"}, {"dirty", false}, {"_pngImage", "AA=="}}));
        QVERIFY(!ok.contains("isError"));
        QCOMPARE(ok["structuredContent"].toObject(), (QJsonObject{{"revision", "r"}, {"dirty", false}}));
        QCOMPARE(text(ok), ok["structuredContent"].toObject());
        const auto content = ok["content"].toArray();
        QCOMPARE(content.size(), 2);
        QCOMPARE(content[1].toObject()["type"].toString(), "image");
    }

    void containment() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto root = dir.path() + "/pack";
        const auto outside = dir.path() + "/outside";
        write(root + "/assets/nova/models/ordinary.json", "{}");
        write(outside + "/secret.json", "{\"secret\":true}");
        const auto canonical = QDir(root).canonicalPath();
        Scope scope(canonical);
        QVERIFY(!scope.identity().isEmpty());
        QVERIFY(scope.open("assets/nova/models/ordinary.json"));
        QVERIFY(!scope.open("assets/nova/models/../../../../outside/secret.json"));
        QVERIFY(!scope.open(outside + "/secret.json"));
        QVERIFY(!scope.open("assets/nova/models/ordinary.json:stream"));
        Scope stale(canonical, "different file ID");
        QVERIFY(!stale.open("assets/nova/models/ordinary.json"));

        auto native = [](const QString &path) { return QDir::toNativeSeparators(path).toStdWString(); };
        const auto secret = native(outside + "/secret.json");
        const auto hard = native(root + "/assets/nova/models/hard.json");
        QVERIFY(CreateHardLinkW(hard.c_str(), secret.c_str(), nullptr));
        QVERIFY(!scope.open("assets/nova/models/hard.json"));
        const auto link = native(root + "/assets/nova/models/link.json");
        const bool symlink = CreateSymbolicLinkW(link.c_str(), secret.c_str(), 0x2);
        if (symlink)
            QVERIFY(!scope.open("assets/nova/models/link.json"));
        else
            qInfo("Symlink creation unavailable (Win32 error %lu)", GetLastError());
        const auto junction = root + "/assets/nova/linked";
        QProcess cmd;
        cmd.setProgram("cmd.exe");
        cmd.setNativeArguments("/d /c mklink /J \"" + QDir::toNativeSeparators(junction) +
                               "\" \"" + QDir::toNativeSeparators(outside) + "\"");
        cmd.start();
        QVERIFY(cmd.waitForFinished());
        QCOMPARE(cmd.exitCode(), 0);
        QVERIFY(!scope.open("assets/nova/linked/secret.json"));
        QVERIFY(!scope.vacant("assets/nova/linked/new.png"));
        QVERIFY(!scope.vacant("assets/nova/models/hard.json"));
        // The reason is reported with the first folder (or the file) at fault.
        QString failed;
        QCOMPARE(scope.vacancy("assets/nova/linked/new.png", &failed), Vacancy::Unsafe);
        QCOMPARE(failed, "assets/nova/linked");
        QCOMPARE(scope.vacancy("assets/nova/models/hard.json", &failed), Vacancy::Occupied);
        QCOMPARE(scope.vacancy("assets/nova/absent/deeper/new.png", &failed), Vacancy::Missing);
        QCOMPARE(failed, "assets/nova/absent");
        QCOMPARE(scope.vacancy("assets/nova/models/new.json", &failed), Vacancy::Vacant);
        QCOMPARE(scope.vacancy("assets/nova/models/../new.png", &failed), Vacancy::Unsafe);
        QVERIFY(!scope.vacant("assets/nova/models/../new.png"));
        QVERIFY(scope.vacant("assets/nova/models/new.png"));
        Snapshot snapshot;
        snapshot.root = canonical;
        snapshot.rootIdentity = scope.identity();
        snapshot.idKey = randomToken().toLatin1();
        snapshot.generation = randomToken();
        snapshot.version = "26.1";
        const QString escaped = "assets/nova/linked/secret.json";
        snapshot.index.files.append(escaped);
        snapshot.index.records.insert(escaped, nova::AssetIdentity::fromPath(canonical, escaped));
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto report = inspect(snapshot, "nova.pack.validate", {{"generation", snapshot.generation}}, cancel);
        QCOMPARE(report["diagnostics"].toArray().first().toObject()["code"].toString(), "unsafe_path");
        const auto listing = inspect(snapshot, "nova.assets.list", {{"generation", snapshot.generation}}, cancel);
        QVERIFY(listing["assets"].toArray().isEmpty());
        const QString hardAsset = "assets/nova/models/hard.json";
        const QString ordinary = "assets/nova/models/ordinary.json";
        write(root + '/' + ordinary, R"({"parent":"nova:hard"})");
        for (const auto &path : {ordinary, hardAsset}) {
            snapshot.index.files.append(path);
            snapshot.index.records.insert(path, nova::AssetIdentity::fromPath(canonical, path));
        }
        const auto related = inspect(snapshot, "nova.assets.context",
            {{"generation", snapshot.generation}, {"assetIds", QJsonArray{assetIdentifier(snapshot, ordinary)}}}, cancel);
        QCOMPARE(related["assets"].toArray().size(), 1);
        QVERIFY(related["references"].toArray().isEmpty());
        QCOMPARE(related["warnings"].toArray().first().toObject()["code"], "unsafe_dependency");
        const auto batch = inspect(snapshot, "nova.assets.inspect_batch",
            {{"generation", snapshot.generation}, {"assetIds", QJsonArray{
                assetIdentifier(snapshot, hardAsset), assetIdentifier(snapshot, escaped)}}}, cancel);
        for (const auto &item : batch["results"].toArray())
            QCOMPARE(item.toObject()["error"].toObject()["code"], "unsafe_asset");
        // Remove only the junction itself, before QTemporaryDir performs fixture cleanup.
        QVERIFY(RemoveDirectoryW(native(junction).c_str()));
    }

    void liveEditor() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qApp->setProperty("settingsFile", dir.path() + "/studio.ini");
        const auto root = dir.path() + "/disposable-pack";
        const auto texture = QString("assets/nova/textures/block/a.png");
        const auto model = QString("assets/nova/models/block/a.json");
        write(root + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Test"}})");
        write(root + '/' + model, R"({"parent":"minecraft:block/cube_all","textures":{"all":"nova:block/a"}})");
        write(root + '/' + texture + ".mcmeta", R"({"animation":{"frametime":2,"frames":[0,1]}})");
        QImage image(8, 16, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        QVERIFY(QDir().mkpath(QFileInfo(root + '/' + texture).path()));
        QVERIFY(image.save(root + '/' + texture));
        const auto other = dir.path() + "/other-pack";
        write(other + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Other"}})");
        write(other + '/' + model, R"({"textures":{"all":"nova:block/a"}})");
        write(other + '/' + texture + ".mcmeta", R"({"animation":{"frames":[0,1]}})");
        QVERIFY(image.save(other + '/' + texture));
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        const auto action = window.findChild<QAction *>("mcpAccess");
        QVERIFY(action);
        action->trigger();
        auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
        QVERIFY(dialog);
        auto status = dialog->findChild<QLabel *>("mcpAccessStatus");
        auto authorize = dialog->findChild<QPushButton *>("mcpAuthorize");
        auto copy = dialog->findChild<QPushButton *>("mcpCopyCode");
        auto revoke = dialog->findChild<QPushButton *>("mcpRevoke");
        QVERIFY(status && authorize && copy && revoke);
        QCOMPARE(status->text(), "Disabled");
        saveScreenshot(dialog, "disabled");
        auto doc = window.documents().open(root + '/' + texture);
        QVERIFY(doc);
        const auto serial = doc->changeSerial();
        const auto history = doc->historyBytes();
        const auto modified = doc->modified();
        const auto before = fingerprint(root);

        QProcess denied;
        start(denied, randomToken());
        QTRY_COMPARE(denied.state(), QProcess::NotRunning);
        QCOMPARE(denied.exitCode(), 2);
        QVERIFY(denied.readAllStandardOutput().isEmpty());
        authorize->setFocus();
        QTest::keyClick(authorize, Qt::Key_Space);
        QCOMPARE(status->text(), "Waiting for client");
        QVERIFY(dialog->findChild<QLabel *>("mcpAccessPermission")->text().endsWith("Read-only"));
        saveScreenshot(dialog, "authorized");
        QVERIFY(copy->isEnabled());
        const auto service = window.findChild<Service *>();
        QVERIFY(service);
        const auto code = service->connectionCode();

        QLocalSocket invalid;
        invalid.connectToServer("nova-mcp-" + code.left(32));
        QTRY_COMPARE(invalid.state(), QLocalSocket::ConnectedState);
        invalid.write(QByteArray(32, '0') + '\n');
        QTRY_VERIFY(status->text().contains("invalid code"));
        saveScreenshot(dialog, "error");

        QProcess client;
        start(client, code);
        initialize(client, "2099-01-01");
        QTRY_VERIFY(status->text().startsWith("Connected"));
        saveScreenshot(dialog, "connected");
        client.write(request(2, "tools/list"));
        QCOMPARE(receive(client)["result"].toObject()["tools"].toArray().size(), 39);
        client.write("broken\n");
        QCOMPARE(receive(client)["error"].toObject()["code"].toInt(), -32700);

        client.write(call(3, "nova.project.info"));
        auto info = payload(receive(client));
        QElapsedTimer retry;
        retry.start();
        while (info.contains("error") && retry.elapsed() < 5000) {
            QTest::qWait(30);
            client.write(call(3, "nova.project.info"));
            info = payload(receive(client));
        }
        QCOMPARE(info["name"].toString(), "disposable-pack");
        const auto generation = info["generation"].toString();
        QVERIFY(!generation.isEmpty());
        QJsonObject args{{"generation", generation}};
        client.write(call(4, "nova.assets.list", {{"generation", generation}, {"limit", 1}}));
        auto first = payload(receive(client));
        QCOMPARE(first["assets"].toArray().size(), 1);
        QCOMPARE(first["nextOffset"].toInt(), 1);
        client.write(call(5, "nova.assets.list", args));
        const auto assets = payload(receive(client))["assets"].toArray();
        QCOMPARE(assets.size(), 4);
        QMap<QString, QString> ids;
        for (const auto &item : assets)
            ids[item.toObject()["path"].toString()] = item.toObject()["assetId"].toString();
        QCOMPARE(ids.value(first["assets"].toArray().first().toObject()["path"].toString()),
                 first["assets"].toArray().first().toObject()["assetId"].toString());
        const QList<std::tuple<QString, QString, QString>> checks{
            {"nova.asset.inspect", model, "modelType"}, {"nova.model.inspect", model, "parent"},
            {"nova.texture.inspect", texture, "layerCount"},
            {"nova.animation.inspect", texture + ".mcmeta", "frameCount"}};
        for (const auto &[tool, path, field] : checks) {
            client.write(call(6, tool, {{"generation", generation}, {"assetId", ids[path]}}));
            const auto item = payload(receive(client));
            QVERIFY2(item["metadata"].toObject().contains(field), qPrintable(tool));
        }
        const QJsonObject workflow{{"generation", generation}, {"projectId", info["projectId"]},
            {"assetIds", QJsonArray{ids[texture]}}, {"scope", "animation_sidecars"}};
        auto relatedArgs = workflow;
        relatedArgs.remove("scope");
        relatedArgs["relationships"] = QJsonArray{"animation"};
        client.write(call(16, "nova.assets.context", relatedArgs));
        const auto related = payload(receive(client))["references"].toArray();
        QCOMPARE(related.size(), 1);
        QCOMPARE(related.first().toObject()["toAssetId"], ids[texture + ".mcmeta"]);
        client.write(call(17, "nova.workflow.validate_changes", workflow));
        QCOMPARE(payload(receive(client))["files"], 2);
        client.write(call(7, "nova.pack.validate", args));
        const auto validation = payload(receive(client));
        QVERIFY(validation["diagnostics"].isArray());
        QCOMPARE(validation["files"].toInt(), 4);
        client.write(call(8, "nova.asset.inspect", {{"generation", generation}, {"assetId", randomToken()}}));
        QCOMPARE(error(receive(client)), "asset_not_found");
        client.write(call(9, "nova.pack.validate", args) +
                     request(-1, "notifications/cancelled", {{"requestId", 9}}) + request(10, "ping"));
        QCOMPARE(receive(client)["id"].toInt(), 10);
        QTRY_VERIFY(!service->findChild<QFutureWatcherBase *>());
        QCOMPARE(doc->changeSerial(), serial);
        QCOMPARE(doc->historyBytes(), history);
        QCOMPARE(doc->modified(), modified);
        QCOMPARE(fingerprint(root), before);

        auto path = window.findChild<QLineEdit *>("projectPath");
        QVERIFY(path);
        {
            WorkerGate gate;
            client.write(call(11, "nova.pack.validate", args) + request(12, "ping"));
            QCOMPARE(receive(client)["id"].toInt(), 12);
            path->setText(other);
            QTest::keyClick(path, Qt::Key_Return);
            QCOMPARE(error(receive(client)), "stale_project");
        }
        QTRY_COMPARE(status->text().section('\n', 0, 0), "Project changed");
        saveScreenshot(dialog, "project-changed");
        client.write(call(13, "nova.asset.inspect", {{"generation", generation}, {"assetId", ids[model]}}));
        QCOMPARE(error(receive(client)), "stale_project");
        client.closeWriteChannel();
        QTRY_COMPARE(client.state(), QProcess::NotRunning);
        QTRY_COMPARE(status->text().section('\n', 0, 0), "Disabled");

        const auto sdk = qEnvironmentVariable("NOVA_MCP_SDK_PYTHON");
        authorize->click();
        const auto newCode = service->connectionCode();
        if (!sdk.isEmpty()) {
            QProcess actual;
            start(actual, newCode, sdk, {QString::fromUtf8(NOVA_MCP_TEST_SDK), bridge()});
            QTRY_COMPARE_WITH_TIMEOUT(actual.state(), QProcess::NotRunning, 20000);
            const auto report = actual.readAllStandardOutput();
            qInfo().noquote() << report;
            QVERIFY2(actual.exitCode() == 0, actual.readAllStandardError().constData());

            const auto beforeOther = fingerprint(other);
            const auto switchDoc = window.documents().open(other + '/' + texture);
            QVERIFY(switchDoc);
            const auto beforeSwitch = switchDoc->changeSerial();
            const auto beforeSwitchCursor = switchDoc->historyCursor();
            grantEditing(window, dialog, service);
            QProcess switching;
            startSdk(switching, service->connectionCode(), "switch");
            QTRY_VERIFY_WITH_TIMEOUT(switching.canReadLine(), 12000);
            QCOMPARE(switching.readLine().trimmed(), QByteArray("switch"));
            path->setText(root);
            QTest::keyClick(path, Qt::Key_Return);
            QCOMPARE(service->status(), "Project changed");
            saveScreenshot(dialog, "sdk-project-switched");
            switching.write("continue\n");
            finishSdk(switching, "switch");
            QCOMPARE(switchDoc->changeSerial(), beforeSwitch);
            QCOMPARE(switchDoc->historyCursor(), beforeSwitchCursor);
            const auto newDoc = window.documents().open(root + '/' + texture);
            QVERIFY(newDoc);
            QVERIFY(!newDoc->modified());
            QCOMPARE(newDoc->image, switchDoc->image);
            QCOMPARE(fingerprint(root), before);
            QCOMPARE(fingerprint(other), beforeOther);
        } else {
            QProcess next;
            start(next, newCode);
            initialize(next);
            next.write(QByteArray(RequestLimit + 2, 'x') + '\n');
            QCOMPARE(receive(next)["error"].toObject()["code"].toInt(), -32600);
            QTRY_COMPARE(next.state(), QProcess::NotRunning);
        }

        for (const bool closeEditor : {false, true}) {
            QTRY_COMPARE(status->text().section('\n', 0, 0), "Disabled");
            authorize->click();
            QProcess ending;
            start(ending, service->connectionCode());
            initialize(ending);
            ending.write(call(2, "nova.project.info"));
            const QJsonValue currentGeneration = payload(receive(ending)).value("generation");
            QVERIFY(currentGeneration.isString());
            {
                WorkerGate gate;
                ending.write(call(3, "nova.pack.validate", {{"generation", currentGeneration}}) +
                             request(4, "ping"));
                QCOMPARE(receive(ending)["id"].toInt(), 4);
                if (closeEditor) {
                    QVERIFY(window.close());
                    QCOMPARE(service->status(), "Disabled");
                }
                else
                    ending.closeWriteChannel();
                QTRY_COMPARE(ending.state(), QProcess::NotRunning);
                QVERIFY(ending.readAllStandardOutput().isEmpty());
            }
            QTRY_VERIFY(!service->findChild<QFutureWatcherBase *>());
        }
    }

    void editing();
    void creationWorkflow();
    void phase4ePack();
    void processRecovery();
    void previewLifecycle();
    void animationProfile_data();
    void animationProfile();
    void workflowLifecycle_data();
    void workflowLifecycle();

    void benchmark() {
        const auto root = qEnvironmentVariable("NOVA_MCP_BENCH_PACK");
        if (root.isEmpty())
            QSKIP("Set NOVA_MCP_BENCH_PACK to an existing disposable fixture.");
        QTemporaryDir tmp;
        qApp->setProperty("settingsFile", tmp.path() + "/studio.ini");
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        auto statusBar = window.statusBar();
        QTRY_VERIFY_WITH_TIMEOUT(statusBar->currentMessage().contains("assets indexed") ||
                                 statusBar->currentMessage().contains("assets ·"), 30000);
        QTest::qWait(1000);
        const auto disabledStart = usage(GetCurrentProcess());
        QTest::qWait(1500);
        const auto disabledEnd = usage(GetCurrentProcess());
        qInfo("disabled_idle_1500ms cpu_ms=%.2f private_mib=%.2f",
              (disabledEnd.cpuTicks - disabledStart.cpuTicks) / 10000.0,
              disabledEnd.privateBytes / 1048576.0);
        window.findChild<QAction *>("mcpAccess")->trigger();
        auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
        dialog->findChild<QPushButton *>("mcpAuthorize")->click();
        const auto code = window.findChild<Service *>()->connectionCode();
        QProcess client;
        QElapsedTimer roundTrip;
        roundTrip.start();
        start(client, code);
        initialize(client);
        qInfo("bridge_initialize_ms=%lld", roundTrip.elapsed());
        dialog->hide();
        const auto bridgeProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                                               DWORD(client.processId()));
        QVERIFY(bridgeProcess);
        const auto enabledStart = usage(GetCurrentProcess());
        const auto bridgeStart = usage(bridgeProcess);
        QTest::qWait(1500);
        const auto enabledEnd = usage(GetCurrentProcess());
        const auto bridgeEnd = usage(bridgeProcess);
        CloseHandle(bridgeProcess);
        qInfo("connected_idle_1500ms editor_cpu_ms=%.2f editor_private_mib=%.2f bridge_cpu_ms=%.2f bridge_private_mib=%.2f",
              (enabledEnd.cpuTicks - enabledStart.cpuTicks) / 10000.0, enabledEnd.privateBytes / 1048576.0,
              (bridgeEnd.cpuTicks - bridgeStart.cpuTicks) / 10000.0, bridgeEnd.privateBytes / 1048576.0);
        roundTrip.restart();
        client.write(request(2, "tools/list"));
        QCOMPARE(receive(client)["id"].toInt(), 2);
        qInfo("tools_list_ms=%lld", roundTrip.elapsed());
        QJsonObject info;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            roundTrip.restart();
            client.write(call(3, "nova.project.info"));
            info = payload(receive(client));
            return info.contains("generation");
        })(), 30000);
        qInfo("project_info_ms=%lld assets=%d", roundTrip.elapsed(), info["assetCount"].toInt());
        const auto generation = info["generation"];
        const auto offset = std::max(0, info["assetCount"].toInt() - 100);
        roundTrip.restart();
        client.write(call(4, "nova.assets.list", {{"generation", generation}, {"offset", offset}}));
        const auto page = payload(receive(client))["assets"].toArray();
        QVERIFY(!page.isEmpty());
        qInfo("last_page_ms=%lld", roundTrip.elapsed());
        QTimer heartbeat;
        heartbeat.setInterval(10);
        qint64 last = timer.elapsed(), maxGap = 0;
        connect(&heartbeat, &QTimer::timeout, this, [&] {
            const auto now = timer.elapsed();
            maxGap = std::max(maxGap, now - last);
            last = now;
        });
        heartbeat.start();
        const QJsonObject contextArgs{{"generation", generation}, {"projectId", info["projectId"]},
            {"assetIds", QJsonArray{page.first().toObject()["assetId"]}}};
        const auto beforeContext = usage(GetCurrentProcess());
        QList<qint64> warm;
        for (int i = 0; i < 10; ++i) {
            roundTrip.restart();
            client.write(call(20 + i, "nova.assets.context", contextArgs));
            const auto response = payload(receive(client, 11000));
            QVERIFY2(response["assets"].isArray(), qPrintable(QString::fromUtf8(QJsonDocument(response).toJson())));
            const auto milliseconds = roundTrip.elapsed();
            if (i == 0)
                qInfo("context_cold_ms=%lld", milliseconds);
            else
                warm.append(milliseconds);
        }
        std::sort(warm.begin(), warm.end());
        qInfo("context_warm_p50_ms=%lld context_warm_max_ms=%lld context_heartbeat_gap_ms=%lld context_private_delta_mib=%.2f",
              warm[warm.size() / 2], warm.last(), maxGap,
              (qint64(usage(GetCurrentProcess()).privateBytes) - qint64(beforeContext.privateBytes)) / 1048576.0);
        maxGap = 0;
        roundTrip.restart();
        client.write(call(5, "nova.pack.validate", {{"generation", generation}}));
        const auto validation = receive(client, 65000);
        qInfo("validate_ms=%lld max_ui_heartbeat_gap_ms=%lld files=%d error=%s", roundTrip.elapsed(),
              maxGap, payload(validation)["files"].toInt(), qPrintable(error(validation)));
        QVERIFY(payload(validation)["diagnostics"].isArray());
        roundTrip.restart();
        client.write(call(6, "nova.pack.validate", {{"generation", generation}}) +
                     request(-1, "notifications/cancelled", {{"requestId", 6}}) + request(7, "ping"));
        QCOMPARE(receive(client)["id"].toInt(), 7);
        qInfo("cancel_and_ping_ms=%lld", roundTrip.elapsed());
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            client.write(call(8, "nova.project.info"));
            return payload(receive(client)).contains("generation");
        })(), 10000);
        qInfo("available_after_cancel_ms=%lld", roundTrip.elapsed());
        client.write(call(9, "nova.pack.validate", {{"generation", generation}}));
        QVERIFY(window.close());
        QCOMPARE(window.findChild<Service *>()->status(), "Disabled");
        QTRY_COMPARE(client.state(), QProcess::NotRunning);
    }
};
void McpTest::phase4ePack() {
    if (qEnvironmentVariableIsEmpty("NOVA_MCP_SDK_PYTHON"))
        QSKIP("Set NOVA_MCP_SDK_PYTHON for the production transport.");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qApp->setProperty("settingsFile", dir.filePath("studio.ini"));
    const auto evidence = qEnvironmentVariable("NOVA_MCP_EVIDENCE");
    const bool live = qEnvironmentVariableIntValue("NOVA_PHASE4E_MINECRAFT") == 1;
    QVERIFY(!live || !evidence.isEmpty());
    const auto root = QDir::fromNativeSeparators(live ? evidence : dir.path()) + "/Phase 4E Test Pack";
    QVERIFY(!QFileInfo::exists(root));
    const QString animation = "assets/minecraft/textures/block/diamond_block.png";
    const QString texture = "assets/minecraft/textures/block/gold_block.png";
    const QString model = "assets/minecraft/models/block/bricks.json";
    write(root + "/pack.mcmeta", R"({"pack":{"description":"Nova Phase 4E development pack","min_format":75,"max_format":75}})");
    write(root + '/' + animation + ".mcmeta", R"({"animation":{"frametime":4,"frames":[0,1]}})");
    QVERIFY(QDir().mkpath(root + "/assets/minecraft/models/block"));
    if (!live) {
        write(root + "/assets/minecraft/models/block/nova_cycle_a.json",
              R"({"parent":"minecraft:block/nova_cycle_b"})");
        write(root + "/assets/minecraft/models/block/nova_cycle_b.json",
              R"({"parent":"minecraft:block/nova_cycle_a","textures":{"all":"minecraft:block/nova_missing"}})");
    }
    QImage sheet(16, 32, QImage::Format_RGBA8888);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 16; ++x)
            sheet.setPixelColor(x, y, QColor(y < 16 ? "#2a3cdf" : "#13c9eb"));
    QVERIFY(sheet.save(root + '/' + animation));
    QElapsedTimer clock;
    clock.start();
    nova::Window window(root, {}, clock);
    window.resize(1050, 710);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(window.activeCanvas());
    QCOMPARE(window.activeCanvas()->document->path, root + '/' + animation);
    auto panel = nova::EditorTestAccess::models(window);
    QTRY_VERIFY(panel->catalog()->editable(live ? animation : "assets/minecraft/models/block/nova_cycle_a.json"));
    auto version = panel->findChild<QComboBox *>("modelVersion");
    QVERIFY(version);
    version->setCurrentText("1.21.11");
    window.findChild<QAction *>("mcpAccess")->trigger();
    auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
    auto service = window.findChild<Service *>();
    QVERIFY(dialog && service);
    grantEditing(window, dialog, service);
    QProcess sdk;
    startSdk(sdk, service->connectionCode(), "phase4e-pack");
    auto next = [&](const QByteArray &expected) {
        QTRY_VERIFY_WITH_TIMEOUT(sdk.canReadLine() || sdk.state() == QProcess::NotRunning, 20000);
        const auto line = sdk.readLine().trimmed();
        QVERIFY2(line == expected, (line + sdk.readAllStandardError()).constData());
    };
    auto validated = [&] {
        QTRY_VERIFY_WITH_TIMEOUT(sdk.canReadLine() || sdk.state() == QProcess::NotRunning, 20000);
        const auto line = sdk.readLine();
        const auto result = QJsonDocument::fromJson(line).object();
        QVERIFY2(!result.isEmpty(), sdk.readAllStandardError().constData());
        QCOMPARE(result["counts"].toObject()["Error"].toInt(), 0);
        if (!evidence.isEmpty()) {
            QFile log(evidence + "/pack-validation.jsonl");
            QVERIFY(log.open(QIODevice::Append));
            QCOMPARE(log.write(line), line.size());
        }
    };
    auto expand = dialog->findChild<QToolButton *>("mcpReviewToggle");
    auto stages = dialog->findChild<QListWidget *>("mcpReviewStages");
    auto compare = dialog->findChild<QPushButton *>("mcpReviewCompare");
    QVERIFY(expand && stages && compare);
    expand->click();
    const auto &captures = nova::EditorTestAccess::comparisons(window);
    next("review-rejected");
    {
        // The stale second stage rejects the workflow before the first stage commits.
        const auto &rejected = service->workflowReview();
        QCOMPARE(rejected.state, "Rejected");
        QCOMPARE(rejected.stages.size(), 2);
        QCOMPARE(rejected.stages[0].state, "Not run");
        QCOMPARE(rejected.stages[1].state, "Rejected");
        QVERIFY(!rejected.error.isEmpty());
        QCOMPARE(stages->count(), 2);
        QVERIFY(stages->item(0)->text().startsWith("Not run"));
        QVERIFY(stages->item(1)->text().startsWith("Rejected"));
        QVERIFY(captures.isEmpty());
        saveScreenshot(dialog, "phase4e-review-rejected");
    }
    sdk.write("continue\n");
    next("review-ready");
    QCOMPARE(service->workflowReview().state, "Ready");
    QCOMPARE(stages->count(), 2);
    QVERIFY(stages->item(0)->text().contains(texture));
    QVERIFY(!compare->isEnabled());
    sdk.write("continue\n");
    validated();
    next("review-raster");
    QCOMPARE(service->workflowReview().state, "Completed");
    for (const auto &stage : service->workflowReview().stages)
        QCOMPARE(stage.state, "Committed");
    saveScreenshot(dialog, "phase4e-review");
    QCOMPARE(captures.size(), 2);
    QCOMPARE(captures[0].before.pixelColor(15, 8), QColor("#f528ab"));
    QCOMPARE(captures[0].after.pixelColor(15, 8), QColor("#52ef25"));
    QCOMPARE(captures[0].before.pixelColor(2, 2), QColor("#f528ab"));
    QCOMPARE(captures[0].after.pixelColor(2, 2), QColor("#2742f5"));
    QCOMPARE(captures[1].before.pixelColor(8, 8), QColor("#2a3cdf"));
    QCOMPARE(captures[1].after.pixelColor(8, 8), QColor("#f37c12"));
    // Animation comparisons report the frame position and the playback change.
    QVERIFY2(captures[1].detail.contains("Before frame 1 of 2 · 2 entries · 4 ticks"), qPrintable(captures[1].detail));
    QVERIFY2(captures[1].detail.contains("After frame 1 of 2 · 2 entries · 40 ticks"), qPrintable(captures[1].detail));
    for (int i = 0; i < 2; ++i) {
        stages->setCurrentRow(i);
        QVERIFY(compare->isEnabled());
        compare->click();
        auto viewer = window.findChild<QDialog *>("revisionComparison");
        QVERIFY(viewer);
        QVERIFY(viewer->findChild<QLabel *>("comparisonIdentity")->text().contains("Current"));
        for (auto combo : viewer->findChildren<QComboBox *>())
            if (combo->accessibleName() == "Pixel zoom")
                QCOMPARE(combo->currentText(), "800%"); // 16 px textures open at a readable scale
        saveScreenshot(viewer, i == 0 ? "phase4e-texture" : "phase4e-animation");
        viewer->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    QVERIFY(!QFileInfo::exists(root + '/' + texture));
    stages->setCurrentRow(0);
    dialog->findChild<QPushButton *>("mcpReviewOpen")->click();
    QCOMPARE(window.activeCanvas()->document->path, root + '/' + texture);
    window.findChild<QAction *>("saveCommand")->trigger();
    if (window.activeCanvas()->document->minecraftOutputPending())
        window.findChild<QAction *>("exportPngCommand")->trigger();
    window.findChild<QAction *>("saveCommand")->trigger();
    QVERIFY(QFileInfo::exists(root + '/' + texture));
    QVERIFY(!window.activeCanvas()->document->minecraftOutputPending());
    compare->click();
    auto viewer = window.findChild<QDialog *>("revisionComparison");
    QVERIFY(viewer);
    QVERIFY(viewer->findChild<QLabel *>("comparisonIdentity")->text().contains("Stale"));
    viewer->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    stages->setCurrentRow(1);
    dialog->findChild<QPushButton *>("mcpReviewOpen")->click();
    QCOMPARE(window.activeCanvas()->document->path, root + '/' + animation);
    QTimer exportDriver;
    connect(&exportDriver, &QTimer::timeout, &window, [&] {
        auto exportDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!exportDialog || exportDialog->objectName() != "animationExportDialog")
            return;
        auto button = exportDialog->findChild<QPushButton *>("commitAnimationExport");
        if (button && button->isEnabled()) {
            exportDriver.stop();
            button->click();
        }
    });
    exportDriver.start(25);
    window.findChild<QAction *>("exportPngCommand")->trigger();
    exportDriver.stop();
    window.findChild<QAction *>("saveCommand")->trigger();
    QCOMPARE(QImage(root + '/' + animation).pixelColor(8, 8), QColor("#f37c12"));
    QTest::qWait(650); // Let filesystem notifications settle before model preflight.
    sdk.write("continue\n");
    validated();
    next("review-model");
    QCOMPARE(service->workflowReview().state, "Completed");
    QVERIFY(captures.size() <= 4);
    QCOMPARE(captures.last().path, model);
    QVERIFY(!captures.last().before.isNull() && !captures.last().after.isNull());
    QVERIFY(captures.last().before != captures.last().after);
    QVERIFY(!captures.last().uvBefore.isNull() && !captures.last().uvAfter.isNull());
    QVERIFY(captures.last().uvBefore != captures.last().uvAfter);
    // Texels outside the mapped face are dimmed: the full face before, the left half after.
    QCOMPARE(captures.last().uvBefore.pixelColor(200, 128), QColor("#52ef25"));
    QVERIFY(captures.last().uvAfter.pixelColor(200, 128).value() < 140);
    QCOMPARE(captures.last().uvAfter.pixelColor(64, 200), QColor("#f528ab"));
    QCOMPARE(captures.last().uvAfter.pixelColor(64, 64), QColor("#2742f5"));
    compare->click();
    viewer = window.findChild<QDialog *>("revisionComparison");
    QVERIFY(viewer);
    saveScreenshot(viewer, "phase4e-model-comparison");
    for (auto combo : viewer->findChildren<QComboBox *>())
        if (combo->accessibleName() == "Preview type")
            combo->setCurrentIndex(1);
    saveScreenshot(viewer, "phase4e-uv-comparison");
    viewer->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    sdk.write("continue\n");
    finishSdk(sdk, "phase4e-pack");
    dialog->hide();

    auto tabs = window.findChild<QTabWidget *>("editorTabs");
    QVERIFY(tabs);
    const auto origin = tabs->currentWidget();
    QCOMPARE(origin->objectName(), "modelDocumentTab");
    const auto count = tabs->count();
    const auto modelDocument = panel->openModelDocument(model);
    QVERIFY(modelDocument && modelDocument->modified());
    // Pack synchronization refuses while the model is modified in memory.
    QVERIFY(nova::EditorTestAccess::unsavedOutputs(window).contains(model));
    const auto cursor = modelDocument->changeSerial();
    auto relatedAction = window.findChild<QAction *>("relatedAssets");
    QVERIFY(relatedAction);
    relatedAction->trigger();
    auto related = window.findChild<QDialog *>("relatedAssetsDialog");
    QVERIFY(related);
    auto list = related->findChild<QListWidget *>("relatedAssetsList");
    QVERIFY(list);
    QVERIFY(!list->findItems(texture, Qt::MatchContains).isEmpty());
    list->setCurrentItem(list->findItems(texture, Qt::MatchContains).first());
    saveScreenshot(related, "phase4e-related");
    related->findChild<QPushButton *>("relatedAssetsOpen")->click();
    QCOMPARE(window.activeCanvas()->document->path, root + '/' + texture);
    QCOMPARE(tabs->count(), count);
    related->findChild<QPushButton *>("relatedAssetsBack")->click();
    QCOMPARE(tabs->currentWidget(), origin);
    QCOMPARE(modelDocument->changeSerial(), cursor);
    QVERIFY(modelDocument->modified());
    window.findChild<QAction *>("saveCommand")->trigger();
    QVERIFY(QFileInfo::exists(root + '/' + model));
    QVERIFY(!modelDocument->modified());
    QVERIFY(nova::EditorTestAccess::unsavedOutputs(window).isEmpty());
    {
        QFile saved(root + '/' + model);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto textures = QJsonDocument::fromJson(saved.readAll()).object()["textures"].toObject();
        QCOMPARE(textures["particle"].toString(), "minecraft:block/gold_block");
    }
    emit tabs->tabCloseRequested(tabs->currentIndex());
    QVERIFY(std::none_of(captures.begin(), captures.end(), [&](const auto &capture) { return capture.path == model; }));

    if (!live) {
        nova::EditorTestAccess::openModel(window, "assets/minecraft/models/block/nova_cycle_b.json");
        relatedAction->trigger();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        related = window.findChild<QDialog *>("relatedAssetsDialog");
        QVERIFY(related);
        list = related->findChild<QListWidget *>("relatedAssetsList");
        const auto missing = list->findItems("Missing or unsafe reference", Qt::MatchContains);
        QVERIFY(!missing.isEmpty());
        QVERIFY(missing.first()->text().contains("nova_missing"));
        list->setCurrentItem(missing.first());
        QVERIFY(!related->findChild<QPushButton *>("relatedAssetsOpen")->isEnabled());
        QVERIFY(!list->findItems("cycle", Qt::MatchContains).isEmpty());
        // A texture lists the models that use it, and an animation lists its metadata.
        const auto tabCount = tabs->count();
        nova::EditorTestAccess::relatedAssets(window, texture);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        related = window.findChild<QDialog *>("relatedAssetsDialog");
        QVERIFY(related);
        list = related->findChild<QListWidget *>("relatedAssetsList");
        QVERIFY(!list->findItems("Uses this texture · " + model, Qt::MatchExactly).isEmpty());
        nova::EditorTestAccess::relatedAssets(window, animation);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        related = window.findChild<QDialog *>("relatedAssetsDialog");
        list = related->findChild<QListWidget *>("relatedAssetsList");
        const auto metadata = list->findItems("Animation metadata · " + animation + ".mcmeta", Qt::MatchExactly);
        QVERIFY(!metadata.isEmpty());
        list->setCurrentItem(metadata.first());
        related->findChild<QPushButton *>("relatedAssetsOpen")->click();
        QCOMPARE(window.activeCanvas()->document->path, root + '/' + animation);
        QCOMPARE(tabs->count(), tabCount); // the open texture document was reused
        // Switching projects closes navigation and drops captured comparisons.
        const auto other = dir.filePath("Other Pack");
        write(other + "/pack.mcmeta", R"({"pack":{"description":"Other","min_format":75,"max_format":75}})");
        QTimer::singleShot(0, &window, [] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box && box->button(QMessageBox::Discard))
                box->button(QMessageBox::Discard)->click();
        });
        auto projectPath = window.findChild<QLineEdit *>("projectPath");
        QVERIFY(projectPath);
        projectPath->setText(other);
        QTest::keyClick(projectPath, Qt::Key_Return);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(!window.findChild<QDialog *>("relatedAssetsDialog"));
        QVERIFY(captures.isEmpty());
        QCOMPARE(service->workflowReview().id, QString{});
        return;
    }
    window.findChild<QAction *>("testInMinecraft")->trigger();
    auto minecraft = window.findChild<nova::FabricTestDialog *>();
    QVERIFY(minecraft);
    auto runtime = minecraft->runtime();
    QTRY_VERIFY_WITH_TIMEOUT(runtime->readyForInput() ||
                                 runtime->state() == nova::testing::FabricRuntime::State::Failed ||
                                 runtime->game() == nova::testing::FabricRuntime::Game::LaunchFailed,
                             180000);
    QVERIFY2(runtime->readyForInput(), qPrintable(runtime->error()));
    QVERIFY(runtime->running());
    saveScreenshot(minecraft, "phase4e-minecraft-launch");
    write(evidence + "/minecraft-ready.txt", minecraft->runtime()->layout().game.toUtf8());
    // Leave time to inspect the rendered assets in game; the stop file ends the run.
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(evidence + "/stop-minecraft.txt"), 1800000);
    minecraft->runtime()->requestStop();
    QTRY_VERIFY_WITH_TIMEOUT(!minecraft->runtime()->running(), 30000);
}

void McpTest::creationWorkflow() {
    const auto sdk = qEnvironmentVariable("NOVA_MCP_SDK_PYTHON");
    if (sdk.isEmpty())
        QSKIP("Set NOVA_MCP_SDK_PYTHON to run the production Phase 4-D transport acceptance.");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qApp->setProperty("settingsFile", dir.path() + "/studio.ini");
    const auto root = dir.path() + "/creation-pack";
    const auto folder = root + "/assets/nova/textures/block/";
    write(root + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Creation"}})");
    QImage seed(16, 16, QImage::Format_RGBA8888);
    seed.fill(QColor("#c73342"));
    for (int y = 0; y < 16; ++y)
        for (int x = 8; x < 16; ++x)
            seed.setPixelColor(x, y, QColor("#32ad7d"));
    QVERIFY(QDir().mkpath(root + "/assets/nova/models/block"));
    const auto modelPath = QStringLiteral("assets/nova/models/block/sdk_model.json");
    QVERIFY(QDir().mkpath(folder));
    QVERIFY(seed.save(folder + "seed.png"));
    const auto before = fingerprint(root, true);
    QElapsedTimer clock;
    clock.start();
    auto window = std::make_unique<nova::Window>(root, QString{}, clock);
    window->show();
    QTRY_VERIFY(window->activeCanvas());
    window->findChild<QAction *>("mcpAccess")->trigger();
    auto dialog = window->findChild<QDialog *>("mcpAccessDialog");
    auto service = window->findChild<Service *>();
    QVERIFY(dialog && service);
    grantEditing(*window, dialog, service);
    QProcess client;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    qint64 lastBeat = clock.elapsed(), maxGap = 0;
    connect(&heartbeat, &QTimer::timeout, this, [&] {
        const auto now = clock.elapsed();
        maxGap = qMax(maxGap, now - lastBeat);
        lastBeat = now;
    });
    const auto memoryBefore = usage(GetCurrentProcess()).privateBytes;
    heartbeat.start();
    startSdk(client, service->connectionCode(), "phase4d");
    finishSdk(client, "phase4d");
    heartbeat.stop();
    const auto memoryAfter = usage(GetCurrentProcess()).privateBytes;
    qInfo() << "Phase 4-D max 10 ms heartbeat gap" << maxGap
            << "ms and private-byte delta" << qint64(memoryAfter) - qint64(memoryBefore);
    QTRY_COMPARE(service->status(), "Disabled");
    auto panel = nova::EditorTestAccess::models(*window);
    auto model = panel->openModelDocument(modelPath);
    QVERIFY(model && model->provisional() && model->modified());
    QCOMPARE(panel->modelAsset(), modelPath);
    QCOMPARE(model->authoritative()["elements"].toArray().size(), 1);
    QVERIFY(!QFileInfo::exists(root + '/' + modelPath));
    auto tabs = window->findChild<QTabWidget *>("editorTabs");
    const auto modelPage = tabs->currentWidget();
    QCOMPARE(modelPage->objectName(), "modelDocumentTab");
    QVERIFY(tabs->tabText(tabs->currentIndex()).endsWith(" ●"));
    auto viewport = modelPage->findChild<nova::Cube *>();
    QVERIFY(viewport && viewport->isVisible() && viewport->focusPolicy() == Qt::StrongFocus);
    const auto editedModel = model->authoritative();
    window->findChild<QAction *>("editUndo")->trigger();
    QVERIFY(model->authoritative() != editedModel);
    window->findChild<QAction *>("editRedo")->trigger();
    QCOMPARE(model->authoritative(), editedModel);
    QTimer::singleShot(0, window.get(), [] {
        if (auto prompt = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            if (auto cancel = prompt->button(QMessageBox::Cancel))
                cancel->click();
    });
    emit tabs->tabCloseRequested(tabs->currentIndex());
    QCOMPARE(tabs->currentWidget(), modelPage);
    QCOMPARE(panel->openModelDocument(modelPath), model);
    saveScreenshot(window.get(), "phase4d2-model");
    auto overlay = panel->findChild<QCheckBox *>("uvOverlay");
    QVERIFY(overlay);
    overlay->setChecked(true);
    panel->selectFace(0);
    QCOMPARE(tabs->currentWidget(), modelPage);
    QVERIFY(panel->findChild<QPushButton *>("uvBegin")->isVisible());
    saveScreenshot(window.get(), "phase4d2-uv");
    auto docs = nova::EditorTestAccess::documents(*window);
    QCOMPARE(docs.size(), size_t(3));
    const auto texture = docs[1], animation = docs[2];
    QVERIFY(texture->modified() && animation->modified());
    QCOMPARE(texture->image.pixelColor(0, 0), QColor("#654321"));
    QCOMPARE(animation->animationState().entries[1].ticks, 9);
    QCOMPARE(fingerprint(root, true), before);

    grantEditing(*window, dialog, service);
    QProcess raw;
    start(raw, service->connectionCode());
    initialize(raw);
    auto rpc = [&](const QString &tool, const QJsonObject &args = {}) {
        raw.write(call(3, tool, args));
        return payload(receive(raw));
    };
    const auto info = rpc("nova.project.info");
    const QJsonObject project{{"generation", info["generation"]}, {"projectId", info["projectId"]}};
    const auto listed = rpc("nova.assets.list", {{"generation", info["generation"]}})["assets"].toArray();
    QJsonArray stages;
    for (const auto &doc : {texture, animation}) {
        QJsonValue id;
        for (const auto &row : listed)
            if (root + '/' + row.toObject()["path"].toString() == doc->path)
                id = row.toObject()["assetId"];
        QVERIFY(id.isString());
        const auto state = rpc("nova.edit.capabilities", {{"generation", info["generation"]}, {"assetId", id}})
                               ["document"].toObject();
        const QJsonObject rename{{"command", "layer_rename"},
            {"layer", state["activeLayer"]}, {"name", "Workflow stage"}};
        stages.append(QJsonObject{{"assetId", id}, {"expectedRevision", state["revision"]},
                                  {"operations", QJsonArray{rename}}});
    }
    auto workflow = project;
    workflow["stages"] = stages;
    const auto cursor = texture->historyCursor();
    const auto otherCursor = animation->historyCursor();
    bool interrupted = false;
    const auto interrupt = connect(service, &Service::changed, this, [&] {
        if (!interrupted && service->editActivity().contains("1/2 stages")) {
            interrupted = true;
            animation->draw({0, 0}, {0, 0}, qRgb(255, 0, 0));
            animation->finish();
        }
    });
    const auto existing = rpc("nova.asset.create_preview", {
        {"generation", info["generation"]}, {"projectId", info["projectId"]},
        {"indexRevision", info["indexRevision"]}, {"namespace", "nova"}, {"path", "block/seed"},
        {"conflictPolicy", "reject"}, {"parameters", QJsonObject{
            {"command", "texture"}, {"width", 16}, {"height", 16}, {"layerName", "Base"},
            {"fill", "transparent"}, {"pixelFormat", "rgba8888"}}}});
    QCOMPARE(existing["error"].toObject()["code"], "asset_conflict");
    const auto stopped = rpc("nova.workflow.execute", workflow);
    disconnect(interrupt);
    QCOMPARE(stopped["status"], "stopped");
    QCOMPARE(stopped["stages"].toArray().size(), 1);
    QCOMPARE(stopped["failure"].toObject()["code"], "revision_conflict");
    QCOMPARE(texture->historyCursor(), cursor + 1);
    QCOMPARE(animation->historyCursor(), otherCursor + 1);
    QVERIFY(animation->layers().front().name != "Workflow stage");

    auto freshStages = stages;
    for (int i = 0; i < freshStages.size(); ++i) {
        auto stage = freshStages[i].toObject();
        const auto state = rpc("nova.edit.capabilities", {{"generation", info["generation"]},
            {"assetId", stage["assetId"]}})["document"].toObject();
        stage["expectedRevision"] = state["revision"];
        stage["operations"] = QJsonArray{QJsonObject{{"command", "layer_rename"},
            {"layer", state["activeLayer"]}, {"name", "Cancellation boundary"}}};
        freshStages[i] = stage;
    }
    workflow["stages"] = freshStages;
    auto cancel = [&] {
        raw.write(request(-1, "notifications/cancelled", {{"requestId", 3}}));
        QTRY_VERIFY_WITH_TIMEOUT(raw.canReadLine(), 5000);
    };
    const auto beforePublish = connect(service, &Service::editPrepared, this, cancel);
    const auto canceledBefore = rpc("nova.workflow.execute", workflow);
    disconnect(beforePublish);
    QCOMPARE(canceledBefore["failure"].toObject()["code"], "operation_canceled");
    QVERIFY(canceledBefore["stages"].toArray().isEmpty());
    QCOMPARE(texture->historyCursor(), cursor + 1);
    QCOMPARE(animation->historyCursor(), otherCursor + 1);

    bool canceledBetween = false;
    const auto between = connect(service, &Service::changed, this, [&] {
        if (!canceledBetween && service->editActivity().contains("1/2 stages")) {
            canceledBetween = true;
            cancel();
        }
    });
    const auto canceledAfterOne = rpc("nova.workflow.execute", workflow);
    disconnect(between);
    QVERIFY(canceledBetween);
    QCOMPARE(canceledAfterOne["failure"].toObject()["code"], "operation_canceled");
    QCOMPARE(canceledAfterOne["stages"].toArray().size(), 1);
    QCOMPARE(canceledAfterOne["stageOutcomes"].toArray()[1].toObject()["status"], "canceled");
    QCOMPARE(texture->historyCursor(), cursor + 2);
    QCOMPARE(animation->historyCursor(), otherCursor + 1);
    saveScreenshot(dialog, "phase4d-workflow");
    saveScreenshot(window.get(), "phase4d-workspace");

    nova::RecoveryStore store(dir.path() + "/recovery");
    QVERIFY(nova::Revision::read(store.pathFor(texture->id())).readable);
    QVERIFY(nova::Revision::read(store.pathFor(animation->id())).readable);
    QTRY_VERIFY(!nova::EditorTestAccess::checkpointBusy(*window));
    nova::EditorTestAccess::checkpoint(*window);
    QTRY_VERIFY(!nova::EditorTestAccess::checkpointBusy(*window));
    const auto checkpoint = store.load(animation->id());
    QVERIFY2(checkpoint.error.isEmpty(), qPrintable(checkpoint.error));
    QCOMPARE(checkpoint.image.pixelColor(0, 0), QColor(Qt::red));
    service->revoke();
    QTRY_COMPARE(raw.state(), QProcess::NotRunning);
    window.reset();

    {
        nova::Window deferred(root, {}, clock);
        int deferredCount = 0;
        QTimer later;
        connect(&later, &QTimer::timeout, this, [&] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || (box->objectName() != "recoveryDialog" && box->objectName() != "modelRecoveryDialog"))
                return;
            for (auto button : box->buttons())
                if (button->text() == "Later") {
                    ++deferredCount;
                    button->click();
                    return;
                }
        });
        later.start(20);
        deferred.show();
        QTRY_COMPARE_WITH_TIMEOUT(deferredCount, 3, 10000);
        later.stop();
        QVERIFY(nova::Revision::read(store.pathFor(texture->id())).readable);
        QVERIFY(nova::Revision::read(store.pathFor(animation->id())).readable);
    }

    nova::Window restarted(root, {}, clock);
    QSet<QString> restored;
    QTimer drive;
    connect(&drive, &QTimer::timeout, this, [&] {
        auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box || (box->objectName() != "recoveryDialog" && box->objectName() != "modelRecoveryDialog") || restored.contains(box->text()))
            return;
        for (auto button : box->buttons())
            if (button->text() == "Restore") {
                restored.insert(box->text());
                button->click();
                return;
            }
    });
    drive.start(20);
    restarted.show();
    QTRY_COMPARE_WITH_TIMEOUT(restored.size(), 3, 10000);
    QTRY_COMPARE(nova::EditorTestAccess::documents(restarted).size(), size_t(3));
    drive.stop();
    QCOMPARE(fingerprint(root, true), before);
    for (const auto &doc : nova::EditorTestAccess::documents(restarted))
        if (doc->id() == animation->id()) {
            QVERIFY(doc->modified() && doc->animated());
            QCOMPARE(doc->animationState().entries[1].ticks, 9);
            QCOMPARE(doc->image.pixelColor(0, 0), QColor(Qt::red));
        }
    auto recoveredPanel = nova::EditorTestAccess::models(restarted);
    auto recoveredModel = recoveredPanel->openModelDocument(modelPath);
    QVERIFY(recoveredModel && recoveredModel->provisional() && recoveredModel->modified());
    QCOMPARE(recoveredModel->authoritative(), model->authoritative());
    recoveredPanel->openAsset(modelPath);
    recoveredPanel->authorModel();
    auto save = restarted.findChild<QPushButton *>("authorSave");
    QVERIFY(save);
    const auto destination = root + '/' + modelPath;
    write(destination, R"({"credit":"External model"})");
    const auto external = nova::Revision::read(destination);
    save->click();
    QVERIFY(recoveredModel->modified() && recoveredModel->provisional());
    QVERIFY(external.sameContent(nova::Revision::read(destination)));
    QVERIFY(QFile::remove(destination)); // Only the disposable collision fixture.
    save->click();
    QVERIFY(!recoveredModel->modified() && !recoveredModel->provisional());
    nova::mc::ModelEdit savedModel;
    QString modelError;
    QVERIFY2(savedModel.open(root, modelPath, &modelError), qPrintable(modelError));
    QCOMPARE(savedModel.authoritative(), model->authoritative());
    QCOMPARE(QDir(root + "/.nova/model-recovery").entryList({"*.json"}, QDir::Files).size(), 0);
    nova::EditorTestAccess::open(restarted, texture->path);
    auto restoredTexture = restarted.activeCanvas()->document;
    QCOMPARE(restoredTexture->id(), texture->id());
    restarted.findChild<QAction *>("saveCommand")->trigger();
    QVERIFY(!restoredTexture->modified());
    QVERIFY(!restoredTexture->minecraftOutputPending());
    QVERIFY(QFileInfo::exists(restoredTexture->editablePath()));
    QVERIFY(!QFileInfo::exists(store.pathFor(texture->id())));
    // MCP Phase 1: Save writes the recovered texture into the pack too (before, it saved only the
    // editable source and left the resource path empty). An external change is still never overwritten.
    QVERIFY(restoredTexture->revision().sameContent(nova::Revision::read(texture->path)));
    write(texture->path, "external collision");
    const auto conflict = nova::Revision::read(texture->path);
    QString error;
    QVERIFY(!restoredTexture->saveAsRevision(texture->path, restoredTexture->revision(), &error));
    QVERIFY(conflict.sameContent(nova::Revision::read(texture->path)));
}

namespace {
const auto recoveryModel = QStringLiteral("assets/nova/models/block/sdk_model.json");

void reportChild(const QJsonObject &message) {
    const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    std::fprintf(stdout, "%s\n", bytes.constData());
    std::fflush(stdout);
}

int recoveryChild(const QString &home, const QString &mode) {
    qApp->setProperty("settingsFile", home + "/studio.ini");
    const auto root = home + "/pack";
    QElapsedTimer clock;
    clock.start();
    nova::Window window(root, {}, clock);
    auto panel = nova::EditorTestAccess::models(window);
    window.findChild<QAction *>("mcpAccess")->trigger();
    window.findChild<QDialog *>("mcpAccessDialog")->hide();
    int decisions = 0;
    bool announced = false, closing = false, paused = false, saved = false;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            if (box->objectName() == "modelRecoveryDialog" || box->objectName() == "recoveryDialog") {
                const auto label = mode == "restore" ? "Restore" : "Later";
                for (auto button : box->buttons())
                    if (button->text() == label) {
                        ++decisions;
                        button->click();
                        return;
                    }
            }
            if (closing && box->windowTitle().startsWith("Unsaved")) {
                if (mode == "boundary") {
                    if (!std::exchange(paused, true))
                        reportChild({{"state", "shutdown-prompt"}});
                } else if (auto discard = box->button(QMessageBox::Discard))
                    discard->click();
            }
            return;
        }
        if (!announced && panel->catalog()->editable("assets/nova/models/block/seed.json")) {
            const auto model = panel->openModelDocument(recoveryModel);
            const auto docs = nova::EditorTestAccess::documents(window);
            if ((mode == "later" || mode == "restore") &&
                (decisions != 3 || (mode == "restore" && (!model || docs.size() != 3))))
                return;
            auto service = window.findChild<Service *>();
            if (mode != "later" && !service->authorize(root, mode != "restore"))
                return;
            announced = true;
            QJsonArray raster;
            for (const auto &doc : docs)
                if (doc->modified())
                    raster.append(QJsonObject{{"name", QFileInfo(doc->path).fileName()},
                        {"pixel", doc->image.pixelColor(0, 0).name()},
                        {"ticks", doc->animated() ? doc->animationState().entries.at(1).ticks : 0}});
            reportChild({{"state", "ready"}, {"pid", qApp->applicationPid()}, {"code", service->connectionCode()},
                         {"model", model ? model->authoritative() : QJsonObject{}},
                         {"dirty", model && model->modified()}, {"raster", raster}, {"decisions", decisions}});
        }
        if (announced && !saved && QFileInfo::exists(home + "/save")) {
            saved = true;
            auto tabs = window.findChild<QTabWidget *>("editorTabs");
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->widget(i)->objectName() == "modelDocumentTab")
                    tabs->setCurrentIndex(i);
            window.findChild<QAction *>("saveCommand")->trigger();
            auto model = panel->openModelDocument(recoveryModel);
            reportChild({{"state", "saved"}, {"dirty", !model || model->modified()}});
        }
        if (announced && !closing && QFileInfo::exists(home + "/close")) {
            closing = true;
            QTimer::singleShot(0, &window, &QWidget::close);
        }
    });
    driver.start(15);
    window.show();
    return qApp->exec();
}
}

void McpTest::processRecovery() {
    QVERIFY2(!qEnvironmentVariableIsEmpty("NOVA_MCP_SDK_PYTHON"), "The official SDK is required.");
    for (const auto mode : {"discard", "crash", "boundary"}) {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const auto root = home.path() + "/pack";
        write(root + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Recovery"}})");
        write(root + "/assets/nova/models/block/seed.json", "{}");
        const auto textures = root + "/assets/nova/textures/block";
        QVERIFY(QDir().mkpath(textures));
        QImage material(16, 16, QImage::Format_RGBA8888);
        material.fill(QColor("#c73342"));
        for (int y = 0; y < 16; ++y)
            for (int x = 8; x < 16; ++x)
                material.setPixelColor(x, y, QColor("#32ad7d"));
        QVERIFY(material.save(textures + "/seed.png"));
        const auto original = fingerprint(root, true);
        auto launch = [&](QProcess &child, const QString &choice) {
            child.start(qApp->applicationFilePath(), {"--recovery-child", home.path(), choice});
            QVERIFY(child.waitForStarted());
        };
        QProcess child;
        launch(child, mode);
        const auto ready = receive(child, 20000);
        QVERIFY2(ready["state"] == "ready", child.readAllStandardError().constData());
        QCOMPARE(ready["pid"].toInteger(), child.processId());
        QProcess sdk;
        startSdk(sdk, ready["code"].toString(), "phase4d");
        finishSdk(sdk, "phase4d-process");
        const auto snapshots = root + "/.nova/model-recovery";
        const auto snapshotNames = QDir(snapshots).entryList({"*.json"}, QDir::Files);
        QCOMPARE(snapshotNames.size(), 1);
        QFile snapshot(snapshots + '/' + snapshotNames.first());
        QVERIFY(snapshot.open(QIODevice::ReadOnly));
        const auto checkpoint = snapshot.readAll();
        snapshot.close();
        // The SDK exercises undo/redo after the synchronous workflow checkpoint.
        // Wait for the periodic writer to publish the final raster state before
        // killing the child. The presence of two files alone can mean older revisions.
        nova::RecoveryStore rasterStore(home.path() + "/recovery");
        const auto rasterCheckpointsCurrent = [&] {
            bool textureCurrent = false, animationCurrent = false;
            for (const auto &entry : rasterStore.entries()) {
                if (!entry.error.isEmpty())
                    continue;
                const auto data = rasterStore.load(entry.id);
                if (!data.error.isEmpty())
                    continue;
                if (data.path.endsWith("/sdk_texture.png"))
                    textureCurrent = data.image.pixelColor(0, 0) == QColor("#654321");
                else if (data.path.endsWith("/sdk_animation.png")) {
                    nova::Document animation(data.path, QImage{});
                    if (animation.decodeNative(data.nativeData, "recovery.png") &&
                        animation.animationState().entries.size() > 1)
                        animationCurrent = animation.animationState().entries[1].ticks == 9;
                }
            }
            return textureCurrent && animationCurrent;
        };
        QTRY_VERIFY_WITH_TIMEOUT(rasterCheckpointsCurrent(), 15000);
        QCOMPARE(fingerprint(root, true), original);
        if (QString(mode) != "crash")
            write(home.path() + "/close", {});
        if (QString(mode) == "discard") {
            QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 10000);
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QCOMPARE(child.exitCode(), 0);
            QVERIFY(QDir(snapshots).entryList({"*.json"}, QDir::Files).isEmpty());
            QVERIFY(QFile::remove(home.path() + "/close"));
            QProcess restarted;
            launch(restarted, "empty");
            const auto state = receive(restarted, 20000);
            QCOMPARE(state["state"], "ready");
            QVERIFY(state["model"].toObject().isEmpty() && state["raster"].toArray().isEmpty());
            write(home.path() + "/close", {});
            QTRY_COMPARE_WITH_TIMEOUT(restarted.state(), QProcess::NotRunning, 10000);
            QCOMPARE(fingerprint(root, true), original);
            continue;
        }
        if (QString(mode) == "boundary")
            QCOMPARE(receive(child, 10000)["state"], "shutdown-prompt");
        QCOMPARE(child.state(), QProcess::Running);
        QCOMPARE(child.processId(), ready["pid"].toInteger());
        QCOMPARE(child.program(), qApp->applicationFilePath());
        child.kill();
        QVERIFY(child.waitForFinished());
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);
        if (QString(mode) == "boundary")
            QVERIFY(QFile::remove(home.path() + "/close"));

        QProcess later;
        launch(later, "later");
        const auto deferred = receive(later, 20000);
        QCOMPARE(deferred["decisions"], 3);
        QVERIFY(deferred["model"].toObject().isEmpty() && deferred["raster"].toArray().isEmpty());
        QVERIFY(snapshot.open(QIODevice::ReadOnly));
        QCOMPARE(snapshot.readAll(), checkpoint);
        snapshot.close();
        write(home.path() + "/close", {});
        QTRY_COMPARE_WITH_TIMEOUT(later.state(), QProcess::NotRunning, 10000);
        QVERIFY(QFile::remove(home.path() + "/close"));

        QProcess restarted;
        launch(restarted, "restore");
        const auto restored = receive(restarted, 20000);
        QCOMPARE(restored["decisions"], 3);
        QVERIFY(restored["dirty"].toBool());
        const auto elements = restored["model"].toObject()["elements"].toArray();
        QCOMPARE(elements.size(), 1);
        const auto element = elements.first().toObject();
        QCOMPARE(element["to"].toArray(), (QJsonArray{12, 16, 16}));
        const auto face = element["faces"].toObject()["down"].toObject();
        QCOMPARE(face["uv"].toArray(), (QJsonArray{2, 3, 9, 11}));
        QCOMPARE(face["rotation"], 90);
        const auto raster = restored["raster"].toArray();
        QVERIFY2(raster.size() == 2, QJsonDocument(raster).toJson(QJsonDocument::Compact).constData());
        for (const auto &value : raster) {
            const auto row = value.toObject();
            if (row["name"] == "sdk_texture.png")
                QCOMPARE(row["pixel"], "#654321");
            else {
                QCOMPARE(row["name"], "sdk_animation.png");
                QCOMPARE(row["ticks"], 9);
            }
        }
        QCOMPARE(fingerprint(root, true), original);
        QProcess preview;
        startSdk(preview, restored["code"].toString(), "restored-preview");
        finishSdk(preview, "restored-preview");
        write(home.path() + "/save", {});
        const auto saved = receive(restarted, 10000);
        QCOMPARE(saved["state"], "saved");
        QCOMPARE(saved["dirty"], false);
        QVERIFY(!QFileInfo::exists(snapshot.fileName()));
        QFile source(root + '/' + recoveryModel);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(source.readAll()).object(), restored["model"].toObject());
        source.close();
        write(home.path() + "/close", {});
        QTRY_COMPARE_WITH_TIMEOUT(restarted.state(), QProcess::NotRunning, 10000);
        QCOMPARE(restarted.exitCode(), 0);
    }
}

void McpTest::previewLifecycle() {
    QVERIFY2(!qEnvironmentVariableIsEmpty("NOVA_MCP_SDK_PYTHON"), "The official SDK is required.");
    QTemporaryDir home;
    QVERIFY(home.isValid());
    qApp->setProperty("settingsFile", home.path() + "/studio.ini");
    const auto root = home.path() + "/preview-pack";
    const auto other = home.path() + "/other-pack";
    const auto modelFolder = QStringLiteral("assets/nova/models/block/");
    const auto target = modelFolder + "lifecycle.json";
    write(root + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Preview"}})");
    write(other + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Other"}})");
    write(root + '/' + modelFolder + "empty.json", "{}");
    const QJsonObject element = QJsonDocument::fromJson(
        R"({"from":[0,0,0],"to":[16,16,16],"faces":{"down":{"texture":"#t"},"up":{"texture":"#t"},"north":{"texture":"#t"},"south":{"texture":"#t"},"west":{"texture":"#t"},"east":{"texture":"#t"}}})").object();
    write(root + '/' + modelFolder + "invalid.json",
          QJsonDocument(QJsonObject{{"textures", QJsonObject{{"t", "nova:block/missing"}}},
                                   {"elements", QJsonArray{element}}}).toJson());
    QJsonArray many;
    for (int i = 0; i < 257; ++i)
        many.append(element);
    write(root + '/' + modelFolder + "oversized.json",
          QJsonDocument(QJsonObject{{"textures", QJsonObject{{"t", "nova:block/seed"}}},
                                   {"elements", many}}).toJson());
    const auto textures = root + "/assets/nova/textures/block";
    QVERIFY(QDir().mkpath(textures));
    QImage material(16, 16, QImage::Format_RGBA8888);
    material.fill(QColor("#c73342"));
    for (int y = 0; y < 16; ++y)
        for (int x = 8; x < 16; ++x)
            material.setPixelColor(x, y, QColor("#32ad7d"));
    QVERIFY(material.save(textures + "/seed.png"));
    QElapsedTimer clock;
    clock.start();
    nova::Window window(root, {}, clock);
    window.show();
    auto panel = nova::EditorTestAccess::models(window);
    QTRY_VERIFY(panel->catalog()->editable(modelFolder + "empty.json"));
    QTRY_VERIFY_WITH_TIMEOUT(nova::EditorTestAccess::indexReady(window), 10000);
    window.findChild<QAction *>("mcpAccess")->trigger();
    auto service = window.findChild<Service *>();
    QVERIFY(service->authorize(root, true));
    const auto oldCode = service->connectionCode();
    auto tabs = window.findChild<QTabWidget *>("editorTabs");
    QProcess sdk;
    startSdk(sdk, oldCode, "preview-lifecycle");
    bool selectOnNextRequest = false;
    connect(service, &Service::requestParsed, &window, [&](const QElapsedTimer &) {
        if (std::exchange(selectOnNextRequest, false))
            nova::EditorTestAccess::openModel(window, modelFolder + "empty.json");
    });
    for (const auto action : {"save", "close", "reopen", "select-race", "empty", "invalid", "oversized",
                              "switch", "revoke"}) {
        QTRY_VERIFY_WITH_TIMEOUT(sdk.canReadLine() || sdk.state() == QProcess::NotRunning, 15000);
        const auto actual = sdk.readLine().trimmed();
        QVERIFY2(actual == action, sdk.readAllStandardError().constData());
        if (actual == "save") {
            QCOMPARE(tabs->currentWidget()->objectName(), "modelDocumentTab");
            QVERIFY(tabs->tabText(tabs->currentIndex()).endsWith(" ●"));
            QCOMPARE(panel->modelAsset(), target);
            QVERIFY(!QFileInfo::exists(root + '/' + target));
            window.findChild<QAction *>("saveCommand")->trigger();
            QVERIFY(QFileInfo::exists(root + '/' + target));
            QVERIFY(!tabs->tabText(tabs->currentIndex()).endsWith(" ●"));
            QTRY_VERIFY_WITH_TIMEOUT(nova::EditorTestAccess::indexReady(window), 10000);
        } else if (actual == "close") {
            emit tabs->tabCloseRequested(tabs->currentIndex());
            QVERIFY(!panel->openModelDocument(target));
        } else if (actual == "reopen") {
            nova::EditorTestAccess::openModel(window, target);
            QTRY_VERIFY(panel->openModelDocument(target));
            QCOMPARE(tabs->currentWidget()->objectName(), "modelDocumentTab");
        } else if (actual == "select-race") {
            selectOnNextRequest = true;
        } else if (actual == "switch") {
            auto path = window.findChild<QLineEdit *>("projectPath");
            path->setText(other);
            QTest::keyClick(path, Qt::Key_Return);
            QCOMPARE(service->status(), "Project changed");
        } else if (actual == "revoke") {
            service->revoke();
        } else {
            nova::EditorTestAccess::openModel(window, modelFolder + QString::fromLatin1(action) + ".json");
            QCOMPARE(panel->modelAsset(), modelFolder + QString::fromLatin1(action) + ".json");
        }
        // Let the native viewport deliver a frame after tab selection.
        QCoreApplication::processEvents();
        sdk.write("continue\n");
    }
    finishSdk(sdk, "preview-lifecycle");
    QProcess stale;
    startSdk(stale, oldCode, "expired");
    finishSdk(stale, "expired-preview-grant");
    auto path = window.findChild<QLineEdit *>("projectPath");
    path->setText(root);
    QTest::keyClick(path, Qt::Key_Return);
    QTRY_VERIFY(panel->catalog()->editable(target));
    nova::EditorTestAccess::openModel(window, target);
    QVERIFY(service->authorize(root));
    QProcess reconnect;
    startSdk(reconnect, service->connectionCode(), "preview-reconnect");
    finishSdk(reconnect, "preview-reconnect");
}

void McpTest::workflowLifecycle_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("cancel-before-preparation") << QString("workflow-cancel-preflight");
    QTest::newRow("cancel-in-preparation") << QString("workflow-cancel-preparation");
    QTest::newRow("disconnect-in-preparation") << QString("workflow-disconnect-preparation");
    QTest::newRow("revoke-in-preparation") << QString("workflow-revoke-preparation");
    QTest::newRow("switch-in-preparation") << QString("workflow-switch-preparation");
    QTest::newRow("disconnect-before") << QString("workflow-disconnect-before");
    QTest::newRow("disconnect-between") << QString("workflow-disconnect-between");
    QTest::newRow("disconnect-after") << QString("workflow-disconnect-after");
    QTest::newRow("cancel-after") << QString("workflow-cancel-after");
    QTest::newRow("revoke-between") << QString("workflow-revoke");
    QTest::newRow("close-between") << QString("workflow-close");
    QTest::newRow("switch-between") << QString("workflow-switch");
}

void McpTest::workflowLifecycle() {
    if (qEnvironmentVariableIsEmpty("NOVA_MCP_SDK_PYTHON"))
        QSKIP("Set NOVA_MCP_SDK_PYTHON for official SDK workflow interruptions.");
    QFETCH(QString, mode);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qApp->setProperty("settingsFile", dir.path() + "/studio.ini");
    const auto root = dir.path() + "/workflow-pack";
    const auto folder = root + "/assets/nova/textures/block/";
    write(root + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Workflow"}})");
    QVERIFY(QDir().mkpath(folder));
    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QVERIFY(image.save(folder + "a.png") && image.save(folder + "b.png"));
    const auto before = fingerprint(root);
    QElapsedTimer clock;
    clock.start();
    nova::Window window(root, {}, clock);
    window.show();
    QTRY_VERIFY(window.activeCanvas());
    nova::EditorTestAccess::open(window, folder + "a.png");
    const auto a = window.activeCanvas()->document;
    nova::EditorTestAccess::open(window, folder + "b.png");
    const auto b = window.activeCanvas()->document;
    QVERIFY(a != b);
    window.findChild<QAction *>("mcpAccess")->trigger();
    auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
    auto service = window.findChild<Service *>();
    QVERIFY(dialog && service);
    grantEditing(window, dialog, service);
    const auto oldCode = service->connectionCode();
    const auto other = dir.path() + "/other-pack";
    write(other + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Other"}})");
    const auto otherBefore = fingerprint(other);
    nova::RecoveryStore recovery(dir.path() + "/recovery");
    bool interrupted = false;
    QProcess client;
    auto disconnectClient = [&] {
        interrupted = true;
        client.write("continue\n");
        QTRY_COMPARE_WITH_TIMEOUT(service->status(), "Disabled", 8000);
    };
    const bool duringPreparation = mode.endsWith("-preparation") || mode.endsWith("-preflight");
    int checkpoints = 0;
    const auto preparing = connect(service, &Service::preparationCheckpoint, this, [&] {
        const int stopAt = mode.endsWith("-preflight") ? 1 : 2;
        if (!duringPreparation || ++checkpoints != stopAt)
            return;
        interrupted = true;
        if (mode.startsWith("workflow-cancel-")) {
            client.write("continue\n");
            QTRY_VERIFY_WITH_TIMEOUT(client.canReadLine(), 8000);
            QCOMPARE(client.readLine().trimmed(), QByteArray("cancel delivered"));
        } else if (mode == "workflow-disconnect-preparation") {
            disconnectClient();
        } else if (mode == "workflow-revoke-preparation") {
            service->revoke();
        } else {
            auto path = window.findChild<QLineEdit *>("projectPath");
            path->setText(other);
            QTest::keyClick(path, Qt::Key_Return);
            QCOMPARE(service->status(), "Project changed");
        }
    });
    const auto prepared = connect(service, &Service::editPrepared, this, [&] {
        if (mode == "workflow-disconnect-before")
            disconnectClient();
    });
    const auto afterPublication = connect(&window.documents(), &nova::DocumentSession::changed, this, [&](QUuid id) {
        if (id != b->id() || interrupted)
            return;
        if (mode == "workflow-disconnect-after") {
            disconnectClient();
        } else if (mode == "workflow-cancel-after") {
            interrupted = true;
            client.write("continue\n");
            QTRY_VERIFY_WITH_TIMEOUT(client.canReadLine(), 8000);
            QCOMPARE(client.readLine().trimmed(), QByteArray("cancel delivered"));
        }
    });
    const auto boundary = connect(service, &Service::changed, this, [&] {
        if (interrupted || !service->editActivity().contains("1/2 stages"))
            return;
        if (mode == "workflow-disconnect-after" || mode == "workflow-cancel-after")
            return;
        interrupted = true;
        if (mode == "workflow-disconnect-between") {
            disconnectClient();
        } else if (mode == "workflow-revoke") {
            service->revoke();
        } else if (mode == "workflow-close") {
            auto tabs = window.findChild<QTabWidget *>("editorTabs");
            emit tabs->tabCloseRequested(tabs->indexOf(window.activeCanvas()));
        } else {
            const auto snapshot = recovery.load(a->id());
            QVERIFY2(snapshot.error.isEmpty(), qPrintable(snapshot.error));
            QCOMPARE(snapshot.image.pixelColor(0, 0), QColor("#ba34ef"));
            QTimer::singleShot(0, &window, [] {
                auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (box && box->button(QMessageBox::Discard))
                    box->button(QMessageBox::Discard)->click();
            });
            auto path = window.findChild<QLineEdit *>("projectPath");
            path->setText(other);
            QTest::keyClick(path, Qt::Key_Return);
            QCOMPARE(service->status(), "Project changed");
        }
    });
    startSdk(client, oldCode, mode);
    finishSdk(client, mode);
    disconnect(boundary);
    disconnect(preparing);
    disconnect(prepared);
    disconnect(afterPublication);
    QVERIFY(interrupted);
    const int commits = duringPreparation || mode == "workflow-disconnect-before" ? 0 : 1;
    const int secondCommit = mode.endsWith("-after") ? 1 : 0;
    const auto &review = service->workflowReview();
    if (mode.contains("switch"))
        QVERIFY(review.id.isEmpty());
    else if (mode == "workflow-close" || mode.endsWith("-between") || mode == "workflow-revoke")
        QCOMPARE(review.state, "Partially committed");
    else if (secondCommit)
        QCOMPARE(review.state, "Completed");
    else if (mode == "workflow-cancel-preflight" || mode == "workflow-cancel-preparation")
        QCOMPARE(review.state, "Canceled");
    else
        QCOMPARE(review.state, "Interrupted");
    if (!review.id.isEmpty() && !commits)
        for (const auto &stage : review.stages)
            QVERIFY2(stage.state == "Canceled" || stage.state == "Interrupted", qPrintable(stage.state));
    if (!review.id.isEmpty() && commits)
        QCOMPARE(review.stages.first().state, "Committed");
    QCOMPARE(a->historyCursor(), commits);
    QCOMPARE(b->historyCursor(), secondCommit);
    QCOMPARE(a->image.pixelColor(0, 0), commits ? QColor("#ba34ef") : QColor(Qt::transparent));
    QCOMPARE(b->image.pixelColor(0, 0), secondCommit ? QColor("#ba34ef") : QColor(Qt::transparent));
    QCOMPARE(fingerprint(root), before);
    QCOMPARE(fingerprint(other), otherBefore);
    if (commits && mode != "workflow-switch") {
        const auto snapshot = recovery.load(a->id());
        QVERIFY2(snapshot.error.isEmpty(), qPrintable(snapshot.error));
        QCOMPARE(snapshot.image.pixelColor(0, 0), QColor("#ba34ef"));
    } else {
        QVERIFY(!QFileInfo::exists(recovery.pathFor(a->id())));
    }
    if (secondCommit) {
        const auto snapshot = recovery.load(b->id());
        QVERIFY2(snapshot.error.isEmpty(), qPrintable(snapshot.error));
        QCOMPARE(snapshot.image.pixelColor(0, 0), QColor("#ba34ef"));
    }
    QTRY_COMPARE(service->status(), "Disabled");
    QProcess stale;
    startSdk(stale, oldCode, "expired");
    finishSdk(stale, "expired");
    if (mode != "workflow-close" && !mode.startsWith("workflow-switch")) {
        QVERIFY(service->authorize(root));
        QProcess fresh;
        startSdk(fresh, service->connectionCode(), "workflow-reconnect");
        finishSdk(fresh, "workflow-reconnect");
        QCOMPARE(a->historyCursor(), commits);
        QCOMPARE(b->historyCursor(), secondCommit);
    }
}

void McpTest::animationProfile_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("animation") << QString("animation_profile");
    QTest::newRow("lifecycle") << QString("lifecycle-profile");
}
void McpTest::animationProfile() {
    QFETCH(QString, mode);
    if (qEnvironmentVariableIsEmpty("NOVA_MCP_SDK_PYTHON"))
        QSKIP("Set NOVA_MCP_SDK_PYTHON to run the official SDK animation profile.");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qApp->setProperty("settingsFile", dir.path() + "/studio.ini");
    const auto root = dir.path() + "/profile-pack";
    const auto texture = root + "/assets/nova/textures/block/seed.png";
    write(root + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Profile"}})");
    QVERIFY(QDir().mkpath(QFileInfo(texture).path()));
    QImage seed(16, 16, QImage::Format_RGBA8888);
    seed.fill(QColor("#c73342"));
    QVERIFY(QDir().mkpath(root + "/assets/nova/models/block"));
    QVERIFY(seed.save(texture));
    QElapsedTimer clock;
    clock.start();
    nova::Window window(root, {}, clock);
    window.show();
    QTRY_VERIFY(window.activeCanvas());
    window.findChild<QAction *>("mcpAccess")->trigger();
    auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
    auto service = window.findChild<Service *>();
    QVERIFY(dialog && service);
    grantEditing(window, dialog, service);
    QProcess client;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    qint64 last = clock.elapsed();
    QList<qint64> gaps;
    connect(&heartbeat, &QTimer::timeout, this, [&] {
        const auto now = clock.elapsed();
        gaps.append(now - last);
        last = now;
    });
    // Optional diagnostic: name the event delivered before each long main-thread gap.
    struct EventGaps : QObject {
        QElapsedTimer clock;
        qint64 last = 0;
        QString previous;
        QMap<QString, QList<double>> gaps;
        bool eventFilter(QObject *object, QEvent *event) override {
            const auto now = clock.nsecsElapsed();
            const double gap = (now - last) / 1e6;
            if (gap >= 50 && !previous.isEmpty())
                gaps[previous].append(gap);
            last = now;
            const auto parent = object->parent();
            previous = QString("%1 '%2' in %3 '%4' event %5")
                           .arg(object->metaObject()->className(), object->objectName(),
                                parent ? parent->metaObject()->className() : "-",
                                parent ? parent->objectName() : QString{})
                           .arg(int(event->type()));
            return false;
        }
    } eventGaps;
    const bool probeGaps = qEnvironmentVariableIntValue("NOVA_PROFILE_EVENT_GAPS") == 1;
    if (probeGaps) {
        eventGaps.clock.start();
        qApp->installEventFilter(&eventGaps);
    }
    const auto before = usage(GetCurrentProcess()).privateBytes;
    heartbeat.start();
    startSdk(client, service->connectionCode(), mode);
    finishSdk(client, mode);
    heartbeat.stop();
    if (probeGaps) {
        qApp->removeEventFilter(&eventGaps);
        for (auto it = eventGaps.gaps.cbegin(); it != eventGaps.gaps.cend(); ++it) {
            auto values = it.value();
            std::sort(values.begin(), values.end());
            qInfo().noquote() << "Event gap" << values.size() << "x median" << values[values.size() / 2]
                              << "ms max" << values.last() << "after" << it.key();
        }
    }
    const auto after = usage(GetCurrentProcess()).privateBytes;
    QVERIFY(!gaps.isEmpty());
    std::sort(gaps.begin(), gaps.end());
    qInfo() << "Profile 10 ms heartbeat n/median/p95/max" << gaps.size()
            << gaps[gaps.size() / 2] << gaps[(95 * gaps.size() - 1) / 100] << gaps.last()
            << "private byte delta" << qint64(after) - qint64(before);
    const auto evidence = qEnvironmentVariable("NOVA_MCP_EVIDENCE");
    if (!evidence.isEmpty())
        QVERIFY(nova::EditorTestAccess::writeMetrics(window, evidence + "/" + mode + "-metrics.json"));
}

void McpTest::editing() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto settings = dir.path() + "/studio.ini";
    qApp->setProperty("settingsFile", settings);
    const auto root = dir.path() + "/edit-pack";
    const QString texture = "assets/nova/textures/block/a.png";
    const QString model = "assets/nova/models/block/a.json";
    write(root + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Edit"}})");
    write(root + '/' + model, R"({"textures":{"all":"nova:block/a"},"custom":17,"elements":[{"from":[0,0,0],"to":[16,16,16],"faces":{"north":{"texture":"#all"}}}]})");
    const QString cycleA = "assets/nova/models/block/cycle_a.json";
    const QString cycleB = "assets/nova/models/block/cycle_b.json";
    write(root + '/' + cycleA, R"({"parent":"nova:block/cycle_b"})");
    write(root + '/' + cycleB, R"({"parent":"nova:block/cycle_a"})");
    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QVERIFY(QDir().mkpath(QFileInfo(root + '/' + texture).path()));
    QVERIFY(image.save(root + '/' + texture));
    const QString wideTexture = "assets/nova/textures/block/wide.png";
    QImage wideImage(256, 256, QImage::Format_RGBA8888);
    wideImage.fill(Qt::transparent);
    QVERIFY(wideImage.save(root + '/' + wideTexture));
    const auto before = fingerprint(root);
    QElapsedTimer clock;
    clock.start();
    auto owner = std::make_unique<nova::Window>(root, QString{}, clock);
    auto &window = *owner;
    window.show();
    nova::EditorTestAccess::open(window, root + '/' + texture);
    auto canvas = window.activeCanvas();
    QVERIFY(canvas);
    const auto doc = canvas->document;
    const auto layer = doc->activeLayer().toString(QUuid::WithoutBraces);
    const auto frame = doc->activeFrame().toString(QUuid::WithoutBraces);
    window.findChild<QAction *>("mcpAccess")->trigger();
    auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
    auto service = window.findChild<Service *>();
    QVERIFY(dialog && service);
    dialog->findChild<QPushButton *>("mcpAuthorize")->click();
    QProcess reader;
    start(reader, service->connectionCode());
    initialize(reader);
    QTimer heartbeat;
    heartbeat.setInterval(10);
    qint64 lastBeat = clock.elapsed(), maxGap = 0;
    connect(&heartbeat, &QTimer::timeout, this, [&] {
        const auto now = clock.elapsed();
        maxGap = std::max(maxGap, now - lastBeat);
        lastBeat = now;
    });
    heartbeat.start();
    auto rpc = [&](QProcess &process, const QString &tool, const QJsonObject &args = {}) {
        QElapsedTimer elapsed;
        elapsed.start();
        process.write(call(3, tool, args));
        const auto result = payload(receive(process));
        if (tool.endsWith(".edit") || tool.startsWith("nova.edit."))
            qInfo().noquote() << tool << args["operation"].toObject()["command"].toString()
                              << "round_trip_us" << elapsed.nsecsElapsed() / 1000;
        return result;
    };
    QJsonObject project;
    QTRY_VERIFY_WITH_TIMEOUT((project = rpc(reader, "nova.project.info")).contains("generation"), 10000);
    auto resolve = [&](QProcess &process, const QString &path) -> QJsonValue {
        const auto assets = rpc(process, "nova.assets.list", {{"generation", project["generation"]}})["assets"].toArray();
        for (const auto &item : assets)
            if (item.toObject()["path"] == path)
                return item.toObject()["assetId"];
        return QJsonValue{};
    };
    const auto paint = QJsonObject{{"command", "stroke"}, {"layer", layer},
        {"points", QJsonArray{QJsonValue(QJsonArray{1, 1})}}, {"size", 1}, {"color", "#ff0000"}, {"opacity", 255}};
    QJsonObject args;
    auto target = [&](QProcess &process, const QString &path) {
        args = {{"generation", project["generation"]}, {"projectId", project["projectId"]},
                {"assetId", resolve(process, path)}};
        const auto caps = rpc(process, "nova.edit.capabilities",
            {{"generation", args["generation"]}, {"assetId", args["assetId"]}});
        args["expectedRevision"] = caps["document"].toObject()["revision"];
        return caps;
    };
    auto err = [](const QJsonObject &response) { return response["error"].toObject()["code"].toString(); };
    QCOMPARE(target(reader, texture)["permission"], "read_only");
    auto single = [&](QProcess &process, const QString &tool, const QJsonObject &operation) {
        auto input = args;
        input["operation"] = operation;
        return rpc(process, tool, input);
    };
    QCOMPARE(err(single(reader, "nova.texture.edit", paint)), "permission_denied");
    auto preview = args;
    preview["operations"] = QJsonArray{paint};
    const auto initialSerial = doc->changeSerial();
    const auto initialCursor = doc->historyCursor();
    const auto initialHistorySize = doc->historyLabels().size();
    const auto initialModified = doc->modified();
    const auto initialImage = doc->image;
    const auto prepared = rpc(reader, "nova.edit.preview", preview);
    QCOMPARE(prepared["permission"], "read_only");
    QCOMPARE(prepared["revision"], args["expectedRevision"]);
    auto targets = [&](const QJsonArray &ids) {
        return QJsonObject{{"generation", project["generation"]}, {"projectId", project["projectId"]},
                           {"assetIds", ids}};
    };
    const auto modelId = resolve(reader, model);
    QVERIFY(modelId.isString());
    auto input = targets({args["assetId"], modelId});
    const auto context = rpc(reader, "nova.assets.context", input);
    QVERIFY(!context["references"].toArray().isEmpty());
    const auto plan = rpc(reader, "nova.workflow.prepare", input);
    QCOMPARE(plan["permission"], "read_only");
    QCOMPARE(plan["results"].toArray()[0].toObject()["revision"], args["expectedRevision"]);
    QCOMPARE(plan["results"].toArray()[1].toObject()["documentOpen"], false);
    QCOMPARE(rpc(reader, "nova.workflow.validate_changes", input)["files"], 2);
    auto cycles = targets({resolve(reader, cycleA)});
    cycles["depth"] = 3;
    const auto cycle = rpc(reader, "nova.assets.context", cycles);
    QCOMPARE(cycle["assets"].toArray().size(), 2);
    QCOMPARE(cycle["warnings"].toArray()[0].toObject()["code"], "parent_cycle");
    cycles["depth"] = 4;
    QCOMPARE(err(rpc(reader, "nova.assets.context", cycles)), "invalid_arguments");
    auto cross = input;
    cross["projectId"] = randomToken();
    QCOMPARE(err(rpc(reader, "nova.assets.context", cross)), "stale_project");
    auto partial = targets({args["assetId"], randomToken(), modelId});
    partial["types"] = QJsonArray{"texture"};
    const auto rows = rpc(reader, "nova.assets.inspect_batch", partial)["results"].toArray();
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows[0].toObject()["metadata"].toObject()["width"], 16);
    QCOMPARE(err(rows[1].toObject()), "asset_not_found");
    QCOMPARE(err(rows[2].toObject()), "invalid_asset_type");
    QJsonArray tooMany;
    for (int i = 0; i < 9; ++i)
        tooMany.append(args["assetId"]);
    QCOMPARE(err(rpc(reader, "nova.assets.inspect_batch", targets(tooMany))), "invalid_arguments");
    QCOMPARE(doc->changeSerial(), initialSerial);
    QCOMPARE(doc->historyCursor(), initialCursor);
    QCOMPARE(doc->historyLabels().size(), initialHistorySize);
    QCOMPARE(doc->modified(), initialModified);
    QCOMPARE(doc->image, initialImage);
    QCOMPARE(fingerprint(root), before);
    service->revoke();
    QTRY_COMPARE(reader.state(), QProcess::NotRunning);

    auto authorizeEditing = [&] {
        grantEditing(window, dialog, service);
    };
    authorizeEditing();
    saveScreenshot(dialog, "editing-authorized");
    QProcess writer;
    start(writer, service->connectionCode());
    initialize(writer);
    project = rpc(writer, "nova.project.info");
    QCOMPARE(target(writer, texture)["permission"], "editing");
    QVERIFY(args["expectedRevision"].isString());
    auto next = [&](const QJsonObject &response) {
        QVERIFY2(response["revision"].isString(), qPrintable(QString::fromUtf8(QJsonDocument(response).toJson())));
        args["expectedRevision"] = response["revision"];
    };
    auto invalid = paint;
    invalid["points"] = QJsonArray{QJsonValue(QJsonArray{16, 1})};
    QCOMPARE(err(single(writer, "nova.texture.edit", invalid)), "invalid_arguments");
    const auto painted = single(writer, "nova.texture.edit", paint);
    QVERIFY(painted["changed"].toBool());
    QCOMPARE(doc->image.pixelColor(1, 1), QColor(Qt::red));
    QVERIFY(doc->modified());
    QCOMPARE(err(single(writer, "nova.texture.edit", paint)), "revision_conflict");
    next(painted);
    saveScreenshot(&window, "texture-edit");
    next(rpc(writer, "nova.edit.undo", args));
    QCOMPARE(doc->image.pixelColor(1, 1).alpha(), 0);
    next(rpc(writer, "nova.edit.redo", args));
    QCOMPARE(doc->image.pixelColor(1, 1), QColor(Qt::red));

    const QJsonObject region{{"command", "fill_rect"}, {"layer", layer}, {"x", 3}, {"y", 4},
        {"width", 2}, {"height", 3}, {"color", "#00ff00"}, {"opacity", 255}};
    const QJsonObject rename{{"command", "layer_rename"}, {"layer", layer}, {"name", "MCP ink"}};
    auto missing = rename;
    missing["layer"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto batch = [&](const QJsonArray &operations) {
        auto input = args;
        input["operations"] = operations;
        return rpc(writer, "nova.edit.batch", input);
    };
    auto cancelArgs = args;
    cancelArgs["operation"] = region;
    writer.write(call(91, "nova.texture.edit", cancelArgs) +
                 request(-1, "notifications/cancelled", {{"requestId", 91}}));
    QCOMPARE(error(receive(writer)), "operation_canceled");
    QCOMPARE(doc->image.pixelColor(3, 4).alpha(), 0);
    const auto cursor = doc->historyCursor();
    const auto serial = doc->changeSerial();
    QCOMPARE(err(batch({region, missing})), "edit_rejected");
    QCOMPARE(doc->historyCursor(), cursor);
    QCOMPARE(doc->changeSerial(), serial);
    QCOMPARE(doc->image.pixelColor(3, 4).alpha(), 0);
    QJsonArray oversized;
    for (int i = 0; i < 9; ++i)
        oversized.append(rename);
    QCOMPARE(err(batch(oversized)), "invalid_arguments");
    const auto userEdit = connect(service, &Service::editPrepared, this, [&] {
        QVERIFY(window.documents().edit(doc, {nova::LayerCommand::Rename, doc->activeLayer(), "Concurrent edit"}));
    }, Qt::SingleShotConnection);
    QCOMPARE(err(batch({region, rename})), "revision_conflict");
    disconnect(userEdit);
    QCOMPARE(doc->image.pixelColor(3, 4).alpha(), 0);
    target(writer, texture);
    const auto afterUserEdit = doc->historyCursor();
    next(batch({region, rename}));
    QCOMPARE(doc->historyCursor(), afterUserEdit + 1);
    QCOMPARE(doc->layers().front().name, "MCP ink");
    QCOMPARE(doc->image.pixelColor(4, 6), QColor(Qt::green));
    next(rpc(writer, "nova.edit.undo", args));
    QCOMPARE(doc->image.pixelColor(3, 4).alpha(), 0);
    QVERIFY(doc->layers().front().name != "MCP ink");
    next(rpc(writer, "nova.edit.redo", args));
    next(single(writer, "nova.layer.edit", {{"command", "layer_rename"}, {"layer", layer}, {"name", "Recovered MCP ink"}}));
    next(single(writer, "nova.animation.edit", {{"command", "frame_add"}, {"frame", frame}}));
    QVERIFY(doc->animated());
    QCOMPARE(doc->animationState().frames.size(), size_t(2));
    next(single(writer, "nova.animation.edit", {{"command", "animation_duration"}, {"entry", 1}, {"ticks", 7}}));
    QCOMPARE(doc->animationState().entries[1].ticks, 7);
    auto animationTargets = targets({args["assetId"]});
    animationTargets["expectedRevisions"] = QJsonArray{QJsonObject{
        {"assetId", args["assetId"]}, {"revision", args["expectedRevision"]}}};
    const auto animationValidation = rpc(writer, "nova.workflow.validate_changes", animationTargets);
    QCOMPARE(animationValidation["diagnostics"].toArray().last().toObject()["code"], "live_animation_checked");
    auto activity = dialog->findChild<QLabel *>("mcpWorkflowActivity");
    QVERIFY(activity && activity->text().contains("Undo available") &&
            activity->text().contains("Last validation") && activity->text().contains("Client"));
    auto theme = window.findChild<QAction *>("darkThemeCommand");
    QVERIFY(theme);
    const bool wasDark = theme->isChecked();
    theme->setChecked(true);
    saveScreenshot(dialog, "workflow-dark");
    theme->setChecked(false);
    saveScreenshot(dialog, "workflow-light");
    theme->setChecked(wasDark);
    saveScreenshot(&window, "animation-edit");
    next(rpc(writer, "nova.edit.undo", args));
    QCOMPARE(err(rpc(writer, "nova.workflow.validate_changes", animationTargets)), "revision_conflict");
    {
        WorkerGate gate;
        writer.write(call(96, "nova.workflow.validate_changes", targets({args["assetId"]})) +
                     request(-1, "notifications/cancelled", {{"requestId", 96}}) + request(97, "ping"));
        QCOMPARE(receive(writer)["id"].toInt(), 97);
    }
    QTRY_VERIFY(!service->findChild<QFutureWatcherBase *>());
    {
        WorkerGate gate;
        writer.write(call(98, "nova.workflow.validate_changes", targets({args["assetId"]})) +
                     request(99, "ping"));
        QCOMPARE(receive(writer)["id"].toInt(), 99);
        QVERIFY(window.documents().edit(doc, {nova::LayerCommand::Rename, doc->activeLayer(), "Native edit"}));
    }
    QCOMPARE(error(receive(writer)), "revision_conflict");
    QCOMPARE(err(rpc(writer, "nova.edit.undo", args)), "revision_conflict");
    target(writer, texture);
    QCOMPARE(err(rpc(writer, "nova.edit.undo", args)), "history_conflict");

    auto backdrop = region;
    backdrop["x"] = 0;
    backdrop["y"] = 0;
    backdrop["width"] = 16;
    backdrop["height"] = 16;
    backdrop["color"] = "#3275b0";
    backdrop["layer"] = doc->activeLayer().toString(QUuid::WithoutBraces);
    next(single(writer, "nova.texture.edit", backdrop));

    auto panel = nova::EditorTestAccess::models(window);
    QVERIFY(panel);
    window.findChild<QDockWidget *>("modelsUv")->show();
    window.findChild<QDockWidget *>("modelsUv")->raise();
    window.findChild<QDockWidget *>("preview")->show();
    panel->openAsset(model);
    panel->selectFace(0);
    auto uv = panel->findChild<QPushButton *>("uvBegin");
    QVERIFY(uv);
    QTRY_VERIFY(uv->isEnabled());
    uv->click();
    QVERIFY(canvas->uvEditing);
    QTest::keyClick(canvas, Qt::Key_Escape);
    QVERIFY(!canvas->uvEditing);
    QVERIFY(target(writer, model)["document"].isObject());
    QCOMPARE(err(single(writer, "nova.texture.edit", paint)), "invalid_asset_type");
    const auto modelTargets = targets({args["assetId"]});
    next(single(writer, "nova.model.edit", {{"command", "model_texture"}, {"alias", "all"},
        {"texture", "nova:block/missing"}}));
    const auto broken = rpc(writer, "nova.workflow.validate_changes", modelTargets);
    QVERIFY(broken["counts"].toObject()["Error"].toInt() > 0);
    next(single(writer, "nova.model.edit", {{"command", "model_texture"}, {"alias", "all"},
        {"texture", "nova:block/a"}}));
    const auto repaired = rpc(writer, "nova.workflow.validate_changes", modelTargets);
    QVERIFY(!repaired.contains("error"));
    QCOMPARE(repaired["counts"].toObject()["Error"].toInt(), 0);
    next(single(writer, "nova.model.edit", {{"command", "model_label"}, {"element", 0}, {"name", "MCP element"}}));
    next(single(writer, "nova.model.edit", {{"command", "model_add"}, {"texture", "#all"}}));
    next(single(writer, "nova.model.edit", {{"command", "model_transform"}, {"element", 1},
        {"from", QJsonArray{4, 0, 4}}, {"to", QJsonArray{12, 12, 12}},
        {"rotation", QJsonObject{{"enabled", false}}}}));
    auto modelVersion = panel->findChild<QComboBox *>("modelVersion");
    QVERIFY(modelVersion);
    const QJsonObject rotation{{"command", "model_transform"}, {"element", 1},
        {"from", QJsonArray{4, 0, 4}}, {"to", QJsonArray{12, 12, 12}},
        {"rotation", QJsonObject{{"enabled", true}, {"origin", QJsonArray{8, 8, 8}},
            {"angles", QJsonArray{0, 15, 0}}, {"rescale", false}}}};
    modelVersion->setCurrentText("1.21.11");
    QCOMPARE(err(single(writer, "nova.model.edit", rotation)), "revision_conflict");
    target(writer, model);
    QCOMPARE(err(single(writer, "nova.model.edit", rotation)), "edit_rejected");
    modelVersion->setCurrentText("26.1");
    target(writer, model);
    next(single(writer, "nova.model.edit", rotation));
    saveScreenshot(&window, "model-edit");
    const QJsonObject badUv{{"command", "uv_set"}, {"element", 0}, {"face", "north"},
                            {"uv", QJsonArray{0, 0, 8, 8}}, {"rotation", 45}};
    QCOMPARE(err(single(writer, "nova.uv.edit", badUv)), "invalid_arguments");
    const auto modelCaps = target(writer, model);
    QCOMPARE(modelCaps["permission"], "editing");
    const auto beforeUv = args.value("expectedRevision");
    const auto inspectModel = [&] {
        return rpc(writer, "nova.model.inspect",
                   {{"generation", args["generation"]}, {"assetId", args["assetId"]}})["metadata"].toObject();
    };
    const auto inspected = inspectModel();
    QVERIFY(inspected["explicitUvFaceCount"].isDouble());
    const auto editedModel = panel->openModelDocument(model);
    QVERIFY(editedModel);
    const auto originalModel = editedModel->authoritative();
    const auto viewport = window.findChild<QDockWidget *>("preview")->findChild<nova::Cube *>();
    QVERIFY(viewport);
    auto viewportFace = [&]() -> nova::mc::Face {
        for (const auto &face : viewport->meshData().faces)
            if (face.elementIndex == 0 && face.name == "north")
                return face;
        return {};
    };
    QCOMPARE(viewportFace().name, "north");
    const auto originalFace = viewportFace();
    auto validUv = badUv;
    validUv["rotation"] = 90;
    auto uvPreview = args;
    uvPreview["operations"] = QJsonArray{validUv};
    const auto plannedUv = rpc(writer, "nova.edit.preview", uvPreview);
    QCOMPARE(plannedUv["permission"], "editing");
    QCOMPARE(plannedUv["revision"], beforeUv);
    QCOMPARE(editedModel->authoritative(), originalModel);
    const auto appliedUv = single(writer, "nova.uv.edit", validUv);
    QVERIFY(appliedUv["changed"].toBool());
    QCOMPARE(appliedUv["changeSummary"].toObject()["previousRevision"], beforeUv);
    next(appliedUv);
    const auto afterUv = args.value("expectedRevision");
    QVERIFY(afterUv != beforeUv);
    auto uvValidation = modelTargets;
    auto expectModelRevision = [&](const QJsonValue &revision) {
        uvValidation["expectedRevisions"] = QJsonArray{
            QJsonObject{{"assetId", args["assetId"]}, {"revision", revision}}};
        return rpc(writer, "nova.workflow.validate_changes", uvValidation);
    };
    QCOMPARE(err(expectModelRevision(beforeUv)), "revision_conflict");
    const auto currentUv = expectModelRevision(afterUv);
    QVERIFY(!currentUv.contains("error"));
    QCOMPARE(currentUv["counts"].toObject()["Error"].toInt(), 0);
    QCOMPARE(currentUv["revisions"], uvValidation["expectedRevisions"]);
    QCOMPARE(currentUv["validationSources"].toObject()["models"], "live_when_open");
    QCOMPARE(inspectModel()["explicitUvFaceCount"].toInt(), inspected["explicitUvFaceCount"].toInt() + 1);
    QCOMPARE(editedModel->authoritative()["custom"], QJsonValue(17));
    const auto north = [&] {
        return editedModel->authoritative()["elements"].toArray()[0].toObject()["faces"].toObject()["north"].toObject();
    };
    QCOMPARE(north()["rotation"].toInt(), 90);
    QTRY_COMPARE(viewportFace().rotation, 90);
    const auto editedFace = viewportFace();
    QVERIFY(editedFace.uv != originalFace.uv);
    const auto uvModel = editedModel->authoritative();
    saveScreenshot(&window, "uv-edit");
    next(rpc(writer, "nova.edit.undo", args));
    const auto undoUv = args.value("expectedRevision");
    QCOMPARE(editedModel->authoritative(), originalModel);
    QTRY_VERIFY(viewportFace().uv == originalFace.uv);
    next(rpc(writer, "nova.edit.redo", args));
    QCOMPARE(editedModel->authoritative(), uvModel);
    QTRY_VERIFY(viewportFace().uv == editedFace.uv);
    qInfo().noquote() << "model_uv_revisions" << beforeUv.toString() << afterUv.toString()
                      << undoUv.toString() << args["expectedRevision"].toString();
    QCOMPARE(fingerprint(root).value(model), before.value(model));

    nova::EditorTestAccess::open(window, root + '/' + wideTexture);
    const auto wide = window.activeCanvas()->document;
    target(writer, wideTexture);
    QJsonArray largeBatch;
    auto fullRect = region;
    fullRect["layer"] = wide->activeLayer().toString(QUuid::WithoutBraces);
    fullRect["x"] = 0;
    fullRect["y"] = 0;
    fullRect["width"] = 256;
    fullRect["height"] = 256;
    for (const auto &color : {"#111111", "#222222", "#333333", "#444444",
                              "#555555", "#666666", "#777777", "#888888"}) {
        fullRect["color"] = color;
        largeBatch.append(fullRect);
    }
    qint64 largeGap = 0;
    lastBeat = clock.elapsed();
    maxGap = 0;
    const auto largeResult = batch(largeBatch);
    largeGap = std::max(maxGap, clock.elapsed() - lastBeat);
    QCOMPARE(largeResult["appliedOperations"].toInt(), 8);
    QCOMPARE(wide->image.pixelColor(255, 255), QColor("#888888"));
    qInfo("eight_full_256px_rectangles_10ms_heartbeat_max_gap_ms=%lld", largeGap);
    next(largeResult);
    next(rpc(writer, "nova.edit.undo", args));
    QVERIFY(!wide->modified());
    QElapsedTimer nativeTime;
    nativeTime.start();
    QVERIFY(window.documents().execute(wide, fullRect));
    qInfo("native_full_256px_rectangle_with_refresh_us=%lld", nativeTime.nsecsElapsed() / 1000);
    QVERIFY(window.documents().dispatch(wide, nova::Command::Undo));
    QVERIFY(!wide->modified());
    if (qEnvironmentVariableIsSet("NOVA_MCP_EDIT_BENCH")) {
        auto timed = [&](const char *scenario, int sample, const QString &tool, QJsonObject input) {
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            connect(&writer, &QProcess::readyReadStandardOutput, &loop, [&] {
                if (writer.canReadLine())
                    loop.quit();
            });
            for (auto it = args.cbegin(); it != args.cend(); ++it)
                input.insert(it.key(), it.value());
            const auto frame = call(55, tool, input);
            QElapsedTimer elapsed;
            maxGap = 0;
            lastBeat = clock.elapsed();
            qInfo("edit_bench_begin %s %d", scenario, sample);
            timeout.start(5000);
            elapsed.start();
            writer.write(frame);
            loop.exec();
            const auto nanoseconds = elapsed.nsecsElapsed();
            const auto gap = std::max(maxGap, clock.elapsed() - lastBeat);
            qInfo("edit_bench_end %s %d %lld %lld", scenario, sample, nanoseconds, gap);
            const auto result = payload(receive(writer, 1));
            return result;
        };
        auto series = [&](const char *name, const QString &tool, const QJsonObject &operation) {
            const auto initial = wide->historyCursor();
            const auto memoryBefore = qint64(usage(GetCurrentProcess()).privateBytes);
            const auto undoName = QByteArray("undo_") + name;
            const auto redoName = QByteArray("redo_") + name;
            for (int i = -5; i < 41; ++i) {
                const auto edited = timed(name, i, tool, operation);
                QVERIFY2(edited["changed"].toBool(), qPrintable(QString::fromUtf8(QJsonDocument(edited).toJson())));
                next(edited);
                QCOMPARE(wide->historyCursor(), initial + 1);
                const auto undone = timed(undoName.constData(), i, "nova.edit.undo", {});
                QVERIFY(undone["changed"].toBool());
                next(undone);
                QCOMPARE(wide->historyCursor(), initial);
                const auto redone = timed(redoName.constData(), i, "nova.edit.redo", {});
                QVERIFY(redone["changed"].toBool());
                next(redone);
                QCOMPARE(wide->historyCursor(), initial + 1);
                next(rpc(writer, "nova.edit.undo", args));
            }
            QCOMPARE(wide->historyCursor(), initial);
            qInfo("edit_bench_memory %s %lld %lld %lld", name, memoryBefore,
                  qint64(usage(GetCurrentProcess()).privateBytes), wide->historyBytes());
        };
        target(writer, wideTexture);
        series("texture_batch", "nova.edit.batch", {{"operations", largeBatch}});
        QVERIFY(!QTest::currentTestFailed());
        const auto anchor = wide->activeFrame().toString(QUuid::WithoutBraces);
        const QJsonObject add{{"command", "frame_add"}, {"frame", anchor}};
        for (int i = 0; i < 7; ++i)
            QVERIFY(window.documents().execute(wide, add));
        QCOMPARE(wide->animationState().frames.size(), size_t(8));
        target(writer, wideTexture);
        const QJsonObject duration{{"command", "animation_duration"}, {"entry", 1}, {"ticks", 7}};
        const QJsonObject move{{"command", "frame_move"}, {"frame", anchor}, {"value", 3}};
        series("frame_add", "nova.animation.edit", {{"operation", add}});
        QVERIFY(!QTest::currentTestFailed());
        series("duration", "nova.animation.edit", {{"operation", duration}});
        QVERIFY(!QTest::currentTestFailed());
        series("reorder", "nova.animation.edit", {{"operation", move}});
        QVERIFY(!QTest::currentTestFailed());
        series("animation_batch", "nova.edit.batch", {{"operations", QJsonArray{add, duration, move}}});
        QVERIFY(!QTest::currentTestFailed());
        const auto afterBench = fingerprint(root);
        for (auto it = before.begin(); it != before.end(); ++it)
            QCOMPARE(afterBench.value(it.key()), it.value());
        writer.closeWriteChannel();
        QTRY_COMPARE(writer.state(), QProcess::NotRunning);
        return;
    }
    nova::EditorTestAccess::open(window, root + '/' + texture);

    nova::EditorTestAccess::checkpoint(window);
    nova::RecoveryStore recovery(dir.path() + "/recovery");
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(recovery.pathFor(doc->id())), 10000);
    const auto snapshot = recovery.load(doc->id());
    QVERIFY2(snapshot.error.isEmpty(), qPrintable(snapshot.error));
    QVERIFY(!snapshot.nativeData.isEmpty());
    nova::Document restored(doc->path, QImage{});
    QString restoreError;
    QVERIFY2(restored.decodeNative(snapshot.nativeData, "recovery.png", &restoreError), qPrintable(restoreError));
    QCOMPARE(restored.animationState().frames.size(), size_t(2));
    const auto after = fingerprint(root);
    for (auto it = before.begin(); it != before.end(); ++it)
        QCOMPARE(after.value(it.key()), it.value());

    target(writer, texture);
    const auto beforeRevoke = doc->changeSerial();
    const auto revokePending = connect(service, &Service::editPrepared, this, [&] {
        dialog->findChild<QPushButton *>("mcpRevoke")->click();
    }, Qt::SingleShotConnection);
    auto pendingArgs = args;
    pendingArgs["operation"] = QJsonObject{{"command", "animation_timing"}, {"ticks", 4}, {"interpolate", true}};
    writer.write(call(93, "nova.animation.edit", pendingArgs));
    QTRY_COMPARE(writer.state(), QProcess::NotRunning);
    disconnect(revokePending);
    QCOMPARE(doc->changeSerial(), beforeRevoke);
    QCOMPARE(service->permission(), "Disabled");
    heartbeat.stop();
    qInfo("late_editing_sequence_10ms_heartbeat_max_gap_ms=%lld", maxGap);

    // Disconnect the real stdio bridge on either side of the commit boundary.
    for (const bool afterCommit : {false, true}) {
        authorizeEditing();
        QProcess dropped;
        start(dropped, service->connectionCode());
        initialize(dropped);
        project = rpc(dropped, "nova.project.info");
        target(dropped, texture);
        const auto beforeDisconnect = doc->changeSerial();
        bool reachedBoundary = false;
        auto disconnectClient = [&] {
            reachedBoundary = true;
            dropped.kill();
            QTRY_COMPARE(dropped.state(), QProcess::NotRunning);
            QTRY_COMPARE(service->status(), "Disabled");
        };
        const auto connection = afterCommit
            ? connect(&window.documents(), &nova::DocumentSession::changed, this,
                      disconnectClient, Qt::SingleShotConnection)
            : connect(service, &Service::editPrepared, this, disconnectClient, Qt::SingleShotConnection);
        auto interrupted = args;
        interrupted["operation"] = QJsonObject{{"command", "fill_rect"},
            {"layer", doc->activeLayer().toString(QUuid::WithoutBraces)}, {"x", 0}, {"y", 0},
            {"width", 1}, {"height", 1}, {"color", "#ffa500"}, {"opacity", 255}};
        const auto previousPixel = doc->image.pixelColor(0, 0);
        dropped.write(call(94, "nova.texture.edit", interrupted));
        QTRY_VERIFY(reachedBoundary);
        disconnect(connection);
        QCOMPARE(doc->changeSerial(), beforeDisconnect + (afterCommit ? 1 : 0));
        QCOMPARE(doc->image.pixelColor(0, 0), afterCommit ? QColor("#ffa500") : previousPixel);
    }

    if (const auto sdk = qEnvironmentVariable("NOVA_MCP_SDK_PYTHON"); !sdk.isEmpty()) {
        const auto sdkSources = fingerprint(root);
        for (const auto &mode : {QString("cancel"), QString("cancel-after"),
                                 QString("drop-before"), QString("drop-after"),
                                 QString("revoke")}) {
            authorizeEditing();
            QProcess client;
            const auto beforeSerial = doc->changeSerial();
            const auto beforeCursor = doc->historyCursor();
            const auto beforePixel = doc->image.pixelColor(0, 0);
            bool boundaryReached = false;
            auto atBoundary = [&] {
                boundaryReached = true;
                QTRY_VERIFY_WITH_TIMEOUT(client.canReadLine(), 5000);
                const bool lateCancel = mode == "cancel-after";
                QCOMPARE(client.readLine().trimmed(), lateCancel ? QByteArray("cancel-after") : QByteArray("drop"));
                client.write("continue\n");
                if (lateCancel) {
                    QTRY_VERIFY_WITH_TIMEOUT(client.canReadLine(), 6000);
                    QCOMPARE(client.readLine().trimmed(), QByteArray("cancel delivered"));
                } else {
                    QTRY_COMPARE_WITH_TIMEOUT(service->status(), "Disabled", 8000);
                }
            };
            QMetaObject::Connection connection;
            if (mode == "drop-before")
                connection = connect(service, &Service::editPrepared, this, atBoundary, Qt::SingleShotConnection);
            else if (mode == "drop-after" || mode == "cancel-after")
                connection = connect(&window.documents(), &nova::DocumentSession::changed,
                                     this, atBoundary, Qt::SingleShotConnection);
            startSdk(client, service->connectionCode(), mode);
            if (mode == "revoke") {
                QTRY_VERIFY_WITH_TIMEOUT(client.canReadLine(), 12000);
                QCOMPARE(client.readLine().trimmed(), QByteArray("revoke"));
                dialog->findChild<QPushButton *>("mcpRevoke")->click();
                client.write("continue\n");
            }
            finishSdk(client, mode);
            disconnect(connection);
            QCOMPARE(boundaryReached, mode.startsWith("drop-") || mode == "cancel-after");
            QTRY_COMPARE(service->status(), "Disabled");
            QVERIFY(service->editActivity().isEmpty());
            if (mode == "drop-after" || mode == "revoke")
                saveScreenshot(dialog, mode == "drop-after" ? "sdk-disconnected-after-commit" : "sdk-revoked");
            const bool committed = mode == "drop-after" || mode == "revoke" || mode == "cancel-after";
            QCOMPARE(doc->changeSerial(), beforeSerial + (committed ? 1 : 0));
            QCOMPARE(doc->historyCursor(), beforeCursor + (committed ? 1 : 0));
            QCOMPARE(doc->image.pixelColor(0, 0), committed ? QColor("#11bc95") : beforePixel);
            qInfo().noquote() << "SDK lifecycle" << mode << "serial" << beforeSerial << doc->changeSerial()
                              << "cursor" << beforeCursor << doc->historyCursor();
            if (committed)
                QVERIFY(window.documents().dispatch(doc, nova::Command::Undo));
        }
        authorizeEditing();
        const auto usedCode = service->connectionCode();
        {
            QProcess actual;
            startSdk(actual, usedCode, "disconnect");
            finishSdk(actual, "disconnect");
            QTRY_COMPARE(service->status(), "Disabled");
        }
        {
            QProcess actual;
            startSdk(actual, usedCode, "expired");
            finishSdk(actual, "expired");
        }
        {
            dialog->findChild<QPushButton *>("mcpAuthorize")->click();
            QProcess actual;
            startSdk(actual, service->connectionCode(), "readonly");
            finishSdk(actual, "readonly");
            QTRY_COMPARE(service->status(), "Disabled");
        }
        authorizeEditing();
        QProcess actual;
        start(actual, service->connectionCode(), sdk, {QString::fromUtf8(NOVA_MCP_TEST_SDK), bridge(), "edit"});
        QTRY_COMPARE_WITH_TIMEOUT(actual.state(), QProcess::NotRunning, 30000);
        qInfo().noquote() << actual.readAllStandardOutput();
        QVERIFY2(actual.exitCode() == 0, actual.readAllStandardError().constData());
        const auto sdkAfter = fingerprint(root);
        for (auto it = sdkSources.begin(); it != sdkSources.end(); ++it)
            if (!it.key().startsWith(".nova/"))
                QCOMPARE(sdkAfter.value(it.key()), it.value());
    }
    owner.reset();
    const auto snapshotPath = recovery.pathFor(doc->id());
    const auto snapshotBytes = nova::Revision::read(snapshotPath);
    QVERIFY(snapshotBytes.readable);
    for (const QString decision : {"Later", "Restore"}) {
        nova::Window restarted(root, {}, clock);
        restarted.show();
        QTimer drive;
        bool handled = false;
        connect(&drive, &QTimer::timeout, this, [&] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            if (box->objectName() == "modelRecoveryDialog") {
                // This case verifies the raster snapshot independently of the model edits.
                for (auto button : box->buttons())
                    if (button->text() == "Later") {
                        button->click();
                        return;
                    }
            }
            if (box->objectName() != "recoveryDialog")
                return;
            if (decision == "Later")
                nova::EditorTestAccess::checkpoint(restarted);
            for (auto button : box->buttons())
                if (button->text() == decision) {
                    handled = true;
                    button->click();
                    return;
                }
        });
        drive.start(20);
        QTRY_VERIFY_WITH_TIMEOUT(handled, 10000);
        drive.stop();
        if (decision == "Later") {
            QVERIFY(restarted.close());
            QVERIFY(snapshotBytes.sameContent(nova::Revision::read(snapshotPath)));
        } else {
            QTRY_VERIFY(restarted.activeCanvas());
            const auto recovered = restarted.activeCanvas()->document;
            QVERIFY(recovered->modified());
            QVERIFY(recovered->animated());
            QCOMPARE(recovered->animationState().frames.front().layers.front().name, "Recovered MCP ink");
            restarted.findChild<QAction *>("saveCommand")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!recovered->modified(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(snapshotPath), 10000);
            // MCP Phase 1: Save writes the recovered animation into the pack (PNG + .png.mcmeta) as
            // well as its editable source; before, it saved only the editable source.
            QVERIFY(!recovered->minecraftOutputPending());
            QVERIFY(fingerprint(root).value(texture) != before.value(texture));
            QVERIFY(recovered->revision().sameContent(nova::Revision::read(root + '/' + texture)));
            QVERIFY(recovered->mcmetaRevision().sameContent(nova::Revision::read(root + '/' + texture + ".mcmeta")));
            const auto other = dir.path() + "/other-pack";
            write(other + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Other"}})");
            restarted.findChild<QAction *>("mcpAccess")->trigger();
            auto switched = restarted.findChild<Service *>();
            QVERIFY(switched->authorize(root, true));
            QProcess old;
            start(old, switched->connectionCode());
            initialize(old);
            QTRY_VERIFY_WITH_TIMEOUT((project = rpc(old, "nova.project.info")).contains("generation"), 10000);
            QVERIFY(target(old, texture)["document"].isObject());
            auto path = restarted.findChild<QLineEdit *>("projectPath");
            path->setText(other);
            QTest::keyClick(path, Qt::Key_Return);
            QTRY_COMPARE(switched->status(), "Project changed");
            QCOMPARE(err(single(old, "nova.texture.edit", paint)), "stale_project");
            QCOMPARE(err(rpc(old, "nova.workflow.prepare", targets({args["assetId"]}))), "stale_project");
            old.closeWriteChannel();
            QTRY_COMPARE(old.state(), QProcess::NotRunning);
            QVERIFY(restarted.close());
        }
    }
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--recovery-child")
        return recoveryChild(args[2], args[3]);
    McpTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "mcp_test.moc"
