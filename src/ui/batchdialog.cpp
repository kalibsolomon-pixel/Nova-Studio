// SPDX-License-Identifier: GPL-3.0-or-later
#include "batchdialog.h"
#include "core.h"
#include "index.h"
#include "opdialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSet>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace nova {
namespace {
QPixmap thumb(const QImage &image, int box = 64) {
    QPixmap pm(box, box);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    for (int y = 0; y < box; y += 8)
        for (int x = 0; x < box; x += 8)
            p.fillRect(x, y, 8, 8, ((x + y) / 8) % 2 ? QColor(70, 70, 70) : QColor(100, 100, 100));
    if (!image.isNull()) {
        const QSize s = image.size().scaled(box, box, Qt::KeepAspectRatio);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(QRect(QPoint((box - s.width()) / 2, (box - s.height()) / 2), s), image);
    }
    return pm;
}
} // namespace

BatchDialog::BatchDialog(BatchContext c, QWidget *parent) : QDialog(parent), context(std::move(c)) {
    setObjectName("batchDialog");
    setWindowTitle("Batch Processor");
    pipeline.name = "Untitled pipeline";
    auto root = new QVBoxLayout(this);
    auto split = new QSplitter;
    root->addWidget(split, 1);
    // ---------------------------------------------------------------- targets
    auto targetsBox = new QGroupBox("1 · Targets");
    auto tv = new QVBoxLayout(targetsBox);
    scope = new QComboBox;
    scope->setObjectName("batchScope");
    scope->setAccessibleName("Target scope");
    scope->addItems(batch::scopeNames());
    if(!context.fixedScopeName.isEmpty()){scope->setItemText(int(batch::Scope::Pack),context.fixedScopeName);scope->setEnabled(false);}
    scopeValue = new QComboBox;
    scopeValue->setObjectName("batchScopeValue");
    scopeValue->setAccessibleName("Scope value");
    scopeValue->setEditable(true);
    count = new QLabel;
    count->setObjectName("batchTargetCount");
    targets = new QListWidget;
    targets->setObjectName("batchTargets");
    targets->setAccessibleName("Batch targets");
    targets->setUniformItemSizes(true);
    sourceMode = new QComboBox;
    sourceMode->setObjectName("batchSourceMode");
    sourceMode->setAccessibleName("Batch source");
    sourceMode->addItems(batch::sourceModeNames());
    sourceMode->setToolTip("Exported PNG: process saved Minecraft PNGs.\nNative: process the layers and "
                           "source frames of .nvdoc documents; never flattened.");
    tv->addWidget(sourceMode);
    tv->addWidget(scope);
    tv->addWidget(scopeValue);
    tv->addWidget(count);
    nativeBox = new QWidget;
    nativeBox->setObjectName("batchNativeScope");
    auto nf = new QFormLayout(nativeBox);
    nf->setContentsMargins(0, 0, 0, 0);
    layerScope = new QComboBox;
    layerScope->setObjectName("batchLayerScope");
    layerScope->setAccessibleName("Native layer scope");
    layerScope->addItems(batch::layerScopeNames());
    layerNames = new QLineEdit;
    layerNames->setObjectName("batchLayerNames");
    layerNames->setPlaceholderText("Layer names, comma separated");
    frameScope = new QComboBox;
    frameScope->setObjectName("batchFrameScope");
    frameScope->setAccessibleName("Native source frame scope");
    frameScope->addItems(batch::frameScopeNames());
    frameScope->setCurrentIndex(int(batch::FrameScope::All));
    frameList = new QLineEdit;
    frameList->setObjectName("batchFrameList");
    frameList->setPlaceholderText("Indices, e.g. 0, 2-4 (selected frames / playback entries)");
    transaction = new QLineEdit(QFileInfo(context.root).absolutePath() + "/" + QFileInfo(context.root).fileName() +
                                "-native-transactions");
    transaction->setObjectName("batchTransaction");
    transaction->setAccessibleName("Transaction folder");
    transaction->setToolTip("Outside the pack: journals and verified copies of every original document");
    auto recover = new QPushButton("Recover…");
    recover->setObjectName("batchRecover");
    recover->setToolTip("Inspect an interrupted or completed native batch journal and roll back or confirm it");
    nf->addRow("Layers", layerScope);
    nf->addRow("", layerNames);
    nf->addRow("Frames", frameScope);
    nf->addRow("", frameList);
    nf->addRow("Transactions", transaction);
    nf->addRow("", recover);
    tv->addWidget(nativeBox);
    nativeBox->hide();
    tv->addWidget(targets, 1);
    // Only the controls a mode actually uses stay enabled: native mode writes the
    // .nvdoc in place (originals go to the transaction folder), so PNG folder/pattern
    // apply only to PNG export, and the PNG backup folder never applies.
    auto sync = [this] {
        const int mode = sourceMode->currentIndex();
        const bool native = mode != int(batch::SourceMode::ExportedPng);
        const bool exporting = mode == int(batch::SourceMode::NativeWithExport) ||
                               layerScope->currentIndex() == int(batch::LayerScope::CompositeOnly);
        nativeBox->setVisible(native);
        inPlace->setEnabled(!native);
        backup->setEnabled(!native);
        if (native)
            toDirectory->setChecked(true);
        toDirectory->setText(native ? "Write exported PNGs to a separate folder"
                                    : "Write results to a separate folder (recommended)");
        for (QWidget *w : std::initializer_list<QWidget *>{toDirectory, directory, pattern, overwrite})
            w->setEnabled(!native || exporting);
        layerNames->setEnabled(layerScope->currentIndex() == int(batch::LayerScope::Selected));
        frameList->setEnabled(frameScope->currentIndex() == int(batch::FrameScope::Selected) ||
                              frameScope->currentIndex() == int(batch::FrameScope::PlaybackEntries));
    };
    connect(sourceMode, &QComboBox::currentIndexChanged, this, [this, sync] {
        sync();
        resolve();
    });
    for (auto box : {layerScope, frameScope})
        connect(box, &QComboBox::currentIndexChanged, this, [this, sync] {
            sync();
            invalidate();
        });
    for (auto edit : {layerNames, frameList, transaction})
        connect(edit, &QLineEdit::textChanged, this, &BatchDialog::invalidate);
    connect(recover, &QPushButton::clicked, this, [this] {
        auto pending = batch::pendingNativeJournals(transaction->text().trimmed());
        auto path = pending.isEmpty() ? QFileDialog::getOpenFileName(this, "Native batch journal",
                                                                     transaction->text().trimmed(),
                                                                     "Journal (journal.json)")
                                      : pending.first();
        if (path.isEmpty())
            return;
        const auto inspection = recoverJournal(path, true, false);
        if (!inspection.ok) {
            status->setText(inspection.error);
            return;
        }
        QMessageBox box(this);
        box.setObjectName("batchRecoveryBox");
        box.setWindowTitle("Native batch recovery");
        box.setText(QString("%1\n%2 would be restored · %3 complete · %4 unchanged · %5 conflict(s)")
                        .arg(path)
                        .arg(inspection.restored)
                        .arg(inspection.completed)
                        .arg(inspection.unchanged)
                        .arg(inspection.conflicts));
        box.setDetailedText(inspection.messages.join('\n'));
        box.setInformativeText("Roll back restores verified original copies only where the document still "
                               "holds this batch's result. Changes made since are never overwritten.");
        auto rollback = box.addButton("Roll back", QMessageBox::DestructiveRole);
        auto confirm = box.addButton("Mark complete", QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != rollback && box.clickedButton() != confirm)
            return;
        const auto result = recoverJournal(path, box.clickedButton() == rollback, true);
        status->setText(result.ok ? QString("Recovery: %1 restored · %2 complete · %3 unchanged · %4 conflict(s)")
                                        .arg(result.restored)
                                        .arg(result.completed)
                                        .arg(result.unchanged)
                                        .arg(result.conflicts)
                                  : result.error);
    });
    split->addWidget(targetsBox);
    // ---------------------------------------------------------------- pipeline
    auto pipeBox = new QGroupBox("2 · Operation stack");
    auto pv = new QVBoxLayout(pipeBox);
    auto presetRow = new QHBoxLayout;
    presetBox = new QComboBox;
    presetBox->setObjectName("batchPresets");
    auto presetMenu = new QToolButton;
    presetMenu->setText("Pipeline ▾");
    presetMenu->setPopupMode(QToolButton::InstantPopup);
    auto pm = new QMenu(presetMenu);
    presetMenu->setMenu(pm);
    presetRow->addWidget(presetBox, 1);
    presetRow->addWidget(presetMenu);
    pv->addLayout(presetRow);
    steps = new QListWidget;
    steps->setObjectName("batchSteps");
    steps->setAccessibleName("Operation steps");
    steps->setToolTip("Steps run top to bottom; untick to disable a step");
    pv->addWidget(steps, 1);
    // Empty state: say how to start instead of showing a blank list.
    auto emptySteps = new QLabel("No operations yet. Use Add ▾ below or choose a saved pipeline; "
                                 "steps run top to bottom.");
    emptySteps->setObjectName("batchStepsEmpty");
    emptySteps->setProperty("role", "hint");
    emptySteps->setWordWrap(true);
    emptySteps->setAlignment(Qt::AlignCenter);
    emptySteps->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto emptyLayout = new QVBoxLayout(steps->viewport());
    emptyLayout->addWidget(emptySteps);
    auto stepRow = new QHBoxLayout;
    auto addButton = new QToolButton;
    addButton->setText("Add ▾");
    addButton->setObjectName("batchAddStep");
    addButton->setPopupMode(QToolButton::InstantPopup);
    auto addMenu = new QMenu(addButton);
    for (const auto &category : ops::categories()) {
        if (category == "Fill")
            continue;
        auto sub = addMenu->addMenu(category);
        for (const auto &info : ops::registry())
            if (info.category == category)
                sub->addAction(info.name, this, [this, id = info.id] {
                    auto params = ops::defaults(id);
                    if (id == "palette_map" && context.paletteText)
                        params["palette"] = context.paletteText();
                    pipeline.steps.push_back({id, params});
                    syncSteps(int(pipeline.steps.size()) - 1);
                    invalidate();
                });
    }
    addButton->setMenu(addMenu);
    auto dup = new QPushButton("Duplicate"), rem = new QPushButton("Remove"), up = new QPushButton("↑"),
         down = new QPushButton("↓");
    stepRow->addWidget(addButton);
    for (auto b : {dup, rem, up, down})
        stepRow->addWidget(b);
    pv->addLayout(stepRow);
    editor = new ParamEditor;
    editor->paletteText = context.paletteText;
    auto editorScroll = new QScrollArea;
    editorScroll->setWidgetResizable(true);
    editorScroll->setWidget(editor);
    editorScroll->setMinimumHeight(220);
    pv->addWidget(editorScroll, 1);
    split->addWidget(pipeBox);
    // ---------------------------------------------------------------- preview / output
    auto outBox = new QGroupBox("3 · Preview, output and run");
    auto ov = new QVBoxLayout(outBox);
    auto sampleWidget = new QWidget;
    samples = new QGridLayout(sampleWidget);
    samples->setContentsMargins(0, 0, 0, 0);
    auto refreshPreview = new QPushButton("Preview samples");
    refreshPreview->setObjectName("batchPreview");
    ov->addWidget(refreshPreview);
    ov->addWidget(sampleWidget);
    toDirectory = new QRadioButton("Write results to a separate folder (recommended)");
    toDirectory->setChecked(true);
    inPlace = new QRadioButton("Update originals (keeps backups, asks for confirmation)");
    inPlace->setObjectName("batchInPlace");
    directory = new QLineEdit(QFileInfo(context.root).absolutePath() + "/" + QFileInfo(context.root).fileName() +
                              "-batch-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmm"));
    directory->setObjectName("batchDirectory");
    auto browse = new QPushButton("…");
    pattern = new QLineEdit("{path}");
    pattern->setObjectName("batchPattern");
    pattern->setToolTip("Tokens: {path} relative path, {dir} folder, {name} file name, {index} 0001…");
    overwrite = new QCheckBox("Overwrite existing outputs (still conflict-checked)");
    backup = new QLineEdit(directory->text() + "-originals");
    backup->setObjectName("batchBackup");
    auto grid = new QGridLayout;
    grid->addWidget(toDirectory, 0, 0, 1, 3);
    grid->addWidget(new QLabel("Folder"), 1, 0);
    grid->addWidget(directory, 1, 1);
    grid->addWidget(browse, 1, 2);
    grid->addWidget(new QLabel("Name pattern"), 2, 0);
    grid->addWidget(pattern, 2, 1, 1, 2);
    grid->addWidget(overwrite, 3, 0, 1, 3);
    grid->addWidget(inPlace, 4, 0, 1, 3);
    grid->addWidget(new QLabel("Backups"), 5, 0);
    grid->addWidget(backup, 5, 1, 1, 2);
    ov->addLayout(grid);
    auto runRow = new QHBoxLayout;
    dryRun = new QPushButton("Dry run");
    dryRun->setObjectName("batchDryRun");
    runButton = new QPushButton("Run batch");
    runButton->setObjectName("batchRun");
    runButton->setEnabled(false);
    cancelButton = new QPushButton("Cancel job");
    cancelButton->setObjectName("batchCancel");
    cancelButton->setEnabled(false);
    runRow->addWidget(dryRun);
    runRow->addWidget(runButton);
    runRow->addWidget(cancelButton);
    ov->addLayout(runRow);
    summary = new QLabel("Run a dry run to review every target before writing.");
    summary->setObjectName("batchSummary");
    summary->setWordWrap(true);
    ov->addWidget(summary);
    progress = new QProgressBar;
    progress->setObjectName("batchProgress");
    ov->addWidget(progress);
    results = new QTreeWidget;
    results->setObjectName("batchResults");
    results->setAccessibleName("Dry run and batch results");
    results->setHeaderLabels({"Source", "Status", "Output / message", "Size"});
    results->setRootIsDecorated(false);
    results->setUniformRowHeights(true);
    results->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    results->setMinimumHeight(120);
    ov->addWidget(results, 1);
    status = new QLabel;
    status->setObjectName("batchStatus");
    status->setWordWrap(true);
    status->setMinimumHeight(status->fontMetrics().lineSpacing() * 3);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ov->addWidget(status);
    split->addWidget(outBox);
    split->setSizes({260, 360, 460});
    auto close = new QPushButton("Close");
    auto bottom = new QHBoxLayout;
    bottom->addStretch();
    bottom->addWidget(close);
    root->addLayout(bottom);
    resize(1240, 760);

    connect(scope, &QComboBox::currentIndexChanged, this, [this] {
        updating = true;
        scopeValue->clear();
        const auto s = batch::Scope(scope->currentIndex());
        QStringList values;
        if (s == batch::Scope::Selected)
            values = {context.selected};
        else if (s == batch::Scope::Folder || s == batch::Scope::FolderTree)
            values = {context.directory};
        else if (s == batch::Scope::Category)
            values = {"Blocks", "Items", "Entities", "GUI", "Particles", "Environment", "Other assets"};
        else if (s == batch::Scope::Namespace) {
            QSet<QString> spaces;
            for (const auto &f : std::as_const(context.files)) {
                const auto parts = f.split('/');
                if (parts.size() > 2 && parts[0] == "assets")
                    spaces.insert(parts[1]);
            }
            values = QStringList(spaces.begin(), spaces.end());
            values.sort();
        } else if (s == batch::Scope::Search)
            values = {context.query};
        scopeValue->addItems(values);
        scopeValue->setEnabled(s != batch::Scope::Pack);
        updating = false;
        resolve();
    });
    connect(scopeValue, &QComboBox::currentTextChanged, this, [this] {
        if (!updating)
            resolve();
    });
    connect(steps, &QListWidget::currentRowChanged, this, [this](int row) {
        if (updating)
            return;
        if (row >= 0 && row < int(pipeline.steps.size()))
            editor->setOperation(pipeline.steps[size_t(row)].id, pipeline.steps[size_t(row)].params);
    });
    connect(steps, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (updating)
            return;
        const int row = steps->row(item);
        if (row >= 0 && row < int(pipeline.steps.size())) {
            pipeline.steps[size_t(row)].enabled = item->checkState() == Qt::Checked;
            invalidate();
        }
    });
    connect(editor, &ParamEditor::edited, this, &BatchDialog::stepEdited);
    connect(dup, &QPushButton::clicked, this, [this] {
        const int row = steps->currentRow();
        if (row < 0)
            return;
        pipeline.steps.insert(pipeline.steps.begin() + row + 1, pipeline.steps[size_t(row)]);
        syncSteps(row + 1);
        invalidate();
    });
    connect(rem, &QPushButton::clicked, this, [this] {
        const int row = steps->currentRow();
        if (row < 0)
            return;
        pipeline.steps.erase(pipeline.steps.begin() + row);
        syncSteps(std::min(row, int(pipeline.steps.size()) - 1));
        invalidate();
    });
    for (auto [button, delta] : {std::pair{up, -1}, {down, 1}})
        connect(button, &QPushButton::clicked, this, [this, delta] {
            const int row = steps->currentRow(), to = row + delta;
            if (row < 0 || to < 0 || to >= int(pipeline.steps.size()))
                return;
            std::swap(pipeline.steps[size_t(row)], pipeline.steps[size_t(to)]);
            syncSteps(to);
            invalidate();
        });
    pm->addAction("Save pipeline as preset…", this, [this] {
        bool ok = false;
        const auto name = QInputDialog::getText(this, "Save pipeline", "Name", QLineEdit::Normal, pipeline.name, &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        pipeline.name = name;
        context.presets->put({"pipeline", name, {}, pipeline.toJson()});
        context.presets->save();
        loadPresets();
        presetBox->setCurrentText(name);
    });
    pm->addAction("Duplicate pipeline", this, [this] {
        pipeline.name += " copy";
        context.presets->put({"pipeline", pipeline.name, {}, pipeline.toJson()});
        context.presets->save();
        loadPresets();
        presetBox->setCurrentText(pipeline.name);
    });
    pm->addAction("Delete preset", this, [this] {
        if (context.presets->remove("pipeline", presetBox->currentText())) {
            context.presets->save();
            loadPresets();
        }
    });
    pm->addSeparator();
    pm->addAction("Export pipeline file…", this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "Export pipeline", pipeline.name + ".novapipe.json",
                                                       "Nova pipeline (*.json)");
        if (path.isEmpty())
            return;
        QString error;
        if (!atomicWrite(path, QJsonDocument(pipeline.toJson()).toJson(), Revision::read(path), &error))
            QMessageBox::warning(this, "Export failed", error);
    });
    pm->addAction("Import pipeline file…", this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "Import pipeline", {}, "Nova pipeline (*.json)");
        if (path.isEmpty())
            return;
        QFile f(path);
        batch::Pipeline loaded;
        QString error;
        if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024 ||
            !batch::Pipeline::fromJson(QJsonDocument::fromJson(f.readAll()).object(), &loaded, &error)) {
            QMessageBox::warning(this, "Import failed", error.isEmpty() ? QString("Unreadable pipeline file") : error);
            return;
        }
        setPipeline(loaded);
    });
    connect(presetBox, &QComboBox::activated, this, [this] {
        if (auto p = context.presets->find("pipeline", presetBox->currentText())) {
            batch::Pipeline loaded;
            QString error;
            if (batch::Pipeline::fromJson(p->data, &loaded, &error))
                setPipeline(loaded);
            else
                status->setText(error);
        }
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, "Output folder", directory->text());
        if (!path.isEmpty())
            directory->setText(path);
    });
    for (auto edit : {directory, pattern, backup})
        connect(edit, &QLineEdit::textChanged, this, &BatchDialog::invalidate);
    connect(overwrite, &QCheckBox::toggled, this, &BatchDialog::invalidate);
    connect(inPlace, &QRadioButton::toggled, this, &BatchDialog::invalidate);
    connect(refreshPreview, &QPushButton::clicked, this, &BatchDialog::previewSamples);
    connect(dryRun, &QPushButton::clicked, this, &BatchDialog::runDryRun);
    connect(runButton, &QPushButton::clicked, this, &BatchDialog::start);
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (cancel)
            cancel->store(true);
        status->setText("Canceling: items in progress finish; remaining items are skipped…");
    });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    loadPresets();
    emit sourceMode->currentIndexChanged(sourceMode->currentIndex()); // initial enablement
    emit scope->currentIndexChanged(context.selected.isEmpty() ? int(batch::Scope::Folder) : 0);
    if (!context.selected.isEmpty())
        scope->setCurrentIndex(0);
    else
        scope->setCurrentIndex(int(batch::Scope::Folder));
}
BatchDialog::~BatchDialog() {
    // The worker reports progress to this dialog's widgets: cancel and wait.
    if (cancel)
        cancel->store(true);
    if (job.isRunning())
        job.waitForFinished();
}
void BatchDialog::loadPresets() {
    presetBox->clear();
    presetBox->addItems(context.presets ? context.presets->names("pipeline") : QStringList());
    presetBox->setPlaceholderText("Saved pipelines");
    presetBox->setCurrentIndex(-1);
}
void BatchDialog::setScope(batch::Scope s, const QString &value) {
    scope->setCurrentIndex(int(s));
    scopeValue->setCurrentText(value);
    resolve();
}
void BatchDialog::setPipeline(const batch::Pipeline &p) {
    pipeline = p;
    syncSteps(pipeline.steps.empty() ? -1 : 0);
    invalidate();
}
void BatchDialog::resolve() {
    resolved = batch::resolveTargets(context.files, batch::Scope(scope->currentIndex()), scopeValue->currentText());
    targets->clear();
    for (const auto &f : resolved.mid(0, 2000))
        targets->addItem(f);
    int natives = 0;
    if (nativeMode())
        for (const auto &f : resolved)
            if (!f.isEmpty() && QFileInfo::exists(nativeDocumentPath(context.root, context.root + '/' + f)))
                ++natives;
    count->setText(QString("%1 PNG texture(s)%2%3")
                       .arg(resolved.size())
                       .arg(nativeMode() ? QString(" · %1 with native documents").arg(natives) : QString())
                       .arg(resolved.size() > 2000 ? " (first 2000 listed)" : ""));
    invalidate();
}
void BatchDialog::syncSteps(int select) {
    updating = true;
    steps->clear();
    int i = 1;
    for (const auto &s : pipeline.steps) {
        const auto info = ops::find(s.id);
        auto item = new QListWidgetItem(QString("%1. %2").arg(i++).arg(info ? info->name : s.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(s.enabled ? Qt::Checked : Qt::Unchecked);
        steps->addItem(item);
    }
    if (auto empty = steps->findChild<QLabel *>("batchStepsEmpty"))
        empty->setVisible(pipeline.steps.empty());
    updating = false;
    steps->setCurrentRow(select);
    if (select < 0)
        editor->setOperation({});
}
void BatchDialog::stepEdited() {
    const int row = steps->currentRow();
    if (row >= 0 && row < int(pipeline.steps.size()) && pipeline.steps[size_t(row)].id == editor->operation()) {
        pipeline.steps[size_t(row)].params = editor->params();
        invalidate();
    }
}
void BatchDialog::invalidate() {
    if (running)
        return;
    planned = false;
    runButton->setEnabled(false);
}
batch::Output BatchDialog::output() const {
    batch::Output out;
    out.inPlace = inPlace->isChecked();
    out.directory = directory->text().trimmed();
    out.pattern = pattern->text().trimmed();
    out.overwrite = overwrite->isChecked();
    out.backupDirectory = backup->text().trimmed();
    return out;
}
void BatchDialog::previewSamples() {
    if (nativeMode()) {
        previewNativeSamples();
        return;
    }
    while (auto item = samples->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    auto copy = pipeline;
    QString error;
    if (!copy.validate(&error)) {
        status->setText(error);
        return;
    }
    // Representative samples spread across the target list.
    QStringList chosen;
    for (int k = 0; k < 4 && k < resolved.size(); ++k)
        chosen.append(resolved[int(qint64(k) * resolved.size() / std::min<qsizetype>(4, resolved.size()))]);
    for (int k = 0; k < chosen.size(); ++k) {
        auto beforeLabel = new QLabel, afterLabel = new QLabel, caption = new QLabel;
        caption->setWordWrap(true);
        caption->setMaximumWidth(150);
        caption->setText(QFileInfo(chosen[k]).fileName() + "\n…");
        // Compact 2 × 2 grid: before, after and caption per sample.
        const int row = k / 2, column = (k % 2) * 3;
        samples->addWidget(beforeLabel, row, column);
        samples->addWidget(afterLabel, row, column + 1);
        samples->addWidget(caption, row, column + 2);
        const QString source = context.root + '/' + chosen[k];
        ops::Context ctx;
        ctx.palette = context.palette;
        auto watcher = new QFutureWatcher<std::pair<QImage, ops::Result>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [=] {
            watcher->deleteLater();
            const auto [in, r] = watcher->result();
            beforeLabel->setPixmap(thumb(in));
            afterLabel->setPixmap(thumb(r.image));
            caption->setText(QString("%1\n%2×%3 → %4\n%5")
                                 .arg(QFileInfo(chosen[k]).fileName())
                                 .arg(in.width())
                                 .arg(in.height())
                                 .arg(r.ok() ? QString("%1×%2").arg(r.image.width()).arg(r.image.height()) : QString("—"))
                                 .arg(r.ok() ? QString("%1 step(s)").arg(copy.enabledSteps()) : r.error));
        });
        watcher->setFuture(QtConcurrent::run([source, copy, ctx] {
            const auto in = loadPng(source);
            return std::pair{in, in.isNull() ? ops::Result{{}, "Unreadable PNG"} : ops::runPipeline(copy.steps, in, ctx)};
        }));
    }
}
bool BatchDialog::nativeMode() const {
    return sourceMode && sourceMode->currentIndex() != int(batch::SourceMode::ExportedPng);
}
namespace {
QList<int> indices(const QString &text) {
    QList<int> out;
    for (auto part : text.split(',', Qt::SkipEmptyParts)) {
        part = part.trimmed();
        const auto range = part.split('-');
        bool a = false, b = false;
        const int from = range[0].trimmed().toInt(&a), to = range.size() == 2 ? range[1].trimmed().toInt(&b) : from;
        if (a && (range.size() == 1 || b))
            for (int i = from; i <= to && out.size() < 16384; ++i)
                out << i;
    }
    return out;
}
} // namespace
batch::NativeOptions BatchDialog::nativeOptions() const {
    batch::NativeOptions o;
    o.mode = batch::SourceMode(sourceMode->currentIndex());
    o.layers = batch::LayerScope(layerScope->currentIndex());
    for (const auto &n : layerNames->text().split(',', Qt::SkipEmptyParts))
        o.layerNames << n.trimmed();
    o.frames = batch::FrameScope(frameScope->currentIndex());
    o.frameIndices = o.playbackIndices = indices(frameList->text());
    o.transactionRoot = transaction->text().trimmed();
    o.exportOutput = output();
    o.exportOutput.inPlace = false;
    if (context.openDocuments)
        o.blocked = context.openDocuments();
    return o;
}
bool BatchDialog::runNativeDryRun() {
    nativePlan = batch::planNative(pipeline, context.root, resolved, nativeOptions());
    results->clear();
    for (const auto &item : nativePlan.items) {
        QString detail = item.message;
        if (detail.isEmpty()) {
            detail = (item.animated ? QString("Frames %1 · ").arg([&] {
                          QStringList f;
                          for (int i : item.targetFrames)
                              f << QString::number(i);
                          return f.join(',');
                      }())
                                    : QString()) +
                     "Layers: " + item.targetLayers.mid(0, 6).join(", ") +
                     (item.targetLayers.size() > 6 ? QString(" +%1").arg(item.targetLayers.size() - 6) : QString());
            if (!item.exportPath.isEmpty())
                detail += " → " + item.exportPath;
            if (!item.warnings.isEmpty())
                detail += " · " + item.warnings.join("; ");
        }
        auto row = new QTreeWidgetItem(results, {item.relative, batch::nativeStatusName(item.status), detail,
                                                 item.outputSize.isValid()
                                                     ? QString("%1×%2").arg(item.outputSize.width()).arg(item.outputSize.height())
                                                     : QString()});
        row->setToolTip(2, detail);
        if (item.status != batch::NativeItem::Status::Ready)
            row->setForeground(1, QColor(229, 115, 115));
    }
    summary->setText(nativePlan.summary() +
                     "\nNative documents keep layers, UUIDs, visibility, opacity and frames; nothing is flattened."
                     "\nOriginals are copied to the transaction folder and each replacement is verified.");
    planned = nativePlan.ready > 0;
    runButton->setEnabled(planned);
    status->setText(planned ? "Review targets, layers and frames, then run." : "Nothing is ready to process.");
    return planned;
}
void BatchDialog::previewNativeSamples() {
    while (auto item = samples->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (nativePlan.items.empty())
        runNativeDryRun();
    int k = 0;
    const auto options = nativeOptions();
    for (const auto &item : nativePlan.items) {
        if (item.status != batch::NativeItem::Status::Ready || k >= 4)
            continue;
        QImage before, after;
        QString error;
        auto beforeLabel = new QLabel, afterLabel = new QLabel, caption = new QLabel;
        caption->setWordWrap(true);
        caption->setMaximumWidth(150);
        if (batch::previewNative(pipeline, item, context.root, options, &before, &after, &error)) {
            beforeLabel->setPixmap(thumb(before));
            afterLabel->setPixmap(thumb(after));
            caption->setText(QFileInfo(item.relative).fileName() + "\n" +
                             (item.animated ? QString("frame %1").arg(item.targetFrames.value(0)) : QString("composite")));
        } else
            caption->setText(QFileInfo(item.relative).fileName() + "\n" + error);
        const int row = k / 2, column = (k % 2) * 3;
        samples->addWidget(beforeLabel, row, column);
        samples->addWidget(afterLabel, row, column + 1);
        samples->addWidget(caption, row, column + 2);
        ++k;
    }
}
batch::NativeRecovery BatchDialog::recoverJournal(const QString &journal, bool rollback, bool apply) {
    return batch::recoverNative(journal, rollback, apply);
}
bool BatchDialog::startNative() {
    if (!planned || running)
        return false;
    const auto options = nativeOptions();
    if (!property("confirmed").toBool() &&
        QMessageBox::question(this, "Update native documents?",
                              QString("Process %1 native document(s) in place? Each original is copied to\n%2\n"
                                      "first and every replacement is verified. This is a sequence of "
                                      "recoverable per-document writes, not one all-or-nothing commit.")
                                  .arg(nativePlan.ready)
                                  .arg(options.transactionRoot),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return false;
    running = true;
    cancel = std::make_shared<std::atomic_bool>(false);
    runButton->setEnabled(false);
    dryRun->setEnabled(false);
    cancelButton->setEnabled(true);
    progress->setRange(0, int(nativePlan.items.size()));
    progress->setValue(0);
    auto shared = std::make_shared<batch::NativePlan>(nativePlan);
    auto pipe = pipeline;
    ops::Context ctx;
    ctx.palette = context.palette;
    auto token = cancel;
    auto count = std::make_shared<std::atomic_int>(0);
    const auto rootPath = context.root;
    auto watcher = new QFutureWatcher<batch::NativeReport>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, shared] {
        watcher->deleteLater();
        const auto report = watcher->result();
        nativePlan = *shared;
        running = false;
        planned = false;
        dryRun->setEnabled(true);
        cancelButton->setEnabled(false);
        progress->setValue(progress->maximum());
        for (int i = 0; i < results->topLevelItemCount() && i < int(nativePlan.items.size()); ++i) {
            const auto &item = nativePlan.items[size_t(i)];
            auto row = results->topLevelItem(i);
            row->setText(1, batch::nativeStatusName(item.status));
            row->setText(2, item.message);
            row->setForeground(1, item.status == batch::NativeItem::Status::Done ? QColor(129, 199, 132)
                                                                                  : QColor(229, 115, 115));
        }
        status->setText(QString("Done %1 · failed %2 · rolled back %3 · skipped %4 · canceled %5 · %6 ms\nJournal: %7%8")
                            .arg(report.done)
                            .arg(report.failed)
                            .arg(report.rolledBack)
                            .arg(report.skipped)
                            .arg(report.canceled)
                            .arg(report.milliseconds)
                            .arg(report.journal)
                            .arg(report.error.isEmpty() ? QString() : "\n" + report.error));
        emit nativeFinished(report);
    });
    watcher->setFuture(QtConcurrent::run([this, shared, pipe, options, ctx, token, count, rootPath] {
        return batch::executeNative(pipe, *shared, rootPath, options, ctx, token.get(), [this, count](int) {
            const int n = ++*count;
            QMetaObject::invokeMethod(progress, [this, n] { progress->setValue(n); }, Qt::QueuedConnection);
        });
    }));
    status->setText("Running native batch in the background…");
    return true;
}
bool BatchDialog::runDryRun() {
    if (nativeMode())
        return runNativeDryRun();
    if(context.sourceGuard) { const auto error=context.sourceGuard();if(!error.isEmpty()){status->setText(error);planned=false;runButton->setEnabled(false);return false;} }
    currentPlan = batch::plan(pipeline, context.root, resolved, output());
    results->clear();
    for (const auto &item : currentPlan.items) {
        auto row = new QTreeWidgetItem(results, {item.relative, batch::statusName(item.status),
                                                 item.message.isEmpty() ? item.output : item.message,
                                                 item.outputSize.isValid() ? QString("%1×%2").arg(item.outputSize.width()).arg(item.outputSize.height())
                                                                           : QString()});
        if (item.status != batch::Item::Status::Ready)
            row->setForeground(1, QColor(229, 115, 115));
    }
    summary->setText(currentPlan.summary() + (output().inPlace ? "\nOriginals will be replaced after backups are written."
                                                                : "\nOriginal files will not be modified."));
    planned = currentPlan.ready > 0;
    runButton->setEnabled(planned);
    status->setText(planned ? "Review the dry run, then run the batch." : "Nothing is ready to process.");
    return planned;
}
bool BatchDialog::start() {
    if (nativeMode())
        return startNative();
    if (!planned || running)
        return false;
    if(context.sourceGuard) { const auto error=context.sourceGuard();if(!error.isEmpty()){status->setText(error);planned=false;runButton->setEnabled(false);return false;} }
    const auto out = output();
    if (out.inPlace && !property("confirmed").toBool() &&
        QMessageBox::question(this, "Update original textures?",
                              QString("Replace %1 original texture(s)? Each original is copied to\n%2\nfirst, and a "
                                      "manifest records every file.")
                                  .arg(currentPlan.ready)
                                  .arg(out.backupDirectory),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return false;
    running = true;
    cancel = std::make_shared<std::atomic_bool>(false);
    runButton->setEnabled(false);
    dryRun->setEnabled(false);
    cancelButton->setEnabled(true);
    progress->setRange(0, int(currentPlan.items.size()));
    progress->setValue(0);
    auto shared = std::make_shared<batch::Plan>(currentPlan);
    auto pipe = pipeline;
    ops::Context ctx;
    ctx.palette = context.palette;
    auto token = cancel;
    auto finishedCount = std::make_shared<std::atomic_int>(0);
    auto watcher = new QFutureWatcher<batch::Report>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, shared] {
        watcher->deleteLater();
        const auto report = watcher->result();
        currentPlan = *shared;
        running = false;
        planned = false;
        dryRun->setEnabled(true);
        cancelButton->setEnabled(false);
        progress->setValue(progress->maximum());
        for (int i = 0; i < results->topLevelItemCount() && i < int(currentPlan.items.size()); ++i) {
            const auto &item = currentPlan.items[size_t(i)];
            auto row = results->topLevelItem(i);
            row->setText(1, batch::statusName(item.status));
            row->setText(2, item.status == batch::Item::Status::Done ? item.output : item.message);
            row->setForeground(1, item.status == batch::Item::Status::Done ? QColor(129, 199, 132) : QColor(229, 115, 115));
        }
        status->setText(QString("Done %1 · failed %2 · skipped %3 · canceled %4 · %5 ms\nManifest: %6%7")
                            .arg(report.done)
                            .arg(report.failed)
                            .arg(report.skipped)
                            .arg(report.canceled)
                            .arg(report.milliseconds)
                            .arg(report.manifest)
                            .arg(report.error.isEmpty() ? QString() : "\n" + report.error));
        emit finished(report);
    });
    job = QtConcurrent::run([this, shared, pipe, out, ctx, token, finishedCount] {
        return batch::execute(pipe, *shared, out, ctx, token.get(), [this, finishedCount](int) {
            const int n = ++*finishedCount;
            // Progress is marshalled to the GUI thread; the event loop stays responsive.
            QMetaObject::invokeMethod(progress, [this, n] { progress->setValue(n); }, Qt::QueuedConnection);
        });
    });
    watcher->setFuture(job);
    status->setText("Running in the background…");
    return true;
}
} // namespace nova
