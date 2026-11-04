// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Native-aware batch processing (Phase 2-C8).
//
// Applies the shared image-operation pipeline to the layers and source frames of
// closed .nvdoc documents without flattening them. Each document is processed and
// replaced individually: originals are copied into a transaction directory, the
// result is encoded and decoded again before writing, the replacement is revision
// checked and verified, and a checksummed journal records every state change so an
// interrupted run can be inspected, completed or rolled back. This is a sequence of
// recoverable per-document transactions, NOT an all-or-nothing multi-file commit.
#include "batch.h"
#include "core.h"
#include <QImage>
#include <functional>

namespace nova::batch {
enum class SourceMode { ExportedPng, Native, NativeWithExport };
enum class LayerScope { Active, Selected, All, Visible, CompositeOnly };
enum class FrameScope { Current, Selected, All, PlaybackEntries };
QStringList sourceModeNames();
QStringList layerScopeNames();
QStringList frameScopeNames();

struct NativeOptions {
    SourceMode mode = SourceMode::Native;
    LayerScope layers = LayerScope::Active;
    QStringList layerNames;     // Selected: layer names or UUID strings (matched per document)
    FrameScope frames = FrameScope::All;
    QList<int> frameIndices;    // Selected: zero-based source-frame indices
    QList<int> playbackIndices; // PlaybackEntries: entries; each referenced frame is processed once
    QString transactionRoot;    // outside the pack: journal and original copies
    Output exportOutput;        // NativeWithExport / CompositeOnly: directory output
    qint64 memoryBudget = 512LL * 1024 * 1024;
    QStringList blocked;        // absolute PNG paths open with unsaved/export-pending edits
    // Test-only failure injection: return true to fail at a stage
    // ("backup", "encode", "commit", "verify", "export", "interrupt").
    std::function<bool(const QString &stage, int index)> fault;
};

struct NativeItem {
    enum class Status { Ready, Conflict, Invalid, Skipped, Done, Failed, Canceled, RolledBack };
    QString relative, png, nvdoc, exportPath, message;
    Status status = Status::Ready;
    bool animated = false;
    int layerCount = 0, frameCount = 0, playbackCount = 0;
    QStringList targetLayers;   // resolved names, e.g. "Base", "Frame 2 / Shade"
    QList<int> targetFrames;    // resolved zero-based source frames
    QSize size, outputSize;
    qint64 estimatedBytes = 0;
    Revision sourceRevision, exportRevision;
    QByteArray resultHash, exportHash;
    QStringList warnings;
};
QString nativeStatusName(NativeItem::Status);

struct NativePlan {
    std::vector<NativeItem> items;
    int ready = 0, conflicts = 0, invalid = 0;
    qint64 peakBytes = 0, totalBytes = 0;
    int workers = 1;
    QString summary() const;
};
// Dry run: opens each native document to resolve layers/frames and estimate work.
// Writes nothing.
NativePlan planNative(Pipeline pipeline, const QString &root, const QStringList &relativePngs,
                      const NativeOptions &);
// Representative before/after composites (first targeted frame) for one planned item.
bool previewNative(const Pipeline &, const NativeItem &, const QString &root, const NativeOptions &,
                   QImage *before, QImage *after, QString *error = nullptr);

struct NativeReport {
    int done = 0, failed = 0, skipped = 0, canceled = 0, rolledBack = 0;
    bool interrupted = false;
    QString journal, error;
    qint64 milliseconds = 0;
};
NativeReport executeNative(const Pipeline &, NativePlan &, const QString &root, const NativeOptions &,
                           const ops::Context &, const std::atomic_bool *cancel,
                           const std::function<void(int)> &progress = {});

struct NativeRecovery {
    bool ok = false;
    QString error;
    int restored = 0, completed = 0, unchanged = 0, conflicts = 0;
    QStringList messages;
};
// Reads and verifies a journal; with apply=false nothing is written (inspection).
// rollback=true restores originals whose current bytes still equal the batch result;
// rollback=false only confirms completed items. Idempotent; never overwrites external edits.
NativeRecovery recoverNative(const QString &journal, bool rollback, bool apply);
QStringList pendingNativeJournals(const QString &transactionRoot);
} // namespace nova::batch
#include <QMetaType>
Q_DECLARE_METATYPE(nova::batch::NativeReport)
