// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "batch.h"
#include "native_batch.h"
#include "presets.h"
#include <QDialog>
#include <QFuture>
#include <atomic>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QCheckBox;
class QTreeWidget;
class QRadioButton;
class QGridLayout;
namespace nova {
class ParamEditor;
struct BatchContext {
    QString root;
    QStringList files;          // project-relative index entries
    QString selected, directory, query;
    PresetStore *presets = nullptr;
    std::vector<QRgb> palette;
    std::function<QString()> paletteText;
    std::function<QString()> sourceGuard;
    QString fixedScopeName;
    // Absolute PNG paths of open documents with unsaved or export-pending edits.
    std::function<QStringList()> openDocuments;
};
class BatchDialog : public QDialog {
    Q_OBJECT
    BatchContext context;
    batch::Pipeline pipeline;
    batch::Plan currentPlan;
    bool planned = false, running = false;
    std::shared_ptr<std::atomic_bool> cancel;
    QFuture<batch::Report> job;
    QComboBox *scope, *scopeValue, *presetBox;
    QLabel *count, *summary, *status;
    QListWidget *targets, *steps;
    ParamEditor *editor;
    QGridLayout *samples;
    QRadioButton *toDirectory, *inPlace;
    QLineEdit *directory, *pattern, *backup;
    QCheckBox *overwrite;
    QTreeWidget *results;
    QProgressBar *progress;
    QPushButton *dryRun, *runButton, *cancelButton;
    QStringList resolved;
    bool updating = false;
    // Native-aware mode (C8)
    QComboBox *sourceMode = nullptr, *layerScope = nullptr, *frameScope = nullptr;
    QLineEdit *layerNames = nullptr, *frameList = nullptr, *transaction = nullptr;
    QWidget *nativeBox = nullptr;
    batch::NativePlan nativePlan;
    bool nativeMode() const;
    batch::NativeOptions nativeOptions() const;
    bool runNativeDryRun();
    bool startNative();
    void previewNativeSamples();
    void resolve();
    void syncSteps(int select = -1);
    void stepEdited();
    void invalidate();
    void previewSamples();
    batch::Output output() const;
    void loadPresets();

  public:
    BatchDialog(BatchContext context, QWidget *parent = nullptr);
    ~BatchDialog() override;
    void setScope(batch::Scope scope, const QString &value);
    void setPipeline(const batch::Pipeline &pipeline);
    const batch::Plan &plan() const { return currentPlan; }
    bool runDryRun();
    bool start();
    const batch::NativePlan &nativeDryRun() const { return nativePlan; }
    // Inspect (apply=false) or apply recovery for a native batch journal.
    batch::NativeRecovery recoverJournal(const QString &journal, bool rollback, bool apply);
  signals:
    void finished(const batch::Report &report);
    void nativeFinished(const batch::NativeReport &report);
};
} // namespace nova
