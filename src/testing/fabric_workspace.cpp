// SPDX-License-Identifier: GPL-3.0-or-later
#include "fabric_workspace.h"
#include "revision.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QTimeZone>
#include <QtCore/private/qzipreader_p.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <userenv.h>
#endif

namespace nova::testing {
namespace {
constexpr qint64 maxNestedJarBytes = 64LL * 1024 * 1024;
constexpr qint64 maxExpandedBytes = 512LL * 1024 * 1024;
constexpr int maxNestingDepth = 4;

QString sha256(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString fileSha256(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}
bool linked(const QString &path) {
    const QFileInfo info(path);
    return info.isSymLink() || info.isJunction();
}
// Java .properties files are read as ISO-8859-1: escape everything else.
QString propertyValue(const QString &value) {
    QString out;
    for (const auto ch : value) {
        if (ch == '\\')
            out += "\\\\";
        else if (ch.unicode() < 0x20 || ch.unicode() > 0x7e)
            out += QString("\\u%1").arg(uint(ch.unicode()), 4, 16, QChar('0'));
        else
            out += ch;
    }
    return out;
}
bool safeJarName(const QString &name) {
    static const QRegularExpression rule("^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}\\.jar$");
    return rule.match(name).hasMatch() && !name.contains("..");
}
} // namespace

bool safeRuntimePath(const QString &path) {
    const auto normalized = QDir::fromNativeSeparators(path);
    if (!QFileInfo(normalized).isAbsolute() || QDir::cleanPath(normalized) != normalized)
        return false;
#ifdef Q_OS_WIN
    if (normalized.size() < 3 || normalized[1] != ':' || normalized[2] != '/' ||
        normalized.mid(2).contains(':'))
        return false;
#endif
    auto info = QFileInfo(normalized);
    for (;;) {
        if (linked(info.absoluteFilePath()))
            return false;
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(info.absoluteFilePath());
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                return false;
        } else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return false;
#endif
        const auto parent = info.absolutePath();
        if (parent == info.absoluteFilePath())
            return true;
        if (info.fileName().endsWith('.') || info.fileName().endsWith(' '))
            return false;
        info.setFile(parent);
    }
}

QString socketDirectoryFor(const QString &localAppData) {
    const auto base = QDir::fromNativeSeparators(localAppData);
    if (base.isEmpty() || !QFileInfo(base).isAbsolute())
        return {};
    return QDir::cleanPath(base + "/Temp/NovaSockets");
}

QString prepareSocketDirectory(const QString &localAppData, int *removed, int staleSeconds,
                               QString *error) {
    if (removed)
        *removed = 0;
    const auto refuse = [&](const QString &reason) {
        if (error)
            *error = reason;
        return QString();
    };
    const auto sockets = socketDirectoryFor(localAppData);
    if (sockets.isEmpty())
        return refuse("The per-user application data folder is not an absolute path");
    const auto native = QDir::toNativeSeparators(sockets);
    // Java refuses socket paths of 107 UTF-8 bytes or more, and its names add
    // "\socket_" and up to ten digits. Concurrent runtime launches may create the
    // same folders at the same moment, so a failed mkpath counts only if the folder
    // still does not exist afterwards.
    if (const auto bytes = sockets.toUtf8().size(); bytes > socketDirectoryMaxBytes)
        return refuse(QString("%1 is %2 bytes long; the limit is %3 bytes")
                          .arg(native).arg(bytes).arg(socketDirectoryMaxBytes));
    if (!safeRuntimePath(sockets))
        return refuse(native + " or a folder above it is a link, junction or unsafe name");
    const auto created = [&] {
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (QDir().mkpath(sockets) || QFileInfo(sockets).isDir())
                return true;
            QThread::msleep(10);
        }
        return false;
    };
    if (!created()) {
#ifdef Q_OS_WIN
        // mkpath does not report why. Repeat its last step to read the system error.
        if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(native.utf16()), nullptr)) {
            const auto code = GetLastError();
            if (code != ERROR_ALREADY_EXISTS || !QFileInfo(sockets).isDir())
                return refuse(QString("Could not create %1 (Windows error %2: %3)")
                                  .arg(native).arg(code).arg(qt_error_string(int(code))));
        }
#else
        return refuse("Could not create " + native);
#endif
    }
    if (!safeRuntimePath(sockets) || !QFileInfo(sockets).isDir())
        return refuse(native + " is not a plain folder");
    // A JVM deletes each socket file right after connecting. Leftovers belong to
    // terminated processes; keep anything recent in case another JVM is binding.
    const auto cutoff = QDateTime::currentDateTimeUtc().addSecs(-staleSeconds);
    QDirIterator entries(sockets, {"socket_*"}, QDir::Files | QDir::System | QDir::Hidden);
    int count = 0;
    while (entries.hasNext() && count < 256) {
        const auto path = entries.next();
        const QFileInfo info(path);
        if (info.absolutePath() != sockets || !info.fileName().startsWith("socket_"))
            continue;
#ifdef Q_OS_WIN
        // Socket files are reparse points. Read and delete the entry itself;
        // DeleteFileW removes a reparse point, never what it designates.
        const auto native = QDir::toNativeSeparators(path);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(reinterpret_cast<LPCWSTR>(native.utf16()), GetFileExInfoStandard, &data) ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        const auto ticks = (qint64(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        const auto modified = QDateTime::fromMSecsSinceEpoch(ticks / 10000 - 11644473600000LL, QTimeZone::UTC);
        if (modified > cutoff)
            continue;
        if (DeleteFileW(reinterpret_cast<LPCWSTR>(native.utf16())))
            ++count;
#else
        if (info.lastModified(QTimeZone::UTC) > cutoff)
            continue;
        if (QFile::remove(path))
            ++count;
#endif
    }
    if (removed)
        *removed = count;
    return QDir::toNativeSeparators(sockets);
}

QString profileLocalAppData() {
#ifdef Q_OS_WIN
    // The profile comes from the process token. The environment and the known-folder
    // API both follow a redirected USERPROFILE, which test harnesses set deep.
    wchar_t buffer[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (!GetUserProfileDirectoryW(GetCurrentProcessToken(), buffer, &size))
        return {};
    const auto local = QDir::fromNativeSeparators(QString::fromWCharArray(buffer)) + "/AppData/Local";
    // Use only an existing Temp folder; never create folders in the profile.
    return QFileInfo(local + "/Temp").isDir() ? QDir::toNativeSeparators(local) : QString();
#else
    return {};
#endif
}

QString chooseSocketDirectory(const QString &localAppData, const QString &fallbackLocalAppData,
                              QString *error) {
    QString reason;
    auto sockets = prepareSocketDirectory(localAppData, nullptr, 600, &reason);
    const auto preferred = socketDirectoryFor(localAppData);
    const auto fallback = socketDirectoryFor(fallbackLocalAppData);
    // Only a path that is too long moves to the profile. Links, junctions and
    // creation failures stay refusals.
    if (sockets.isEmpty() && !preferred.isEmpty() && !fallback.isEmpty() &&
        preferred.toUtf8().size() > socketDirectoryMaxBytes &&
        fallback.compare(preferred, Qt::CaseInsensitive) != 0) {
        QString fallbackReason;
        sockets = prepareSocketDirectory(fallbackLocalAppData, nullptr, 600, &fallbackReason);
        if (sockets.isEmpty())
            reason += ". " + fallbackReason;
    }
    if (sockets.isEmpty() && error)
        *error = reason;
    return sockets;
}

bool safeRuntimeTree(const QString &path) {
    if (!safeRuntimePath(path))
        return false;
    QDirIterator entries(path, QDir::AllEntries | QDir::NoDotAndDotDot |
                                QDir::Hidden | QDir::System,
                         QDirIterator::Subdirectories);
    while (entries.hasNext())
        if (!safeRuntimePath(entries.next()))
            return false;
    return true;
}

QString checkGradleHome(const RuntimeLayout &layout) {
    if (!safeRuntimePath(layout.gradleHome))
        return "Unsafe Nova Gradle home";
    const auto forbidden = [&](const QString &base, const QString &name) {
        const auto path = base + '/' + name;
        return !safeRuntimePath(path) || QFileInfo::exists(path);
    };
    for (const QString name : {"init.d", "init.gradle", "init.gradle.kts", "gradle.properties"})
        if (forbidden(layout.gradleHome, name))
            return "Unexpected Gradle configuration in Nova's Gradle home: " + name;

    // The wrapper can use an existing unpacked distribution. Gradle also loads
    // initialization scripts from that installation's init.d.
    const auto distributions = layout.gradleHome + "/wrapper/dists";
    if (!safeRuntimePath(distributions))
        return "Unsafe Gradle distribution cache";
    QDirIterator entries(distributions, QDir::AllEntries | QDir::NoDotAndDotDot |
                                          QDir::Hidden | QDir::System,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const auto entry = entries.nextFileInfo();
        if (!safeRuntimePath(entry.absoluteFilePath()))
            return "Unsafe path in the Gradle distribution cache";
        if (entry.dir().dirName().compare("init.d", Qt::CaseInsensitive) == 0 &&
            (entry.isDir() || entry.suffix().compare("gradle", Qt::CaseInsensitive) == 0 ||
             entry.suffix().compare("kts", Qt::CaseInsensitive) == 0))
            return "Unexpected initialization file in the Gradle distribution: " + entry.fileName();
    }
    return {};
}

namespace {
bool verifyGradleDistribution(const FabricProfile &profile, const RuntimeLayout &layout) {
    if (profile.gradleDistributionTreeSha256.size() != 64)
        return false;
    const auto version = "gradle-" + profile.gradleVersion;
    const QDir versionCache(layout.gradleHome + "/wrapper/dists/" + version + "-bin");
    const auto directoryFlags = QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
    for (const auto &slot : versionCache.entryInfoList(directoryFlags)) {
        const auto roots = QDir(slot.absoluteFilePath()).entryInfoList(directoryFlags);
        for (const auto &root : roots) {
            if (root.fileName() != version)
                return false; // the wrapper accepts any single root with a launcher JAR
            QMap<QString, QString> hashes;
            const QDir directory(root.absoluteFilePath());
            QDirIterator entries(directory.path(), QDir::Files | QDir::Hidden | QDir::System,
                                 QDirIterator::Subdirectories);
            while (entries.hasNext()) {
                const auto path = entries.next();
                const auto hash = fileSha256(path);
                if (hash.isEmpty())
                    return false;
                hashes.insert(directory.relativeFilePath(path), hash);
            }
            QCryptographicHash digest(QCryptographicHash::Sha256);
            for (auto it = hashes.cbegin(); it != hashes.cend(); ++it) {
                digest.addData(it.key().toUtf8());
                digest.addData(QByteArrayView("\n"));
                digest.addData(it.value().toLatin1());
                digest.addData(QByteArrayView("\n"));
            }
            if (QString::fromLatin1(digest.result().toHex()) != profile.gradleDistributionTreeSha256)
                return false;
        }
    }
    return true;
}
} // namespace

RuntimeLayout RuntimeLayout::make(const QString &base, const FabricProfile &profile) {
    RuntimeLayout l;
    l.base = QDir::fromNativeSeparators(QDir::cleanPath(base));
    l.workspace = l.base + "/workspaces/" + profile.key;
    l.game = l.base + "/instances/" + profile.minecraftVersion;
    l.gradleHome = l.base + "/gradle-home";
    l.javaRoot = l.base + "/java";
    l.downloads = l.base + "/downloads";
    l.logs = l.base + "/logs";
    l.metadata = l.base + "/runtime.json";
    return l;
}

QString RuntimeLayout::defaultBase(const QString &localAppData) {
    if (!localAppData.isEmpty())
        return localAppData + "/NovaStudio/fabric-runtime";
    // A missing LOCALAPPDATA (scrubbed environment or broken profile) must not
    // resolve to a drive-root-relative path; fall back to the standard per-user
    // data root, then to the home directory, so storage stays user-scoped.
    const auto appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!appData.isEmpty())
        return QFileInfo(appData).absolutePath() + "/NovaStudio/fabric-runtime";
    return QDir::homePath() + "/NovaStudio/fabric-runtime";
}

ExpandedMods expandModJars(const QStringList &verifiedJars, const QString &libsDir) {
    ExpandedMods out;
    qint64 total = 0;
    QHash<QString, QString> written; // file name -> sha256
    std::function<bool(const QString &, const QByteArray &, int)> add =
        [&](const QString &name, const QByteArray &bytes, int depth) -> bool {
        if (!safeJarName(name)) {
            out.error = "A mod contains a nested jar with an unsafe name: " + name.left(80);
            return false;
        }
        const auto hash = sha256(bytes);
        if (written.contains(name)) {
            if (written.value(name) == hash)
                return true; // the same module nested twice
            out.error = "Two mods bundle different files named " + name;
            return false;
        }
        total += bytes.size();
        if (total > maxExpandedBytes) {
            out.error = "Expanded mods exceed the size limit";
            return false;
        }
        QFile file(libsDir + '/' + name);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(bytes) != bytes.size()) {
            out.error = "Could not write " + name;
            return false;
        }
        file.close();
        written.insert(name, hash);
        QZipReader zip(libsDir + '/' + name);
        if (zip.status() != QZipReader::NoError) {
            out.error = name + " is not a readable jar";
            return false;
        }
        const auto metadata = zip.fileData("fabric.mod.json");
        if (metadata.isEmpty()) {
            out.libraries << name;
            return true;
        }
        out.mods << name;
        if (depth >= maxNestingDepth) {
            out.error = "Mods are nested too deeply";
            return false;
        }
        const auto jars = QJsonDocument::fromJson(metadata).object().value("jars").toArray();
        for (const auto &value : jars) {
            const auto entry = value.toObject().value("file").toString();
            if (!entry.startsWith("META-INF/jars/") || entry.contains("..") ||
                entry.contains('\\')) {
                out.error = name + " declares a nested jar outside META-INF/jars";
                return false;
            }
            const auto nestedName = entry.section('/', -1);
            bool found = false;
            for (const auto &info : zip.fileInfoList())
                if (info.filePath == entry && info.isFile) {
                    found = true;
                    if (info.size > maxNestedJarBytes) {
                        out.error = "A nested jar in " + name + " is too large";
                        return false;
                    }
                }
            if (!found) {
                out.error = name + " declares a missing nested jar " + nestedName;
                return false;
            }
            if (!add(nestedName, zip.fileData(entry), depth + 1))
                return false;
        }
        return true;
    };
    for (const auto &jar : verifiedJars) {
        QFile file(jar);
        if (!file.open(QIODevice::ReadOnly)) {
            out.error = "Cannot read " + QFileInfo(jar).fileName();
            return out;
        }
        if (!add(QFileInfo(jar).fileName(), file.readAll(), 0))
            return out;
    }
    out.mods.sort();
    out.libraries.sort();
    return out;
}

QByteArray settingsGradle() {
    return "// Generated by Nova Studio. Nova verifies this file before running Gradle.\n"
           "pluginManagement {\n"
           "\trepositories {\n"
           "\t\tmaven {\n"
           "\t\t\tname = 'Fabric'\n"
           "\t\t\turl = 'https://maven.fabricmc.net/'\n"
           "\t\t}\n"
           "\t\tmavenCentral()\n"
           "\t\tgradlePluginPortal()\n"
           "\t}\n"
           "}\n"
           "rootProject.name = 'nova-fabric-runtime'\n";
}

QByteArray gradleProperties(const FabricProfile &, const QString &gameJavaHome) {
    QString text = "# Generated by Nova Studio. Nova verifies this file before running Gradle.\n"
                   "org.gradle.jvmargs=-Xmx1G\n"
                   "org.gradle.daemon=false\n"
                   "org.gradle.configuration-cache=false\n"
                   "org.gradle.java.installations.auto-detect=false\n"
                   "org.gradle.java.installations.auto-download=false\n";
    text += "org.gradle.java.installations.paths=" +
            propertyValue(QDir::fromNativeSeparators(gameJavaHome)) + '\n';
    return text.toLatin1();
}

QByteArray wrapperProperties(const FabricProfile &profile) {
    return QString("distributionBase=GRADLE_USER_HOME\n"
                   "distributionPath=wrapper/dists\n"
                   "distributionUrl=https\\://services.gradle.org/distributions/gradle-%1-bin.zip\n"
                   "distributionSha256Sum=%2\n"
                   "networkTimeout=10000\n"
                   "validateDistributionUrl=true\n"
                   "zipStoreBase=GRADLE_USER_HOME\n"
                   "zipStorePath=wrapper/dists\n")
        .arg(profile.gradleVersion, profile.gradleDistributionSha256)
        .toLatin1();
}

QByteArray buildGradle(const FabricProfile &profile, const QString &runDirRelative,
                       const QStringList &mods, const QStringList &libraries) {
    auto list = [](const QStringList &names) {
        QStringList quoted;
        for (const auto &name : names)
            quoted << "\t\t'libs/" + name + "'";
        return quoted.join(",\n");
    };
    QString text =
        QString(R"(// Generated by Nova Studio. Nova verifies this file before running Gradle.
plugins {
	id '%1' version '%2'
}

java {
	toolchain {
		languageVersion = JavaLanguageVersion.of(%3)
	}
}

loom {
	runs {
		client {
			runDir = '%4'
			if (project.hasProperty('%5')) {
				programArgs '--quickPlaySingleplayer', project.property('%5').toString()
			}
		}
	}
}

dependencies {
	minecraft "com.mojang:minecraft:%6"
	mappings loom.officialMojangMappings()
	modImplementation "net.fabricmc:fabric-loader:%7"
)")
            .arg(profile.loomPluginId, profile.loomVersion)
            .arg(profile.gameJavaMajor)
            .arg(runDirRelative, quickPlayProperty, profile.minecraftVersion,
                 profile.loaderVersion);
    if (!mods.isEmpty())
        text += "\tmodLocalRuntime files(\n" + list(mods) + ")\n";
    if (!libraries.isEmpty())
        text += "\truntimeOnly files(\n" + list(libraries) + ")\n";
    text += QString(R"(}

// Resolves everything runClient needs (assets, natives, run configuration and
// the runtime classpath) without starting Minecraft, so launches can be offline.
tasks.register('%1') {
	description = 'Prepares the Nova Studio test runtime without launching the game.'
	dependsOn 'configureClientLaunch'
	inputs.files(sourceSets.main.runtimeClasspath)
	doLast {}
}
)")
                .arg(prepareTask);
    return text.toUtf8();
}

WorkspaceResult writeWorkspace(const FabricProfile &profile, const RuntimeLayout &layout,
                               const WorkspaceInputs &inputs) {
    WorkspaceResult result;
    auto fail = [&](const QString &message) {
        result.error = message;
        return result;
    };
    const auto libs = layout.workspace + "/libs";
    for (const auto &path : {layout.workspace + "/gradle/wrapper", layout.game, libs,
                             layout.workspace + '/' + workspaceManifest})
        if (!safeRuntimePath(path))
            return fail("Unsafe runtime workspace path: " + path);
    if (!QDir().mkpath(layout.workspace + "/gradle/wrapper") || !QDir().mkpath(layout.game))
        return fail("Could not create the runtime folders");
    // libs/ is regenerated from verified inputs every time.
    if (!safeRuntimeTree(libs) || (QFileInfo::exists(libs) && !QDir(libs).removeRecursively()))
        return fail("Could not reset the runtime libraries folder");
    if (!QDir().mkpath(libs))
        return fail("Could not create the runtime libraries folder");
    for (const auto &jar : inputs.verifiedMods)
        if (!safeRuntimePath(jar))
            return fail("Unsafe runtime artifact path: " + jar);
    const auto expanded = expandModJars(inputs.verifiedMods, libs);
    if (!expanded.error.isEmpty())
        return fail(expanded.error);
    QFile wrapper(profile.wrapperResource);
    if (!wrapper.open(QIODevice::ReadOnly))
        return fail("Nova's Gradle wrapper is missing from this build");
    const auto wrapperBytes = wrapper.readAll();
    if (sha256(wrapperBytes) != profile.gradleWrapperJarSha256)
        return fail("Nova's Gradle wrapper does not match Gradle's published checksum");
    const auto runDir = QDir(layout.workspace).relativeFilePath(layout.game);
    const QList<std::pair<QString, QByteArray>> files = {
        {"settings.gradle", settingsGradle()},
        {"gradle.properties", gradleProperties(profile, inputs.gameJavaHome)},
        {"build.gradle", buildGradle(profile, runDir, expanded.mods, expanded.libraries)},
        {"gradle/wrapper/gradle-wrapper.properties", wrapperProperties(profile)},
        {"gradle/wrapper/gradle-wrapper.jar", wrapperBytes},
    };
    QJsonObject hashes;
    for (const auto &[relative, bytes] : files) {
        const auto path = layout.workspace + '/' + relative;
        QString error;
        if (!safeRuntimePath(path) ||
            !atomicWrite(path, bytes, Revision::read(path, 64LL * 1024 * 1024), &error))
            return fail("Could not write " + relative +
                        (error.isEmpty() ? QString() : ": " + error));
        hashes.insert(relative, sha256(bytes));
    }
    for (const auto &name : expanded.mods + expanded.libraries)
        hashes.insert("libs/" + name, fileSha256(libs + '/' + name));
    const QJsonObject manifest{{"schema", 1}, {"profile", profile.key}, {"files", hashes}};
    const auto manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    QString error;
    const auto manifestPath = layout.workspace + '/' + workspaceManifest;
    if (!atomicWrite(manifestPath, manifestBytes, Revision::read(manifestPath), &error))
        return fail("Could not record the workspace manifest: " + error);
    result.ok = true;
    result.manifestHash = sha256(QJsonDocument(hashes).toJson(QJsonDocument::Compact));
    return result;
}

WorkspaceResult verifyWorkspace(const FabricProfile &profile, const RuntimeLayout &layout,
                                const QString &gameJavaHome) {
    WorkspaceResult result;
    auto fail = [&](const QString &message) {
        result.error = message;
        return result;
    };
    const auto homeError = checkGradleHome(layout);
    if (!homeError.isEmpty())
        return fail(homeError);
    if (!verifyGradleDistribution(profile, layout))
        return fail("The cached Gradle distribution differs from Nova's pinned release");
    const auto manifestPath = layout.workspace + '/' + workspaceManifest;
    if (!safeRuntimePath(manifestPath))
        return fail("Unsafe runtime workspace manifest path");
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly))
        return fail("The runtime workspace has not been prepared");
    const auto manifest = QJsonDocument::fromJson(manifestFile.read(4 * 1024 * 1024)).object();
    if (manifest.value("schema").toInt() != 1 ||
        manifest.value("profile").toString() != profile.key)
        return fail("The runtime workspace belongs to a different configuration");
    const auto hashes = manifest.value("files").toObject();
    const QHash<QString, QString> fixed = {
        {"settings.gradle", sha256(settingsGradle())},
        {"gradle/wrapper/gradle-wrapper.properties", sha256(wrapperProperties(profile))},
        {"gradle/wrapper/gradle-wrapper.jar", profile.gradleWrapperJarSha256}
    };
    for (const auto &required : {"settings.gradle", "build.gradle", "gradle.properties",
                                "gradle/wrapper/gradle-wrapper.properties",
                                "gradle/wrapper/gradle-wrapper.jar"})
        if (!hashes.contains(required))
            return fail(QString("Missing trusted workspace file: ") + required);

    QStringList mods, libraries;
    for (auto it = hashes.begin(); it != hashes.end(); ++it) {
        const auto name = it.key();
        const auto path = layout.workspace + '/' + name;
        const bool library = name.startsWith("libs/") && safeJarName(name.mid(5));
        if ((!library && !fixed.contains(name) && name != "build.gradle" &&
             name != "gradle.properties") || !safeRuntimePath(path) ||
            !QFileInfo(path).isFile() || fileSha256(path) != it.value().toString())
            return fail("Runtime file changed outside Nova: " + name);
        if (fixed.contains(name) && fixed.value(name) != it.value().toString())
            return fail("Runtime file differs from Nova's pinned configuration: " + name);
        if (library) {
            QZipReader zip(path);
            if (zip.status() != QZipReader::NoError)
                return fail("Unreadable runtime library: " + name);
            (zip.fileData("fabric.mod.json").isEmpty() ? libraries : mods) << name.mid(5);
        }
    }
    mods.sort();
    libraries.sort();
    const auto runDir = QDir(layout.workspace).relativeFilePath(layout.game);
    if (hashes.value("build.gradle").toString() !=
        sha256(buildGradle(profile, runDir, mods, libraries)))
        return fail("build.gradle differs from Nova's generated build configuration");
    if (!gameJavaHome.isEmpty() && hashes.value("gradle.properties").toString() !=
        sha256(gradleProperties(profile, gameJavaHome)))
        return fail("gradle.properties differs from Nova's generated JVM configuration");
    // Anything able to add build logic must be absent.
    for (const QString relative : {"buildSrc", "settings.gradle.kts", "build.gradle.kts",
                                    "init.gradle", "init.gradle.kts", "gradle/libs.versions.toml",
                                    "gradle/gradle-daemon-jvm.properties", "gradle/init.d",
                                    "gradle.lockfile"})
        if (!safeRuntimePath(layout.workspace + '/' + relative) ||
            QFileInfo::exists(layout.workspace + '/' + relative))
            return fail("Unexpected build configuration in the runtime workspace: " + relative);

    QDirIterator entries(layout.workspace, QDir::AllEntries | QDir::NoDotAndDotDot |
                                             QDir::Hidden | QDir::System,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const auto info = entries.nextFileInfo();
        const auto relative = QDir(layout.workspace).relativeFilePath(info.filePath());
        if (!safeRuntimePath(info.absoluteFilePath()))
            return fail("Unsafe path in the runtime workspace: " + relative);
        const auto suffix = info.suffix().toLower();
        if (!hashes.contains(relative) &&
            (relative.startsWith("libs/", Qt::CaseInsensitive) ||
             (info.isFile() && (suffix == "gradle" || suffix == "kts"))))
            return fail("Unexpected build script or runtime library: " + relative);
    }
    result.ok = true;
    result.manifestHash = sha256(QJsonDocument(hashes).toJson(QJsonDocument::Compact));
    return result;
}

} // namespace nova::testing
