// SPDX-License-Identifier: GPL-3.0-or-later
// MCP reliability: asset creation distinguishes invalid, occupied, missing-folder and unsafe
// destinations, and a stale index entry is reconciled by re-indexing instead of blocking creation.
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
#include <QTemporaryDir>
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
};

QTEST_MAIN(McpReliabilityTest)
#include "mcp_reliability_test.moc"
