// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Read-only discovery of launchers that can sign in and start Minecraft for
// resource-pack testing. Nova never authenticates, never reads launcher
// account files and never sees Microsoft or Minecraft tokens: the official
// Minecraft Launcher or Aurora Launcher performs sign-in, the Java Edition
// ownership check and the authenticated launch.
#include <QList>
#include <QString>

namespace nova::testing {

// Directory roots discovery reads. Injectable so tests use fixture folders.
struct Environment {
    QString localAppData, roamingAppData, programFiles, programFilesX86;
    static Environment current();
};

enum class LauncherKind { Official, OfficialStore, Aurora };

struct Launcher {
    LauncherKind kind = LauncherKind::Official;
    QString label;      // shown to the user
    QString executable; // empty for the Store launcher (started through its app id)
    QString dataRoot;   // Aurora managed-data root; official: the .minecraft folder
};

// The official launcher's Microsoft Store application identity.
inline constexpr auto officialStorePackage = "Microsoft.4297127D64EC6_8wekyb3d8bbwe";
inline constexpr auto officialStoreAppId = "Microsoft.4297127D64EC6_8wekyb3d8bbwe!Minecraft";

QList<Launcher> discoverLaunchers(const Environment &environment);
QString officialMinecraftDirectory(const Environment &environment);

// One Aurora instance from Aurora's non-secret instances.json registry.
struct AuroraInstance {
    QString id, displayName, minecraftVersion, platform, state;
    bool modpackManaged = false;
    QString gameDirectory; // the instance root Minecraft uses as --gameDir
};

// Parses <dataRoot>/launcher/instances.json. Unknown schema versions and
// malformed entries are rejected rather than guessed. Never writes.
QList<AuroraInstance> readAuroraInstances(const QString &dataRoot, QString *error = nullptr);
QList<AuroraInstance> parseAuroraInstances(const QByteArray &json, const QString &dataRoot,
                                           QString *error = nullptr);

// Whether a launcher process is running, by image name only (command lines
// carry access tokens and are never read).
bool processRunning(const QStringList &imageNames);

} // namespace nova::testing
