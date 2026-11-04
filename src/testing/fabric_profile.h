// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Pinned, known-good component sets for Nova's Fabric development runtime.
// Every artifact Nova downloads itself carries a size and a hash from its
// publisher's metadata; nothing is resolved as "latest" at run time. Adding a
// Minecraft version means adding one profile here, not changing the manager.
#include <QList>
#include <QString>

namespace nova::testing {

struct PinnedArtifact {
    QString id;   // stable short name ("rrls", "temurin-21")
    QString name; // human name
    QString version;
    QString fileName;
    QString url;       // exact HTTPS download URL from the publisher
    QString algorithm; // "sha256" or "sha512"
    QString hash;      // lowercase hex
    qint64 size = -1;
    QString source; // publisher / distribution channel
    QString license;
};

struct FabricProfile {
    QString key;               // cache key: all pins that change the workspace
    QString minecraftVersion;  // e.g. 1.21.11
    int gameJavaMajor = 0;     // from Mojang's version manifest (exact)
    int gradleJavaMinimum = 0; // the Loom plugin's minimum JVM for Gradle
    QString loaderVersion;
    QString loomPluginId; // remapping plugin for obfuscated versions
    QString loomVersion;
    QString gradleVersion;
    QString gradleDistributionSha256, gradleDistributionTreeSha256;
    QString gradleWrapperJarSha256;
    QString wrapperResource;    // Qt resource path of the verified wrapper jar
    QList<PinnedArtifact> mods; // installed into <game>/mods
    PinnedArtifact gameJdk;     // provisioned when no local JDK matches
    PinnedArtifact gradleJdk;
};

const QList<FabricProfile> &fabricProfiles();
const FabricProfile *fabricProfile(const QString &minecraftVersion);
QString defaultMinecraftVersion();

} // namespace nova::testing
