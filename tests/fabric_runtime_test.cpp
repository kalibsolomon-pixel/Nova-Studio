// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 3-D.1: Nova's Fabric development runtime. Everything runs offline:
// downloads come from a local HTTP server, Gradle is a scripted fake process,
// JDKs are synthetic archives. The real pinned profile is checked separately.
#include "fabric_runtime.h"
#include "fabric_testing.h"
#include "packsync.h"
#include "settings.h"
#include <QBuffer>
#include <QComboBox>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <thread>
#include <QtCore/private/qzipwriter_p.h>
#include <QtTest>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <afunix.h>
#endif

using namespace nova;
using namespace nova::testing;

namespace nova::testing {
class FabricRuntimeTestAccess {
  public:
    static void watch(FabricRuntime &runtime, const ProcessIdentity &identity) { runtime.watch(identity); }
};
} // namespace nova::testing

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
QString hex(const QByteArray &bytes, QCryptographicHash::Algorithm algorithm) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, algorithm).toHex());
}
QByteArray zip(const QList<std::pair<QString, QByteArray>> &entries) {
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QZipWriter writer(&buffer);
    for (const auto &[name, bytes] : entries)
        writer.addFile(name, bytes);
    writer.close();
    return buffer.data();
}
QByteArray modJar(const QString &id, const QList<std::pair<QString, QByteArray>> &nested = {}) {
    QJsonArray jars;
    QList<std::pair<QString, QByteArray>> entries;
    for (const auto &[name, bytes] : nested) {
        jars.append(QJsonObject{{"file", "META-INF/jars/" + name}});
        entries.append({"META-INF/jars/" + name, bytes});
    }
    QJsonObject metadata{{"schemaVersion", 1}, {"id", id}, {"version", "1.0"}};
    if (!jars.isEmpty())
        metadata.insert("jars", jars);
    entries.prepend({"fabric.mod.json", QJsonDocument(metadata).toJson()});
    return zip(entries);
}
QByteArray jdkZip(const QString &version) {
    const QString top = "jdk-" + version + "/";
    return zip({{top + "release",
                 QString("JAVA_VERSION=\"%1\"\nIMPLEMENTOR=\"Test\"\n").arg(version).toUtf8()},
                {top + "bin/java.exe", "MZ"},
                {top + "bin/javac.exe", "MZ"},
                {top + "lib/modules", QByteArray(1024, 'x')}});
}
void fakeJdk(const QString &home, const QString &version, bool javac = true) {
    write(home + "/release", QString("JAVA_VERSION=\"%1\"\n").arg(version).toUtf8());
    write(home + "/bin/java.exe", "MZ");
    if (javac)
        write(home + "/bin/javac.exe", "MZ");
}

class MockServer : public QTcpServer {
  public:
    struct Reply {
        int status = 200;
        QByteArray body;
    };
    QMap<QString, Reply> routes;
    QStringList requests;
    MockServer() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto socket = nextPendingConnection())
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const auto path = QString::fromLatin1(socket->readAll().split(' ').value(1));
                    requests << path;
                    const auto reply = routes.value(path, {404, "missing"});
                    socket->write("HTTP/1.1 " + QByteArray::number(reply.status) +
                                  " X\r\nContent-Length: " + QByteArray::number(reply.body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + reply.body);
                    socket->disconnectFromHost();
                });
        });
        listen(QHostAddress::LocalHost);
    }
    QString url(const QString &path) const {
        return QString("http://127.0.0.1:%1%2").arg(serverPort()).arg(path);
    }
};

PinnedArtifact served(MockServer &server, const QString &id, const QString &file,
                      const QByteArray &bytes, const QString &algorithm) {
    server.routes["/" + file] = {200, bytes};
    return {
        id,
        id,
        "1.0",
        file,
        server.url("/" + file),
        algorithm,
        hex(bytes, algorithm == "sha512" ? QCryptographicHash::Sha512 : QCryptographicHash::Sha256),
        bytes.size(),
        "test",
        "test"};
}
} // namespace

class FabricRuntimeTest : public QObject {
    Q_OBJECT
    QTemporaryDir temp;
    MockServer server;
    FabricProfile profile;
    QString scripts, counter;
    int caseNumber = 0;

    FabricRuntime::Options options(const QString &base) {
        FabricRuntime::Options o;
        o.base = base;
        o.environment = Environment{temp.filePath("local"), temp.filePath("roaming"),
                                    temp.filePath("pf-empty"), temp.filePath("pf86-empty")};
        o.policy = DownloadPolicy{{}, true, 64LL * 1024 * 1024};
        o.profile = profile;
        o.processProgram = QCoreApplication::applicationDirPath() + "/nova-fake-gradle.exe";
        o.stopGraceMs = 3000;
        return o;
    }
    void script(const char *variable, const QString &body) {
        const auto path = scripts + QString("/%1-%2.txt").arg(variable).arg(++caseNumber);
        write(path, body.toUtf8());
        qputenv(variable, path.toUtf8());
    }
    int invocations(const QString &kind) {
        int count = 0;
        for (const auto &line : QString::fromUtf8(read(counter)).split('\n'))
            count += line.startsWith(kind + ' ');
        return count;
    }
    bool prepareAndWait(FabricRuntime &runtime, bool repair = false) {
        QSignalSpy finished(&runtime, &FabricRuntime::preparationFinished);
        runtime.prepare(repair);
        if (finished.isEmpty() && !finished.wait(30000))
            return false;
        return finished.first().first().toBool();
    }
    QString preparedBase(const QString &name) {
        const auto base = temp.filePath(name);
        script("NOVA_FAKE_GRADLE_PREPARE",
               "print > Task :downloadAssets\nprint BUILD SUCCESSFUL in 1s\n");
        FabricRuntime runtime(options(base));
        if (!prepareAndWait(runtime))
            qWarning() << "prepare failed:" << runtime.error();
        return base;
    }
    QString runScript(const QString &tail) {
        return "print > Task :runClient\n"
               "print [10:00:00] [main/INFO] (FabricLoader/GameProvider) Loading Minecraft 1.21.11 "
               "with Fabric Loader 0.20.1\n"
               "print [10:00:00] [main/INFO] (FabricLoader) Loading 3 mods:\n"
               "print \t- fabric-api 0.141.6+1.21.11\n"
               "print \t- rrls 5.1.15+mc.1.21.11\n"
               "print [10:00:01] [Render thread/INFO] (Minecraft) Setting user: Player1\n"
               "print [10:00:02] [Render thread/INFO] (Minecraft) Sound engine started\n" +
               tail;
    }

  private slots:
    void initTestCase() {
        qunsetenv("JAVA_HOME");
        scripts = temp.filePath("scripts");
        counter = temp.filePath("counter.txt");
        qputenv("NOVA_FAKE_GRADLE_COUNTER", counter.toUtf8());
        QVERIFY(
            QFileInfo::exists(QCoreApplication::applicationDirPath() + "/nova-fake-gradle.exe"));
        profile = *fabricProfile("1.21.11");
        profile.key = "test-profile";
        const auto nested = modJar("nested-module");
        profile.mods = {served(server, "fabric-api", "api.jar",
                               modJar("fabric-api", {{"nested-module.jar", nested}}), "sha512"),
                        served(server, "rrls", "rrls.jar", modJar("rrls"), "sha512")};
        profile.gameJdk = served(server, "temurin-21", "jdk21.zip", jdkZip("21.0.13"), "sha256");
        profile.gradleJdk = served(server, "temurin-25", "jdk25.zip", jdkZip("25.0.5"), "sha256");
    }

    void realProfileIsPinnedAndComplete() {
        const auto p = fabricProfile("1.21.11");
        QVERIFY(p);
        QCOMPARE(p->loaderVersion, QString("0.20.1"));
        QCOMPARE(p->loomPluginId, QString("net.fabricmc.fabric-loom-remap"));
        QCOMPARE(p->gameJavaMajor, 21);
        QCOMPARE(p->gradleJavaMinimum, 25);
        QStringList ids;
        for (const auto &mod : p->mods) {
            ids << mod.id;
            QVERIFY(mod.url.startsWith("https://cdn.modrinth.com/"));
            QCOMPARE(mod.algorithm, QString("sha512"));
            QCOMPARE(mod.hash.size(), 128);
            QVERIFY(mod.size > 0);
            QVERIFY(urlAllowed(QUrl(mod.url), runtimePolicy()));
        }
        QCOMPARE(ids, QStringList({"fabric-api", "forgeconfigapiport", "rrls"}));
        for (const auto &jdk : {p->gameJdk, p->gradleJdk}) {
            QVERIFY(jdk.url.startsWith("https://github.com/adoptium/"));
            QCOMPARE(jdk.hash.size(), 64);
        }
        QVERIFY(!fabricProfile("1.8.9"));
        QFile wrapper(p->wrapperResource);
        QVERIFY(wrapper.open(QIODevice::ReadOnly));
        QCOMPARE(hex(wrapper.readAll(), QCryptographicHash::Sha256), p->gradleWrapperJarSha256);
    }
    void javaInspectionAndSelection() {
        const auto root = temp.filePath("javas");
        fakeJdk(root + "/jdk21", "21.0.13");
        fakeJdk(root + "/jdk25", "25.0.4");
        fakeJdk(root + "/jre21", "21.0.7", false);
        fakeJdk(root + "/old", "1.8.0_402");
        write(root + "/broken/release", "JAVA_VERSION=\"x\"\n");
        QCOMPARE(javaMajor("1.8.0_402"), 8);
        QCOMPARE(javaMajor("21.0.13"), 21);
        QVERIFY(!inspectJavaHome(root + "/broken"));
        QVERIFY(!inspectJavaHome(root + "/jre21")->jdk);
        const auto homes = discoverJavaHomes(Environment{}, root);
        QCOMPARE(homes.size(), 4);
        QCOMPARE(chooseJava(homes, 21, true)->path, root + "/jdk21"); // never the JRE, never 25
        QCOMPARE(chooseJava(homes, 25, false)->major, 25);
        QVERIFY(!chooseJava(homes, 17, true));
    }
    void classifiesGradleAndMinecraftOutput() {
        QCOMPARE(classifyRunOutput("> Task :runClient", false).kind, RunLine::RunTask);
        QCOMPARE(classifyRunOutput("> Task :downloadAssets", false).kind, RunLine::GradleWork);
        auto loader = classifyRunOutput("[1] [main/INFO] (FabricLoader/GameProvider) Loading "
                                        "Minecraft 1.21.11 with Fabric Loader 0.20.1",
                                        false);
        QCOMPARE(loader.kind, RunLine::LoaderStart);
        QCOMPARE(loader.value, QString("0.20.1"));
        QCOMPARE(classifyRunOutput("[1] [main/INFO] (FabricLoader) Loading 52 mods:", false).kind,
                 RunLine::ModListStart);
        const auto entry = classifyRunOutput("\t- rrls 5.1.15+mc.1.21.11", true);
        QCOMPARE(entry.kind, RunLine::ModEntry);
        QCOMPARE(entry.key, QString("rrls"));
        QCOMPARE(classifyRunOutput("\t- rrls 5.1.15", false).kind,
                 RunLine::None); // only inside the list
        QCOMPARE(
            classifyRunOutput("[1] [Render thread/INFO] (Minecraft) Sound engine started", false)
                .kind,
            RunLine::MenuReady);
        for (const auto &line :
             {"[1] [main/ERROR] (FabricLoader) Failed to remap mods!", "BUILD FAILED in 4s",
              "      > No cached version of net.fabricmc:x:1 available for offline mode.",
              "---- Minecraft Crash Report ----",
              "> Dependency requires at least JVM runtime version 25. This build uses a Java 21 "
              "JVM."})
            QCOMPARE(classifyRunOutput(line, false).kind, RunLine::Failure);
        QVERIFY(classifyRunOutput("No cached version of a", false).value.contains("Repair"));
    }
    void workspaceIsGeneratedVerifiedAndGuarded() {
        const auto base = temp.filePath("ws-base");
        auto real = *fabricProfile("1.21.11");
        const auto layout = RuntimeLayout::make(base, real);
        const auto mods = temp.filePath("ws-mods");
        write(mods + "/a.jar",
              modJar("a", {{"lib-core.jar", zip({{"x.class", "x"}})}, {"sub.jar", modJar("sub")}}));
        write(mods + "/rrls.jar", modJar("rrls"));
        WorkspaceInputs inputs{QString::fromUtf8("C:/Users/J\xc3\xb6rg/jdk 21"),
                               {mods + "/a.jar", mods + "/rrls.jar"}};
        const auto result = writeWorkspace(real, layout, inputs);
        QVERIFY2(result.ok, qPrintable(result.error));
        const auto build = QString::fromUtf8(read(layout.workspace + "/build.gradle"));
        QVERIFY(build.contains("id 'net.fabricmc.fabric-loom-remap' version '1.19.3'"));
        QVERIFY(build.contains("languageVersion = JavaLanguageVersion.of(21)"));
        QVERIFY(build.contains("runDir = '../../instances/1.21.11'"));
        QVERIFY(build.contains("'libs/sub.jar'")); // nested mod listed explicitly
        QVERIFY(build.contains("runtimeOnly files(\n\t\t'libs/lib-core.jar')")); // plain library
        QVERIFY(build.contains("tasks.register('novaPrepareRuntime')"));
        QVERIFY(read(layout.workspace + "/gradle.properties")
                    .contains("org.gradle.java.installations.paths=C:/Users/J\\u00f6rg/jdk 21"));
        QVERIFY(read(layout.workspace + "/gradle.properties").contains("org.gradle.daemon=false"));
        QVERIFY(read(layout.workspace + "/gradle/wrapper/gradle-wrapper.properties")
                    .contains("distributionSha256Sum=" + real.gradleDistributionSha256.toLatin1()));
        QVERIFY(verifyWorkspace(real, layout).ok);
        QCOMPARE(verifyWorkspace(real, layout).manifestHash, result.manifestHash);
        auto tamper = [&](auto change, const QString &expected,
                          std::function<void()> cleanup = {}) {
            change();
            const auto check = verifyWorkspace(real, layout);
            QVERIFY2(!check.ok && check.error.contains(expected), qPrintable(check.error));
            if (cleanup)
                cleanup(); // injected files are removed by the runtime's full repair
            QVERIFY(writeWorkspace(real, layout, inputs).ok); // repair
            QVERIFY(verifyWorkspace(real, layout).ok);
        };
        tamper([&] { write(layout.workspace + "/build.gradle", "apply from: 'evil.gradle'"); },
               "changed outside Nova");
        tamper([&] { QDir().mkpath(layout.workspace + "/buildSrc"); }, "buildSrc",
               [&] { QDir(layout.workspace + "/buildSrc").removeRecursively(); });
        tamper([&] { write(layout.workspace + "/libs/extra.jar", "x"); }, "extra.jar");
        tamper([&] { write(layout.workspace + "/evil.gradle", "x"); }, "evil.gradle",
               [&] { QFile::remove(layout.workspace + "/evil.gradle"); });
        write(layout.gradleHome + "/init.d/inject.gradle", "x");
        QVERIFY(verifyWorkspace(real, layout).error.contains("init.d"));
        QDir(layout.gradleHome + "/init.d").removeRecursively();
        QVERIFY(verifyWorkspace(real, layout).ok);
    }
    void nestedJarsAreBoundedAndSafe() {
        const auto libs = temp.filePath("expand-libs");
        QDir().mkpath(libs);
        const auto evil = temp.filePath("evil.jar");
        write(
            evil,
            zip({{"fabric.mod.json",
                  R"({"schemaVersion":1,"id":"e","jars":[{"file":"META-INF/jars/../../x.jar"}]})"}}));
        QVERIFY(expandModJars({evil}, libs).error.contains("outside META-INF/jars"));
        const auto missing = temp.filePath("missing.jar");
        write(
            missing,
            zip({{"fabric.mod.json",
                  R"({"schemaVersion":1,"id":"m","jars":[{"file":"META-INF/jars/gone.jar"}]})"}}));
        QVERIFY(expandModJars({missing}, libs).error.contains("missing nested jar"));
        auto deep = modJar("d0");
        for (int i = 1; i < 7; ++i)
            deep = modJar(QString("d%1").arg(i), {{QString("d%1.jar").arg(i - 1), deep}});
        write(temp.filePath("deep.jar"), deep);
        QVERIFY(expandModJars({temp.filePath("deep.jar")}, libs).error.contains("too deeply"));
    }
    void firstRunDownloadsVerifiesAndPrepares() {
        const auto base = temp.filePath("first");
        server.requests.clear();
        script("NOVA_FAKE_GRADLE_PREPARE",
               "print Downloading https://services.gradle.org/distributions/gradle-9.8.2-bin.zip\n"
               "print > Task :downloadAssets\nprint BUILD SUCCESSFUL in 1s\n");
        const int before = invocations("prepare");
        FabricRuntime runtime(options(base));
        QCOMPARE(runtime.state(), FabricRuntime::State::NotConfigured);
        QList<FabricRuntime::State> seen;
        connect(&runtime, &FabricRuntime::changed, this, [&] {
            if (seen.isEmpty() || seen.last() != runtime.state())
                seen << runtime.state();
        });
        QVERIFY2(prepareAndWait(runtime), qPrintable(runtime.error()));
        QCOMPARE(runtime.state(), FabricRuntime::State::Ready);
        QVERIFY(runtime.prepared());
        QVERIFY(seen.contains(FabricRuntime::State::Checking));
        QVERIFY(seen.contains(FabricRuntime::State::Downloading));
        QVERIFY(seen.contains(FabricRuntime::State::Preparing));
        QVERIFY(seen.contains(FabricRuntime::State::GeneratingRunConfig));
        QCOMPARE(server.requests.size(), 4);
        QCOMPARE(invocations("prepare"), before + 1);
        QCOMPARE(inspectJavaHome(base + "/java/temurin-21")->major, 21);
        QCOMPARE(inspectJavaHome(base + "/java/temurin-25")->major, 25);
        QVERIFY(QFileInfo::exists(runtime.layout().workspace + "/libs/nested-module.jar"));
        QVERIFY(read(runtime.prepareLogPath()).contains("BUILD SUCCESSFUL"));
        // Cached: nothing is downloaded and Gradle does not run again.
        FabricRuntime again(options(base));
        QCOMPARE(again.state(), FabricRuntime::State::Ready);
        QVERIFY(prepareAndWait(again));
        QCOMPARE(server.requests.size(), 4);
        QCOMPARE(invocations("prepare"), before + 1);
        QVERIFY(again.lastPrepareMs() >= 0);
    }
    void checksumMismatchAndDownloadFailuresAreReported() {
        auto bad = profile;
        bad.key = "bad-profile";
        bad.mods = {profile.mods.first()};
        bad.mods.first().hash = QString(128, '0');
        auto o = options(temp.filePath("bad"));
        o.profile = bad;
        fakeJdk(o.base + "/java/temurin-21", "21.0.13"); // skip the JDK downloads
        fakeJdk(o.base + "/java/temurin-25", "25.0.5");
        FabricRuntime runtime(o);
        QVERIFY(!prepareAndWait(runtime));
        QCOMPARE(runtime.state(), FabricRuntime::State::Failed);
        QVERIFY2(runtime.error().contains("checksum"), qPrintable(runtime.error()));
        QVERIFY(!QFileInfo::exists(o.base + "/downloads/api.jar"));
        bad.mods.first() = profile.mods.first();
        bad.mods.first().url = server.url("/missing.jar");
        o.profile = bad;
        FabricRuntime missing(o);
        QVERIFY(!prepareAndWait(missing));
        QVERIFY2(missing.error().contains("404"), qPrintable(missing.error()));
    }
    void gradleFailureAndCancellation() {
        auto o = options(temp.filePath("gradle-fail"));
        script("NOVA_FAKE_GRADLE_PREPARE", "print * What went wrong:\nprint Could not resolve all "
                                           "files for configuration\nprint * Try:\nexit 1\n");
        FabricRuntime failing(o);
        QVERIFY(!prepareAndWait(failing));
        QVERIFY2(failing.error().contains("Could not resolve"), qPrintable(failing.error()));
        QVERIFY(!failing.prepared());
        script("NOVA_FAKE_GRADLE_PREPARE", "spawn\nsleep 60000\n");
        FabricRuntime slow(o);
        QSignalSpy finished(&slow, &FabricRuntime::preparationFinished);
        slow.prepare();
        QTRY_COMPARE_WITH_TIMEOUT(slow.state(), FabricRuntime::State::GeneratingRunConfig, 30000);
        QTest::qWait(500);
        QElapsedTimer timer;
        timer.start();
        slow.cancel();
        QVERIFY(timer.elapsed() < 10000);
        QCOMPARE(slow.state(), FabricRuntime::State::NotConfigured);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.first().first().toBool());
    }
    void repairRestoresATamperedWorkspace() {
        const auto base = preparedBase("repair");
        FabricRuntime runtime(options(base));
        QVERIFY(runtime.prepared());
        write(runtime.layout().workspace + "/build.gradle", "// changed");
        QString error;
        QVERIFY(!runtime.launch({}, &error));
        QVERIFY2(error.contains("Repair"), qPrintable(error));
        const int before = invocations("prepare");
        QVERIFY(prepareAndWait(runtime, true));
        QCOMPARE(invocations("prepare"), before + 1); // repair re-runs Gradle preparation
        QVERIFY(verifyWorkspace(runtime.profile(), runtime.layout()).ok);
    }
    void preparationRefusesContaminationBeforeSpawn() {
        const auto base = preparedBase("preflight-contamination");
        FabricRuntime runtime(options(base));
        QVERIFY(runtime.prepared());
        const auto layout = runtime.layout();
        const int before = invocations("prepare");
        const auto injection = layout.gradleHome + "/init.d/marker.gradle";
        write(injection, "println 'NOVA_PREFLIGHT_INJECTION_MARKER'");
        QVERIFY(!prepareAndWait(runtime));
        QVERIFY(runtime.error().contains("init.d"));
        QVERIFY(!prepareAndWait(runtime, true));
        QString error;
        QVERIFY(!runtime.launch({}, &error));
        QCOMPARE(invocations("prepare"), before);
        QVERIFY(QFile::remove(injection));
        QVERIFY(QDir().rmdir(layout.gradleHome + "/init.d"));
        write(layout.gradleHome + "/gradle.properties", "org.gradle.jvmargs=-version");
        QVERIFY(!prepareAndWait(runtime));
        QCOMPARE(invocations("prepare"), before);
        QVERIFY(QFile::remove(layout.gradleHome + "/gradle.properties"));
        QVERIFY(prepareAndWait(runtime, true));
        QCOMPARE(invocations("prepare"), before + 1);
        QVERIFY(prepareAndWait(runtime));
        QCOMPARE(invocations("prepare"), before + 1);
    }
    void preflightRejectsChangesImmediatelyBeforeSpawn() {
        const auto base = preparedBase("preflight-last-moment");
        FabricRuntime runtime(options(base));
        const int before = invocations("prepare");
        for (const QString path : {"build.gradle", "gradle/wrapper/gradle-wrapper.jar",
                                   "gradle/gradle-daemon-jvm.properties"}) {
            const auto injection = connect(&runtime, &FabricRuntime::changed, this, [&] {
                if (runtime.state() == FabricRuntime::State::GeneratingRunConfig)
                    write(runtime.layout().workspace + '/' + path, "unexpected");
            });
            QVERIFY(!prepareAndWait(runtime, true));
            QVERIFY(runtime.error().contains(path));
            disconnect(injection);
            QCOMPARE(invocations("prepare"), before);
        }
        QVERIFY(prepareAndWait(runtime, true));
        QCOMPARE(invocations("prepare"), before + 1);
    }
    void inheritedOverridesNeverReachGradle() {
        const auto base = preparedBase("preflight-environment");
        const QList<QByteArray> names{"JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS",
                                     "JAVA_OPTS", "GRADLE_OPTS", "GRADLE_USER_HOME",
                                     "GRADLE_RO_DEP_CACHE", "ORG_GRADLE_PROJECT_inject"};
        QHash<QByteArray, QByteArray> saved;
        const auto restore = qScopeGuard([&] {
            for (auto it = saved.cbegin(); it != saved.cend(); ++it)
                if (it.value().isNull())
                    qunsetenv(it.key());
                else
                    qputenv(it.key(), it.value());
        });
        QString commands;
        for (const auto &name : names) {
            saved.insert(name, qgetenv(name));
            qputenv(name, "untrusted-marker");
            commands += "printenv " + QString::fromLatin1(name) + '\n';
        }
        script("NOVA_FAKE_GRADLE_PREPARE", commands + "print BUILD SUCCESSFUL");
        FabricRuntime runtime(options(base));
        QVERIFY2(prepareAndWait(runtime, true), qPrintable(runtime.error()));
        const auto log = read(runtime.prepareLogPath());
        QVERIFY(log.contains("GRADLE_USER_HOME="));
        QVERIFY(!log.contains("untrusted-marker"));
    }
    void socketDirectoryIsShortCheckedAndCleaned() {
#ifdef Q_OS_WIN
        QTemporaryDir own;
        QVERIFY(own.isValid());
        const auto local = QDir::toNativeSeparators(own.filePath("l"));
        int removed = -1;
        const auto sockets = prepareSocketDirectory(local, &removed);
        QCOMPARE(sockets, QDir::toNativeSeparators(own.filePath("l/Temp/NovaSockets")));
        QVERIFY(QFileInfo(QDir::fromNativeSeparators(sockets)).isDir());
        QCOMPARE(removed, 0);
        QCOMPARE(prepareSocketDirectory(local, &removed), sockets); // repeated startup
        // A bound and closed AF_UNIX socket leaves the reparse-point file a killed JVM would.
        WSADATA wsa;
        QCOMPARE(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
        const auto bindSocket = [](const QString &path) {
            const auto handle = socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            const auto bytes = QDir::toNativeSeparators(path).toLocal8Bit();
            memcpy(address.sun_path, bytes.constData(), std::min<size_t>(bytes.size(), sizeof address.sun_path - 1));
            const bool bound = handle != INVALID_SOCKET &&
                               bind(handle, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0;
            closesocket(handle);
            return bound;
        };
        const auto dir = QDir::fromNativeSeparators(sockets);
        QVERIFY(bindSocket(dir + "/socket_1111111111"));
        write(dir + "/socket_2222222222", "stale");
        write(dir + "/unrelated.txt", "kept");
        QVERIFY(QDir().mkpath(dir + "/socket_dir"));
        // Recent sockets may belong to a JVM that is binding right now.
        QCOMPARE(prepareSocketDirectory(local, &removed), sockets);
        QCOMPARE(removed, 0);
        QVERIFY(QDir(dir).entryList({"socket_1111111111"}, QDir::Files | QDir::System).size() == 1);
        QCOMPARE(prepareSocketDirectory(local, &removed, 0), sockets);
        QCOMPARE(removed, 2);
        QVERIFY(QDir(dir).entryList({"socket_1*", "socket_2*"}, QDir::Files | QDir::System | QDir::Hidden).isEmpty());
        QVERIFY(QFileInfo::exists(dir + "/unrelated.txt"));
        QVERIFY(QFileInfo(dir + "/socket_dir").isDir());
        WSACleanup();
        // A junction anywhere on the path is refused, as are relative and long paths.
        const auto outside = own.filePath("outside");
        QVERIFY(QDir().mkpath(outside));
        QVERIFY(QDir().mkpath(own.filePath("j")));
        QCOMPARE(QProcess::execute("cmd.exe", {"/c", "mklink", "/J",
                     QDir::toNativeSeparators(own.filePath("j/Temp")), QDir::toNativeSeparators(outside)}), 0);
        QVERIFY(prepareSocketDirectory(QDir::toNativeSeparators(own.filePath("j"))).isEmpty());
        QVERIFY(!QFileInfo::exists(outside + "/NovaSockets"));
        QVERIFY(prepareSocketDirectory("relative/path").isEmpty());
        QVERIFY(prepareSocketDirectory({}).isEmpty());
        QVERIFY(prepareSocketDirectory(own.filePath(QString(64, 'x'))).isEmpty());
#endif
    }
    // The 88-byte limit is exact and counted in UTF-8 bytes. A refusal says why, with
    // the system error when Windows refuses the folder, and creates nothing.
    void socketDirectoryBoundaryUnicodeAndErrors() {
#ifdef Q_OS_WIN
        QTemporaryDir own;
        QVERIFY(own.isValid());
        // A base whose "<base>/Temp/NovaSockets" is exactly `bytes` UTF-8 bytes long.
        const auto sized = [&](const QString &name, int bytes) {
            const auto prefix = own.path() + '/' + name;
            const auto pad = bytes - int((prefix + "/Temp/NovaSockets").toUtf8().size());
            return pad >= 0 ? prefix + QString(pad, 'x') : QString();
        };
        const auto fits = sized("fit", socketDirectoryMaxBytes);
        QVERIFY2(!fits.isEmpty(), "TEMP is too long for this test; use the short-TEMP driver");
        QString error;
        const auto sockets = prepareSocketDirectory(QDir::toNativeSeparators(fits), nullptr, 600, &error);
        QVERIFY2(!sockets.isEmpty(), qPrintable(error));
        QCOMPARE(int(QDir::fromNativeSeparators(sockets).toUtf8().size()), socketDirectoryMaxBytes);
        // The longest name Java generates still binds there (106 bytes).
        WSADATA wsa;
        QCOMPARE(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
        const auto handle = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto longest = (sockets + "\\socket_2147483647").toUtf8();
        QCOMPARE(longest.size(), 106);
        memcpy(address.sun_path, longest.constData(), size_t(longest.size()));
        QCOMPARE(bind(handle, reinterpret_cast<sockaddr *>(&address), sizeof address), 0);
        closesocket(handle);
        WSACleanup();
        QVERIFY(QFile::remove(QString::fromUtf8(longest)));

        const auto over = sized("over", socketDirectoryMaxBytes + 1);
        QVERIFY(prepareSocketDirectory(QDir::toNativeSeparators(over), nullptr, 600, &error).isEmpty());
        QVERIFY2(error.contains("is 89 bytes long; the limit is 88 bytes"), qPrintable(error));
        QVERIFY(!QFileInfo::exists(over));
        // "é" is two bytes: 89 bytes are refused even though they are only 87 characters.
        const auto wideOver = sized("é d é", socketDirectoryMaxBytes + 1);
        QCOMPARE(int((wideOver + "/Temp/NovaSockets").size()), socketDirectoryMaxBytes - 1);
        QVERIFY(prepareSocketDirectory(QDir::toNativeSeparators(wideOver), nullptr, 600, &error).isEmpty());
        QVERIFY2(error.contains("89 bytes"), qPrintable(error));
        const auto wide = sized("é d é", socketDirectoryMaxBytes);
        QVERIFY(!wide.isEmpty());
        QCOMPARE(prepareSocketDirectory(QDir::toNativeSeparators(wide), nullptr, 600, &error),
                 QDir::toNativeSeparators(wide + "/Temp/NovaSockets"));
        QVERIFY(QFileInfo(wide + "/Temp/NovaSockets").isDir());

        // A file in the folder's place: refused with Windows' reason, file kept.
        const auto blocked = own.filePath("blocked");
        write(blocked + "/Temp/NovaSockets", "not a folder");
        QVERIFY(prepareSocketDirectory(QDir::toNativeSeparators(blocked), nullptr, 600, &error).isEmpty());
        QVERIFY2(error.contains("Windows error 183"), qPrintable(error));
        QCOMPARE(read(blocked + "/Temp/NovaSockets"), QByteArray("not a folder"));
        // Folder creation denied on this test-owned Temp folder only.
        const auto denied = own.filePath("denied");
        QVERIFY(QDir().mkpath(denied + "/Temp"));
        const auto user = qEnvironmentVariable("USERNAME");
        const auto icacls = [](const QStringList &args) {
            QProcess p;
            p.start("icacls", args);
            return p.waitForFinished(10000) && p.exitCode() == 0;
        };
        QVERIFY(icacls({QDir::toNativeSeparators(denied + "/Temp"), "/deny", user + ":(AD)"}));
        const auto refused = prepareSocketDirectory(QDir::toNativeSeparators(denied), nullptr, 600, &error);
        QVERIFY(icacls({QDir::toNativeSeparators(denied + "/Temp"), "/remove:d", user}));
        QVERIFY(refused.isEmpty());
        QVERIFY2(error.contains("Windows error 5"), qPrintable(error));
        QVERIFY(!QFileInfo::exists(denied + "/Temp/NovaSockets"));
#endif
    }
    // Only a folder that is too long moves to the user's own profile. Links and
    // creation failures stay refusals, and the deep location is left untouched.
    void socketDirectoryFallsBackOnlyWhenTooLong() {
#ifdef Q_OS_WIN
        QTemporaryDir own;
        QVERIFY(own.isValid());
        const auto deep = own.filePath(QString(80, 'd'));
        const auto profile = QDir::toNativeSeparators(own.filePath("p"));
        QString error;
        QCOMPARE(chooseSocketDirectory(QDir::toNativeSeparators(deep), profile, &error),
                 QDir::toNativeSeparators(own.filePath("p/Temp/NovaSockets")));
        QVERIFY(!QFileInfo::exists(deep));
        // A configured location that fits is kept.
        QCOMPARE(chooseSocketDirectory(QDir::toNativeSeparators(own.filePath("s")), profile),
                 QDir::toNativeSeparators(own.filePath("s/Temp/NovaSockets")));
        // A junction is refused, not bypassed through the fallback.
        const auto outside = own.filePath("outside");
        QVERIFY(QDir().mkpath(outside));
        QVERIFY(QDir().mkpath(own.filePath("j")));
        QCOMPARE(QProcess::execute("cmd.exe", {"/c", "mklink", "/J",
                     QDir::toNativeSeparators(own.filePath("j/Temp")), QDir::toNativeSeparators(outside)}), 0);
        const auto other = QDir::toNativeSeparators(own.filePath("other"));
        QVERIFY(chooseSocketDirectory(QDir::toNativeSeparators(own.filePath("j")), other, &error).isEmpty());
        QVERIFY2(error.contains("link, junction"), qPrintable(error));
        QVERIFY(!QFileInfo::exists(own.filePath("other")));
        QVERIFY(!QFileInfo::exists(outside + "/NovaSockets"));
        // Both too long, or no fallback: the error names every refused folder.
        QVERIFY(chooseSocketDirectory(QDir::toNativeSeparators(deep), QDir::toNativeSeparators(deep + "2"), &error).isEmpty());
        QCOMPARE(error.count("bytes long"), 2);
        QVERIFY(chooseSocketDirectory(QDir::toNativeSeparators(deep), {}, &error).isEmpty());
        QCOMPARE(error.count("bytes long"), 1);
        // Concurrent launches agree on the fallback.
        const auto race = QDir::toNativeSeparators(own.filePath("race"));
        std::vector<QString> seen(16);
        std::vector<std::thread> threads;
        for (int i = 0; i < 16; ++i)
            threads.emplace_back([&, i] { seen[size_t(i)] = chooseSocketDirectory(QDir::toNativeSeparators(deep), race); });
        for (auto &t : threads)
            t.join();
        for (const auto &s : seen)
            QCOMPARE(s, QDir::toNativeSeparators(own.filePath("race/Temp/NovaSockets")));
        QVERIFY(!QFileInfo::exists(deep));
        // The profile comes from the process token, not a redirected environment.
        const auto real = profileLocalAppData();
        QVERIFY(!real.isEmpty());
        QVERIFY(QFileInfo(QDir::fromNativeSeparators(real) + "/Temp").isDir());
        const auto profileVariable = qgetenv("USERPROFILE"), localVariable = qgetenv("LOCALAPPDATA");
        qputenv("USERPROFILE", QDir::toNativeSeparators(deep).toLocal8Bit());
        qputenv("LOCALAPPDATA", QDir::toNativeSeparators(deep).toLocal8Bit());
        const auto redirected = profileLocalAppData();
        qputenv("USERPROFILE", profileVariable);
        qputenv("LOCALAPPDATA", localVariable);
        QCOMPARE(redirected, real);
#endif
    }
    void junctionCannotRedirectManagedGradleHome() {
#ifdef Q_OS_WIN
        const auto base = temp.filePath("preflight-junction");
        const auto layout = RuntimeLayout::make(base, profile);
        const auto outside = temp.filePath("preflight-outside");
        QVERIFY(QDir().mkpath(base));
        QVERIFY(QDir().mkpath(outside));
        QCOMPARE(QProcess::execute("cmd.exe", {"/c", "mklink", "/J",
                     QDir::toNativeSeparators(layout.gradleHome),
                     QDir::toNativeSeparators(outside)}), 0);
        FabricRuntime runtime(options(base));
        const int before = invocations("prepare");
        QVERIFY(!prepareAndWait(runtime));
        QVERIFY(runtime.error().contains("Unsafe"));
        QCOMPARE(invocations("prepare"), before);
#endif
    }
    void concurrentPreparationDoesNotWriteOrSpawnTwice() {
        const auto base = preparedBase("preflight-concurrent");
        FabricRuntime first(options(base)), second(options(base));
        const int before = invocations("prepare");
        QSignalSpy finished(&first, &FabricRuntime::preparationFinished);
        first.prepare(true);
        QVERIFY(!prepareAndWait(second, true));
        QVERIFY(second.error().contains("lock"));
        QVERIFY(!finished.isEmpty() || finished.wait(30000));
        QVERIFY(finished.first().first().toBool());
        QCOMPARE(invocations("prepare"), before + 1);
    }
    void launchSupervisionStopAndDuplicatePrevention() {
        const auto base = preparedBase("launch");
        script("NOVA_FAKE_GRADLE_RUN", runScript("window\n"));
        FabricRuntime runtime(options(base));
        QString error;
        QVERIFY2(runtime.launch("Nova Test World", &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.readyForInput(), 15000);
        QCOMPARE(runtime.state(), FabricRuntime::State::Running);
        QCOMPARE(runtime.game(), FabricRuntime::Game::MinecraftRunning);
        QCOMPARE(runtime.components().loadedMods.value("rrls"), QString("5.1.15+mc.1.21.11"));
        QVERIFY(QString::fromUtf8(read(counter)).contains("-PnovaQuickPlay=Nova Test World"));
        QVERIFY(QString::fromUtf8(read(counter)).contains("--offline runClient"));
        QVERIFY(!runtime.launch({}, &error)); // never a second client
        QVERIFY(error.contains("already running"));
        QTRY_VERIFY_WITH_TIMEOUT(
            treeVisibleWindows(identifyProcess(
                runtime.metadata().value("session").toObject().value("pid").toInteger())) > 0,
            5000);
        QSignalSpy exited(&runtime, &FabricRuntime::gameExited);
        runtime.requestStop(); // WM_CLOSE: the game closes itself
        QVERIFY(exited.wait(10000));
        QCOMPARE(exited.first().first().toInt(), 0);
        QCOMPARE(runtime.game(), FabricRuntime::Game::Exited);
        QCOMPARE(runtime.state(), FabricRuntime::State::Ready);
        QVERIFY(!runtime.metadata().contains("session"));
    }
    void forceStopEndsTheWholeTree() {
        const auto base = preparedBase("tree");
        script("NOVA_FAKE_GRADLE_RUN", "spawn\nprint > Task :runClient\nwait\n");
        FabricRuntime runtime(options(base));
        QVERIFY(runtime.launch({}));
        const auto session = runtime.metadata().value("session").toObject();
        const auto root = identifyProcess(session.value("pid").toInteger());
        QTRY_VERIFY_WITH_TIMEOUT(!descendants(root).isEmpty(), 5000);
        const auto child = descendants(root).first();
        QTRY_COMPARE_WITH_TIMEOUT(runtime.game(), FabricRuntime::Game::MinecraftLaunching, 5000);
        QSignalSpy exited(&runtime, &FabricRuntime::gameExited);
        runtime.cancel(); // launch not ready yet: cancellation ends the tree
        QVERIFY(exited.wait(10000));
        QTRY_VERIFY_WITH_TIMEOUT(!processAlive(child), 5000);
        QVERIFY(!processAlive(root));
        QCOMPARE(runtime.game(), FabricRuntime::Game::Exited);
    }
    void failuresAreDetectedWhileTheProcessLingers() {
        const auto base = preparedBase("linger");
        script("NOVA_FAKE_GRADLE_RUN", "print > Task :runClient\nprint [1] [main/ERROR] "
                                       "(FabricLoader) Failed to remap mods!\nwait\n");
        FabricRuntime runtime(options(base));
        QVERIFY(runtime.launch({}));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.game(), FabricRuntime::Game::LaunchFailed, 5000);
        QVERIFY(runtime.running()); // a hung JVM after a loader failure is still reported
        QVERIFY(runtime.error().contains("Fabric Loader"));
        QSignalSpy exited(&runtime, &FabricRuntime::gameExited);
        runtime.forceStop();
        QVERIFY(exited.wait(10000));
        QCOMPARE(runtime.game(), FabricRuntime::Game::LaunchFailed);
        // An early exit without reaching the menu is a launch failure too.
        script("NOVA_FAKE_GRADLE_RUN", "print > Task :runClient\neprint Exception in thread "
                                       "\"main\" java.lang.Error\nexit 1\n");
        QSignalSpy second(&runtime, &FabricRuntime::gameExited);
        QVERIFY(runtime.launch({}));
        QVERIFY(second.wait(10000));
        QCOMPARE(runtime.game(), FabricRuntime::Game::LaunchFailed);
    }
    // Force the actual supervision boundary: a valid process identity whose last handle has
    // closed before watch starts. This makes the detached-process race deterministic.
    void anExitBeforeWatchIsReported() {
        const auto base = preparedBase("before-watch");
        ProcessIdentity identity;
        {
            QProcess child;
            child.start(QCoreApplication::applicationDirPath() + "/nova-fake-gradle.exe", {"--child"});
            QVERIFY(child.waitForStarted());
            identity = identifyProcess(child.processId());
            QVERIFY(identity.valid());
            child.kill();
            QVERIFY(child.waitForFinished());
        }
        QVERIFY(!processAlive(identity));
        FabricRuntime runtime(options(base));
        QSignalSpy exited(&runtime, &FabricRuntime::gameExited);
        FabricRuntimeTestAccess::watch(runtime, identity);
        QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 10000);
        QCOMPARE(runtime.game(), FabricRuntime::Game::LaunchFailed);
        QCOMPARE(runtime.state(), FabricRuntime::State::Ready);
        QVERIFY(!runtime.metadata().contains("session"));
    }
    // Gradle can end before Nova starts watching it (startDetached keeps no handle). That exit
    // must still be reported, or the runtime stays "Launching" and refuses every later launch.
    void anImmediateExitIsAlwaysReported() {
        const auto base = preparedBase("immediate");
        script("NOVA_FAKE_GRADLE_RUN", "exit 1\n");
        FabricRuntime runtime(options(base));
        for (int attempt = 0; attempt < 25; ++attempt) {
            QSignalSpy exited(&runtime, &FabricRuntime::gameExited);
            QString error;
            QVERIFY2(runtime.launch({}, &error), qPrintable(QString("attempt %1: %2").arg(attempt).arg(error)));
            QTRY_VERIFY2_WITH_TIMEOUT(exited.size() == 1,
                                      qPrintable(QString("attempt %1: no exit signal").arg(attempt)), 10000);
            QCOMPARE(runtime.game(), FabricRuntime::Game::LaunchFailed);
            QCOMPARE(runtime.state(), FabricRuntime::State::Ready);
            QVERIFY(!runtime.metadata().contains("session"));
        }
    }
    void novaShutdownLeavesTheGameAndReattaches() {
        const auto base = preparedBase("shutdown");
        script("NOVA_FAKE_GRADLE_RUN", runScript("window\n"));
        ProcessIdentity root;
        {
            FabricRuntime runtime(options(base));
            QVERIFY(runtime.launch({}));
            QTRY_VERIFY_WITH_TIMEOUT(runtime.readyForInput(), 15000);
            root = identifyProcess(
                runtime.metadata().value("session").toObject().value("pid").toInteger());
        } // Nova closes; the user chose to leave Minecraft running
        QVERIFY(processAlive(root));
        FabricRuntime later(options(base));
        QVERIFY(later.attachExisting());
        QVERIFY(later.running());
        QString error;
        QVERIFY(!later.launch({}, &error)); // still no duplicate
        QSignalSpy exited(&later, &FabricRuntime::gameExited);
        later.requestStop();
        QVERIFY(exited.wait(10000));
        QVERIFY(!processAlive(root));
    }
    void dialogSyncsLaunchesOnceAndRespectsPackChoice() {
        const auto base = preparedBase("dialog");
        const auto pack = temp.filePath("Dialog Pack");
        write(pack + "/pack.mcmeta",
              R"({"pack":{"description":"t","min_format":[75,0],"max_format":[75,0]}})");
        write(pack + "/assets/minecraft/textures/block/stone.png", "PNG");
        script("NOVA_FAKE_GRADLE_PREPARE", "print BUILD SUCCESSFUL\n");
        script("NOVA_FAKE_GRADLE_RUN", runScript("window\n"));
        Preferences preferences;
        preferences.fabricLocation = base;
        QStringList unsaved, activity;
        FabricTestDialog::Context context;
        context.projectRoot = [&] { return pack; };
        context.packName = [] { return QString("Dialog Pack"); };
        context.unsavedDocuments = [&] { return unsaved; };
        context.activity = [&](const QString &m) { activity << m; };
        context.preferences = &preferences;
        context.runtime = options(base);
        context.interactive = false;
        FabricTestDialog dialog(context);
        auto engine = dialog.runtime();
        QVERIFY(engine->prepared());
        unsaved = {"assets/minecraft/textures/block/stone.png"};
        QVERIFY(!dialog.syncPack());
        unsaved.clear();
        const int runsBefore = invocations("run");
        dialog.testInMinecraft();
        QTRY_VERIFY_WITH_TIMEOUT(engine->readyForInput(), 20000);
        QCOMPARE(invocations("run"), runsBefore + 1);
        const auto game = engine->layout().game;
        QVERIFY(QFileInfo::exists(game + "/resourcepacks/Nova Test - Dialog Pack/pack.mcmeta"));
        QVERIFY(read(game + "/options.txt").contains("file/Nova Test - Dialog Pack"));
        // Already running: Test in Minecraft syncs without a second client.
        dialog.testInMinecraft();
        QTest::qWait(500);
        QCOMPARE(invocations("run"), runsBefore + 1);
        QVERIFY(dialog.findChild<QLabel *>("fabricRrlsValue")
                    ->text()
                    .contains("confirmed by Fabric Loader"));
        QSignalSpy exited(engine, &FabricRuntime::gameExited);
        engine->requestStop();
        QVERIFY(exited.wait(10000));
        // The player removed the pack in game: Nova does not re-enable it.
        write(game + "/options.txt", "resourcePacks:[\"vanilla\"]\n");
        dialog.testInMinecraft();
        QTRY_VERIFY_WITH_TIMEOUT(engine->running(), 20000);
        QCOMPARE(read(game + "/options.txt"), QByteArray("resourcePacks:[\"vanilla\"]\n"));
        QSignalSpy exited2(engine, &FabricRuntime::gameExited);
        QTRY_VERIFY_WITH_TIMEOUT(engine->readyForInput(), 20000);
        engine->requestStop();
        QVERIFY(exited2.wait(10000));
        // The world opens directly once it exists.
        write(game + "/saves/Nova Test World/level.dat", "x");
        QVERIFY(dialog.testWorldExists());
        QSignalSpy exited3(engine, &FabricRuntime::gameExited);
        dialog.testInMinecraft();
        QTRY_VERIFY_WITH_TIMEOUT(engine->readyForInput(), 20000);
        QVERIFY(QString::fromUtf8(read(counter))
                    .split('\n')
                    .filter("run ")
                    .last()
                    .contains("Nova Test World"));
        QVERIFY(dialog.confirmShutdown()); // non-interactive: leave it running
        engine->requestStop();
        QVERIFY(exited3.wait(10000));
    }
};

QTEST_MAIN(FabricRuntimeTest)
#include "fabric_runtime_test.moc"
