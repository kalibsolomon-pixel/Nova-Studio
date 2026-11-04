// SPDX-License-Identifier: GPL-3.0-or-later
#include "java_locator.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

namespace nova::testing {

int javaMajor(const QString &version) {
    const auto parts = version.split('.');
    bool ok = false;
    int major = parts.value(0).toInt(&ok);
    if (ok && major == 1)
        major = parts.value(1).toInt(&ok);
    return ok ? major : 0;
}

std::optional<JavaHome> inspectJavaHome(const QString &path, const QString &origin) {
    const QString home = QDir::fromNativeSeparators(QDir::cleanPath(path));
    QFile release(home + "/release");
    if (home.isEmpty() || QFileInfo(release).isSymLink() || !release.open(QIODevice::ReadOnly))
        return std::nullopt;
    if (!QFileInfo(home + "/bin/java.exe").isFile())
        return std::nullopt;
    JavaHome java;
    java.path = home;
    java.origin = origin;
    static const QRegularExpression line("^([A-Z_]+)=\"?([^\"]*)\"?$");
    for (const auto &raw : QString::fromUtf8(release.read(64 * 1024)).split('\n')) {
        const auto match = line.match(raw.trimmed());
        if (!match.hasMatch())
            continue;
        if (match.captured(1) == "JAVA_VERSION")
            java.version = match.captured(2);
        else if (match.captured(1) == "IMPLEMENTOR")
            java.vendor = match.captured(2);
    }
    java.major = javaMajor(java.version);
    java.jdk = QFileInfo(home + "/bin/javac.exe").isFile();
    if (java.major <= 0)
        return std::nullopt;
    return java;
}

QList<JavaHome> discoverJavaHomes(const Environment &environment, const QString &managedRoot,
                                  bool includeEnvironment) {
    QList<JavaHome> homes;
    QStringList seen;
    auto add = [&](const QString &path, const QString &origin) {
        if (auto java = inspectJavaHome(path, origin);
            java && !seen.contains(java->path, Qt::CaseInsensitive)) {
            seen << java->path;
            homes << *java;
        }
    };
    auto scan = [&](const QString &root, const QString &origin) {
        if (root.isEmpty())
            return;
        for (const auto &entry : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (!entry.isSymLink())
                add(entry.absoluteFilePath(), origin);
    };
    scan(managedRoot, "managed");
    if (includeEnvironment)
        add(qEnvironmentVariable("JAVA_HOME"), "JAVA_HOME");
    for (const auto &vendor :
         {"Eclipse Adoptium", "Java", "Microsoft", "Zulu", "Amazon Corretto", "BellSoft", "Semeru"})
        scan(environment.programFiles + '/' + vendor, "installed");
    return homes;
}

std::optional<JavaHome> chooseJava(const QList<JavaHome> &homes, int major, bool exact) {
    QList<JavaHome> suitable;
    for (const auto &java : homes)
        if (java.jdk && (exact ? java.major == major : java.major >= major))
            suitable << java;
    if (suitable.isEmpty())
        return std::nullopt;
    std::stable_sort(suitable.begin(), suitable.end(), [](const JavaHome &a, const JavaHome &b) {
        const bool am = a.origin == "managed" || a.origin == "chosen";
        const bool bm = b.origin == "managed" || b.origin == "chosen";
        if (am != bm)
            return am;
        return a.version > b.version;
    });
    return suitable.first();
}

} // namespace nova::testing
