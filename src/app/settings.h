// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QRect>
#include <QStringList>

namespace nova {
struct Preferences {
    bool reopenLast = true;
    bool dark = true, gridMode = false, pixelGrid = true;
    int thumbnailSize = 72, historyMiB = 16;
    QString accent = "violet";
    QStringList recentProjects;
    QString projectPath, directory, selected, query, activeDocument;
    QStringList openDocuments;
    QByteArray geometry, layout;
    QMap<QString, QByteArray> layouts; // named workspace layouts
    // Per workspace preset (id -> packed layout): the user's own arrangement of that
    // workspace, restored when switching back to it. Absent: the product default.
    QMap<QString, QByteArray> presetLayouts;
    QStringList recentColors;
    QString activePalette;
    QByteArray toolState;              // JSON tool settings
    bool timelineVisible = true;
    QByteArray animationView;
    QString workspacePreset = "essentials"; // Phase 3-B task workspace
    bool showTips = true;                   // inspector usage hints
    QString minecraftTestTarget;            // Phase 3-D launcher target key (no credentials)
    // Phase 3-D.1 Fabric development runtime.
    QString fabricMinecraftVersion = "1.21.11";
    QString fabricGameJava;   // empty = automatic
    QString fabricLocation;   // empty = %LOCALAPPDATA%/NovaStudio/fabric-runtime
    bool fabricDetailedLogs = false;
    bool fabricOpenTestWorld = true;
    bool experimentalAgentSession = false; // shows Resources › Agent Session (ACP, experimental)
};
class Settings {
    QString path;

  public:
    explicit Settings(QString file = {});
    QString fileName() const { return path; }
    Preferences load() const;
    bool save(const Preferences &preferences) const;
    QJsonObject projectPreview(const QString &project) const;
    bool saveProjectPreview(const QString &project, const QJsonObject &state) const;
    static QRect visibleGeometry(QRect requested, const QList<QRect> &screens);
};
} // namespace nova
