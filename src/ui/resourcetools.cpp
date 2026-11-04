// SPDX-License-Identifier: GPL-3.0-or-later
#include "resourcetools.h"
#include <QJsonDocument>
#include <QtConcurrent>
#include <QtWidgets>
namespace nova {
ResourceTools::ResourceTools(QWidget *p) : QWidget(p) {
    setObjectName("resourceTools");
    auto v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    auto row = new QHBoxLayout;
    v->addLayout(row);
    scope = new QComboBox;
    scope->setObjectName("validationScope");
    scope->addItems({"Current asset", "Current folder", "Namespace", "Entire pack"});
    scope->setCurrentIndex(3);
    row->addWidget(scope);
    run = new QPushButton("Validate");
    run->setObjectName("runValidation");
    row->addWidget(run);
    cancelButton = new QPushButton("Cancel");
    cancelButton->setEnabled(false);
    row->addWidget(cancelButton);
    status = new QLabel("Validate supported Minecraft resource fields and dependencies.");
    status->setWordWrap(true);
    v->addWidget(status);
    progress = new QProgressBar;
    progress->setObjectName("validationProgress");
    progress->hide();
    v->addWidget(progress);
    problems = new QTreeWidget;
    problems->setObjectName("problemsList");
    problems->setColumnCount(5);
    problems->setHeaderLabels(
        {"Severity", "File", "Field / line", "Explanation", "Suggested action"});
    problems->setRootIsDecorated(false);
    problems->setUniformRowHeights(true);
    problems->setAlternatingRowColors(false);
    for (int column = 2; column < 5; ++column)
        problems->hideColumn(column);
    problems->header()->setStretchLastSection(false);
    problems->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    v->addWidget(problems, 1);
    detail = new QLabel;
    detail->setWordWrap(true);
    detail->setTextFormat(Qt::PlainText);
    detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(detail);
    auto actions = new QHBoxLayout;
    v->addLayout(actions);
    auto open = new QPushButton("Open related asset");
    open->setObjectName("openProblemAsset");
    actions->addWidget(open);
    repair = new QPushButton("Preview repair…");
    repair->setObjectName("previewRepair");
    actions->addWidget(repair);
    repair->setEnabled(false);
    exportText = new QPushButton("Export text…");
    actions->addWidget(exportText);
    exportJson = new QPushButton("Export JSON…");
    actions->addWidget(exportJson);
    exportText->setEnabled(false);
    exportJson->setEnabled(false);
    QObject::connect(run, &QPushButton::clicked, this, &ResourceTools::validate);
    QObject::connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (cancel)
            cancel->store(true);
    });
    QObject::connect(problems, &QTreeWidget::itemSelectionChanged, this, &ResourceTools::select);
    QObject::connect(open, &QPushButton::clicked, this, [this] {
        auto i = problems->currentItem();
        if (i && navigate)
            navigate(report.diagnostics[i->data(0, Qt::UserRole).toInt()].file);
    });
    QObject::connect(problems, &QTreeWidget::itemDoubleClicked, this, [open] { open->click(); });
    QObject::connect(repair, &QPushButton::clicked, this, &ResourceTools::fix);
    QObject::connect(exportText, &QPushButton::clicked, this, [this] { exportReport(false); });
    QObject::connect(exportJson, &QPushButton::clicked, this, [this] { exportReport(true); });
}
ResourceTools::~ResourceTools() {
    if (cancel)
        cancel->store(true);
    if (watcher)
        watcher->future().waitForFinished();
}
void ResourceTools::validate() {
    if (watcher || !context)
        return;
    const auto c = context();
    if (!c.library || c.root.isEmpty()) {
        status->setText("Open a resource pack first.");
        return;
    }
    QStringList selected;
    if (scope->currentIndex() == 0) {
        if (c.asset.isEmpty()) {
            status->setText("Select a resource asset first.");
            return;
        }
        selected << c.asset;
    }
    if (scope->currentIndex() == 1 && !c.folder.isEmpty())
        selected << c.folder;
    if (scope->currentIndex() == 2) {
        auto p = c.asset.split('/');
        if (p.size() < 3 || p[0] != "assets") {
            status->setText("Select an asset in the desired namespace first.");
            return;
        }
        selected << "assets/" + p[1];
    }
    cancel = std::make_shared<std::atomic_bool>(false);
    ++population;
    run->setEnabled(false);
    cancelButton->setEnabled(true);
    repair->setEnabled(false);
    exportText->setEnabled(false);
    exportJson->setEnabled(false);
    status->setText("Validating… cancellation is available.");
    watcher = new QFutureWatcher<QPair<mc::ValidationEngine, mc::ValidationReport>>(this);
    auto active = watcher;
    auto cancellation = cancel;
    auto previous = engine;
    const auto library = *c.library;
    auto completed = std::make_shared<std::atomic_int>(0),
         total = std::make_shared<std::atomic_int>(0);
    auto progressTimer = new QTimer(this);
    progress->setRange(0, 0);
    progress->show();
    QObject::connect(progressTimer, &QTimer::timeout, this, [this, completed, total] {
        progress->setRange(0, total->load());
        progress->setValue(completed->load());
    });
    progressTimer->start(100);
    QObject::connect(active, &QFutureWatcherBase::finished, this, [this, active, c, progressTimer] {
        progressTimer->stop();
        progressTimer->deleteLater();
        progress->hide();
        auto result = active->result();
        watcher = nullptr;
        active->deleteLater();
        run->setEnabled(true);
        cancelButton->setEnabled(false);
        if (!context || context().root != c.root) {
            status->setText("Project changed; stale results discarded.");
            return;
        }
        engine = std::move(result.first);
        report = std::move(result.second);
        reportRoot = c.root;
        ++population;
        problems->clear();
        populate();
        exportText->setEnabled(true);
        exportJson->setEnabled(true);
        status->setText(QString("%1 files · %2 issues · %3 ms · %4 cached%5%6")
                            .arg(report.files)
                            .arg(report.diagnostics.size())
                            .arg(report.milliseconds)
                            .arg(report.reused)
                            .arg(report.canceled ? " · canceled" : "",
                                 report.truncated ? " · partial (budget)" : ""));
    });
    active->setFuture(QtConcurrent::run([previous = std::move(previous), c, library, selected,
                                         cancellation, completed, total]() mutable {
        auto r = previous.run(
            c.root, c.index, library, selected, {}, cancellation,
            [completed, total](int done, int count) {
                completed->store(done);
                total->store(count);
            },
            c.documents);
        return qMakePair(std::move(previous), std::move(r));
    }));
}
void ResourceTools::populate(int offset) {
    const int end = std::min(offset + 128, int(report.diagnostics.size()));
    for (int i = offset; i < end; ++i) {
        const auto &d = report.diagnostics[i];
        auto item =
            new QTreeWidgetItem({mc::severityName(d.severity), d.file,
                                 d.field + (d.line ? " line " + QString::number(d.line) : ""),
                                 d.message, d.suggestion});
        item->setData(0, Qt::UserRole, i);
        item->setToolTip(3, d.message);
        item->setToolTip(4, d.suggestion);
        problems->addTopLevelItem(item);
    }
    if (end < report.diagnostics.size())
        QTimer::singleShot(0, this, [this, end, generation = population] {
            if (generation == population)
                populate(end);
        });
    else {
        problems->setColumnWidth(0, 90);
        problems->setColumnWidth(1, 260);
        problems->setColumnWidth(2, 150);
        problems->setColumnWidth(3, 350);
    }
}
void ResourceTools::select() {
    auto i = problems->currentItem();
    if (!i) {
        repair->setEnabled(false);
        return;
    }
    const auto &d = report.diagnostics[i->data(0, Qt::UserRole).toInt()];
    detail->setText(d.file + " " + d.field + (d.line ? " · line " + QString::number(d.line) : "") +
                    "\n" + d.message + "\n" + d.suggestion +
                    (d.affected.isEmpty() ? "" : "\nAffected: " + d.affected.join(", ")));
    repair->setEnabled(!watcher && !d.replacement.isEmpty());
}
void ResourceTools::fix() {
    auto i = problems->currentItem();
    if (!i || !context || context().root != reportRoot)
        return;
    auto d = report.diagnostics[i->data(0, Qt::UserRole).toInt()];
    if (d.replacement.isEmpty())
        return;
    QDialog dialog(this);
    dialog.setObjectName("repairPreview");
    dialog.setWindowTitle("Review deterministic repair");
    auto v = new QVBoxLayout(&dialog);
    auto info = new QLabel(
        d.file + "\n" + d.suggestion +
        "\nThe original JSON is backed up in .nova/repairs. Stale revisions are rejected.");
    info->setWordWrap(true);
    v->addWidget(info);
    auto text = new QPlainTextEdit;
    text->setReadOnly(true);
    text->setPlainText(QString::fromUtf8(QJsonDocument(d.replacement).toJson()));
    v->addWidget(text);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Apply)->setObjectName("applyRepair");
    v->addWidget(buttons);
    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(700, 530);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto result = mc::applyRepair(reportRoot, d);
    status->setText(result.applied ? "Repair saved · backup: " + result.backup : result.error);
    if (result.applied) {
        engine = {};
        repair->setEnabled(false);
        if (changed)
            changed({d.file});
    }
}
void ResourceTools::exportReport(bool json) {
    if (reportRoot.isEmpty())
        return;
    const auto path =
        QFileDialog::getSaveFileName(this, "Export validation report",
                                     reportRoot + "/validation-report." + (json ? "json" : "txt"),
                                     json ? "JSON (*.json)" : "Text (*.txt)");
    if (path.isEmpty())
        return;
    auto rev = Revision::read(path);
    auto snapshot = report;
    auto watcher = new QFutureWatcher<QString>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        status->setText(watcher->result().isEmpty() ? "Validation report exported"
                                                    : watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path, rev, snapshot, json] {
        QString e;
        atomicWrite(path, json ? QJsonDocument(snapshot.json()).toJson() : snapshot.text().toUtf8(),
                    rev, &e);
        return e;
    }));
}
void createAssetDialog(const ResourceContext &c, QWidget *parent,
                       const std::function<void(const QStringList &)> &created) {
    if (c.root.isEmpty() || !c.library) {
        QMessageBox::information(parent, "Create asset", "Open a resource pack first.");
        return;
    }
    QDialog dialog(parent);
    dialog.setObjectName("createAssetDialog");
    dialog.setWindowTitle("Create Minecraft asset");
    auto v = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    v->addLayout(form);
    auto type = new QComboBox;
    type->setObjectName("assetType");
    type->addItems({"Block model", "Item model", "Basic blockstate", "Item model definition",
                    "Texture template", "Animated texture", "Model / texture pair"});
    form->addRow("Create", type);
    auto id = new QLineEdit("nova:block/new_model");
    id->setObjectName("assetIdentifier");
    form->addRow("Namespace:path", id);
    auto shape = new QComboBox;
    shape->setObjectName("assetTemplate");
    shape->addItems({"Full cube", "Single cuboid", "Crossed planes", "Flat item", "Layered item",
                     "Multi-element block"});
    form->addRow("Template", shape);
    auto ref = new QLineEdit;
    ref->setObjectName("assetReference");
    ref->setPlaceholderText("Texture ID; blockstate/item definition: model ID");
    form->addRow("Association", ref);
    auto size = new QSpinBox;
    size->setRange(1, 1024);
    size->setValue(16);
    form->addRow("Texture size", size);
    auto info = new QLabel("Target " + c.library->version() +
                           " · Original procedural artwork only. Existing files are preserved.");
    info->setWordWrap(true);
    v->addWidget(info);
    auto preview = new QPlainTextEdit;
    preview->setReadOnly(true);
    preview->setObjectName("assetPlan");
    v->addWidget(preview, 1);
    auto row = new QHBoxLayout;
    v->addLayout(row);
    auto plan = new QPushButton("Preview planned files");
    plan->setObjectName("previewAssetPlan");
    auto apply = new QPushButton("Create");
    apply->setObjectName("createAssetApply");
    apply->setEnabled(false);
    auto close = new QPushButton("Cancel");
    row->addWidget(plan);
    row->addWidget(apply);
    row->addWidget(close);
    mc::CreationPlan pending;
    auto invalidate = [&] {
        apply->setEnabled(false);
        pending = {};
    };
    QObject::connect(type, &QComboBox::currentIndexChanged, &dialog, invalidate);
    QObject::connect(id, &QLineEdit::textChanged, &dialog, invalidate);
    QObject::connect(shape, &QComboBox::currentIndexChanged, &dialog, invalidate);
    QObject::connect(ref, &QLineEdit::textChanged, &dialog, invalidate);
    QObject::connect(size, &QSpinBox::valueChanged, &dialog, invalidate);
    QObject::connect(plan, &QPushButton::clicked, &dialog, [&] {
        mc::CreationRequest r;
        r.type = mc::AssetType(type->currentIndex());
        r.shape = mc::ModelTemplate(shape->currentIndex());
        r.id = id->text();
        r.texture = ref->text();
        r.textureSize = size->value();
        r.version = c.library->version();
        pending = mc::planCreation(c.root, r);
        QString text = pending.error;
        for (const auto &f : pending.files)
            text += f.relative + " (" + QString::number(f.bytes.size()) + " bytes)\n" +
                    (f.relative.endsWith(".json") || f.relative.endsWith(".mcmeta")
                         ? QString::fromUtf8(f.bytes)
                         : "Original checker texture\n");
        preview->setPlainText(text);
        apply->setEnabled(pending.valid());
    });
    QObject::connect(apply, &QPushButton::clicked, &dialog, [&] {
        const auto result = mc::executeCreation(pending);
        if (created && !result.written.isEmpty())
            created(result.written);
        if (result.complete)
            dialog.accept();
        else {
            info->setText(result.error + "\nRecovery journal: " + result.journal);
            apply->setEnabled(false);
        }
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    dialog.resize(650, 570);
    dialog.exec();
}
} // namespace nova
