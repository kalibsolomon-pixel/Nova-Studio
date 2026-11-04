// SPDX-License-Identifier: GPL-3.0-or-later
#include "fabric_runtime.h"
#include "revision.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QtConcurrent>
#include <QtCore/private/qzipreader_p.h>
#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#include <qt_windows.h>
#endif

namespace nova::testing {
namespace {
constexpr qint64 maxJdkEntryBytes = 512LL * 1024 * 1024;
constexpr qint64 maxJdkTotalBytes = 2LL * 1024 * 1024 * 1024;

QString hashFile(const QString &path, const QString &algorithm,
                 const std::shared_ptr<std::atomic_bool> &cancel = {}) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(algorithm == "sha512" ? QCryptographicHash::Sha512
                                                  : QCryptographicHash::Sha256);
    QByteArray chunk;
    while (!(chunk = file.read(4 * 1024 * 1024)).isEmpty()) {
        if (cancel && cancel->load())
            return {};
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}
bool artifactValid(const QString &path, const PinnedArtifact &artifact,
                   const std::shared_ptr<std::atomic_bool> &cancel = {}) {
    const QFileInfo info(path);
    return safeRuntimePath(path) && info.isFile() &&
           (artifact.size < 0 || info.size() == artifact.size) &&
           hashFile(path, artifact.algorithm, cancel) == artifact.hash;
}
QString token() {
    return QString::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0');
}
bool linked(const QString &path) {
    return !safeRuntimePath(path);
}
// Prevent another Nova process from rewriting the managed workspace between
// preflight and process startup, or while a preparation worker is still active.
std::shared_ptr<QLockFile> lockRuntime(const RuntimeLayout &paths) {
    const auto path = paths.base + "/.nova-operation.lock";
    if (!safeRuntimePath(path) || !QDir().mkpath(paths.base))
        return {};
    auto lock = std::make_shared<QLockFile>(path);
    lock->setStaleLockTime(0);
    return lock->tryLock(0) ? lock : nullptr;
}

bool recordedGameIsRunning(const QJsonObject &metadata) {
    const auto session = metadata.value("session").toObject();
    return processAlive({session.value("pid").toInteger(),
                         session.value("created").toString().toULongLong()});
}

QList<JavaHome> runtimeJavaHomes(const FabricRuntime::Options &options,
                                 const RuntimeLayout &paths) {
    auto environment = options.environment;
#ifdef Q_OS_WIN
    if (options.processProgram.isEmpty()) {
        const QSettings windows(R"(HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion)",
                                 QSettings::NativeFormat);
        environment.programFiles = QDir::fromNativeSeparators(windows.value("ProgramFilesDir").toString());
    }
#endif
    auto homes = discoverJavaHomes(environment, paths.javaRoot, false);
    homes.removeIf([](const JavaHome &home) {
        return !safeRuntimePath(home.path + "/bin/java.exe") ||
               !safeRuntimePath(home.path + "/release");
    });
    return homes;
}

class GradleInputGuard {
#ifdef Q_OS_WIN
    QList<HANDLE> handles;
#endif
  public:
    ~GradleInputGuard() {
#ifdef Q_OS_WIN
        for (const auto handle : handles)
            CloseHandle(handle);
#endif
    }

    bool pin(const QString &path, bool directory = false) {
        if (!safeRuntimePath(path))
            return false;
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(path);
        const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
                                        FILE_SHARE_READ | (directory ? FILE_SHARE_WRITE : 0),
                                        nullptr, OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT |
                                            (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0),
                                        nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        handles.append(handle);
        BY_HANDLE_FILE_INFORMATION info{};
        return GetFileInformationByHandle(handle, &info) &&
               !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
        return QFileInfo::exists(path);
#endif
    }
};

std::shared_ptr<GradleInputGuard> pinGradleInputs(const RuntimeLayout &paths,
                                                  const QString &gradleJava) {
    auto guard = std::make_shared<GradleInputGuard>();
    for (const auto &dir : {paths.base, paths.gradleHome, paths.workspace + "/gradle/wrapper",
                            paths.workspace + "/gradle", paths.workspace,
                            QFileInfo(paths.workspace).absolutePath()})
        if (!guard->pin(dir, true))
            return {};
    for (const QString name : {"settings.gradle", "build.gradle", "gradle.properties",
                               "gradle/wrapper/gradle-wrapper.jar",
                               "gradle/wrapper/gradle-wrapper.properties", workspaceManifest})
        if (!guard->pin(paths.workspace + '/' + name))
            return {};
    if (!gradleJava.isEmpty() && !guard->pin(gradleJava + "/bin/java.exe"))
        return {};
    return guard;
}

// Extracts a JDK zip (one top-level folder) into destination, refusing
// absolute paths, traversal, links and oversized entries.
QString extractZip(const QString &zipPath, const QString &destination,
                   const std::shared_ptr<std::atomic_bool> &cancel) {
    QZipReader zip(zipPath);
    if (zip.status() != QZipReader::NoError)
        return "The JDK archive cannot be read";
    qint64 total = 0;
    QString top;
    for (const auto &info : zip.fileInfoList()) {
        if (cancel->load())
            return "Cancelled";
        const auto name = info.filePath;
        if (name.isEmpty() || name.startsWith('/') || name.contains('\\') || name.contains(':') ||
            name.split('/').contains(".."))
            return "The JDK archive contains an unsafe path";
        const auto first = name.section('/', 0, 0);
        if (top.isEmpty())
            top = first;
        if (first != top)
            return "The JDK archive has an unexpected layout";
        const auto relative = name.section('/', 1);
        if (relative.isEmpty())
            continue;
        const auto out = destination + '/' + relative;
        if (info.isSymLink)
            return "The JDK archive contains a link";
        if (info.isDir) {
            if (!QDir().mkpath(out))
                return "Could not create a JDK folder";
            continue;
        }
        if (info.size > maxJdkEntryBytes || (total += info.size) > maxJdkTotalBytes)
            return "The JDK archive is larger than expected";
        QDir().mkpath(QFileInfo(out).absolutePath());
        const auto bytes = zip.fileData(name);
        QFile file(out);
        if (bytes.size() != info.size || !file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            file.write(bytes) != bytes.size())
            return "Could not extract " + relative;
    }
    return {};
}
QJsonObject readJson(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.read(1024 * 1024)).object()
                                          : QJsonObject{};
}
} // namespace

DownloadPolicy runtimePolicy() {
    return {{"cdn.modrinth.com", "github.com", "objects.githubusercontent.com",
             "release-assets.githubusercontent.com"},
            false,
            512LL * 1024 * 1024};
}

QString stateText(FabricRuntime::State state) {
    using S = FabricRuntime::State;
    switch (state) {
    case S::NotConfigured:
        return "Not configured";
    case S::Checking:
        return "Checking dependencies";
    case S::Downloading:
        return "Downloading dependencies";
    case S::Preparing:
        return "Preparing workspace";
    case S::GeneratingRunConfig:
        return "Generating run configuration";
    case S::Ready:
        return "Ready";
    case S::Launching:
        return "Launching";
    case S::Running:
        return "Running";
    case S::Stopping:
        return "Stopping";
    case S::Failed:
        return "Failed";
    }
    return {};
}

QString gameText(FabricRuntime::Game game) {
    using G = FabricRuntime::Game;
    switch (game) {
    case G::Idle:
        return "Ready";
    case G::Starting:
        return "Starting";
    case G::GradlePreparing:
        return "Gradle preparing";
    case G::MinecraftLaunching:
        return "Minecraft launching";
    case G::MinecraftRunning:
        return "Minecraft running";
    case G::Exited:
        return "Minecraft exited";
    case G::LaunchFailed:
        return "Launch failed";
    case G::CancellationRequested:
        return "Cancellation requested";
    }
    return {};
}

RunLine classifyRunOutput(const QString &raw, bool inModList) {
    const auto line = raw.trimmed();
    RunLine out;
    out.text = line.left(400);
    if (line.isEmpty())
        return out;
    static const QRegularExpression entry("^(?:\\|--|-)\\s+([a-z0-9_.\\-]+)\\s+(\\S+)$");
    if (inModList) {
        if (const auto m = entry.match(line); m.hasMatch()) {
            out.kind = RunLine::ModEntry;
            out.key = m.captured(1);
            out.value = m.captured(2);
            return out;
        }
    }
    static const QStringList failures = {"BUILD FAILED",
                                         "FAILURE: Build failed",
                                         "Failed to remap mods",
                                         "Incompatible mods found",
                                         "Mod resolution failed",
                                         "Could not execute entrypoint",
                                         "---- Minecraft Crash Report ----",
                                         "requires at least JVM runtime version",
                                         "No cached version of",
                                         "UnsupportedClassVersionError",
                                         "Could not find or load main class",
                                         "Exception in thread \"main\"",
                                         "Game crashed!"};
    for (const auto &marker : failures)
        if (line.contains(marker)) {
            out.kind = RunLine::Failure;
            if (marker == "No cached version of")
                out.value =
                    "Some dependencies are missing from Nova's offline cache. Use Repair runtime.";
            else if (marker == "requires at least JVM runtime version" ||
                     marker == "UnsupportedClassVersionError")
                out.value =
                    "A Java runtime is incompatible with this configuration. Use Repair runtime.";
            else if (marker.contains("mods") || marker.contains("entrypoint"))
                out.value = "Fabric Loader could not start the mods: " + line.left(200);
            else if (marker.contains("Crash") || marker.contains("crashed"))
                out.value =
                    "Minecraft crashed. The crash report is in the game folder's crash-reports.";
            else
                out.value = line.left(200);
            return out;
        }
    static const QRegularExpression loader("Loading Minecraft (\\S+) with Fabric Loader (\\S+)");
    if (const auto m = loader.match(line); m.hasMatch()) {
        out.kind = RunLine::LoaderStart;
        out.key = m.captured(1);
        out.value = m.captured(2);
        return out;
    }
    static const QRegularExpression mods("\\(FabricLoader\\) Loading \\d+ mods:$");
    if (mods.match(line).hasMatch())
        out.kind = RunLine::ModListStart;
    else if (line.contains("Setting user: "))
        out.kind = RunLine::PlayerSet;
    else if (line.contains("Sound engine started"))
        out.kind = RunLine::MenuReady;
    else if (line.startsWith("> Task :runClient"))
        out.kind = RunLine::RunTask;
    else if (line.startsWith("> Task :") || line.contains("single-use Daemon") ||
             line.startsWith("Starting a Gradle Daemon"))
        out.kind = RunLine::GradleWork;
    else if (line.startsWith("BUILD SUCCESSFUL"))
        out.kind = RunLine::BuildOk;
    return out;
}

FabricRuntime::FabricRuntime(Options opts, QObject *parent)
    : QObject(parent), options(std::move(opts)), poll(new QTimer(this)), grace(new QTimer(this)) {
    if (options.profile)
        pins = *options.profile;
    else if (auto known = fabricProfile(options.minecraftVersion))
        pins = *known;
    valid = !pins.key.isEmpty();
    const auto base = options.base.isEmpty()
                          ? RuntimeLayout::defaultBase(options.environment.localAppData)
                          : options.base;
    paths = RuntimeLayout::make(base, pins);
    meta = readJson(paths.metadata);
    poll->setInterval(1000);
    poll->setTimerType(Qt::VeryCoarseTimer);
    connect(poll, &QTimer::timeout, this, &FabricRuntime::pollRun);
    grace->setSingleShot(true);
    connect(grace, &QTimer::timeout, this, [this] {
        if (running()) {
            graceExpired = true;
            detail = "Minecraft has not closed yet. You can keep waiting or stop it now.";
            emit changed();
        }
    });
    if (!valid) {
        current = State::Failed;
        failure =
            "Minecraft " + options.minecraftVersion + " has no supported Fabric configuration";
    } else if (prepared())
        current = State::Ready;
}

FabricRuntime::~FabricRuntime() {
    // Preparation is Nova's own transient work: never leave it orphaned.
    if (cancelFlag)
        cancelFlag->store(true);
    if (fetch)
        fetch->cancel();
    if (gradle && gradle->state() != QProcess::NotRunning) {
        terminateTree(identifyProcess(gradle->processId()));
        gradle->kill();
        gradle->waitForFinished(5000);
    }
    // A running game was started detached on purpose: the user decides
    // whether it keeps running when Nova closes.
    unwatch();
}

bool FabricRuntime::prepared() const {
    const auto p = meta.value("prepared").toObject();
    return valid && p.value("profile").toString() == pins.key &&
           QFileInfo::exists(paths.workspace + '/' + workspaceManifest);
}

bool FabricRuntime::busy() const {
    return current == State::Checking || current == State::Downloading ||
           current == State::Preparing || current == State::GeneratingRunConfig;
}

bool FabricRuntime::running() const { return processAlive(root); }

QString FabricRuntime::runLogPath() const { return paths.logs + "/gradle-run.log"; }
QString FabricRuntime::prepareLogPath() const { return paths.logs + "/gradle-prepare.log"; }
QJsonObject FabricRuntime::metadata() const { return meta; }

void FabricRuntime::recordMetadata(const QString &key, const QJsonValue &value) {
    meta.insert(key, value);
    saveMetadata();
}

void FabricRuntime::saveMetadata() {
    QDir().mkpath(paths.base);
    QString error;
    atomicWrite(paths.metadata, QJsonDocument(meta).toJson(QJsonDocument::Indented),
                Revision::read(paths.metadata), &error);
}

void FabricRuntime::setState(State state, const QString &text) {
    current = state;
    if (!text.isEmpty())
        detail = text;
    emit changed();
}

void FabricRuntime::fail(const QString &message) {
    failure = message;
    cancelFlag.reset();
    preparationLock.reset();
    done = 0;
    total = -1;
    setState(State::Failed, message);
    emit preparationFinished(false);
}

void FabricRuntime::prepare(bool repair) {
    if (!valid || busy() || running())
        return;
    preparationLock = lockRuntime(paths);
    if (!preparationLock)
        return fail("Nova cannot lock the managed runtime for preparation");
    meta = readJson(paths.metadata);
    if (recordedGameIsRunning(meta))
        return fail("Minecraft is already using the managed runtime");
    repairing = repair;
    failure.clear();
    cancelFlag = std::make_shared<std::atomic_bool>(false);
    prepareStarted = QDateTime::currentDateTimeUtc();
    pendingDownloads.clear();
    checkStep();
}

void FabricRuntime::checkStep() {
    setState(State::Checking, "Checking Java, Fabric and Remove Reloading Screen…");
    done = 0;
    total = -1;
    const auto homeError = checkGradleHome(paths);
    if (!homeError.isEmpty())
        return fail(homeError);
    for (const auto &dir : {paths.base, paths.downloads, paths.logs, paths.javaRoot, paths.game,
                            paths.gradleHome, paths.workspace})
        if (!safeRuntimePath(dir) || !QDir().mkpath(dir))
            return fail("Nova cannot use the test environment folder " +
                        QDir::toNativeSeparators(dir));
    if (repairing) {
        meta.remove("prepared");
        saveMetadata();
        if (!safeRuntimeTree(paths.workspace) || !QDir(paths.workspace).removeRecursively())
            return fail("Could not reset the managed Gradle workspace");
    }
    // Java: the game needs the exact major Mojang declares; Loom needs a minimum.
    const auto homes = runtimeJavaHomes(options, paths);
    std::optional<JavaHome> game;
    if (!options.chosenGameJava.isEmpty()) {
        auto chosen = inspectJavaHome(options.chosenGameJava, "chosen");
        if (!chosen || !chosen->jdk || chosen->major != pins.gameJavaMajor)
            return fail(QString("The selected Java is not a Java %1 JDK").arg(pins.gameJavaMajor));
        game = chosen;
    } else
        game = chooseJava(homes, pins.gameJavaMajor, true);
    auto gradleJava = chooseJava(homes, pins.gradleJavaMinimum, false);
    parts.gameJava = game ? game->path : QString();
    parts.gradleJava = gradleJava ? gradleJava->path : QString();
    if (!game)
        pendingDownloads << pins.gameJdk;
    if (!gradleJava)
        pendingDownloads << pins.gradleJdk;
    // Verified artifacts are cached by exact file name and hash.
    const auto flag = cancelFlag;
    const auto mods = pins.mods;
    const auto downloads = paths.downloads;
    auto watcher = new QFutureWatcher<QList<PinnedArtifact>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, flag] {
        watcher->deleteLater();
        if (cancelFlag != flag || flag->load())
            return;
        pendingDownloads << watcher->result();
        const auto p = meta.value("prepared").toObject();
        const bool cached = !repairing && pendingDownloads.isEmpty() &&
                            p.value("profile").toString() == pins.key &&
                            p.value("gameJava").toString() == parts.gameJava &&
                            p.value("gradleJava").toString() == parts.gradleJava;
        if (cached) {
            const auto check = verifyWorkspace(pins, paths, parts.gameJava);
            if (check.ok && check.manifestHash == p.value("manifest").toString()) {
                preparedMs = prepareStarted.msecsTo(QDateTime::currentDateTimeUtc());
                cancelFlag.reset();
                preparationLock.reset();
                setState(State::Ready, "Ready — using the prepared runtime");
                emit preparationFinished(true);
                return;
            }
        }
        if (!pendingDownloads.isEmpty())
            downloadNext();
        else
            workspaceStep();
    });
    watcher->setFuture(QtConcurrent::run([mods, downloads, flag, lock = preparationLock] {
        QList<PinnedArtifact> missing;
        for (const auto &mod : mods)
            if (!artifactValid(downloads + '/' + mod.fileName, mod, flag)) {
                if (flag->load())
                    break;
                if (safeRuntimePath(downloads + '/' + mod.fileName))
                    QFile::remove(downloads + '/' + mod.fileName); // never reuse a bad copy
                missing << mod;
            }
        return missing;
    }));
}

void FabricRuntime::downloadNext() {
    if (!cancelFlag || cancelFlag->load())
        return;
    if (pendingDownloads.isEmpty()) {
        // Re-resolve Java now that Nova-managed JDKs may exist.
        const auto homes = runtimeJavaHomes(options, paths);
        if (parts.gameJava.isEmpty())
            if (auto java = chooseJava(homes, pins.gameJavaMajor, true))
                parts.gameJava = java->path;
        if (parts.gradleJava.isEmpty())
            if (auto java = chooseJava(homes, pins.gradleJavaMinimum, false))
                parts.gradleJava = java->path;
        if (parts.gameJava.isEmpty() || parts.gradleJava.isEmpty())
            return fail("A required JDK could not be installed");
        return workspaceStep();
    }
    downloading = pendingDownloads.takeFirst();
    setState(State::Downloading,
             QString("Downloading %1 %2…").arg(downloading.name, downloading.version));
    done = 0;
    total = downloading.size;
    if (!network)
        network = new QNetworkAccessManager(this);
    fetch = new Fetch(network, options.policy, this);
    connect(fetch, &Fetch::progress, this, [this](qint64 got, qint64 size) {
        done = got;
        total = size > 0 ? size : downloading.size;
        emit changed();
    });
    connect(fetch, &Fetch::finished, this, &FabricRuntime::finishDownload);
    const auto destination = paths.downloads + '/' + downloading.fileName;
    if (!safeRuntimePath(destination))
        return fail("Unsafe artifact destination");
    fetch->start(QUrl(downloading.url), destination);
}

void FabricRuntime::finishDownload(const QString &error) {
    if (fetch)
        fetch->deleteLater();
    if (!cancelFlag || cancelFlag->load())
        return;
    if (!error.isEmpty())
        return fail(QString("Could not download %1: %2").arg(downloading.name, error));
    setState(State::Downloading, "Verifying " + downloading.name + "…");
    total = -1;
    const auto artifact = downloading;
    const auto path = paths.downloads + '/' + artifact.fileName;
    const auto flag = cancelFlag;
    auto watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, artifact, path, flag] {
        watcher->deleteLater();
        if (cancelFlag != flag || flag->load())
            return;
        if (!watcher->result()) {
            QFile::remove(path);
            return fail(artifact.name +
                        " does not match its published checksum. The download was deleted.");
        }
        if (artifact.id.startsWith("temurin"))
            return extractJdk(artifact, path);
        downloadNext();
    });
    watcher->setFuture(
        QtConcurrent::run([path, artifact, flag] { return artifactValid(path, artifact, flag); }));
}

void FabricRuntime::extractJdk(const PinnedArtifact &jdk, const QString &zip) {
    setState(State::Downloading, "Installing " + jdk.name + " " + jdk.version + "…");
    const auto target = paths.javaRoot + '/' + jdk.id;
    const auto staging = paths.javaRoot + "/." + jdk.id + "-" + token();
    const auto flag = cancelFlag;
    auto watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, jdk, target, staging, flag] {
        watcher->deleteLater();
        if (cancelFlag != flag || flag->load()) {
            QDir(staging).removeRecursively();
            return;
        }
        auto error = watcher->result();
        if (error.isEmpty()) {
            const auto java = inspectJavaHome(staging, "managed");
            const int wanted =
                jdk.id == pins.gameJdk.id ? pins.gameJavaMajor : pins.gradleJavaMinimum;
            if (!java || !java->jdk || java->major < wanted ||
                (jdk.id == pins.gameJdk.id && java->major != wanted))
                error = jdk.name + " did not contain the expected Java version";
        }
        if (error.isEmpty()) {
            if (!linked(target))
                QDir(target).removeRecursively();
            if (!QDir().rename(staging, target))
                error = "Could not install " + jdk.name;
        }
        if (!error.isEmpty()) {
            QDir(staging).removeRecursively();
            if (cancelFlag && !cancelFlag->load())
                fail(error);
            return;
        }
        if (cancelFlag && !cancelFlag->load())
            downloadNext();
    });
    watcher->setFuture(QtConcurrent::run([zip, staging, flag, lock = preparationLock] {
        if (!QDir().mkpath(staging))
            return QString("Could not create the JDK folder");
        return extractZip(zip, staging, flag);
    }));
}

void FabricRuntime::workspaceStep() {
    setState(State::Preparing, "Writing the Fabric workspace…");
    done = 0;
    total = -1;
    WorkspaceInputs inputs;
    inputs.gameJavaHome = parts.gameJava;
    for (const auto &mod : pins.mods)
        inputs.verifiedMods << paths.downloads + '/' + mod.fileName;
    const auto profile = pins;
    const auto layout = paths;
    const auto flag = cancelFlag;
    auto watcher = new QFutureWatcher<WorkspaceResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, flag] {
        watcher->deleteLater();
        if (cancelFlag != flag || flag->load())
            return;
        const auto result = watcher->result();
        if (!result.ok)
            return fail(result.error);
        gradleStep(result.manifestHash);
    });
    watcher->setFuture(QtConcurrent::run(
        [profile, layout, inputs, lock = preparationLock] { return writeWorkspace(profile, layout, inputs); }));
}

QProcessEnvironment FabricRuntime::gradleEnvironment() const {
    const auto inherited = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment environment;
    for (const QString name : {"SystemRoot", "WINDIR", "SystemDrive", "USERPROFILE",
                               "HOMEDRIVE", "HOMEPATH", "LOCALAPPDATA", "APPDATA",
                               "ProgramData", "ProgramFiles", "ProgramFiles(x86)",
                               "ProgramW6432", "USERNAME", "OS", "PROCESSOR_ARCHITECTURE",
                               "NUMBER_OF_PROCESSORS"})
        if (inherited.contains(name))
            environment.insert(name, inherited.value(name));
#ifdef Q_OS_WIN
    wchar_t systemDirectory[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length && length < MAX_PATH) {
        const auto system = QString::fromWCharArray(systemDirectory, length);
        environment.insert("PATH", QDir::toNativeSeparators(parts.gradleJava + "/bin") +
                                       ';' + system);
        environment.insert("SystemRoot", QFileInfo(QDir::fromNativeSeparators(system)).absolutePath());
        environment.insert("WINDIR", environment.value("SystemRoot"));
        environment.insert("ComSpec", system + "/cmd.exe");
    }
#endif
    const auto temp = QDir::toNativeSeparators(paths.base + "/gradle-temp");
    environment.insert("TEMP", temp);
    environment.insert("TMP", temp);
#ifdef Q_OS_WIN
    if (options.processProgram.isEmpty() && !options.environment.localAppData.isEmpty()) {
        // Windows NIO uses Unix-domain socket files. Use a short, checked
        // subdirectory of the per-user Windows temp directory for those sockets.
        // Keep all other Gradle temporary files under the managed runtime.
        const auto sockets = prepareSocketDirectory(options.environment.localAppData);
        if (sockets.isEmpty())
            return {};
        environment.insert("JDK_JAVA_OPTIONS", QString("\"-Djdk.net.unixdomain.tmpdir=%1\"").arg(sockets));
    }
#endif
    if (!options.processProgram.isEmpty())
        for (const auto &name : inherited.keys())
            if (name.startsWith("NOVA_FAKE_GRADLE_"))
                environment.insert(name, inherited.value(name));
    environment.insert("JAVA_HOME", QDir::toNativeSeparators(parts.gradleJava));
    environment.insert("GRADLE_USER_HOME", QDir::toNativeSeparators(paths.gradleHome));
    return environment;
}

QStringList FabricRuntime::gradleCommand(const QStringList &tasks, QString *program) const {
    if (!options.processProgram.isEmpty()) {
        *program = options.processProgram;
        return options.processPrefix + tasks;
    }
    *program = QDir::toNativeSeparators(parts.gradleJava + "/bin/java.exe");
    QStringList arguments{
        "-Dorg.gradle.appname=gradlew", "-Dorg.gradle.native=false", "-classpath",
        QDir::toNativeSeparators(paths.workspace + "/gradle/wrapper/gradle-wrapper.jar"),
        "org.gradle.wrapper.GradleWrapperMain", "--console=plain", "--no-daemon",
        "--no-watch-fs", "--no-build-cache", "--no-configuration-cache",
        "-Dorg.gradle.native=false"};
    if (options.detailedLogs)
        arguments << "--info";
    return arguments + tasks;
}

void FabricRuntime::gradleStep(const QString &manifestHash) {
    setState(State::GeneratingRunConfig, "Preparing Minecraft and Fabric with Gradle…");
    done = 0;
    total = -1;
    if (!cancelFlag || cancelFlag->load())
        return;
    const auto java = options.processProgram.isEmpty() ? parts.gradleJava : QString();
    if (options.processProgram.isEmpty() &&
        (java.isEmpty() || !safeRuntimePath(parts.gameJava + "/bin/java.exe")))
        return fail("The selected Java runtime is unsafe");
    const auto guard = pinGradleInputs(paths, java);
    const auto check = verifyWorkspace(pins, paths, parts.gameJava);
    const auto temp = paths.base + "/gradle-temp";
    if (!guard || !check.ok || check.manifestHash != manifestHash ||
        !safeRuntimePath(temp) || !QDir().mkpath(temp))
        return fail(check.ok ? QString("Could not secure the Gradle execution inputs") : check.error);
    const auto environment = gradleEnvironment();
    if (environment.isEmpty())
        return fail("Could not prepare the Java socket directory");
    gradleBuffer.clear();
    QFile::remove(prepareLogPath());
    gradle = new QProcess(this);
    QString program;
    gradle->setArguments(gradleCommand({prepareTask}, &program));
    gradle->setProgram(program);
    gradle->setWorkingDirectory(paths.workspace);
    gradle->setProcessEnvironment(environment);
    gradle->setProcessChannelMode(QProcess::MergedChannels);
    connect(gradle, &QProcess::readyRead, this, [this] { gradleOutput(gradle->readAll()); });
    connect(gradle, &QProcess::finished, this,
            [this, guard = guard, manifestHash, flag = cancelFlag,
             lock = preparationLock](int code, QProcess::ExitStatus status) mutable {
        guard.reset();
        lock.reset();
        gradleOutput(gradle->readAll());
        gradle->deleteLater();
        gradle = nullptr;
        if (cancelFlag != flag || flag->load())
            return;
        if (status != QProcess::NormalExit || code != 0 ||
            !gradleBuffer.contains("BUILD SUCCESSFUL")) {
            QString reason;
            const auto at = gradleBuffer.indexOf("* What went wrong:");
            if (at >= 0)
                reason = gradleBuffer.mid(at + 18, 600).section("* Try:", 0, 0).simplified();
            return fail("Gradle could not prepare the runtime" +
                        (reason.isEmpty() ? QString(".") : ": " + reason.left(300)) +
                        " Details: " + QDir::toNativeSeparators(prepareLogPath()));
        }
        const auto check = verifyWorkspace(pins, paths, parts.gameJava);
        if (!check.ok || check.manifestHash != manifestHash)
            return fail(check.ok ? QString("Workspace changed during Gradle preparation") : check.error);
        preparedMs = prepareStarted.msecsTo(QDateTime::currentDateTimeUtc());
        meta.insert("prepared", QJsonObject{{"profile", pins.key},
                                            {"manifest", check.manifestHash},
                                            {"gameJava", parts.gameJava},
                                            {"gradleJava", parts.gradleJava},
                                            {"preparedAt",
                                             QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                            {"milliseconds", preparedMs}});
        saveMetadata();
        cancelFlag.reset();
        preparationLock.reset();
        setState(State::Ready, "Ready");
        emit preparationFinished(true);
    });
    gradle->start();
    if (gradle && !gradle->waitForStarted(15000) && gradle) {
        const auto error = gradle->errorString();
        delete gradle;
        gradle = nullptr;
        fail("Gradle could not be started: " + error);
    }
}

void FabricRuntime::gradleOutput(const QByteArray &bytes) {
    if (bytes.isEmpty())
        return;
    QFile log(prepareLogPath());
    if (log.open(QIODevice::Append))
        log.write(bytes);
    const auto text = QString::fromUtf8(bytes);
    gradleBuffer += text;
    if (gradleBuffer.size() > 512 * 1024)
        gradleBuffer = gradleBuffer.right(256 * 1024);
    for (const auto &line : text.split('\n')) {
        const auto t = line.trimmed();
        if (t.startsWith("Downloading https://services.gradle.org"))
            detail = "Downloading Gradle " + pins.gradleVersion + "…";
        else if (t == "> Task :downloadAssets")
            detail = "Downloading Minecraft " + pins.minecraftVersion + " assets…";
        else if (t.startsWith("> Task :generate") || t == "> Task :configureLaunch")
            detail = "Generating the run configuration…";
        else if (t.startsWith("> Task :") || t.startsWith("> Configure project"))
            detail = "Preparing Minecraft and Fabric with Gradle…";
    }
    emit changed();
}

void FabricRuntime::cancel() {
    if (busy()) {
        if (cancelFlag)
            cancelFlag->store(true);
        if (fetch) {
            disconnect(fetch, nullptr, this, nullptr);
            fetch->cancel();
            fetch->deleteLater();
        }
        if (gradle && gradle->state() != QProcess::NotRunning) {
            terminateTree(identifyProcess(gradle->processId()));
            gradle->kill();
        }
        cancelFlag.reset();
        preparationLock.reset();
        failure.clear();
        setState(prepared() ? State::Ready : State::NotConfigured, "Preparation cancelled");
        emit preparationFinished(false);
        return;
    }
    if (running() && !gameReady) {
        phase = Game::CancellationRequested;
        setState(State::Stopping, "Cancelling the launch…");
        forceStop();
    }
}

bool FabricRuntime::launch(const QString &quickPlayWorld, QString *error) {
    auto refuse = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!valid)
        return refuse(failure);
    if (running() || current == State::Launching || current == State::Running)
        return refuse("Minecraft is already running from Nova's test environment");
    if (busy())
        return refuse("The runtime is still being prepared");
    const auto lock = lockRuntime(paths);
    if (!lock)
        return refuse("Nova cannot lock the managed runtime for launch");
    meta = readJson(paths.metadata);
    if (recordedGameIsRunning(meta))
        return refuse("Minecraft is already using the managed runtime");
    if (!prepared())
        return refuse("The runtime is not prepared yet");
    const auto p = meta.value("prepared").toObject();
    parts.gameJava = p.value("gameJava").toString();
    parts.gradleJava = p.value("gradleJava").toString();
    if (options.processProgram.isEmpty()) {
        const auto homes = runtimeJavaHomes(options, paths);
        const auto hasHome = [&](const QString &path, int major, bool exact) {
            return std::any_of(homes.cbegin(), homes.cend(), [&](const JavaHome &home) {
                return home.jdk && (exact ? home.major == major : home.major >= major) &&
                       home.path.compare(path, Qt::CaseInsensitive) == 0;
            });
        };
        const auto chosen = options.chosenGameJava.isEmpty()
                                ? std::optional<JavaHome>{} : inspectJavaHome(options.chosenGameJava);
        const bool gameTrusted = chosen && chosen->jdk && chosen->major == pins.gameJavaMajor &&
                                 chosen->path.compare(parts.gameJava, Qt::CaseInsensitive) == 0 &&
                                 safeRuntimePath(parts.gameJava + "/bin/java.exe");
        if ((!gameTrusted && !hasHome(parts.gameJava, pins.gameJavaMajor, true)) ||
            !hasHome(parts.gradleJava, pins.gradleJavaMinimum, false))
            return refuse("A prepared Java runtime is unsafe or missing. Use Repair runtime.");
    }
    const auto guard = pinGradleInputs(paths, options.processProgram.isEmpty() ? parts.gradleJava : QString());
    const auto check = verifyWorkspace(pins, paths, parts.gameJava);
    const auto temp = paths.base + "/gradle-temp";
    if (!guard || !check.ok || check.manifestHash != p.value("manifest").toString() ||
        !safeRuntimePath(temp) || !QDir().mkpath(temp) || !safeRuntimePath(paths.logs))
        return refuse((check.ok ? QString("Could not secure the prepared Gradle inputs") : check.error) +
                      ". Use Repair runtime.");
    const auto environment = gradleEnvironment();
    if (environment.isEmpty())
        return refuse("Could not prepare the Java socket directory");
    QDir().mkpath(paths.logs);
    for (const auto &suffix : {".log", ".err.log"}) {
        const auto path = paths.logs + "/gradle-run" + suffix;
        QFile::remove(paths.logs + "/gradle-run.1" + suffix);
        QFile::rename(path, paths.logs + "/gradle-run.1" + suffix);
    }
    QStringList tasks{"--offline", "runClient"};
    if (!quickPlayWorld.isEmpty())
        tasks << QString("-P%1=%2").arg(quickPlayProperty, quickPlayWorld);
    QString program;
    QProcess process;
    process.setArguments(gradleCommand(tasks, &program));
    process.setProgram(program);
    process.setWorkingDirectory(paths.workspace);
    process.setProcessEnvironment(environment);
    process.setStandardOutputFile(runLogPath());
    process.setStandardErrorFile(paths.logs + "/gradle-run.err.log");
    process.setStandardInputFile(QProcess::nullDevice());
    qint64 pid = 0;
    if (!process.startDetached(&pid))
        return refuse("Gradle could not be started: " + process.errorString());
    const auto identity = identifyProcess(pid);
    meta.insert("session",
                QJsonObject{{"pid", pid},
                            {"created", QString::number(identity.created)},
                            {"started", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
    saveMetadata();
    parts.loadedMods.clear();
    failure.clear();
    phase = Game::Starting;
    gameReady = false;
    setState(State::Launching, "Starting Gradle…");
    watch(identity);
    if (running())
        launchGuard = guard;
    return true;
}

bool FabricRuntime::attachExisting() {
    if (running())
        return true;
    const auto session = meta.value("session").toObject();
    const ProcessIdentity identity{qint64(session.value("pid").toDouble()),
                                   session.value("created").toString().toULongLong()};
    if (!processAlive(identity))
        return false;
    phase = Game::MinecraftRunning;
    setState(State::Running, "Reconnected to Minecraft started by an earlier Nova session");
    watch(identity);
    return true;
}

void FabricRuntime::watch(const ProcessIdentity &identity) {
    unwatch();
    root = identity;
    knownTree.clear();
    runOffset = errOffset = 0;
    runPartial.clear();
    errPartial.clear();
    modList = false;
    failureSeen = false;
    graceExpired = false;
#ifdef Q_OS_WIN
    processHandle =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(identity.pid));
    if (processHandle) {
        exitNotifier = new QWinEventNotifier(processHandle, this);
        connect(exitNotifier, &QWinEventNotifier::activated, this, &FabricRuntime::processExited);
    }
#endif
    poll->start();
    pollRun();
}

void FabricRuntime::unwatch() {
    launchGuard.reset();
    poll->stop();
    grace->stop();
    if (exitNotifier) {
        exitNotifier->setEnabled(false);
        exitNotifier->deleteLater();
        exitNotifier = nullptr;
    }
#ifdef Q_OS_WIN
    if (processHandle)
        CloseHandle(static_cast<HANDLE>(processHandle));
#endif
    processHandle = nullptr;
}

void FabricRuntime::pollRun() {
    for (const auto &child : descendants(root)) {
        bool known = false;
        for (const auto &entry : knownTree)
            known = known || entry.pid == child.pid;
        if (!known)
            knownTree << child;
    }
    bool any = false;
    auto tail = [&](const QString &path, qint64 &offset, QByteArray &partial) {
        QFile log(path);
        if (!log.open(QIODevice::ReadOnly) || log.size() <= offset)
            return;
        log.seek(offset);
        const auto bytes = log.read(1024 * 1024);
        offset += bytes.size();
        auto lines = (partial + bytes).split('\n');
        partial = lines.takeLast().right(64 * 1024);
        for (const auto &line : lines)
            classifyRunLine(QString::fromUtf8(line));
        any = true;
    };
    tail(runLogPath(), runOffset, runPartial);
    tail(paths.logs + "/gradle-run.err.log", errOffset, errPartial);
    if (any)
        emit changed();
}

void FabricRuntime::classifyRunLine(const QString &line) {
    const auto out = classifyRunOutput(line, modList);
    if (out.kind != RunLine::ModEntry)
        modList = out.kind == RunLine::ModListStart;
    switch (out.kind) {
    case RunLine::GradleWork:
        if (phase == Game::Starting) {
            phase = Game::GradlePreparing;
            detail = "Gradle is preparing the launch…";
        }
        break;
    case RunLine::RunTask:
        phase = Game::MinecraftLaunching;
        detail = "Starting Minecraft…";
        break;
    case RunLine::LoaderStart:
        phase = Game::MinecraftLaunching;
        detail = QString("Fabric Loader %1 is starting Minecraft %2…").arg(out.value, out.key);
        break;
    case RunLine::ModEntry:
        parts.loadedMods.insert(out.key, out.value);
        break;
    case RunLine::PlayerSet:
        phase = Game::MinecraftRunning;
        current = State::Running;
        detail = "Minecraft is loading resources…";
        break;
    case RunLine::MenuReady:
        phase = Game::MinecraftRunning;
        current = State::Running;
        gameReady = true;
        detail = "Minecraft is running";
        break;
    case RunLine::Failure:
        if (!failureSeen) {
            failureSeen = true;
            failure = out.value;
            phase = Game::LaunchFailed;
            detail = out.value;
        }
        break;
    default:
        break;
    }
}

void FabricRuntime::processExited() {
    pollRun();
    int code = -1;
#ifdef Q_OS_WIN
    DWORD exitCode = 0;
    if (processHandle && GetExitCodeProcess(static_cast<HANDLE>(processHandle), &exitCode))
        code = int(exitCode);
#endif
    // The Gradle client is gone; end any process it left behind only if the
    // user asked for a stop (otherwise its game would have ended with it).
    if (current == State::Stopping)
        for (const auto &child : knownTree)
            if (processAlive(child))
                terminateTree(child);
    unwatch();
    root = {};
    meta.remove("session");
    saveMetadata();
    const bool cancelled = phase == Game::CancellationRequested;
    if (!cancelled && (failureSeen || (code != 0 && current != State::Stopping && !gameReady))) {
        phase = Game::LaunchFailed;
        if (failure.isEmpty())
            failure =
                QString("Minecraft stopped before it finished starting (exit code %1). Details: %2")
                    .arg(code)
                    .arg(QDir::toNativeSeparators(runLogPath()));
        detail = failure;
    } else {
        phase = Game::Exited;
        detail = cancelled ? "Launch cancelled" : "Minecraft exited";
    }
    gameReady = false;
    graceExpired = false;
    current = prepared() ? State::Ready : State::NotConfigured;
    emit changed();
    emit gameExited(code);
}

void FabricRuntime::requestStop() {
    if (!running())
        return;
    setState(State::Stopping, "Asking Minecraft to close…");
    graceExpired = false;
    if (requestTreeWindowsClose(root) == 0) {
        // No game window yet: nothing to save, so end the launch directly.
        forceStop();
        return;
    }
    grace->start(options.stopGraceMs);
}

void FabricRuntime::forceStop() {
    if (!processAlive(root) && knownTree.isEmpty())
        return;
    current = State::Stopping;
    detail = "Stopping Minecraft…";
    emit changed();
    for (const auto &child : knownTree)
        if (processAlive(child))
            terminateTree(child);
    terminateTree(root);
}

} // namespace nova::testing
