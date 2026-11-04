// SPDX-License-Identifier: GPL-3.0-or-later
#include "packsync.h"
#include "project.h"
#include "revision.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStorageInfo>

namespace nova::testing {
namespace {
bool linked(const QString &path) {
    const QFileInfo info(path);
    return info.isSymLink() || info.isJunction();
}
QString token() {
    return QString::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0');
}
bool markerIsNova(const QString &folder, TestPackState *state = nullptr) {
    QFile marker(folder + '/' + testPackMarker);
    if (linked(marker.fileName()) || !marker.open(QIODevice::ReadOnly))
        return false;
    const auto o = QJsonDocument::fromJson(marker.read(16 * 1024)).object();
    if (o.value("novaTestPack").toInt() != 1)
        return false;
    if (state) {
        state->syncedAt = QDateTime::fromString(o.value("syncedAt").toString(), Qt::ISODate);
        state->sourceIdentity = o.value("source").toString();
    }
    return true;
}
bool sameVolume(const QString &a, const QString &b) {
    const QStorageInfo first(a), second(b);
    return first.isValid() && second.isValid() && first.rootPath() == second.rootPath();
}
// Removes leftovers of earlier interrupted syncs: only Nova-named children of
// the Nova-owned staging root.
void sweepStaging(const QString &stagingRoot) {
    static const QRegularExpression rule("^sync-[0-9a-f]{16}$");
    for (const auto &entry : QDir(stagingRoot).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (rule.match(entry.fileName()).hasMatch() && !linked(entry.absoluteFilePath()))
            QDir(entry.absoluteFilePath()).removeRecursively();
}
} // namespace

QString testPackFolderName(const QString &packName) {
    QString clean;
    for (const auto ch : packName.trimmed())
        clean += (ch.isLetterOrNumber() || QString(" -_.()").contains(ch)) ? ch : QChar('_');
    clean = clean.simplified().left(48);
    while (clean.endsWith('.') || clean.endsWith(' '))
        clean.chop(1);
    return "Nova Test - " + (clean.isEmpty() ? QString("Pack") : clean);
}
QString novaTestGameDirectory(const QString &localAppData) {
    return localAppData + "/NovaStudio/minecraft-test";
}
QString novaStagingRoot(const QString &localAppData) {
    return localAppData + "/NovaStudio/test-staging";
}

TestPackState inspectTestPack(const QString &gameDirectory, const QString &folderName) {
    TestPackState state;
    const auto folder = gameDirectory + "/resourcepacks/" + folderName;
    state.present = QFileInfo(folder).isDir();
    state.novaOwned = state.present && !linked(folder) && markerIsNova(folder, &state);
    return state;
}

SyncOutcome syncTestPack(const QString &projectRoot, const QString &packName,
                         const QString &gameDirectory, const QString &requestedStaging,
                         const std::function<void()> &beforeSwap) {
    SyncOutcome out;
    out.folderName = testPackFolderName(packName);
    auto fail = [&](const QString &message) {
        out.error = message;
        return out;
    };
    if (!QFileInfo(gameDirectory).isDir() || linked(gameDirectory))
        return fail("The Minecraft game folder is missing or is a link");
    const auto packs = gameDirectory + "/resourcepacks";
    if (linked(packs))
        return fail("The resourcepacks folder is a link; Nova will not write through it");
    if (!QDir().mkpath(packs))
        return fail("Could not create the resourcepacks folder");
    const auto target = packs + '/' + out.folderName;
    out.packPath = target;
    const bool existed = QFileInfo::exists(target);
    if (existed && (linked(target) || !QFileInfo(target).isDir() || !markerIsNova(target)))
        return fail("\"" + out.folderName +
                    "\" already exists in resourcepacks and was not created by Nova. Rename or "
                    "move it, then sync again.");
    // Stage on the same volume so the final step is a rename, never a copy.
    QString stagingRoot = requestedStaging;
    if (!QDir().mkpath(stagingRoot) || !sameVolume(stagingRoot, packs)) {
        stagingRoot = gameDirectory + "/.nova-staging";
        if (!QDir().mkpath(stagingRoot))
            return fail("Could not create a staging folder");
    }
    sweepStaging(stagingRoot);
    const auto work = stagingRoot + "/sync-" + token();
    if (!QDir().mkpath(work))
        return fail("Could not create a staging folder");
    auto cleanup = [&] { QDir(work).removeRecursively(); };
    const auto staged = work + "/pack";
    QString error;
    if (!Project::exportMinecraft(projectRoot, staged, &error)) {
        cleanup();
        return fail(error.isEmpty() ? "The pack could not be exported" : error);
    }
    out.syncedAt = QDateTime::currentDateTimeUtc();
    const QJsonObject marker{{"novaTestPack", 1},
                             {"source", pathIdentity(projectRoot)},
                             {"syncedAt", out.syncedAt.toString(Qt::ISODateWithMs)}};
    QFile markerFile(staged + '/' + testPackMarker);
    if (!markerFile.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        markerFile.write(QJsonDocument(marker).toJson()) < 0 || !markerFile.flush()) {
        cleanup();
        return fail("Could not record the test-pack marker");
    }
    markerFile.close();
    if (beforeSwap)
        beforeSwap();
    const auto retired = work + "/retired";
    if (existed && !QDir().rename(target, retired)) {
        cleanup();
        return fail("Minecraft or another program is using the current test pack, so it was "
                    "left unchanged. Leave the Resource Packs screen or close Minecraft, then "
                    "sync again.");
    }
    if (!QDir().rename(staged, target)) {
        if (existed && !QDir().rename(retired, target)) {
            out.error = "The new export could not be placed and the previous test pack is kept "
                        "at " +
                        QDir::toNativeSeparators(retired);
            return out; // keep work: it holds the user's previous pack
        }
        cleanup();
        return fail("The new export could not be placed; the previous test pack is unchanged");
    }
    cleanup(); // removes the retired previous export
    QDir(stagingRoot).rmdir(".");
    out.ok = true;
    out.replaced = existed;
    return out;
}

QByteArray mergeResourcePackOption(const QByteArray &options, const QString &folderName,
                                   bool *changed) {
    const QString entry = "file/" + folderName;
    auto lines = QString::fromUtf8(options).split('\n');
    bool found = false, modified = false;
    for (auto &line : lines) {
        const bool cr = line.endsWith('\r');
        QString body = cr ? line.chopped(1) : line;
        if (!body.startsWith("resourcePacks:"))
            continue;
        found = true;
        auto array = QJsonDocument::fromJson(body.mid(14).toUtf8()).array();
        if (!array.contains(entry)) {
            if (array.isEmpty())
                array.append("vanilla");
            array.append(entry);
            body = "resourcePacks:" +
                   QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
            line = cr ? body + '\r' : body;
            modified = true;
        }
    }
    if (!found) {
        if (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
        lines.append(
            "resourcePacks:" +
            QString::fromUtf8(
                QJsonDocument(QJsonArray{"vanilla", entry}).toJson(QJsonDocument::Compact)));
        lines.append(QString());
        modified = true;
    }
    if (changed)
        *changed = modified;
    return lines.join('\n').toUtf8();
}

bool enableTestPackOption(const QString &gameDirectory, const QString &folderName, QString *error) {
    const auto path = gameDirectory + "/options.txt";
    if (linked(path)) {
        if (error)
            *error = "options.txt is a link";
        return false;
    }
    const auto revision = Revision::read(path, 1024 * 1024);
    QByteArray current;
    if (revision.exists) {
        QFile file(path);
        if (!revision.readable || !file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = "options.txt cannot be read";
            return false;
        }
        current = file.readAll();
    }
    bool changed = false;
    const auto merged = mergeResourcePackOption(current, folderName, &changed);
    return !changed || atomicWrite(path, merged, revision, error);
}

QByteArray mergeLauncherProfile(const QByteArray &profiles, const QString &gameDirectory,
                                const QString &versionId, QString *error) {
    QJsonParseError parse;
    auto document = QJsonDocument::fromJson(profiles, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object().value("profiles").isObject()) {
        if (error)
            *error = "The launcher's installation list is not in a recognised format";
        return {};
    }
    auto root = document.object();
    auto all = root.value("profiles").toObject();
    auto entry = all.value(launcherProfileKey).toObject();
    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    if (entry.isEmpty()) {
        entry.insert("created", now);
        entry.insert("icon", "Furnace");
        entry.insert("type", "custom");
    }
    entry.insert("name", "Nova Studio Test");
    entry.insert("gameDir", QDir::toNativeSeparators(gameDirectory));
    entry.insert("lastVersionId", versionId);
    if (!entry.contains("lastUsed"))
        entry.insert("lastUsed", now);
    all.insert(launcherProfileKey, entry);
    root.insert("profiles", all);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

ProfileOutcome installLauncherProfile(const QString &minecraftDirectory,
                                      const QString &gameDirectory, const QString &versionId) {
    ProfileOutcome out;
    const auto path = minecraftDirectory + "/launcher_profiles.json";
    if (!QFileInfo(path).isFile() || linked(path)) {
        out.error = "The Minecraft Launcher has not created its installation list yet. Open the "
                    "launcher once, close it, then try again.";
        return out;
    }
    const auto revision = Revision::read(path, 8 * 1024 * 1024);
    QFile file(path);
    if (!revision.readable || !file.open(QIODevice::ReadOnly)) {
        out.error = "The launcher's installation list cannot be read";
        return out;
    }
    const auto original = file.readAll();
    file.close();
    QString error;
    const auto merged = mergeLauncherProfile(original, gameDirectory, versionId, &error);
    if (merged.isEmpty()) {
        out.error = error;
        return out;
    }
    out.backupPath =
        path + ".nova-backup-" + QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmss");
    if (QFileInfo::exists(out.backupPath) || !QFile::copy(path, out.backupPath)) {
        out.error = "Could not back up the launcher's installation list";
        out.backupPath.clear();
        return out;
    }
    if (!atomicWrite(path, merged, revision, &error)) {
        out.error =
            error.isEmpty() ? "The installation list changed while Nova was updating it" : error;
        return out;
    }
    out.ok = true;
    return out;
}

bool launcherProfilePresent(const QString &minecraftDirectory, QString *gameDirectory,
                            QString *versionId) {
    QFile file(minecraftDirectory + "/launcher_profiles.json");
    if (linked(file.fileName()) || !file.open(QIODevice::ReadOnly))
        return false;
    const auto entry = QJsonDocument::fromJson(file.read(8 * 1024 * 1024))
                           .object()
                           .value("profiles")
                           .toObject()
                           .value(launcherProfileKey)
                           .toObject();
    if (entry.isEmpty())
        return false;
    if (gameDirectory)
        *gameDirectory = QDir::fromNativeSeparators(entry.value("gameDir").toString());
    if (versionId)
        *versionId = entry.value("lastVersionId").toString();
    return true;
}

} // namespace nova::testing
