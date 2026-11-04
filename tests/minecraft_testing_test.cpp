// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 3-D: launcher hand-off for resource-pack testing. All fixtures are
// synthetic; no real launcher data, account files or credentials are read.
#include "discovery.h"
#include "gamelog.h"
#include "installer.h"
#include "minecraft_animation.h"
#include "minecraft_testing.h"
#include "packsync.h"
#include "project.h"
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace nova;
using namespace nova::testing;

namespace {
void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
}
QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QString makePack(const QString &root, const QByteArray &texture = "PNG-A") {
    write(
        root + "/pack.mcmeta",
        R"({"pack":{"description":"Test","min_format":[75,0],"max_format":[75,0]},"x-unknown":{"keep":true}})");
    write(root + "/assets/minecraft/textures/block/stone.png", texture);
    write(root + "/assets/minecraft/models/item/thing.json",
          R"({"parent":"item/generated","x-field":1})");
    return root;
}
QByteArray auroraRegistry() {
    return R"({"schemaVersion":6,"instances":[
      {"id":"0123456789abcdef0123456789abcdef","displayName":"Vanilla Test","state":"ready",
       "installed":{"minecraftVersion":"1.21.11","platform":{"kind":"vanilla"}},"pack":null},
      {"id":"fedcba9876543210fedcba9876543210","displayName":"Pack","state":"installing",
       "installed":{"minecraftVersion":"1.21.11","platform":{"kind":"fabric"}},"pack":{"provider":"modrinth"}},
      {"id":"../../escape","displayName":"Bad","state":"ready","installed":{}}]})";
}

// Minimal HTTP server: path -> (status, headers, body).
class MockServer : public QTcpServer {
  public:
    struct Reply {
        int status = 200;
        QByteArray body;
        QByteArray location;
    };
    QMap<QString, Reply> routes;
    QStringList requests;
    MockServer() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const auto head = socket->readAll();
                    const auto path = QString::fromLatin1(head.split(' ').value(1));
                    requests << path;
                    const auto reply = routes.value(path, {404, "missing", {}});
                    QByteArray out = "HTTP/1.1 " + QByteArray::number(reply.status) + " X\r\n";
                    if (!reply.location.isEmpty())
                        out += "Location: " + reply.location + "\r\n";
                    out += "Content-Length: " + QByteArray::number(reply.body.size()) +
                           "\r\nConnection: close\r\n\r\n" + reply.body;
                    socket->write(out);
                    socket->disconnectFromHost();
                });
            }
        });
        listen(QHostAddress::LocalHost);
    }
    QString base() const { return QString("http://127.0.0.1:%1").arg(serverPort()); }
};
QString runFetch(const QUrl &url, const DownloadPolicy &policy, const QString &destination,
                 QByteArray *body = nullptr) {
    QNetworkAccessManager network;
    Fetch fetch(&network, policy);
    QSignalSpy done(&fetch, &Fetch::finished);
    fetch.start(url, destination);
    if (done.isEmpty() && !done.wait(10000))
        return "timeout";
    if (body)
        *body = fetch.body();
    return done.first().first().toString();
}
} // namespace

class MinecraftTestingTest : public QObject {
    Q_OBJECT
  private slots:
    void discoveryFindsLaunchersFromFixtures() {
        QTemporaryDir root;
        Environment e{root.filePath("local"), root.filePath("roaming"), root.filePath("pf"),
                      root.filePath("pf86")};
        QVERIFY(discoverLaunchers(e).isEmpty());
        write(e.programFilesX86 + "/Minecraft Launcher/MinecraftLauncher.exe", "MZ");
        write(e.localAppData + "/Aurora Launcher Native Preview/aurora-native.exe", "MZ");
        write(e.localAppData + "/Aurora Launcher Native Preview/native-package.ini",
              "[Package]\nIdentity=com.aurora.launcher.native-preview\n");
        write(e.localAppData + "/Aurora Launcher/aurora-launcher.exe", "MZ");
        QDir().mkpath(e.localAppData + "/Packages/" + officialStorePackage);
        const auto found = discoverLaunchers(e);
        QCOMPARE(found.size(), 4);
        QCOMPARE(found[0].kind, LauncherKind::Official);
        QCOMPARE(found[1].kind, LauncherKind::OfficialStore);
        QCOMPARE(found[2].dataRoot, e.localAppData + "/com.aurora.launcher.native-preview");
        QCOMPARE(found[3].dataRoot, e.localAppData + "/com.aurora.launcher");
    }
    void auroraIdentityOutsideItsNamespaceIsIgnored() {
        QTemporaryDir root;
        Environment e{root.filePath("local"), {}, {}, {}};
        write(e.localAppData + "/Aurora Launcher Native Preview/aurora-native.exe", "MZ");
        write(e.localAppData + "/Aurora Launcher Native Preview/native-package.ini",
              "[Package]\nIdentity=..\\..\\Windows\n");
        QVERIFY(discoverLaunchers(e).isEmpty());
    }
    void auroraRegistryParsingIsDefensive() {
        QString error;
        const auto instances = parseAuroraInstances(auroraRegistry(), "C:/data", &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(instances.size(), 2); // the traversal id is rejected
        QCOMPARE(instances[0].gameDirectory,
                 QString("C:/data/instances/0123456789abcdef0123456789abcdef"));
        QCOMPARE(instances[0].platform, QString("vanilla"));
        QVERIFY(!instances[0].modpackManaged);
        QVERIFY(instances[1].modpackManaged);
        QVERIFY(parseAuroraInstances(R"({"schemaVersion":99,"instances":[]})", "C:/d", &error)
                    .isEmpty());
        QVERIFY(error.contains("99"));
        QVERIFY(parseAuroraInstances("{oops", "C:/d", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void folderNamesAreSafe() {
        QCOMPARE(testPackFolderName("My Pack"), QString("Nova Test - My Pack"));
        QCOMPARE(testPackFolderName("a/b\\c:d*?.."), QString("Nova Test - a_b_c_d__"));
        QCOMPARE(testPackFolderName("  "), QString("Nova Test - Pack"));
        QVERIFY(testPackFolderName(QString(300, 'x')).size() <= 12 + 48);
    }
    void syncCreatesThenAtomicallyReplacesOnlyNovaPacks() {
        QTemporaryDir root;
        const auto pack = makePack(root.filePath("Source Pack"));
        const auto game = root.filePath("game");
        QDir().mkpath(game);
        const auto staging = root.filePath("staging");
        auto first = syncTestPack(pack, "Source Pack", game, staging);
        QVERIFY2(first.ok, qPrintable(first.error));
        QVERIFY(!first.replaced);
        const auto target = game + "/resourcepacks/Nova Test - Source Pack";
        QCOMPARE(read(target + "/assets/minecraft/textures/block/stone.png"), QByteArray("PNG-A"));
        QVERIFY(read(target + "/pack.mcmeta").contains("x-unknown")); // unknown fields preserved
        QVERIFY(inspectTestPack(game, "Nova Test - Source Pack").novaOwned);
        QVERIFY(!QFileInfo::exists(target + "/.nova"));
        // Minecraft must never see a half-written pack: during the swap window
        // the old complete export is still in place and nothing else is listed.
        write(pack + "/assets/minecraft/textures/block/stone.png", "PNG-B");
        QStringList visible;
        auto second = syncTestPack(pack, "Source Pack", game, staging, [&] {
            visible = QDir(game + "/resourcepacks")
                          .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
            QCOMPARE(read(target + "/assets/minecraft/textures/block/stone.png"),
                     QByteArray("PNG-A"));
        });
        QVERIFY2(second.ok, qPrintable(second.error));
        QVERIFY(second.replaced);
        QCOMPARE(visible, QStringList{"Nova Test - Source Pack"});
        QCOMPARE(read(target + "/assets/minecraft/textures/block/stone.png"), QByteArray("PNG-B"));
        QCOMPARE(QDir(staging).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{});
    }
    void syncRefusesFoldersNovaDidNotCreate() {
        QTemporaryDir root;
        const auto pack = makePack(root.filePath("P"));
        const auto game = root.filePath("game");
        write(game + "/resourcepacks/Nova Test - P/precious.txt", "user data");
        const auto outcome = syncTestPack(pack, "P", game, root.filePath("s"));
        QVERIFY(!outcome.ok);
        QVERIFY(outcome.error.contains("not created by Nova"));
        QCOMPARE(read(game + "/resourcepacks/Nova Test - P/precious.txt"), QByteArray("user data"));
        // Unrelated packs are never touched either.
        write(game + "/resourcepacks/Other/pack.mcmeta", "{}");
        QDir(game + "/resourcepacks/Nova Test - P").removeRecursively();
        QVERIFY(syncTestPack(pack, "P", game, root.filePath("s")).ok);
        QCOMPARE(read(game + "/resourcepacks/Other/pack.mcmeta"), QByteArray("{}"));
    }
    void syncRefusesUnsavedNativeStateThroughExportGuards() {
        QTemporaryDir root;
        const auto pack = makePack(root.filePath("P"));
        write(pack + "/.nova/documents/broken.nvdoc", "NOTNOVA0garbage");
        QDir().mkpath(root.filePath("game"));
        const auto outcome = syncTestPack(pack, "P", root.filePath("game"), root.filePath("s"));
        QVERIFY(!outcome.ok);
        QVERIFY2(outcome.error.contains("Corrupt editable source"), qPrintable(outcome.error));
        QVERIFY(!QFileInfo::exists(root.filePath("game/resourcepacks/Nova Test - P")));
    }
    // Found in the live run: opening an animated texture is an undoable import
    // step, which made Export Pack and Sync refuse an untouched pack. Phase 3-E
    // additionally requires the untouched open to be clean for the save prompt.
    void openingAnAnimatedTextureDoesNotBlockSync() {
        QTemporaryDir root;
        const auto png = root.filePath("assets/minecraft/textures/block/anim.png");
        QImage sheet(16, 32, QImage::Format_RGBA8888);
        sheet.fill(Qt::red);
        QDir().mkpath(QFileInfo(png).absolutePath());
        QVERIFY(sheet.save(png));
        write(png + ".mcmeta", R"({"animation":{"frametime":5}})");
        QString error;
        auto doc = openMinecraftAnimation(png, "1.21.11", &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(!doc->modified());               // opening is not a modification
        QVERIFY(!doc->exportModified());         // disk matches the imported state
        QVERIFY(!doc->minecraftOutputPending());
        QVERIFY(doc->applyFrames({doc->activeFrame()}, {"invert", {}}));
        QVERIFY(doc->modified());                // a real edit diverges from the baseline
        QVERIFY(doc->minecraftOutputPending());  // and is pending for the game files
        doc->undo();
        QVERIFY(!doc->modified());               // back at the opened baseline: clean again
        QVERIFY(!doc->minecraftOutputPending());
    }
    void lockedPackIsLeftIntactAndReported() {
#ifdef Q_OS_WIN
        QTemporaryDir root;
        const auto pack = makePack(root.filePath("P"));
        const auto game = root.filePath("game");
        QDir().mkpath(game);
        QVERIFY(syncTestPack(pack, "P", game, root.filePath("s")).ok);
        const auto locked =
            game + "/resourcepacks/Nova Test - P/assets/minecraft/textures/block/stone.png";
        // Hold the file the way an open zip/reader would (no delete sharing).
        HANDLE handle =
            CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(locked).utf16()),
                        GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);
        write(pack + "/assets/minecraft/textures/block/stone.png", "PNG-NEW");
        const auto outcome = syncTestPack(pack, "P", game, root.filePath("s"));
        CloseHandle(handle);
        QVERIFY(!outcome.ok);
        QVERIFY(outcome.error.contains("using the current test pack"));
        QCOMPARE(read(locked), QByteArray("PNG-A"));
        QVERIFY(syncTestPack(pack, "P", game, root.filePath("s")).ok); // succeeds once released
        QCOMPARE(read(locked), QByteArray("PNG-NEW"));
#else
        QSKIP("Windows sharing semantics");
#endif
    }
    void optionsMergePreservesEverything() {
        bool changed = false;
        auto merged =
            mergeResourcePackOption("version:4440\r\nresourcePacks:[\"vanilla\"]\r\nfov:0.5\r\n",
                                    "Nova Test - P", &changed);
        QVERIFY(changed);
        QCOMPARE(merged, QByteArray("version:4440\r\nresourcePacks:[\"vanilla\",\"file/Nova Test - "
                                    "P\"]\r\nfov:0.5\r\n"));
        merged = mergeResourcePackOption(merged, "Nova Test - P", &changed);
        QVERIFY(!changed);
        merged = mergeResourcePackOption("", "Nova Test - P", &changed);
        QCOMPARE(merged, QByteArray("resourcePacks:[\"vanilla\",\"file/Nova Test - P\"]\n"));
    }
    void launcherProfileMergeIsAdditive() {
        const QByteArray original = R"({"profiles":{"abc":{"name":"Mine","type":"custom","x":1}},
          "settings":{"keepLauncherOpen":true},"version":3,"unknownTop":{"a":[1,2]}})";
        QString error;
        const auto merged =
            QJsonDocument::fromJson(
                mergeLauncherProfile(original, "C:/Nova/minecraft-test", "1.21.11", &error))
                .object();
        QVERIFY(error.isEmpty());
        QCOMPARE(merged.value("version").toInt(), 3);
        QCOMPARE(merged.value("unknownTop").toObject().value("a").toArray().size(), 2);
        QCOMPARE(merged.value("settings").toObject().value("keepLauncherOpen").toBool(), true);
        const auto profiles = merged.value("profiles").toObject();
        QCOMPARE(profiles.value("abc").toObject().value("x").toInt(), 1);
        const auto nova = profiles.value(launcherProfileKey).toObject();
        QCOMPARE(nova.value("lastVersionId").toString(), QString("1.21.11"));
        QCOMPARE(nova.value("gameDir").toString(), QString("C:\\Nova\\minecraft-test"));
        QVERIFY(mergeLauncherProfile("[]", "C:/x", "1.21.11", &error).isEmpty());
        QVERIFY(mergeLauncherProfile("{\"noProfiles\":1}", "C:/x", "1.21.11", &error).isEmpty());
    }
    void launcherProfileInstallBacksUpAndRoundTrips() {
        QTemporaryDir root;
        const auto mc = root.filePath(".minecraft");
        QVERIFY(!installLauncherProfile(mc, root.filePath("game"), "1.21.11").ok); // no list yet
        write(mc + "/launcher_profiles.json", R"({"profiles":{}})");
        const auto outcome = installLauncherProfile(mc, root.filePath("game"), "1.21.11");
        QVERIFY2(outcome.ok, qPrintable(outcome.error));
        QCOMPARE(read(outcome.backupPath), QByteArray(R"({"profiles":{}})"));
        QString dir, version;
        QVERIFY(launcherProfilePresent(mc, &dir, &version));
        QCOMPARE(dir, root.filePath("game"));
        QCOMPARE(version, QString("1.21.11"));
    }
    void logClassificationFollowsMinecraftsOwnLines() {
        const QString entry = "file/Nova Test - P";
        QCOMPARE(
            classifyLogLine("[10:00:00] [Render thread/INFO]: Setting user: Steve", entry).event,
            LogEvent::PlayerSet);
        QCOMPARE(classifyLogLine("[10:00:01] [Render thread/INFO]: Reloading ResourceManager: "
                                 "vanilla, file/Nova Test - P",
                                 entry)
                     .event,
                 LogEvent::PackActive);
        QCOMPARE(classifyLogLine(
                     "[10:00:01] [Render thread/INFO]: Reloading ResourceManager: vanilla", entry)
                     .event,
                 LogEvent::ResourceReload);
        QCOMPARE(classifyLogLine("[10:00:02] [Render thread/INFO]: Created: 1024x512x4 "
                                 "minecraft:textures/atlas/blocks.png-atlas",
                                 entry)
                     .event,
                 LogEvent::ResourcesLoaded);
        QCOMPARE(
            classifyLogLine(
                "[10:00:03] [Worker-Main-3/WARN]: Unable to load model: 'minecraft:item/thing'",
                entry)
                .event,
            LogEvent::PackWarning);
        QCOMPARE(classifyLogLine(
                     "[10:00:03] [Render thread/WARN]: Ignoring unrelated network jitter", entry)
                     .event,
                 LogEvent::None);
        QCOMPARE(classifyLogLine("[10:00:04] [Render thread/INFO]: Stopping!", entry).event,
                 LogEvent::Stopped);
        // Noise observed in real modded 1.21.11 logs is not attributed to the pack.
        QCOMPARE(classifyLogLine("[1] [Worker-Main-2/ERROR]: Invalid path in mod resource-pack "
                                 "vulkanmod: vulkanmod:shaders/a.json, ignoring",
                                 entry)
                     .event,
                 LogEvent::None);
        QCOMPARE(classifyLogLine("[1] [Worker-Main-42/ERROR]: Failed to verify signature on "
                                 "property Property[name=textures, value=x]",
                                 entry)
                     .event,
                 LogEvent::None);
        QCOMPARE(classifyLogLine("[1] [Server thread/ERROR]: Some mod failed to tick", entry).event,
                 LogEvent::None);
        QCOMPARE(classifyLogLine("[1] [main/FATAL]: Unreported exception thrown!", entry).event,
                 LogEvent::Error);
        QCOMPARE(
            classifyLogLine("[1] [Render thread/ERROR]: Failed to load file/Nova Test - P", entry)
                .event,
            LogEvent::PackWarning);
    }
    // Opt-in evidence: NOVA_PHASE3D_REAL_LOG=<latest.log> replays a real
    // Minecraft log through the monitor and prints counts only (no content).
    void realLogReplayIsQuiet() {
        const auto path = qEnvironmentVariable("NOVA_PHASE3D_REAL_LOG");
        if (path.isEmpty())
            QSKIP("set NOVA_PHASE3D_REAL_LOG to replay a real log");
        QTemporaryDir root;
        write(root.filePath("logs/latest.log"), read(path));
        GameLogMonitor monitor;
        monitor.watch(root.path(), "file/Nova Test - P");
        monitor.stop();
        const auto &s = monitor.state();
        qInfo().noquote() << QString("replay: reloads=%1 resourcesLoaded=%2 running=%3 findings=%4")
                                 .arg(s.reloads)
                                 .arg(s.resourcesLoaded)
                                 .arg(s.running)
                                 .arg(s.warnings.size());
        QVERIFY(s.reloads > 0);
        QVERIFY(s.warnings.size() <= 20);
        for (const auto &w : s.warnings)
            QVERIFY(!w.contains("Property[name=textures"));
    }
    void tokensAreRedacted() {
        const QString jwt = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.c2lnbmF0dXJlLXZhbHVl";
        const QStringList lines{"[x] [main/ERROR]: bad --accessToken " + jwt,
                                "[x] [main/ERROR]: accessToken=SECRET-TOKEN-VALUE pack",
                                "[x] [main/ERROR]: \"access_token\":\"SECRET-TOKEN-VALUE\" pack"};
        for (const auto &line : lines) {
            const auto red = redactLogLine(line);
            QVERIFY2(!red.contains("SECRET-TOKEN-VALUE") && !red.contains(jwt), qPrintable(red));
            QVERIFY(!classifyLogLine(line, "file/x").text.contains("SECRET-TOKEN-VALUE"));
        }
        QVERIFY(!redactLogLine(QString(400, 'A')).contains(QString(160, 'A')));
    }
    // Found in the Phase 3-D.1 live run: Fabric Loom's development log4j layout
    // is "[time] [thread/LEVEL] (Logger) text", not "...]: text".
    void fabricDevelopmentLogLayoutAndRrlsCompletion() {
        const QString entry = "file/Nova Test - P";
        QCOMPARE(classifyLogLine("[20:29:50] [Render thread/INFO] (Minecraft) Reloading "
                                 "ResourceManager: vanilla, fabric-api, rrls, file/Nova Test - P",
                                 entry)
                     .event,
                 LogEvent::PackActive);
        QCOMPARE(classifyLogLine("[20:29:50] [Render thread/INFO] (Minecraft) Created: 2048x2048x4 "
                                 "minecraft:textures/atlas/blocks.png-atlas",
                                 entry)
                     .event,
                 LogEvent::ResourcesLoaded);
        QCOMPARE(
            classifyLogLine("[20:29:56] [Render thread/INFO] (rrls) Overlay is removed!", entry)
                .event,
            LogEvent::ReloadFinished);
        QCOMPARE(
            classifyLogLine("[1] [Render thread/INFO] (other) Overlay is removed!", entry).event,
            LogEvent::None);
        QCOMPARE(
            classifyLogLine(
                "[1] [Render thread/INFO] (rrls) Reloading texture 'minecraft:textures/gui/x'!",
                entry)
                .event,
            LogEvent::None); // INFO chatter, not a warning
        QCOMPARE(
            classifyLogLine(
                "[1] [Worker-Main-3/WARN] (Minecraft) Unable to load model: 'minecraft:item/thing'",
                entry)
                .event,
            LogEvent::PackWarning);
        QCOMPARE(
            classifyLogLine("[21:00:05] [Render thread/INFO] (Minecraft) Stopping!", entry).event,
            LogEvent::Stopped);
        // Live run: 52 mod packs precede the test pack in a ~1500-character line.
        QStringList packs{"vanilla"};
        for (int i = 0; i < 52; ++i)
            packs << QString("fabric-module-number-%1-v1").arg(i);
        packs << entry;
        const auto longReload = classifyLogLine(
            "[22:09:01] [Render thread/INFO] (Minecraft) Reloading ResourceManager: " +
                packs.join(", "),
            entry);
        QCOMPARE(longReload.event, LogEvent::PackActive);
        QVERIFY(longReload.text.size() <= 600); // stored text stays bounded
        // A replaced log that is already longer than the old offset, with the
        // same creation time (file-system tunnelling), is still detected.
        QTemporaryDir root;
        const auto log = root.filePath("logs/latest.log");
        write(log, "[1] [Render thread/INFO] (Minecraft) Setting user: Old\n"
                   "[1] [Render thread/INFO] (Minecraft) Stopping!\n");
        GameLogMonitor monitor;
        monitor.watch(root.path(), entry);
        QVERIFY(!monitor.state().running);
        write(log, QByteArray("[2] [Render thread/INFO] (Minecraft) Setting user: New\n") +
                       QByteArray(200, '#') +
                       "\n[2] [Render thread/INFO] (Minecraft) Reloading ResourceManager: vanilla, "
                       "file/Nova Test - P\n"
                       "[2] [Render thread/INFO] (rrls) Overlay is removed!\n");
        monitor.poll();
        QVERIFY(monitor.state().running);
        QCOMPARE(monitor.state().player, QString("New"));
        QVERIFY(monitor.state().packActive);
        QVERIFY(monitor.state().reloadFinished);
        monitor.stop();
    }
    void monitorTracksSessionsRotationAndCrashes() {
        QTemporaryDir root;
        const auto game = root.path();
        GameLogMonitor monitor;
        monitor.watch(game, "file/Nova Test - P");
        QVERIFY(!monitor.state().running);
        write(game + "/logs/latest.log",
              "[1] [Render thread/INFO]: Setting user: Alex\n"
              "[2] [Render thread/INFO]: Reloading ResourceManager: vanilla, file/Nova Test - P\n"
              "[3] [Worker-Main-1/WARN]: Missing textures in model "
              "minecraft:item/thing#inventory\n[4] [Render th");
        monitor.poll();
        QVERIFY(monitor.state().running);
        QVERIFY(monitor.state().packActive);
        QCOMPARE(monitor.state().player, QString("Alex"));
        QCOMPARE(monitor.state().warnings.size(), 1);
        {
            QFile file(game + "/logs/latest.log");
            QVERIFY(file.open(QIODevice::Append));
            file.write("read/INFO]: Created: 1x1x0 minecraft:textures/atlas/blocks.png-atlas\n");
        }
        monitor.poll();
        QVERIFY(monitor.state().resourcesLoaded); // partial line joined correctly
        write(game + "/logs/latest.log", "[1] [Render thread/INFO]: Stopping!\n");
        monitor.poll();
        QVERIFY(!monitor.state().running);
        write(game + "/crash-reports/crash-1.txt", "boom");
        monitor.poll();
        QVERIFY(monitor.state().crashed);
        monitor.stop();
        QVERIFY(!monitor.active());
    }
    void auroraReleaseSelectionAndManifest() {
        const QByteArray releases = R"([
          {"tag_name":"native-v9.0.0","draft":true,"prerelease":false,"assets":[]},
          {"tag_name":"v1.5.1","draft":false,"prerelease":false,"assets":[]},
          {"tag_name":"native-v2.0.0","draft":false,"prerelease":false,"assets":[
            {"name":"native-release-assets.json","browser_download_url":"https://github.com/kalibsolomon-pixel/Aurora-Launcher/releases/download/native-v2.0.0/native-release-assets.json"}]}])";
        QString version, error;
        const auto url = pickAuroraRelease(releases, &version, &error);
        QVERIFY(url);
        QCOMPARE(version, QString("2.0.0"));
        // An asset URL outside the pinned repository is ignored.
        QVERIFY(!pickAuroraRelease(
            QByteArray(releases).replace("kalibsolomon-pixel/Aurora-Launcher", "evil/fork"),
            &version));
        const QByteArray manifest =
            R"({"schemaVersion":1,"product":"com.aurora.launcher.native-preview","version":"2.0.0",
          "artifacts":[{"name":"Aurora.Native.Preview_2.0.0_x64-setup.exe","sizeBytes":10222028,
          "sha256":"b72888bb1a57921e94b0fdbdec895c834c2f1522b4437f346827e1de3e28e09f"}]})";
        const auto plan = parseAuroraAssetManifest(manifest, *url, &error);
        QVERIFY2(plan, qPrintable(error));
        QCOMPARE(plan->url.toString(),
                 QString("https://github.com/kalibsolomon-pixel/Aurora-Launcher/releases/download/"
                         "native-v2.0.0/Aurora.Native.Preview_2.0.0_x64-setup.exe"));
        QCOMPARE(plan->expectedSize, 10222028);
        QVERIFY(!parseAuroraAssetManifest(QByteArray(manifest).replace("b72888bb", "XYZ"), *url,
                                          &error));
        QVERIFY(!parseAuroraAssetManifest(
            QByteArray(manifest).replace("x64-setup.exe", "x64-setup.exe/../a.exe"), *url, &error));
    }
    void downloadPolicyIsPinned() {
        const auto official = officialLauncherPolicy();
        QVERIFY(urlAllowed(officialLauncherPlan().url, official));
        QVERIFY(!urlAllowed(QUrl("http://launcher.mojang.com/x"), official));
        QVERIFY(!urlAllowed(QUrl("https://launcher.mojang.com.evil.example/x"), official));
        QVERIFY(!urlAllowed(QUrl("https://user@launcher.mojang.com/x"), official));
        QVERIFY(!urlAllowed(QUrl("http://127.0.0.1:1/x"), official));
        QVERIFY(urlAllowed(QUrl("https://release-assets.githubusercontent.com/x"), auroraPolicy()));
        QVERIFY(!urlAllowed(QUrl("https://gist.githubusercontent.com/x"), auroraPolicy()));
    }
    void fetchVerifiesRedirectsSizeAndHash() {
        MockServer server;
        DownloadPolicy policy{{}, true, 64};
        server.routes["/ok"] = {200, "installer-bytes", {}};
        server.routes["/hop"] = {302, {}, (server.base() + "/ok").toLatin1()};
        server.routes["/evil"] = {302, {}, "https://evil.example/payload"};
        server.routes["/big"] = {200, QByteArray(200, 'x'), {}};
        QTemporaryDir root;
        const auto out = root.filePath("dl/setup.exe");
        QCOMPARE(runFetch(QUrl(server.base() + "/hop"), policy, out), QString());
        QCOMPARE(read(out), QByteArray("installer-bytes"));
        QVERIFY(!QFileInfo::exists(out + ".part"));
        QVERIFY(runFetch(QUrl(server.base() + "/evil"), policy, root.filePath("e.exe"))
                    .contains("unexpected address"));
        QVERIFY(!QFileInfo::exists(root.filePath("e.exe")));
        QVERIFY(runFetch(QUrl(server.base() + "/big"), policy, root.filePath("b.exe"))
                    .contains("larger"));
        QVERIFY(!QFileInfo::exists(root.filePath("b.exe.part")));
        QVERIFY(runFetch(QUrl(server.base() + "/missing"), policy, root.filePath("m.exe"))
                    .contains("404"));
        InstallerPlan plan;
        plan.expectedSize = 15;
        plan.sha256 =
            QCryptographicHash::hash("installer-bytes", QCryptographicHash::Sha256).toHex();
        SignatureCheck signature;
        QCOMPARE(verifyInstaller(out, plan, &signature), QString());
        QCOMPARE(signature.status, SignatureCheck::Unsigned);
        plan.sha256 = QByteArray(64, '0');
        QVERIFY(verifyInstaller(out, plan, nullptr).contains("SHA-256"));
        plan.sha256.clear();
        plan.requiredSigner = "Microsoft";
        QVERIFY(verifyInstaller(out, plan, nullptr).contains("not signed by Microsoft"));
    }
    void authenticodeRecognisesASignedSystemBinary() {
#ifdef Q_OS_WIN
        const auto check = checkAuthenticode(
            QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot")) + "/explorer.exe");
        // explorer.exe is catalog-signed; embedded-signature checks may report it unsigned,
        // but it must never be reported as tampered.
        QVERIFY(check.status != SignatureCheck::Invalid);
#endif
    }
    void dialogOffersTargetsAndSyncsWithoutAccounts() {
        QTemporaryDir root;
        Environment e{root.filePath("local"), root.filePath("roaming"), root.filePath("pf"),
                      root.filePath("pf86")};
        write(e.localAppData + "/Aurora Launcher/aurora-launcher.exe", "MZ");
        write(e.localAppData + "/com.aurora.launcher/launcher/instances.json", auroraRegistry());
        write(e.programFilesX86 + "/Minecraft Launcher/MinecraftLauncher.exe", "MZ");
        const auto pack = makePack(root.filePath("Dialog Pack"));
        QString remembered = "aurora:0123456789abcdef0123456789abcdef";
        QStringList activity, unsaved;
        MinecraftTestDialog::Context context;
        context.projectRoot = [&] { return pack; };
        context.packName = [] { return QString("Dialog Pack"); };
        context.unsavedDocuments = [&] { return unsaved; };
        context.activity = [&](const QString &m) { activity << m; };
        context.rememberedTarget = &remembered;
        context.environment = e;
        context.interactive = false;
        MinecraftTestDialog dialog(context);
        QCOMPARE(dialog.targets().size(), 2); // official + the one ready Aurora instance
        QCOMPARE(dialog.versionForPack(), QString("1.21.11"));
        auto box = dialog.findChild<QComboBox *>("minecraftTestTarget");
        QCOMPARE(box->currentData().toString(), remembered);
        unsaved = {"assets/minecraft/textures/block/stone.png"};
        QVERIFY(!dialog.syncPack());
        unsaved.clear();
        const auto instance =
            e.localAppData + "/com.aurora.launcher/instances/0123456789abcdef0123456789abcdef";
        QVERIFY(!dialog.syncPack()); // Nova never creates an instance folder Aurora does not have
        QVERIFY(activity.last().contains("missing"));
        QDir().mkpath(instance);
        QVERIFY(dialog.syncPack());
        QVERIFY(QFileInfo::exists(instance + "/resourcepacks/Nova Test - Dialog Pack/pack.mcmeta"));
        QVERIFY(!QFileInfo::exists(instance +
                                   "/options.txt")); // Aurora instance settings are the user's
        box->setCurrentIndex(box->findData("official"));
        QCOMPARE(remembered, QString("official"));
        QVERIFY(dialog.syncPack());
        const auto isolated = novaTestGameDirectory(e.localAppData);
        QVERIFY(read(isolated + "/options.txt").contains("file/Nova Test - Dialog Pack"));
        // The registry file Aurora owns was only read.
        QCOMPARE(read(e.localAppData + "/com.aurora.launcher/launcher/instances.json"),
                 auroraRegistry());
        auto pack_status = dialog.findChild<QLabel *>("minecraftTestPackStatus");
        dialog.show();
        QVERIFY(pack_status->text().contains("Minecraft has the export"));
        QVERIFY(dialog.monitor()->active());
        dialog.hide();
        QVERIFY(!dialog.monitor()->active()); // no polling while hidden and idle
    }
};

QTEST_MAIN(MinecraftTestingTest)
#include "minecraft_testing_test.moc"
