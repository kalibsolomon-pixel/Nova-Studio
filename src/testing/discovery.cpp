// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <tlhelp32.h>
#endif

namespace nova::testing {
namespace {
QString env(const char *name) { return QDir::fromNativeSeparators(qEnvironmentVariable(name)); }
bool executable(const QString &path) {
    const QFileInfo info(path);
    return info.isFile() && !info.isSymLink() && info.size() > 0;
}
// Aurora's managed-data identities are reverse-DNS names under its own prefix.
bool validAuroraIdentity(const QString &identity) {
    static const QRegularExpression rule("^com\\.aurora\\.launcher(\\.[a-z0-9-]{1,48})?$");
    return rule.match(identity).hasMatch();
}
QString packageIdentity(const QString &installDir) {
    const auto ini = installDir + "/native-package.ini";
    if (!QFileInfo(ini).isFile() || QFileInfo(ini).size() > 4096)
        return {};
    const QSettings settings(ini, QSettings::IniFormat);
    const auto identity = settings.value("Package/Identity").toString().trimmed();
    return validAuroraIdentity(identity) ? identity : QString();
}
} // namespace

Environment Environment::current() {
    // Acceptance runs point discovery at disposable fixture folders so they
    // never read or write a real launcher's data.
    const auto fixture = env("NOVA_TEST_LAUNCHER_ROOT");
    if (!fixture.isEmpty() && QDir::isAbsolutePath(fixture))
        return {fixture + "/local", fixture + "/roaming", fixture + "/pf", fixture + "/pf86"};
    return {env("LOCALAPPDATA"), env("APPDATA"), env("ProgramFiles"), env("ProgramFiles(x86)")};
}

QString officialMinecraftDirectory(const Environment &e) {
    return e.roamingAppData.isEmpty() ? QString() : e.roamingAppData + "/.minecraft";
}

QList<Launcher> discoverLaunchers(const Environment &e) {
    QList<Launcher> found;
    QSet<QString> seen;
    // Official Minecraft Launcher: the standalone Win32 install first.
    for (const auto &root : {e.programFilesX86, e.programFiles}) {
        if (root.isEmpty())
            continue;
        const auto exe = root + "/Minecraft Launcher/MinecraftLauncher.exe";
        if (executable(exe) && !seen.contains(exe.toLower())) {
            seen.insert(exe.toLower());
            found.append(
                {LauncherKind::Official, "Minecraft Launcher", exe, officialMinecraftDirectory(e)});
        }
    }
    // Microsoft Store build: present when its package data folder exists.
    if (!e.localAppData.isEmpty() &&
        QFileInfo(e.localAppData + "/Packages/" + officialStorePackage).isDir())
        found.append({LauncherKind::OfficialStore,
                      "Minecraft Launcher (Microsoft Store)",
                      {},
                      officialMinecraftDirectory(e)});
    // Aurora Launcher builds installed per user by their own installers.
    struct Candidate {
        const char *folder, *exe, *identity, *label;
    };
    const Candidate aurora[] = {
        {"Aurora Launcher Native Preview", "aurora-native.exe", nullptr,
         "Aurora Launcher (Native)"},
        {"Aurora Launcher", "aurora-launcher.exe", "com.aurora.launcher", "Aurora Launcher"},
    };
    for (const auto &c : aurora) {
        for (const auto &base : {e.localAppData, e.programFiles}) {
            if (base.isEmpty())
                continue;
            const auto dir = base + '/' + c.folder;
            const auto exe = dir + '/' + c.exe;
            if (!executable(exe) || seen.contains(exe.toLower()))
                continue;
            const auto identity = c.identity ? QString(c.identity) : packageIdentity(dir);
            if (identity.isEmpty() || e.localAppData.isEmpty())
                continue;
            seen.insert(exe.toLower());
            found.append({LauncherKind::Aurora, c.label, exe, e.localAppData + '/' + identity});
        }
    }
    return found;
}

QList<AuroraInstance> parseAuroraInstances(const QByteArray &json, const QString &dataRoot,
                                           QString *error) {
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return QList<AuroraInstance>{};
    };
    if (json.size() > 4 * 1024 * 1024)
        return fail("Aurora's instance registry is unexpectedly large");
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return fail("Aurora's instance registry is not valid JSON");
    const auto root = document.object();
    const int schema = root.value("schemaVersion").toInt(-1);
    // Schemas 5 and 6 share the fields read here; newer ones are not guessed.
    if (schema < 5 || schema > 6)
        return fail(QString("Aurora instance registry schema %1 is not supported").arg(schema));
    static const QRegularExpression idRule("^[0-9a-f]{32}$");
    QList<AuroraInstance> result;
    for (const auto &value : root.value("instances").toArray()) {
        const auto o = value.toObject();
        AuroraInstance instance;
        instance.id = o.value("id").toString();
        if (!idRule.match(instance.id).hasMatch())
            continue; // never derive a path from an unvalidated identifier
        instance.displayName = o.value("displayName").toString().left(80);
        instance.state = o.value("state").toString();
        const auto installed = o.value("installed").toObject();
        instance.minecraftVersion = installed.value("minecraftVersion").toString().left(32);
        instance.platform =
            installed.value("platform").toObject().value("kind").toString("vanilla");
        instance.modpackManaged = o.value("pack").isObject();
        instance.gameDirectory = dataRoot + "/instances/" + instance.id;
        if (instance.displayName.isEmpty())
            instance.displayName = instance.id.left(8);
        result.append(instance);
    }
    if (error)
        error->clear();
    return result;
}

QList<AuroraInstance> readAuroraInstances(const QString &dataRoot, QString *error) {
    QFile file(dataRoot + "/launcher/instances.json");
    if (!file.exists()) {
        if (error)
            *error = "Aurora has no instances yet";
        return {};
    }
    if (QFileInfo(file).isSymLink() || !file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = "Aurora's instance registry cannot be read";
        return {};
    }
    return parseAuroraInstances(file.read(4 * 1024 * 1024 + 1), dataRoot, error);
}

bool processRunning(const QStringList &imageNames) {
#ifdef Q_OS_WIN
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !running;
         ok = Process32NextW(snapshot, &entry)) {
        const auto name = QString::fromWCharArray(entry.szExeFile);
        for (const auto &wanted : imageNames)
            if (name.compare(wanted, Qt::CaseInsensitive) == 0)
                running = true;
    }
    CloseHandle(snapshot);
    return running;
#else
    Q_UNUSED(imageNames);
    return false;
#endif
}

} // namespace nova::testing
