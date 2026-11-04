// SPDX-License-Identifier: GPL-3.0-or-later
#include "presets.h"
#include "revision.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>

namespace nova {
bool PresetStore::load(QString *error) {
    presets.clear();
    QFile f(file);
    if (file.isEmpty() || !f.exists())
        return true;
    if (f.size() > 8 * 1024 * 1024 || !f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = "Preset library is unreadable";
        return false;
    }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || doc.object().value("schema").toInt() != 1) {
        if (error)
            *error = "Preset library has an unsupported schema; it was left untouched";
        return false;
    }
    for (const auto &v : doc.object().value("presets").toArray()) {
        const auto o = v.toObject();
        Preset p{o.value("kind").toString(), o.value("name").toString().left(128),
                 o.value("operation").toString(), o.value("data").toObject()};
        if (!p.kind.isEmpty() && !p.name.trimmed().isEmpty())
            presets.push_back(std::move(p));
    }
    return true;
}
bool PresetStore::save(QString *error) const {
    QJsonArray list;
    for (const auto &p : presets)
        list.append(QJsonObject{{"kind", p.kind}, {"name", p.name}, {"operation", p.operation}, {"data", p.data}});
    const auto data = QJsonDocument(QJsonObject{{"schema", 1}, {"presets", list}}).toJson();
    if (data.size() > 8 * 1024 * 1024) {
        if (error)
            *error = "Preset library exceeds the 8 MiB limit; the saved library was left untouched";
        return false;
    }
    QDir().mkpath(QFileInfo(file).absolutePath());
    return atomicWrite(file, data, Revision::read(file), error);
}
QStringList PresetStore::names(const QString &kind, const QString &operation) const {
    QStringList out;
    for (const auto &p : presets)
        if (p.kind == kind && (operation.isEmpty() || p.operation == operation))
            out.append(p.name);
    return out;
}
const Preset *PresetStore::find(const QString &kind, const QString &name, const QString &operation) const {
    for (const auto &p : presets)
        if (p.kind == kind && p.name == name && (operation.isEmpty() || p.operation == operation))
            return &p;
    return nullptr;
}
void PresetStore::put(const Preset &preset) {
    for (auto &p : presets)
        if (p.kind == preset.kind && p.name == preset.name && p.operation == preset.operation) {
            p = preset;
            return;
        }
    presets.push_back(preset);
}
bool PresetStore::remove(const QString &kind, const QString &name, const QString &operation) {
    const auto before = presets.size();
    std::erase_if(presets, [&](const Preset &p) {
        return p.kind == kind && p.name == name && (operation.isEmpty() || p.operation == operation);
    });
    return presets.size() != before;
}
} // namespace nova
