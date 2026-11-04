// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "creation.h"
#include <QDateTime>
namespace nova::mc {
enum class Severity { Error, Warning, Information, Unsupported };
QString severityName(Severity);
struct Diagnostic {
    Severity severity = Severity::Error;
    QString code, file, field, message, suggestion;
    QStringList affected;
    int line = 0;
    QJsonObject
        replacement; // complete unknown-field-preserving proposal, only for deterministic repairs
    Revision revision;
    QJsonObject json() const;
};
struct ValidationReport {
    QString project, version, scope;
    QDateTime time;
    int files = 0, inspected = 0, reused = 0;
    qint64 milliseconds = 0;
    bool canceled = false, truncated = false;
    QVector<Diagnostic> diagnostics;
    QJsonObject json() const;
    QString text() const;
};
class ValidationEngine {
    struct Cached {
        qint64 size = 0, modified = 0;
        QVector<Diagnostic> diagnostics;
    };
    QMap<QString, Cached> cache;
    qsizetype cacheDiagnosticBytes = 0;
    QString project, version;

  public:
    // Scope is explicit project-relative paths. Library is an existing worker snapshot.
    ValidationReport run(const QString &root, const Index &, Library library,
                         const QStringList &scope = {}, const QStringList &changed = {},
                         const std::shared_ptr<std::atomic_bool> &cancel = {},
                         const std::function<void(int, int)> &progress = {},
                         const QMap<QString,QJsonObject> &documents = {},
                         const Library::AssetReader &reader = {});
};
struct RepairResult {
    bool applied = false;
    QString backup, error;
};
// Call only after showing/confirming the diagnostic's replacement. Rejects stale revisions.
RepairResult applyRepair(const QString &root, const Diagnostic &);
QStringList modelTextureScope(Library library, const QString &modelAsset, bool family);
} // namespace nova::mc
