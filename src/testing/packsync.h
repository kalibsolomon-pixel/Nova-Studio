// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Safe hand-off of the current resource pack to a Minecraft game directory.
//
// The pack is exported (Project::exportMinecraft, which refuses unsaved or
// unreconciled native documents) into a Nova-owned staging folder that
// Minecraft never scans, then swapped into <gameDir>/resourcepacks with
// directory renames, so Minecraft can only ever observe the previous complete
// export or the new complete export. Only folders carrying Nova's marker are
// ever replaced or removed.
#include <QDateTime>
#include <QString>

namespace nova::testing {

inline constexpr auto testPackMarker = "nova-test-pack.json";
inline constexpr auto launcherProfileKey = "nova-studio-test";

QString testPackFolderName(const QString &packName);
QString novaTestGameDirectory(const QString &localAppData);
QString novaStagingRoot(const QString &localAppData);

struct TestPackState {
    bool present = false, novaOwned = false;
    QDateTime syncedAt;
    QString sourceIdentity;
};
TestPackState inspectTestPack(const QString &gameDirectory, const QString &folderName);

struct SyncOutcome {
    bool ok = false, replaced = false;
    QString error, folderName, packPath;
    QDateTime syncedAt;
};
// beforeSwap is a test hook run after staging completes and before the swap.
SyncOutcome syncTestPack(const QString &projectRoot, const QString &packName,
                         const QString &gameDirectory, const QString &stagingRoot,
                         const std::function<void()> &beforeSwap = {});

// options.txt: enables "file/<folder>" in resourcePacks, preserving every
// other line. Used only for Nova's own isolated test game directory.
QByteArray mergeResourcePackOption(const QByteArray &options, const QString &folderName,
                                   bool *changed = nullptr);
bool enableTestPackOption(const QString &gameDirectory, const QString &folderName,
                          QString *error = nullptr);

// launcher_profiles.json: adds or updates only Nova's own installation entry,
// preserving all other profiles, settings and unknown fields.
QByteArray mergeLauncherProfile(const QByteArray &profiles, const QString &gameDirectory,
                                const QString &versionId, QString *error = nullptr);
struct ProfileOutcome {
    bool ok = false;
    QString error, backupPath;
};
ProfileOutcome installLauncherProfile(const QString &minecraftDirectory,
                                      const QString &gameDirectory, const QString &versionId);
bool launcherProfilePresent(const QString &minecraftDirectory, QString *gameDirectory = nullptr,
                            QString *versionId = nullptr);

} // namespace nova::testing
