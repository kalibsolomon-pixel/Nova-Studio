// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Nova-owned Fabric Loom workspace: generation, nested-jar expansion and
// verification. The workspace is executable code (Gradle runs it), so Nova
// records a hash for every file it writes and refuses to run anything it did
// not write.
#include "fabric_profile.h"
#include <QJsonObject>
#include <QStringList>

namespace nova::testing {

struct RuntimeLayout {
    QString base;       // e.g. %LOCALAPPDATA%/NovaStudio/fabric-runtime
    QString workspace;  // base/workspaces/<profile key>
    QString game;       // base/instances/<minecraft version>: saves, options, packs, logs
    QString gradleHome; // base/gradle-home (shared Gradle and Loom caches)
    QString javaRoot;   // base/java (Nova-provisioned JDKs)
    QString downloads;  // base/downloads (verified artifacts)
    QString logs;       // base/logs (Gradle output)
    QString metadata;   // base/runtime.json
    static RuntimeLayout make(const QString &base, const FabricProfile &profile);
    static QString defaultBase(const QString &localAppData);
};

struct WorkspaceInputs {
    QString gameJavaHome;     // JDK for Minecraft (exact profile major)
    QStringList verifiedMods; // absolute paths, already hash-verified
};

// Recursively extracts the jars a Fabric mod declares in fabric.mod.json
// "jars" (Fabric's jar-in-jar). Loom strips nested jars from file
// dependencies, so Nova lists every module explicitly instead.
struct ExpandedMods {
    QStringList mods, libraries; // file names inside libsDir
    QString error;
};
ExpandedMods expandModJars(const QStringList &verifiedJars, const QString &libsDir);

QByteArray buildGradle(const FabricProfile &profile, const QString &runDirRelative,
                       const QStringList &mods, const QStringList &libraries);
QByteArray gradleProperties(const FabricProfile &profile, const QString &gameJavaHome);
QByteArray settingsGradle();
QByteArray wrapperProperties(const FabricProfile &profile);

struct WorkspaceResult {
    bool ok = false;
    QString error;
    QString manifestHash; // hash of the manifest (cache key for prepared state)
};
WorkspaceResult writeWorkspace(const FabricProfile &profile, const RuntimeLayout &layout,
                               const WorkspaceInputs &inputs);
// Checks every manifest file's hash and that nothing able to inject build
// logic was added (buildSrc, init scripts, Kotlin/TOML build files).
WorkspaceResult verifyWorkspace(const FabricProfile &profile, const RuntimeLayout &layout,
                                const QString &gameJavaHome = {});
bool safeRuntimePath(const QString &path);
bool safeRuntimeTree(const QString &path);
// Windows NIO binds short-lived Unix-domain socket files. Their full path must fit
// in sun_path (108 bytes), which a long or virtualized profile path can exceed.
// Returns the checked, short per-user directory (empty on refusal, with the reason
// in *error) and removes stale socket files left by terminated JVMs. Only socket_*
// entries are touched.
inline constexpr int socketDirectoryMaxBytes = 88;
QString socketDirectoryFor(const QString &localAppData);
QString prepareSocketDirectory(const QString &localAppData, int *removed = nullptr,
                               int staleSeconds = 600, QString *error = nullptr);
// <profile>\AppData\Local of the user running Nova, read from the process token,
// or empty when that profile has no Temp folder.
QString profileLocalAppData();
// Prepares the socket directory under localAppData. Only when that path is too long
// does it use the same folder under fallbackLocalAppData instead.
QString chooseSocketDirectory(const QString &localAppData, const QString &fallbackLocalAppData,
                              QString *error = nullptr);
QString checkGradleHome(const RuntimeLayout &layout);

inline constexpr auto workspaceManifest = ".nova-workspace.json";
inline constexpr auto prepareTask = "novaPrepareRuntime";
inline constexpr auto quickPlayProperty = "novaQuickPlay";

} // namespace nova::testing
