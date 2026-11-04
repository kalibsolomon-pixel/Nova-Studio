// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "timeline.h"
#include "opdialog.h"
#include "minecraft_animation.h"
#include "animation_export.h"
#include <QtConcurrent>
#include <QtWidgets>
namespace nova {
void Window::buildAnimation(QMenu *view) {
    // Cheap placeholder lets QMainWindow restore placement before lazy panel creation.
    animationDock = new QDockWidget("Animation", this); animationDock->setObjectName("animationDock");
    addDockWidget(Qt::BottomDockWidgetArea, animationDock); animationDock->hide();
    view->addAction(animationDock->toggleViewAction());
    connect(animationDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) ensureTimeline();
        if (!animationSyncing && current() && current()->document->animated()) preferences.timelineVisible = visible;
    });
    auto menu = menuBar()->addMenu("&Animation"); menu->setObjectName("animationMenu");
    auto add = [this, menu](const QString &text, const QString &name, const QString &shortcut, std::function<void()> callback) {
        auto action = menu->addAction(text); action->setObjectName(name);
        if (!shortcut.isEmpty()) action->setShortcut(QKeySequence(shortcut));
        connect(action, &QAction::triggered, this, callback); return action;
    };
    add("New Animated Texture…", "newAnimation", "Ctrl+Alt+N", [this] { newAnimation(); });
    add("Convert Current Texture to Animation", "convertAnimation", {}, [this] {
        if (!current()) return;
        auto state = current()->document->animationState(); state.enabled = true; QString error;
        if (!session.animationEdit(current()->document, state, "Convert to Animation", &error)) QMessageBox::warning(this, "Animation", error);
    });
    add("Import Minecraft Animation…", "importAnimation", {}, [this] {
        auto file = QFileDialog::getOpenFileName(this, "Open Minecraft PNG + .png.mcmeta", project, "PNG spritesheets (*.png)");
        if (!file.isEmpty()) openTexture(file);
    });
    menu->addSeparator();
    add("Play / Pause", "playAnimation", "Ctrl+Return", [this] {
        if (!current() || !current()->document->animated()) return;
        ensureTimeline();
        if (timeline->playback.playing()) timeline->playback.pause(); else { current()->commitFloating(); current()->document->finish(); timeline->playback.play(); }
    });
    add("Stop", "stopAnimation", {}, [this] { if (timeline) timeline->playback.stop(); });
    for (int delta : {-1, 1}) add(delta < 0 ? "Previous Frame" : "Next Frame", delta < 0 ? "previousFrame" : "nextFrame",
                                  delta < 0 ? "Alt+Left" : "Alt+Right", [this, delta] { if (timeline) timeline->navigate(delta); });
    add("First Frame", "firstFrame", "Alt+Home", [this] { if (timeline) timeline->navigate(0, true); });
    add("Last Frame", "lastFrame", "Alt+End", [this] { if (timeline) timeline->navigate(-1, true); });
    add("Add Blank Frame", "addAnimationFrame", "Ctrl+Alt+Insert", [this] { if (timeline) timeline->frameCommand(FrameCommand::Add); });
    add("Duplicate Frame", "duplicateAnimationFrame", "Ctrl+Alt+D", [this] { if (timeline) timeline->frameCommand(FrameCommand::Duplicate); });
    add("Toggle Onion Skin", "toggleOnion", "Alt+O", [this] { if (timeline) if (auto box = timeline->findChild<QCheckBox *>("onionEnabled")) box->toggle(); });
    menu->addSeparator();
    add("Animation Properties / Metadata…", "animationMetadata", {}, [this] { animationProperties(); });
    add("Process Frames…", "animationScope", {}, [this] { processAnimationFrames(); });
    add("Export Minecraft Animation…", "exportAnimation", {}, [this] { exportAnimation(); });
    add("Recover Interrupted Export…", "recoverAnimationExport", {}, [this] { recoverAnimationExport(); });
    add("Animation Resource Limits…", "animationLimits", {}, [this] {
        if (!current() || !current()->document->animated()) return;
        auto doc = current()->document; auto limits = doc->frameLimits();
        QDialog dialog(this); dialog.setWindowTitle("Current Animation Resource Limits"); auto form = new QFormLayout(&dialog);
        auto spin = [&](const QString &label, int value, int min, int max) { auto box = new QSpinBox; box->setRange(min, max); box->setValue(value); form->addRow(label, box); return box; };
        auto frames = spin("Source frames", limits.sourceFrames, 1, 4096), entries = spin("Playback entries", limits.playbackEntries, 1, 65536);
        auto decoded = spin("Decoded pixels (MiB)", int(limits.decodedBytes / 1048576), 1, 128);
        auto composites = spin("Frame composites cache (MiB)", int(limits.cacheBytes / 1048576), 0, 64);
        auto encoded = spin("Native/recovery payload (MiB)", int(limits.serializedBytes / 1048576), 1, 120);
        auto thumbs = spin("Timeline thumbnails (MiB)", 8, 0, 64);
        auto note = new QLabel("Limits apply to this open document. Undo history limits are in Edit › Preferences. Decoded pixels include all layer buffers and one composite; memory sharing does not bypass admission limits."); note->setWordWrap(true); form->addRow(note);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            limits.sourceFrames = frames->value(); limits.playbackEntries = entries->value();
            limits.decodedBytes = qint64(decoded->value()) * 1048576; limits.cacheBytes = qint64(composites->value()) * 1048576;
            limits.serializedBytes = qint64(encoded->value()) * 1048576; QString error;
            if (!doc->setAnimationLimits(limits, &error)) { QMessageBox::warning(&dialog, "Resource limits", error); return; }
            timeline->setThumbnailBudget(thumbs->value() * 1048576); dialog.accept();
        }); dialog.exec();
    });
    add("Export Active Frame as Static PNG…", "flattenAnimation", {}, [this] {
        if (!current() || !current()->document->animated()) return;
        auto target = QFileDialog::getSaveFileName(this, "Flatten active frame to static PNG", {}, "PNG (*.png)");
        if (target.isEmpty()) return;
        if (pathIdentity(target) == pathIdentity(current()->document->path) || QFileInfo::exists(target + ".mcmeta")) {
            QMessageBox::warning(this, "Static export", "Choose a separate path without animation metadata."); return;
        }
        auto expected = Revision::read(target); QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        current()->document->image.save(&buffer, "PNG"); QString error;
        if (!atomicWrite(target, bytes, expected, &error)) QMessageBox::warning(this, "Static export", error);
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (timeline && state != Qt::ApplicationActive) timeline->playback.pause();
    });
}
void Window::ensureTimeline() {
    if (timeline) return;
    timeline = new TimelinePanel(&session); animationDock->setWidget(timeline);
    timeline->setMetrics(&metrics);
    connect(timeline, &TimelinePanel::failure, this, [this](const QString &error) { QMessageBox::warning(this, "Animation", error); });
    connect(timeline, &TimelinePanel::beforeContextChange, this, [this] {
        if (auto canvas = current()) { canvas->commitFloating(); canvas->document->finish(); canvas->cancelInteraction(); canvas->clearExternalPreview(); }
    });
    connect(timeline, &TimelinePanel::propertiesRequested, this, &Window::animationProperties);
    connect(timeline, &TimelinePanel::scopeOperationRequested, this, [this] { processAnimationFrames(); });
    connect(&timeline->playback, &PlaybackController::displayed, this, [this](const QImage &image, int) {
        if (auto canvas = current()) { canvas->setPlaybackPreview(image); cube->setPlaybackPreview(image); }
        previewDock->setWindowTitle("3D Preview · Playing");
    });
    connect(&timeline->playback, &PlaybackController::stopped, this, [this] {
        for (int i = 0; i < tabs->count(); ++i)
            if (auto canvas = dynamic_cast<Canvas *>(tabs->widget(i)))
                canvas->setPlaybackPreview({});
        cube->setPlaybackPreview({});
        if (previewDock) previewDock->setWindowTitle("3D Preview");
    });
    connect(timeline, &TimelinePanel::onionChanged, this, [this](bool enabled, int previous, int next, int opacity, bool tint) {
        if (auto canvas = current()) { canvas->onion = {enabled, previous, next, opacity, tint}; canvas->update(); }
    });
    if (current()) timeline->setDocument(current()->document);
    timeline->setViewSettings(QJsonDocument::fromJson(preferences.animationView).object());
    if (preferences.animationView.isEmpty()) QTimer::singleShot(0, this, [this] {
        if (animationDock->isVisible()) { resizeDocks({animationDock, previewDock}, {900, 300}, Qt::Horizontal); resizeDocks({animationDock}, {275}, Qt::Vertical); }
    });
}
void Window::animationRefresh() {
    QScopedValueRollback<bool> syncing(animationSyncing, true);
    auto canvas = current();
    const bool enabled = canvas && canvas->document->animated();
    setCorner(Qt::BottomLeftCorner, enabled ? Qt::BottomDockWidgetArea : Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, enabled ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea);
    if (enabled) {
        ensureTimeline(); timeline->setDocument(canvas->document); animationDock->setVisible(preferences.timelineVisible);
        const auto view = timeline->viewSettings();
        canvas->onion = {view["onionEnabled"].toBool(), view["onionPrevious"].toInt(1), view["onionNext"].toInt(1), view["onionOpacity"].toInt(25), view["onionTint"].toBool(true)};
    }
    else { if (timeline) timeline->setDocument({}); animationDock->hide(); }
}
void Window::newAnimation() {
    if (project.isEmpty()) { QMessageBox::information(this, "New animation", "Open or create a resource pack first."); return; }
    QDialog dialog(this); dialog.setWindowTitle("New Animated Texture"); auto layout = new QFormLayout(&dialog);
    auto width = new QSpinBox; width->setRange(1, 4096); width->setValue(16); width->setObjectName("newFrameWidth");
    auto height = new QSpinBox; height->setRange(1, 4096); height->setValue(16); height->setObjectName("newFrameHeight");
    layout->addRow("Frame width", width); layout->addRow("Frame height", height);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    auto target = QFileDialog::getSaveFileName(this, "Animation asset path (saved as editable source first)", project + "/assets/minecraft/textures/block/animation.png", "PNG (*.png)");
    if (target.isEmpty()) return;
    if (QFileInfo::exists(target) || QFileInfo::exists(target + ".mcmeta") || nativeDocumentPath(project, target).isEmpty() ||
        QFileInfo::exists(nativeDocumentPath(project, target))) { QMessageBox::warning(this, "New animation", "Choose an unused asset path inside the current pack."); return; }
    QImage image(width->value(), height->value(), QImage::Format_RGBA8888); image.fill(Qt::transparent);
    auto doc = std::make_shared<Document>(target, image); QString error;
    if (!doc->convertToAnimation(&error) || !session.adopt(doc)) { QMessageBox::warning(this, "New animation", error.isEmpty() ? "Session limits reached" : error); return; }
    openTexture(target);
}
void Window::saveAnimationNative() {
    auto canvas = current(); if (!canvas || !canvas->document->animated()) return;
    if (canvas->hasFloating() && !canvas->commitFloating()) return;
    auto doc = canvas->document; doc->finish();
    if (timeline) timeline->playback.stop();
    const auto serial = doc->changeSerial(); auto snapshot = doc->recoverySnapshot();
    const auto root = project.isEmpty() ? QFileInfo(doc->path).absolutePath() : project;
    QProgressDialog progress("Saving editable animation…", QString(), 0, 0, this); progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(200);
    QFutureWatcher<QString> watcher; QEventLoop wait;
    connect(&watcher, &QFutureWatcherBase::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(&ioPool, [snapshot, root] { QString error; snapshot->saveNative(root, &error); return error; }));
    if (!watcher.isFinished()) wait.exec(); progress.close();
    if (!watcher.result().isEmpty()) QMessageBox::warning(this, "Editable animation save failed", watcher.result());
    else {
        doc->acceptNativeSave(*snapshot, serial);
        if (!doc->modified()) recovery->remove(doc->id());
        statusBar()->showMessage("Editable animation saved · Export Minecraft Animation to update the game files", 5000);
    }
    session.notify(doc);
}
void Window::animationProperties() {
    auto canvas = current(); if (!canvas || !canvas->document->animated()) return;
    auto doc = canvas->document; if (timeline) timeline->playback.stop();
    QDialog dialog(this); dialog.setObjectName("animationPropertiesDialog"); dialog.setWindowTitle("Animation Properties"); dialog.resize(610, 530);
    auto layout = new QVBoxLayout(&dialog); auto form = new QFormLayout;
    auto ticks = new QSpinBox; ticks->setRange(1, INT_MAX); ticks->setValue(doc->animationState().defaultTicks);
    auto interpolate = new QCheckBox("Blend RGBA between frames at game ticks"); interpolate->setChecked(doc->animationState().interpolate);
    auto target = new QComboBox; target->addItems({"1.21.11", "26.1", "26.3"}); target->setCurrentText(doc->animationState().target);
    form->addRow("Default ticks (50 ms each)", ticks); form->addRow("Interpolation", interpolate); form->addRow("Java format profile", target); layout->addLayout(form);
    auto warning = new QLabel("Animation availability depends on the game's atlas and asset type. Unknown metadata sections are preserved."); warning->setWordWrap(true); layout->addWidget(warning);
    layout->addWidget(new QLabel("Advanced metadata (JSON; use timeline for ordinary timing edits)"));
    QString error; auto plan = planAnimationExport(*doc, 1, {}, &error);
    auto raw = new QPlainTextEdit(QString::fromUtf8(plan.mcmeta)); raw->setObjectName("animationRawMetadata"); layout->addWidget(raw, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close); layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [=, &dialog, this] {
        if (raw->toPlainText().size() > 512 * 1024) { QMessageBox::warning(&dialog, "Metadata", "Metadata exceeds 512 KiB"); return; }
        QJsonParseError parse; auto json = QJsonDocument::fromJson(raw->toPlainText().toUtf8(), &parse); AnimationState parsed;
        QString error;
        if (parse.error != QJsonParseError::NoError || !json.isObject() || !parseAnimation(plan.sheet, json.object(), target->currentText(), &parsed, &error, doc->frameLimits())) {
            QMessageBox::warning(&dialog, "Metadata", error.isEmpty() ? parse.errorString() : error); return;
        }
        auto state = doc->animationState();
        if (parsed.frames.size() != state.frames.size() || parsed.frames[0].layers[0].pixels.size() != doc->image.size()) {
            QMessageBox::warning(&dialog, "Metadata", "Use image operations to resize frames; metadata dimensions must match the editable frames."); return;
        }
        QHash<QUuid, int> indices; for (int i = 0; i < int(parsed.frames.size()); ++i) indices[parsed.frames[size_t(i)].id] = i;
        state.entries.clear(); for (const auto &entry : parsed.entries) state.entries.push_back({state.frames[size_t(indices[entry.frame])].id, entry.ticks});
        state.metadata = json.object(); state.target = target->currentText();
        // Friendly controls win when changed; raw JSON controls otherwise retain its edits.
        state.defaultTicks = ticks->value() == doc->animationState().defaultTicks ? parsed.defaultTicks : ticks->value();
        state.interpolate = interpolate->isChecked() == doc->animationState().interpolate ? parsed.interpolate : interpolate->isChecked();
        if (!session.animationEdit(doc, state, "Animation Properties", &error)) QMessageBox::warning(&dialog, "Metadata", error);
        else dialog.accept();
    });
    dialog.exec();
}
void Window::processAnimationFrames(const QString &operation) {
    auto canvas = current(); if (!canvas || !canvas->document->animated()) return;
    auto doc = canvas->document; timeline->playback.stop(); canvas->commitFloating(); doc->finish();
    QDialog dialog(this); dialog.setWindowTitle("Process Animation Frames"); dialog.setObjectName("animationScopeDialog"); dialog.resize(540, 520);
    auto layout = new QVBoxLayout(&dialog); auto scope = new QComboBox; scope->addItems({"Active source frame", "Selected source frames", "All source frames"}); scope->setObjectName("frameOperationScope"); layout->addWidget(scope);
    auto operations = new QComboBox;
    for (const auto &op : ops::registry()) operations->addItem(op.name, op.id);
    if (!operation.isEmpty()) { operations->setCurrentIndex(operations->findData(operation)); if (ops::find(operation)->changesSize) scope->setCurrentIndex(2); }
    layout->addWidget(operations); auto params = new ParamEditor; layout->addWidget(params);
    params->setOperation(operations->currentData().toString());
    connect(operations, &QComboBox::currentIndexChanged, &dialog, [=](int) { params->setOperation(operations->currentData().toString()); });
    auto note = new QLabel("Operations use each frame's active layer. Resizing requires all frames and processes every layer. A shared procedural seed is used for every frame."); note->setWordWrap(true); layout->addWidget(note);
    auto preview = new QLabel("Preview before applying"); preview->setAlignment(Qt::AlignCenter); layout->addWidget(preview, 1);
    auto status = new QLabel; status->setWordWrap(true); layout->addWidget(status);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); auto dryRun = buttons->addButton("Preview", QDialogButtonBox::ActionRole);
    auto apply = buttons->addButton("Apply", QDialogButtonBox::AcceptRole); apply->setEnabled(false); layout->addWidget(buttons);
    std::shared_ptr<Document> prepared; quint64 serial = 0; std::shared_ptr<std::atomic_bool> canceled;
    auto invalidate = [&] { apply->setEnabled(false); prepared.reset(); if (canceled) canceled->store(true); };
    connect(scope, &QComboBox::currentIndexChanged, &dialog, invalidate); connect(params, &ParamEditor::edited, &dialog, invalidate);
    connect(operations, &QComboBox::currentIndexChanged, &dialog, invalidate);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(dryRun, &QPushButton::clicked, &dialog, [&] {
        std::vector<QUuid> ids;
        if (scope->currentIndex() == 0) ids = {doc->activeFrame()};
        else if (scope->currentIndex() == 1) ids = timeline->selectedFrames();
        else for (const auto &frame : doc->animationState().frames) ids.push_back(frame.id);
        auto snapshot = doc->recoverySnapshot(); serial = doc->changeSerial();
        if (scope->currentIndex() == 0 && doc->hasSelection()) snapshot->setSelection(doc->selection());
        const ops::Spec spec{operations->currentData().toString(), params->params()};
        canceled = std::make_shared<std::atomic_bool>(false); const auto cancelFlag = canceled;
        QProgressDialog progress("Preparing frame preview…", "Cancel", 0, 0, &dialog); progress.setWindowModality(Qt::WindowModal);
        QFutureWatcher<QString> watcher; QEventLoop wait;
        connect(&watcher, &QFutureWatcherBase::finished, &wait, &QEventLoop::quit);
        connect(&progress, &QProgressDialog::canceled, &dialog, [cancelFlag] { cancelFlag->store(true); });
        watcher.setFuture(QtConcurrent::run([snapshot, ids, spec, cancelFlag] {
            ops::Context context; context.cancel = cancelFlag.get(); QString error;
            snapshot->applyFrames(ids, spec, &error, &context); return error;
        }));
        if (!watcher.isFinished()) wait.exec(); progress.reset();
        if (!watcher.result().isEmpty() || canceled->load() || serial != doc->changeSerial()) {
            status->setText(watcher.result().isEmpty() ? "Canceled or stale preview" : watcher.result()); return;
        }
        prepared = snapshot; preview->setPixmap(QPixmap::fromImage(snapshot->image.scaled(192, 192, Qt::KeepAspectRatio, Qt::FastTransformation)));
        if (!snapshot->canUndo()) {
            status->setText("This operation exceeds the current history budget. Increase Edit › Preferences › Undo history or process fewer source frames, then preview again.");
            apply->setEnabled(false);
        } else {
            status->setText(QString("%1 source frames prepared; one undo action").arg(ids.size())); apply->setEnabled(true);
        }
    });
    connect(apply, &QPushButton::clicked, &dialog, [&] {
        QString error;
        if (!prepared || serial != doc->changeSerial()) { status->setText("Preview is stale; preview again."); return; }
        if (!session.animationEdit(doc, prepared->animationState(), "Process Animation Frames", &error)) { status->setText(error); return; }
        dialog.accept();
    });
    dialog.exec(); if (canceled) canceled->store(true);
}
void Window::exportAnimation(bool as) {
    auto canvas = current(); if (!canvas || !canvas->document->animated()) return;
    if (canvas->hasFloating() && !canvas->commitFloating()) return;
    auto doc = canvas->document; doc->finish(); timeline->playback.stop();
    QString destination = doc->path;
    if (as) {
        destination = QFileDialog::getSaveFileName(this, "Export animation PNG + .png.mcmeta", doc->path, "PNG spritesheets (*.png)");
        if (destination.isEmpty()) return;
    }
    if (!pendingAnimationPairs(destination).isEmpty()) { recoverAnimationExport(destination); return; }
    const bool original = pathIdentity(destination) == pathIdentity(doc->path);
    const auto expectedPng = original ? doc->revision() : Revision::read(destination);
    const auto expectedMeta = original ? doc->mcmetaRevision() : Revision::read(destination + ".mcmeta");
    QDialog dialog(this); dialog.setObjectName("animationExportDialog"); dialog.setWindowTitle("Export Minecraft Animation"); dialog.resize(620, 570);
    auto layout = new QVBoxLayout(&dialog); auto form = new QFormLayout;
    auto columns = new QSpinBox; columns->setObjectName("exportColumns"); columns->setRange(1, int(doc->animationState().frames.size()));
    auto target = new QComboBox; target->addItems({"1.21.11", "26.1", "26.3"}); target->setCurrentText(doc->animationState().target);
    auto summary = new QLabel; summary->setTextInteractionFlags(Qt::TextSelectableByMouse); summary->setWordWrap(true);
    form->addRow("Columns", columns); form->addRow("Java format profile", target); layout->addLayout(form); layout->addWidget(summary);
    auto preview = new QLabel; preview->setAlignment(Qt::AlignCenter); preview->setMinimumHeight(120); layout->addWidget(preview, 1);
    auto diagnostics = new QLabel; diagnostics->setObjectName("exportDiagnostics"); diagnostics->setWordWrap(true); layout->addWidget(diagnostics);
    auto note = new QLabel("Each file is replaced atomically. The pair uses retained backups and a recovery journal. Keep the editable source saved separately."); note->setWordWrap(true); layout->addWidget(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); auto exportButton = buttons->addButton("Export pair", QDialogButtonBox::AcceptRole);
    exportButton->setObjectName("commitAnimationExport"); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    AnimationExport planned; quint64 plannedSerial = 0;
    auto dryRun = [&] {
        exportButton->setEnabled(false); planned = {}; QString error;
        if (!expectedPng.sameContent(Revision::read(destination)) || !expectedMeta.sameContent(Revision::read(destination + ".mcmeta"))) {
            diagnostics->setText("CONFLICT: PNG or .mcmeta changed externally. Export to a separate path or review the disk files. No files were changed."); return;
        }
        auto snapshot = doc->recoverySnapshot(); plannedSerial = doc->changeSerial();
        QProgressDialog progress("Validating spritesheet and metadata…", QString(), 0, 0, &dialog); progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(200);
        QFutureWatcher<std::pair<AnimationExport, QString>> watcher; QEventLoop wait;
        connect(&watcher, &QFutureWatcherBase::finished, &wait, &QEventLoop::quit);
        const int count = columns->value(); const auto version = target->currentText();
        watcher.setFuture(QtConcurrent::run(&ioPool, [snapshot, count, version] {
            QString error; auto plan = planAnimationExport(*snapshot, count, version, &error); return std::make_pair(plan, error);
        }));
        if (!watcher.isFinished()) wait.exec(); progress.close();
        planned = watcher.result().first; error = watcher.result().second;
        if (!planned.ok() || plannedSerial != doc->changeSerial()) { diagnostics->setText(error.isEmpty() ? "Document changed; validate again" : error); return; }
        // Export As retains unrelated metadata already present at the new destination.
        if (!original && expectedMeta.exists) {
            QFile file(destination + ".mcmeta");
            if (!file.open(QIODevice::ReadOnly) || file.size() > 512 * 1024) { diagnostics->setText("Destination metadata is unreadable or oversized"); return; }
            QJsonParseError parse; auto json = QJsonDocument::fromJson(file.readAll(), &parse);
            if (parse.error != QJsonParseError::NoError || !json.isObject()) { diagnostics->setText("Destination metadata is malformed; preserve and repair it first"); return; }
            auto merged = planned.metadata;
            const auto existing = json.object();
            for (auto it = existing.begin(); it != existing.end(); ++it) if (it.key() != "animation") merged[it.key()] = it.value();
            planned.metadata = merged; planned.mcmeta = QJsonDocument(merged).toJson(QJsonDocument::Indented);
            planned.diagnostics.append("Unrelated sections at the destination are retained.");
        }
        const auto state = doc->animationState();
        summary->setText(QString("Frames: %1 × %2 px · %3 source frames · %4 playback entries\nSpritesheet: %5 × %6 px\nDefault: %7 ticks (%8 ms)\nPNG: %9\nMetadata: %9.mcmeta%10")
            .arg(doc->image.width()).arg(doc->image.height()).arg(state.frames.size()).arg(state.entries.size())
            .arg(planned.sheet.width()).arg(planned.sheet.height()).arg(state.defaultTicks).arg(qint64(state.defaultTicks) * 50).arg(destination)
            .arg(expectedPng.exists || expectedMeta.exists ? "\nExisting files will be replaced using the revisions validated here." : ""));
        preview->setPixmap(QPixmap::fromImage(planned.sheet.scaled(480, 180, Qt::KeepAspectRatio, Qt::FastTransformation)));
        diagnostics->setText("Dry run passed. " + planned.diagnostics.join('\n')); exportButton->setEnabled(true);
    };
    connect(columns, &QSpinBox::valueChanged, &dialog, dryRun); connect(target, &QComboBox::currentIndexChanged, &dialog, dryRun);
    connect(exportButton, &QPushButton::clicked, &dialog, [&] {
        if (!planned.ok() || plannedSerial != doc->changeSerial()) { diagnostics->setText("Document changed; validate again"); return; }
        QProgressDialog progress("Writing guarded PNG + metadata pair…", QString(), 0, 0, &dialog); progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(200);
        struct Result { QString error, journal; bool ok = false; Revision png, meta; };
        QFutureWatcher<Result> watcher; QEventLoop wait; connect(&watcher, &QFutureWatcherBase::finished, &wait, &QEventLoop::quit);
        auto png = planned.png, mcmeta = planned.mcmeta;
        watcher.setFuture(QtConcurrent::run(&ioPool, [destination, png, mcmeta, expectedPng, expectedMeta] {
            Result result; result.journal = stageAnimationPair(destination, png, mcmeta, expectedPng, expectedMeta, &result.error);
            if (!result.journal.isEmpty()) result.ok = finishAnimationPair(result.journal, PairAction::Retry, &result.error);
            if (result.ok) { result.png = Revision::read(destination); result.meta = Revision::read(destination + ".mcmeta"); }
            return result;
        }));
        if (!watcher.isFinished()) wait.exec(); progress.close(); const auto result = watcher.result();
        if (!result.ok) {
            diagnostics->setText(result.error + (result.journal.isEmpty() ? "" : "\nRecovery journal: " + result.journal));
            QMessageBox::warning(&dialog, "Animation export conflict or failure", diagnostics->text()); exportButton->setEnabled(false); return;
        }
        if (original) { doc->acceptAnimationExport(result.png, result.meta, plannedSerial == doc->changeSerial()); session.notify(doc); }
        statusBar()->showMessage("Minecraft animation pair exported · Recoverable backups retained · Save editable source to retain the export baseline", 7000);
        dialog.accept();
    });
    dryRun(); dialog.exec();
}
void Window::recoverAnimationExport(const QString &requested) {
    auto canvas = current();
    const auto path = requested.isEmpty() ? (canvas ? canvas->document->path : QString{}) : requested;
    if (path.isEmpty()) return;
    auto doc = canvas && pathIdentity(canvas->document->path) == pathIdentity(path) ? canvas->document : nullptr;
    const auto pending = pendingAnimationPairs(path);
    if (pending.isEmpty()) { QMessageBox::information(this, "Export recovery", "No pending export transaction exists for this asset."); return; }
    if (timeline) timeline->playback.stop();
    QDialog dialog(this); dialog.setObjectName("animationExportRecovery"); dialog.setWindowTitle("Recover Interrupted Animation Export"); dialog.resize(620, 380);
    auto layout = new QVBoxLayout(&dialog); auto list = new QListWidget; list->addItems(pending); list->setCurrentRow(0); layout->addWidget(list);
    auto description = new QLabel("The two files may be at different revisions. Retry finishes the saved pair; rollback restores the recorded originals. Both actions reject external changes and damaged backups."); description->setWordWrap(true); layout->addWidget(description);
    auto status = new QLabel; status->setWordWrap(true); layout->addWidget(status);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close); auto retry = buttons->addButton("Retry saved pair", QDialogButtonBox::ActionRole);
    auto rollback = buttons->addButton("Roll back pair", QDialogButtonBox::ActionRole); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto finish = [&](PairAction action) {
        if (!list->currentItem()) return;
        const auto journal = list->currentItem()->text(); QFutureWatcher<QString> watcher; QEventLoop wait;
        connect(&watcher, &QFutureWatcherBase::finished, &wait, &QEventLoop::quit); retry->setEnabled(false); rollback->setEnabled(false);
        watcher.setFuture(QtConcurrent::run(&ioPool, [journal, action] { QString error; finishAnimationPair(journal, action, &error); return error; }));
        if (!watcher.isFinished()) wait.exec(); retry->setEnabled(true); rollback->setEnabled(true);
        if (!watcher.result().isEmpty()) { status->setText(watcher.result()); return; }
        bool matches = false;
        if (action == PairAction::Retry && doc && doc->animated()) {
            auto planned = planAnimationExport(*doc); QFile png(doc->path), meta(doc->path + ".mcmeta");
            if (png.open(QIODevice::ReadOnly) && meta.open(QIODevice::ReadOnly)) matches = png.read(128LL * 1024 * 1024 + 1) == planned.png && meta.read(512 * 1024 + 1) == planned.mcmeta;
        }
        if (doc && doc->animated()) {
            doc->acceptAnimationExport(Revision::read(doc->path), Revision::read(doc->path + ".mcmeta"), matches); session.notify(doc);
        }
        status->setText(action == PairAction::Retry ? "Saved pair recovered. Save the editable document to retain its new revision baseline." : "Original pair restored; animation remains editable.");
        list->currentItem()->setText(journal + " · finished"); list->currentItem()->setFlags(Qt::NoItemFlags);
    };
    connect(retry, &QPushButton::clicked, &dialog, [&] { finish(PairAction::Retry); });
    connect(rollback, &QPushButton::clicked, &dialog, [&] { finish(PairAction::Rollback); });
    dialog.exec();
}
} // namespace nova
