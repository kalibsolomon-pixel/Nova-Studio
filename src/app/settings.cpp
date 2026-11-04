// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

namespace nova {
namespace {
QString previewKey(const QString &project) {
    return QString::fromLatin1(
        QCryptographicHash::hash(QFileInfo(project).canonicalFilePath().toCaseFolded().toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
}
} // namespace
QJsonObject Settings::projectPreview(const QString &project) const {
    if (QFileInfo(project).canonicalFilePath().isEmpty())
        return {};
    QSettings s(path, QSettings::IniFormat);
    const auto bytes = s.value("projectPreview/" + previewKey(project)).toByteArray();
    return bytes.size() <= 16384 ? QJsonDocument::fromJson(bytes).object() : QJsonObject();
}
bool Settings::saveProjectPreview(const QString &project, const QJsonObject &state) const {
    if (QFileInfo(project).canonicalFilePath().isEmpty())
        return false;
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    if (bytes.size() > 16384)
        return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSettings s(path, QSettings::IniFormat);
    s.beginGroup("projectPreview");
    const auto key = previewKey(project);
    s.setValue(key, bytes);
    auto keys = s.childKeys();
    keys.removeAll(key);
    while (s.childKeys().size() > 32 && !keys.isEmpty())
        s.remove(keys.takeFirst());
    s.endGroup();
    s.sync();
    return s.status() == QSettings::NoError;
}
Settings::Settings(QString file) : path(std::move(file)) {
    if (path.isEmpty())
        path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/studio.ini";
}
Preferences Settings::load() const {
    QSettings s(path, QSettings::IniFormat);
    Preferences p;
    p.reopenLast = s.value("projects/reopenLast", true).toBool();
    p.dark = s.value("appearance/dark", true).toBool();
    p.accent = s.value("appearance/accent", "violet").toString();
    if (p.accent != "blue")
        p.accent = "violet";
    p.gridMode = s.value("explorer/grid", false).toBool();
    bool valid = false;
    int size = s.value("explorer/thumbnailSize", 72).toInt(&valid);
    p.thumbnailSize = valid ? std::clamp(size, 48, 128) : 72;
    p.historyMiB = std::clamp(s.value("editor/historyMiB", 16).toInt(), 1, 64);
    p.pixelGrid = s.value("editor/grid", true).toBool();
    p.timelineVisible = s.value("animation/timelineVisible", true).toBool();
    p.animationView = s.value("animation/view").toByteArray();
    if (p.animationView.size() > 4096) p.animationView.clear();
    p.workspacePreset = s.value("workspace/preset", "essentials").toString().left(32);
    p.showTips = s.value("appearance/showTips", true).toBool();
    p.minecraftTestTarget = s.value("testing/target").toString().left(64);
    p.fabricMinecraftVersion = s.value("fabric/minecraftVersion", "1.21.11").toString().left(32);
    p.fabricGameJava = s.value("fabric/gameJava").toString().left(1024);
    p.fabricLocation = s.value("fabric/location").toString().left(1024);
    p.fabricDetailedLogs = s.value("fabric/detailedLogs", false).toBool();
    p.fabricOpenTestWorld = s.value("fabric/openTestWorld", true).toBool();
    p.geometry = s.value("workspace/geometry").toByteArray();
    p.layout = s.value("workspace/layout").toByteArray();
    p.recentProjects = s.value("projects/recent").toStringList().mid(0, 10);
    p.projectPath = s.value("workspace/project").toString();
    p.directory = s.value("workspace/directory").toString();
    p.selected = s.value("workspace/selected").toString();
    p.query = s.value("workspace/query").toString().left(1024);
    p.openDocuments = s.value("workspace/documents").toStringList().mid(0, 32);
    p.activeDocument = s.value("workspace/activeDocument").toString();
    s.beginGroup("layouts");
    for (const auto &name : s.childKeys().mid(0, 32))
        p.layouts.insert(QString::fromUtf8(QByteArray::fromHex(name.toLatin1())), s.value(name).toByteArray());
    s.endGroup();
    p.recentColors = s.value("editor/recentColors").toStringList().mid(0, 16);
    p.activePalette = s.value("editor/activePalette").toString().left(128);
    // Brush settings may contain several bounded 256² PNG stamps. The former
    // 64 KiB truncation could split JSON and silently discard every tool setting.
    p.toolState = s.value("editor/toolState").toByteArray();
    if (p.toolState.size() > 8 * 1024 * 1024)
        p.toolState.clear();
    return p;
}
bool Settings::save(const Preferences &p) const {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSettings s(path, QSettings::IniFormat);
    s.setValue("schema", 1);
    s.setValue("projects/reopenLast", p.reopenLast);
    s.setValue("appearance/dark", p.dark);
    s.setValue("appearance/accent", p.accent);
    s.setValue("explorer/grid", p.gridMode);
    s.setValue("explorer/thumbnailSize", p.thumbnailSize);
    s.setValue("editor/grid", p.pixelGrid);
    s.setValue("animation/timelineVisible", p.timelineVisible);
    s.setValue("animation/view", p.animationView);
    s.setValue("workspace/preset", p.workspacePreset);
    s.setValue("appearance/showTips", p.showTips);
    s.setValue("testing/target", p.minecraftTestTarget);
    s.setValue("fabric/minecraftVersion", p.fabricMinecraftVersion);
    s.setValue("fabric/gameJava", p.fabricGameJava);
    s.setValue("fabric/location", p.fabricLocation);
    s.setValue("fabric/detailedLogs", p.fabricDetailedLogs);
    s.setValue("fabric/openTestWorld", p.fabricOpenTestWorld);
    s.setValue("editor/historyMiB", p.historyMiB);
    s.setValue("workspace/geometry", p.geometry);
    s.setValue("workspace/layout", p.layout);
    s.setValue("projects/recent", p.recentProjects.mid(0, 10));
    s.setValue("workspace/project", p.projectPath);
    s.setValue("workspace/directory", p.directory);
    s.setValue("workspace/selected", p.selected);
    s.setValue("workspace/query", p.query);
    s.setValue("workspace/documents", p.openDocuments);
    s.setValue("workspace/activeDocument", p.activeDocument);
    s.remove("layouts");
    s.beginGroup("layouts");
    for (auto it = p.layouts.begin(); it != p.layouts.end(); ++it)
        s.setValue(QString::fromLatin1(it.key().toUtf8().toHex()), it.value());
    s.endGroup();
    s.setValue("editor/recentColors", p.recentColors.mid(0, 16));
    s.setValue("editor/activePalette", p.activePalette);
    s.setValue("editor/toolState", p.toolState);
    s.sync();
    return s.status() == QSettings::NoError;
}
QRect Settings::visibleGeometry(QRect requested, const QList<QRect> &screens) {
    if (screens.isEmpty())
        return requested;
    for (const auto &screen : screens) {
        // Keep the title bar and enough of the window reachable.
        QRect title(requested.topLeft(), QSize(std::min(requested.width(), 160), 32));
        if (screen.contains(title)) {
            requested.setSize(requested.size().boundedTo(screen.size()));
            requested.moveLeft(std::clamp(requested.left(), screen.left(),
                                          screen.right() - requested.width() + 1));
            requested.moveTop(std::clamp(requested.top(), screen.top(),
                                         screen.bottom() - requested.height() + 1));
            return requested;
        }
    }
    auto screen = screens.first();
    auto size = requested.size().expandedTo(QSize(640, 480)).boundedTo(screen.size());
    return QRect(screen.center() - QPoint(size.width() / 2, size.height() / 2), size);
}
} // namespace nova
