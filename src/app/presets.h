// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>

namespace nova {
// User presets shared across documents and restarts: brush settings, image
// operation parameters (filters, adjustments, shading) and batch pipelines.
// File format: {"schema":1,"presets":[{"kind","name","operation","data"}]}.
struct Preset {
    QString kind;      // "brush", "operation", "pipeline", "tool"
    QString name;
    QString operation; // operation id for kind == "operation"
    QJsonObject data;
};
class PresetStore {
    QString file;

  public:
    std::vector<Preset> presets;
    explicit PresetStore(QString path = {}) : file(std::move(path)) {}
    QString path() const { return file; }
    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr) const;
    QStringList names(const QString &kind, const QString &operation = {}) const;
    const Preset *find(const QString &kind, const QString &name, const QString &operation = {}) const;
    void put(const Preset &preset); // replaces a preset with the same kind/name/operation
    bool remove(const QString &kind, const QString &name, const QString &operation = {});
};
} // namespace nova
