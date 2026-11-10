// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "project_config.h"
#include "revision.h"
#include <QJsonObject>
#include <QStringList>
#include <optional>
namespace nova {
struct PackFormat {
    int major = 0, minor = 0;
    auto operator<=>(const PackFormat &) const = default;
    QJsonValue json() const;
    static std::optional<PackFormat> parse(QJsonValue value, bool maximum = false);
    QString text() const;
};
struct GameVersion {
    QString id, releaseType;
    PackFormat format;
    bool rangeSchema;
};
const QList<GameVersion> &gameVersions();
struct Project {
    enum class Kind { ResourcePack, NeedsRepair, Folder, Invalid };
    QString root, name;
    QStringList namespaces, diagnostics;
    Kind kind = Kind::Invalid;
    QByteArray metadata;
    Revision revision;
    QJsonObject json;
    // .nova/project.json: absent for legacy projects; configDiagnostic explains a rejected file.
    std::optional<BaselineRef> baseline;
    QString configDiagnostic;
    static Project inspect(const QString &root);
    static bool exportMinecraft(const QString &root, const QString &destination,
                                QString *error = nullptr);
    static QString validateMetadata(const QByteArray &bytes);
    bool saveMetadata(const QByteArray &bytes, QString *error);
    static bool create(const QString &destination, const QString &name, const GameVersion &version,
                       const QString &description, const QString &nameSpace, const QString &icon,
                       bool folders, QString *error, const BaselineRef *baseline = nullptr);
};
} // namespace nova
