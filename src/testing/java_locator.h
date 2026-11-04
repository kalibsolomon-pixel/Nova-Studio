// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Finds Java Development Kits by reading each installation's `release` file
// (never by executing an unknown binary). Requirements are exact where the
// game declares them (Minecraft's version manifest) and minimums where a tool
// does (Fabric Loom's Gradle JVM).
#include "discovery.h"
#include <QList>
#include <QString>
#include <optional>

namespace nova::testing {

struct JavaHome {
    QString path;
    int major = 0;
    QString version, vendor, origin; // origin: "managed", "JAVA_HOME", "installed", "chosen"
    bool jdk = false;                // has javac
};

std::optional<JavaHome> inspectJavaHome(const QString &path, const QString &origin = {});
int javaMajor(const QString &version); // "21.0.13" -> 21, "1.8.0_402" -> 8
QList<JavaHome> discoverJavaHomes(const Environment &environment, const QString &managedRoot,
                                  bool includeEnvironment = true);
// Prefers Nova-managed homes, then the newest suitable installation.
std::optional<JavaHome> chooseJava(const QList<JavaHome> &homes, int major, bool exact);

} // namespace nova::testing
