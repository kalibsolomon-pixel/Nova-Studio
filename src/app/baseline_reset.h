// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QStringList>
#include <QVector>
#include <functional>
namespace nova {
// Reset to Vanilla / Remove from Project: the project files of each texture (PNG, .png.mcmeta and
// .nova/documents sidecar) are renamed into <root>/.nova/baseline-resets/<uuid>/ next to a
// journal.json (prepared -> applied <-> undone). Nothing is ever deleted from the project.
enum class ResetKind { ResetToVanilla, RemoveFromProject };
struct ResetJournal {
    // prepared / undoing / redoing: interrupted (see recoverReset); applied, undone and
    // rolled_back are stable; invalid: journal.json could not be read (error explains why).
    QString id, state, error;
    ResetKind kind = ResetKind::ResetToVanilla;
    QStringList assets;
    QJsonArray files; // {path, retained, sha256, size, before_sha256}; sha256 = last observed content
    QDateTime created, updated;
    bool interrupted() const { return state == "prepared" || state == "undoing" || state == "redoing"; }
};
using BaselineCheck = std::function<bool(const QString &asset, QString *error)>;
QString resetFolder(const QString &root);
// `assets` are project-relative assets/<ns>/....png paths. ResetToVanilla calls baselineAvailable
// for every asset before anything is touched; any failure changes nothing. `beforeMoving` runs
// once after journal.json is prepared and before the first rename (tests: concurrent changes).
bool applyReset(const QString &root, const QStringList &assets, ResetKind kind,
                const BaselineCheck &baselineAvailable, QString *journalId, QString *error = nullptr,
                const std::function<void()> &beforeMoving = {});
// Restores an applied journal only if every destination is vacant and every retained file still
// matches its recorded hash; otherwise nothing changes and the error names the path.
bool undoReset(const QString &root, const QString &journalId, QString *error = nullptr);
// Re-applies an undone journal only if the project files still match the hashes recorded at undo.
// Pass baselineAvailable for ResetToVanilla journals so a missing baseline refuses the redo.
bool redoReset(const QString &root, const QString &journalId, QString *error = nullptr,
               const BaselineCheck &baselineAvailable = {});
// All journals, newest first, including interrupted and unreadable ones.
QVector<ResetJournal> resetJournals(const QString &root, QString *error = nullptr);
// Rolls an interrupted step back (prepared -> rolled_back, redoing -> undone, undoing -> applied).
// When rolling back would overwrite a file that appeared meanwhile, the step is completed instead
// (-> applied). Either way no file is lost; stable journals are left as they are.
bool recoverReset(const QString &root, const QString &journalId, QString *error = nullptr);
// Deletes stable (applied, undone, rolled_back) journals older than maxAgeDays or beyond the
// newest `keep`. Only regular files directly inside .nova/baseline-resets/<uuid> are removed.
void pruneResets(const QString &root, int keep = 64, int maxAgeDays = 30);
} // namespace nova
