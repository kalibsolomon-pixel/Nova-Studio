// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Batch processing: applies the same validated image-operation pipeline as the
// editor to many PNG files. A job is a collection of individually validated
// file operations — not an all-or-nothing filesystem transaction. Every item's
// outcome is recorded in a JSON manifest for auditing and recovery.
#include "imageops.h"
#include "revision.h"
#include <QJsonObject>
#include <QStringList>
#include <atomic>
#include <functional>
#include <vector>

namespace nova::batch {
struct Pipeline {
    static constexpr int Schema = 1;
    QString name;
    std::vector<ops::Spec> steps;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, Pipeline *out, QString *error = nullptr);
    bool validate(QString *error = nullptr); // normalises step parameters
    int enabledSteps() const;
};
enum class Scope { Selected, Folder, FolderTree, Category, Namespace, Search, Pack };
QStringList scopeNames();
// Resolves PNG targets (project-relative paths) from an index file list.
QStringList resolveTargets(const QStringList &indexFiles, Scope scope, const QString &value);
struct Output {
    bool inPlace = false;          // update originals (requires confirmation; backups kept)
    QString directory;             // destination root for directory mode
    QString pattern = "{path}";    // tokens: {path} {dir} {name} {index}
    bool overwrite = false;        // directory mode: replace existing outputs (guarded)
    QString backupDirectory;       // in-place mode: original copies are written here first
};
bool safeOutputPath(const Output &output, const QString &relative, int index, QString *path,
                    QString *error);
struct Item {
    enum class Status { Ready, Conflict, Invalid, Skipped, Done, Failed, Canceled };
    QString relative, source, output, backup, message;
    Status status = Status::Ready;
    QSize size, outputSize;
    qint64 estimatedBytes = 0;
    Revision sourceRevision, outputRevision;
    QByteArray resultHash;
};
QString statusName(Item::Status status);
struct Plan {
    std::vector<Item> items;
    qint64 peakItemBytes = 0;
    int ready = 0, conflicts = 0, invalid = 0;
    int workers = 1;
    QString summary() const;
};
// Dry run: identifies targets, validates output paths, reads PNG headers only
// (no full decode), detects conflicts/duplicates and estimates memory.
Plan plan(Pipeline pipeline, const QString &root, const QStringList &relativeFiles, const Output &output,
          int maxWorkers = 4);
struct Report {
    int done = 0, failed = 0, skipped = 0, canceled = 0;
    QString manifest;
    QString error;
    qint64 milliseconds = 0;
};
// Executes a plan with bounded worker concurrency. Workers pull the next item
// only after finishing one (backpressure), so at most `plan.workers` images are
// decoded at once. Never call on the GUI thread for large jobs.
Report execute(const Pipeline &pipeline, Plan &plan, const Output &output, const ops::Context &context,
               const std::atomic_bool *cancel, const std::function<void(int index)> &progress = {});
QJsonObject manifestJson(const Pipeline &pipeline, const Plan &plan, const Output &output,
                         const Report &report);
} // namespace nova::batch
#include <QMetaType>
Q_DECLARE_METATYPE(nova::batch::Report)
