// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "index.h"
#include <QByteArray>
#include <QJsonObject>
namespace nova::mc {
struct ResourceSource {
    QString root, label, version;
    bool vanilla = false;
    Index index;
    QJsonObject hashes;
    static ResourceSource open(const QString &folder, const QString &target, QString *error);
};
struct ResourceCandidate {
    QString path, label;
    bool editable = false;
    QString root;
    QByteArray sha256;
};
// Uses the existing Index snapshots. No independent filesystem crawler/watcher.
class ResourceStack {
    QMap<QString, QVector<ResourceCandidate>> candidates;
    Index combined;

  public:
    QStringList diagnostics;
    void build(const QString &editableRoot, const Index &editable,
               const QVector<ResourceSource> &fallbacks, const QString &version);
    const Index &index() const { return combined; }
    QString path(const QString &asset, int provider = 0) const;
    QVector<ResourceCandidate> providers(const QString &asset) const {
        return candidates.value(asset);
    }
    bool editable(const QString &asset) const;
};
} // namespace nova::mc
