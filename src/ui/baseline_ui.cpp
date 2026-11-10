// SPDX-License-Identifier: GPL-3.0-or-later
// Vanilla texture baseline in the workspace (docs/vanilla-baseline/DESIGN.md): cache loading,
// the merged asset-browser index, virtual baseline documents, Reset to Vanilla / Remove from
// Project with a project-level history, the download flow and cache management.
// `collection` and workspaceIndex() stay project-only; nothing here runs for a legacy project.
#include "baseline_ui.h"
#include "project.h"
#include "theme.h"
#include "window.h"
#include "vanilla/overlay.h"
#include <QtConcurrent>
#include <QtWidgets>

namespace nova {
using vanilla::BaselineCache;
using vanilla::BaselineEntry;
using InstallError = vanilla::BaselineInstaller::Error;

namespace {
QString megabytes(qint64 bytes) {
    const double mb = bytes / 1048576.0;
    return QString("%1 MB").arg(mb, 0, 'f', mb < 10 ? 1 : 0);
}
// The existing blocking-progress idiom: work runs off the GUI thread while a modal progress
// dialog keeps the event loop (and repainting) alive.
template <typename T, typename F>
T blocking(QWidget *parent, QThreadPool *pool, const QString &label, F task) {
    QProgressDialog progress(label, QString(), 0, 0, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);
    QFutureWatcher<T> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(pool ? QtConcurrent::run(pool, task) : QtConcurrent::run(task));
    if (!watcher.isFinished())
        loop.exec();
    progress.close();
    return watcher.result();
}
bool texture(const QString &relative) {
    return relative.startsWith("assets/") && relative.contains("/textures/") &&
           relative.endsWith(".png", Qt::CaseInsensitive);
}
// Verifies the cache objects a reset would show again (the PNG and, if vanilla has one, its .mcmeta).
BaselineCheck verifiedBaseline(std::shared_ptr<const BaselineEntry> entry) {
    return [entry](const QString &asset, QString *error) {
        if (!entry || !entry->contains(asset)) {
            if (error)
                *error = "The vanilla baseline has no " + asset;
            return false;
        }
        const BaselineCache cache(entry->root);
        QByteArray bytes;
        if (!cache.readVerified(*entry, asset, &bytes, error))
            return false;
        return !entry->contains(asset + ".mcmeta") ||
               cache.readVerified(*entry, asset + ".mcmeta", &bytes, error);
    };
}
struct StepResult {
    bool ok = false;
    QString id, error;
};
} // namespace

// ------------------------------------------------------------------ shared helpers (ui)
namespace ui {
std::function<void(vanilla::InstallerOptions &)> baselineInstallerHook;

QString baselineConsentText(const QString &version) {
    return QString("Nova will download the official Minecraft Java %1 client from Mojang (≈%2) and keep "
                   "only its textures in your local cache. Vanilla textures are Mojang content for your own "
                   "use; they are never added to your pack unless you edit them.")
        .arg(version, QString(baselineDownloadSize).remove("about "));
}
QString baselineAttribution() {
    return "Minecraft textures are © Mojang Studios, downloaded from Mojang's official servers for local use. "
           "Nova Studio is not affiliated with or endorsed by Mojang or Microsoft.";
}
BaselineRef baselineRef(const BaselineEntry &entry) {
    BaselineRef ref;
    ref.version = entry.version;
    ref.identity = entry.identity;
    ref.clientSha1 = entry.source.value("client").toObject().value("sha1").toString();
    const auto index = entry.source.value("asset_index").toObject();
    ref.assetIndexId = index.value("id").toString();
    ref.assetIndexSha1 = index.value("sha1").toString();
    return ref;
}
QString baselineErrorText(InstallError kind, const QString &version, const QString &detail) {
    const auto why = detail.isEmpty() ? QString() : "\n\n" + detail;
    switch (kind) {
    case InstallError::Network:
        return "Nova could not download the vanilla textures from Mojang's servers. Check your internet "
               "connection and try again." + why;
    case InstallError::Integrity:
        return "A file from Mojang did not match its published checksum or contained unexpected data, so "
               "nothing was kept. Try again later." + why;
    case InstallError::DiskSpace:
        return "There is not enough free disk space for the Minecraft client download. Free some space and "
               "try again." + why;
    case InstallError::Cancelled:
        return "The download was cancelled. Nothing was kept.";
    case InstallError::Busy:
        return "Another Nova Studio window is updating the vanilla texture cache. Try again when it has "
               "finished." + why;
    case InstallError::NotListed:
        return vanilla::notListedMessage(version) + ".";
    case InstallError::MetadataChanged:
        return QString("Mojang's official metadata for Minecraft Java %1 no longer matches the baseline this "
                       "project was created with.").arg(version) + why;
    case InstallError::Io:
        return "The vanilla texture cache could not be written." + why;
    case InstallError::None:
        break;
    }
    return detail;
}
QString cachedBaselineSummary(const QString &version) {
    std::optional<vanilla::BaselineSummary> best;
    for (const auto &s : BaselineCache().list())
        if (s.version == version && s.status == vanilla::BaselineSummary::Status::Complete &&
            (!best || s.created > best->created))
            best = s;
    if (!best)
        return {};
    return QString("Vanilla Java %1 textures are already in your local cache (downloaded %2 · %3 files · %4). "
                   "No download is needed.")
        .arg(version, best->created.toLocalTime().toString("yyyy-MM-dd"), QString::number(best->files),
             megabytes(best->bytes));
}
std::optional<BaselineEntry> obtainBaseline(QWidget *parent, const QString &version,
                                            const QString &expectedIdentity, bool repair,
                                            InstallError *kind, QString *error, bool askConsent) {
    *kind = InstallError::None;
    error->clear();
    if (!repair) {
        // A complete cached entry is used as it is: no network access at all.
        auto cached = blocking<std::optional<BaselineEntry>>(
            parent, nullptr, "Checking the vanilla texture cache…", [version, expectedIdentity] {
                const BaselineCache cache;
                return expectedIdentity.isEmpty() ? cache.latest(version)
                                                  : cache.load(version, expectedIdentity);
            });
        if (cached)
            return cached;
        if (askConsent &&
            QMessageBox::question(parent, "Download vanilla textures?",
                                  baselineConsentText(version) + "\n\nDownload them now?",
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes) {
            *kind = InstallError::Cancelled;
            return std::nullopt;
        }
    }
    vanilla::InstallerOptions options;
    options.expectedIdentity = expectedIdentity;
    options.repair = repair;
    if (baselineInstallerHook)
        baselineInstallerHook(options);
    vanilla::BaselineInstaller installer(options);
    QProgressDialog progress(QString("Preparing vanilla Java %1 textures…").arg(version), "Cancel", 0, 0, parent);
    progress.setObjectName("baselineDownloadProgress");
    progress.setWindowTitle("Vanilla textures");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    QObject::connect(&progress, &QProgressDialog::canceled, &installer, &vanilla::BaselineInstaller::cancel);
    QObject::connect(&installer, &vanilla::BaselineInstaller::progress, &progress,
                     [&](const QString &stage, qint64 done, qint64 total) {
                         progress.setLabelText(total > 0 ? QString("%1…\n%2 of %3").arg(stage, megabytes(done), megabytes(total))
                                                         : done > 0 ? QString("%1…\n%2").arg(stage, megabytes(done))
                                                                    : stage + "…");
                         // No invented percentage: an unknown total stays indeterminate.
                         if (total > 0) {
                             progress.setMaximum(1000);
                             progress.setValue(int(qBound(qint64(0), done * 1000 / total, qint64(1000))));
                         } else
                             progress.setMaximum(0);
                     });
    QEventLoop loop;
    bool finished = false;
    QString result;
    QObject::connect(&installer, &vanilla::BaselineInstaller::finished, &loop, [&](const QString &e) {
        result = e;
        finished = true;
        loop.quit();
    });
    installer.start(version);
    if (!finished)
        loop.exec();
    progress.close();
    if (result.isEmpty() && installer.entry().isValid())
        return installer.entry();
    *kind = installer.error() == InstallError::None ? InstallError::Io : installer.error();
    *error = baselineErrorText(*kind, version, *kind == InstallError::Cancelled ? QString() : installer.errorString());
    return std::nullopt;
}
} // namespace ui

// ------------------------------------------------------------------ state and browser index
Index Window::browserIndex() {
    auto view = workspaceIndex();
    if (!baseline.index)
        return view;
    // Merging ~11k records costs a few ms: once per change of the project view, not per call.
    if (browserSerial == collectionSerial && browserFiles == view.files)
        return browserCache;
    QElapsedTimer timer;
    timer.start();
    browserCache = vanilla::mergeBaseline(view, *baseline.index);
    browserSerial = collectionSerial;
    browserFiles = view.files;
    metrics.record("baseline_browser_merge", timer);
    return browserCache;
}
void Window::resetBaselineState(bool sameProject) {
    ++baseline.generation;
    baseline.ref.reset();
    baseline.entry.reset();
    baseline.index.reset();
    baseline.unavailable.clear();
    baseline.diagnostic.clear();
    baseline.loading = false;
    if (!sameProject) {
        resetHistory.clear();
        resetCursor = 0;
        baselineDocs.clear();
    }
    baselineOpening.clear();
    baselineRestore.clear();
    browserCache = {};
    browserSerial = ~0ull;
    browserFiles.clear();
    model->setBaselineVersion({});
    updateBaselineStatus();
}
void Window::loadBaseline(bool sameProject) {
    resetBaselineState(sameProject);
    if (project.isEmpty())
        return;
    const auto detected = Project::inspect(project);
    baseline.ref = detected.baseline;
    QStringList notes;
    if (!detected.configDiagnostic.isEmpty())
        notes << detected.configDiagnostic;
    for (const auto &d : detected.diagnostics)
        if (d.startsWith("pack.mcmeta does not target the vanilla baseline"))
            notes << d;
    baseline.diagnostic = notes.join("\n");
    if (!baseline.diagnostic.isEmpty())
        recordActivity(baseline.diagnostic);
    if (!baseline.ref) {
        updateBaselineStatus();
        return;
    }
    baseline.loading = true;
    updateBaselineStatus();
    struct Loaded {
        std::shared_ptr<const BaselineEntry> entry;
        std::shared_ptr<const Index> index;
        QString error;
        QStringList interrupted;
    };
    const auto generation = baseline.generation;
    const auto root = project;
    const auto ref = *baseline.ref;
    auto watcher = new QFutureWatcher<Loaded>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, root, ref] {
        const auto loaded = watcher->result();
        watcher->deleteLater();
        if (generation != baseline.generation || root != project)
            return;
        baseline.loading = false;
        baseline.entry = loaded.entry;
        baseline.index = loaded.index;
        baseline.unavailable = loaded.error;
        browserSerial = ~0ull;
        model->setBaselineVersion(loaded.entry ? ref.version : QString());
        // The initial scan installs the merged index itself when it finishes.
        if (!(indexBusy && collection.files.isEmpty() && collection.directories.isEmpty())) {
            QElapsedTimer install;
            install.start();
            model->setIndex(browserIndex(), project);
            metrics.record("baseline_model_install", install);
            setDirectory(nav.directory);
        }
        const auto restore = std::exchange(baselineRestore, {});
        if (loaded.entry) {
            for (const auto &relative : restore)
                openBaselineTexture(relative, project + '/' + relative == preferences.activeDocument);
        } else
            statusBar()->showMessage(QString("Vanilla Java %1 baseline not available · Resources › Vanilla Baseline › "
                                             "Download / Repair Baseline…").arg(ref.version), 10000);
        if (!baseline.diagnostic.isEmpty())
            statusBar()->showMessage(baseline.diagnostic.section('\n', 0, 0), 10000);
        updateBaselineStatus();
        updateBaselineActions();
        updateInspector();
        if (!loaded.interrupted.isEmpty())
            QTimer::singleShot(0, this, [this, ids = loaded.interrupted, generation] {
                if (generation == baseline.generation)
                    offerResetRecovery(ids);
            });
    });
    watcher->setFuture(QtConcurrent::run(&ioPool, [root, ref] {
        Loaded loaded;
        QString error;
        // Offline: the recorded identity selects the entry; nothing is downloaded here.
        if (auto entry = BaselineCache().load(ref.version, ref.identity, BaselineCache::Check::Sizes, &error)) {
            loaded.index = std::make_shared<const Index>(vanilla::baselineIndex(*entry));
            loaded.entry = std::make_shared<const BaselineEntry>(std::move(*entry));
        } else
            loaded.error = error.isEmpty() ? "It is not in the local cache." : error;
        pruneResets(root);
        for (const auto &journal : resetJournals(root))
            if (journal.interrupted())
                loaded.interrupted << journal.id;
        return loaded;
    }));
}
void Window::updateBaselineStatus() {
    if (!baselineLabel) {
        baselineLabel = new QLabel;
        baselineLabel->setObjectName("baselineStatus");
        baselineLabel->setAccessibleName("Vanilla baseline");
        statusBar()->addPermanentWidget(baselineLabel);
    }
    QString text, role = "hint", tip;
    if (!project.isEmpty() && baseline.ref) {
        const auto &v = baseline.ref->version;
        tip = QString("Vanilla Java %1 · identity %2").arg(v, baseline.ref->identity.left(12));
        if (baseline.loading)
            text = QString("Baseline: Vanilla Java %1 · loading…").arg(v);
        else if (baseline.entry)
            text = QString("Baseline: Vanilla Java %1").arg(v);
        else {
            text = QString("Vanilla Java %1 baseline not available · Resources › Vanilla Baseline › Download…").arg(v);
            role = "warning";
            tip += "\n" + baseline.unavailable;
        }
    } else if (!project.isEmpty() && !baseline.diagnostic.isEmpty()) {
        text = "Project settings ignored";
        role = "warning";
    }
    if (!baseline.diagnostic.isEmpty()) {
        tip += (tip.isEmpty() ? "" : "\n") + baseline.diagnostic;
        if (baseline.ref && role != "warning") {
            text += " · check pack.mcmeta";
            role = "warning";
        }
    }
    baselineLabel->setText(text);
    baselineLabel->setToolTip(tip);
    if (baselineLabel->property("role").toString() != role) {
        baselineLabel->setProperty("role", role);
        baselineLabel->style()->unpolish(baselineLabel);
        baselineLabel->style()->polish(baselineLabel);
    }
    baselineLabel->setVisible(!text.isEmpty());
}
bool Window::baselineBacked(const Document &doc) const {
    return !project.isEmpty() && baselineDocs.contains(doc.id()) && !doc.revision().exists;
}
bool Window::baselineCandidate(const QString &path) const {
    if (project.isEmpty() || !baseline.ref)
        return false;
    const auto relative = QDir(project).relativeFilePath(path);
    return !relative.startsWith("..") && !QDir::isAbsolutePath(relative) && texture(relative) &&
           vanilla::validBaselinePath(relative);
}
QString Window::baselineOrigin(const Document &doc) const {
    if (project.isEmpty() || !baseline.ref)
        return {};
    const auto v = baseline.ref->version;
    if (baselineBacked(doc))
        return QString("Vanilla Java %1 (not in project)").arg(v);
    const auto relative = QDir(project).relativeFilePath(doc.path);
    if (relative.startsWith("..") || QDir::isAbsolutePath(relative))
        return {};
    if (!baseline.entry)
        return "Project asset (baseline not available)";
    return baseline.entry->contains(relative) ? QString("Override of vanilla Java %1").arg(v) : QString("Project asset");
}

// ------------------------------------------------------------------ commands
QString Window::baselineSelection() const {
    if (!browserHasFocus() && current())
        return QDir(project).relativeFilePath(current()->document->path);
    return nav.selected;
}
bool Window::browserHasFocus() const {
    const auto focus = QApplication::focusWidget();
    return focus && (focus == tree || focus == grid || tree->isAncestorOf(focus) || grid->isAncestorOf(focus));
}
bool Window::canResetToVanilla(const QString &relative) const {
    return baseline.entry && texture(relative) && collection.records.contains(relative) &&
           baseline.entry->contains(relative);
}
bool Window::canRemoveFromProject(const QString &relative) const {
    return baseline.ref && baseline.entry && texture(relative) && collection.records.contains(relative) &&
           !baseline.entry->contains(relative);
}
QStringList Window::baselineOverrides() const {
    QStringList overrides;
    if (!baseline.entry)
        return overrides;
    for (const auto &path : collection.files)
        if (texture(path) && baseline.entry->contains(path))
            overrides << path;
    return overrides;
}
void Window::updateBaselineActions() {
    const auto selected = baselineSelection();
    auto enable = [this](const char *name, bool on) {
        if (auto a = findChild<QAction *>(name))
            a->setEnabled(on);
    };
    enable("resetToVanillaCommand", canResetToVanilla(selected));
    enable("removeFromProjectCommand", canRemoveFromProject(selected));
    enable("resetAllToVanillaCommand", !baselineOverrides().isEmpty());
    enable("undoBaselineResetCommand", baseline.ref && resetCursor > 0);
    enable("redoBaselineResetCommand", baseline.ref && resetCursor < resetHistory.size());
    enable("useVanillaBaselineCommand", !project.isEmpty() && !baseline.ref);
    enable("repairBaselineCommand", bool(baseline.ref));
}
void Window::buildBaselineMenu(QMenu *resources) {
    resources->addSeparator();
    baselineMenu = resources->addMenu("Vanilla Baseline");
    baselineMenu->setObjectName("menu-VanillaBaseline");
    auto add = [this](const QString &text, const QString &name, const QString &tip, auto callback) {
        auto a = baselineMenu->addAction(text);
        a->setObjectName(name);
        a->setStatusTip(tip);
        connect(a, &QAction::triggered, this, callback);
        return a;
    };
    add("Reset to Vanilla", "resetToVanillaCommand",
        "Move the selected override out of the project so the vanilla texture shows again (undoable)", [this] {
            const auto selected = baselineSelection();
            if (canResetToVanilla(selected))
                baselineReset({selected}, ResetKind::ResetToVanilla);
            else
                statusBar()->showMessage("Select a project texture that overrides a vanilla texture", 5000);
        });
    add("Remove from Project…", "removeFromProjectCommand",
        "Move a project texture without a vanilla counterpart out of the pack (undoable)", [this] {
            const auto selected = baselineSelection();
            if (canRemoveFromProject(selected))
                baselineReset({selected}, ResetKind::RemoveFromProject);
            else
                statusBar()->showMessage("Select a project texture that has no vanilla counterpart", 5000);
        });
    add("Reset All Overrides to Vanilla…", "resetAllToVanillaCommand",
        "Move every vanilla override out of the project in one undoable step", [this] {
            const auto overrides = baselineOverrides();
            if (!overrides.isEmpty())
                baselineReset(overrides, ResetKind::ResetToVanilla);
            else
                statusBar()->showMessage("This project has no vanilla overrides", 5000);
        });
    baselineMenu->addSeparator();
    add("Undo Reset", "undoBaselineResetCommand", "Restore the files of the last Reset to Vanilla or Remove from Project",
        [this] { baselineHistory(false); });
    add("Redo Reset", "redoBaselineResetCommand", "Apply the last undone Reset to Vanilla or Remove from Project again",
        [this] { baselineHistory(true); });
    baselineMenu->addSeparator();
    add("Use Vanilla Baseline for This Project…", "useVanillaBaselineCommand",
        "Show a Minecraft version's vanilla textures under this pack's own files", [this] { useVanillaBaseline(); });
    add("Download / Repair Baseline…", "repairBaselineCommand",
        "Download or re-verify this project's vanilla textures in the local cache", [this] { repairBaseline(); });
    add("Manage Cached Baselines…", "manageBaselinesCommand",
        "Review, verify or remove vanilla textures kept in the local cache", [this] { manageBaselines(); });
    connect(baselineMenu, &QMenu::aboutToShow, this, &Window::updateBaselineActions);
    updateBaselineActions();
}
QAction *Window::addBaselineContextAction(QMenu &menu, const QString &relative) {
    // Legacy projects (and unavailable baselines) keep the context menu exactly as it was.
    if (!baseline.entry || !texture(relative) || !collection.records.contains(relative))
        return nullptr;
    menu.addSeparator();
    const bool shadows = baseline.entry->contains(relative);
    auto a = menu.addAction(shadows ? "Reset to Vanilla" : "Remove from Project…");
    a->setObjectName(shadows ? "assetResetToVanilla" : "assetRemoveFromProject");
    a->setData(relative);
    return a;
}
void Window::triggerBaselineContextAction(QAction *action) {
    const auto relative = action->data().toString();
    if (action->objectName() == "assetResetToVanilla" && canResetToVanilla(relative))
        baselineReset({relative}, ResetKind::ResetToVanilla);
    else if (action->objectName() == "assetRemoveFromProject" && canRemoveFromProject(relative))
        baselineReset({relative}, ResetKind::RemoveFromProject);
}
bool Window::closeBaselineAssets(const QStringList &assets, QStringList *reopen) {
    QList<Canvas *> pages;
    for (int i = 0; i < tabs->count(); ++i)
        if (auto canvas = dynamic_cast<Canvas *>(tabs->widget(i)))
            if (assets.contains(QDir(project).relativeFilePath(canvas->document->path), Qt::CaseInsensitive))
                pages << canvas;
    for (auto canvas : pages)
        if (canvas->document->modified() || canvas->hasFloating()) {
            tabs->setCurrentWidget(canvas);
            if (QMessageBox::question(this, "Unsaved texture",
                                      "Discard unsaved changes to " + QFileInfo(canvas->document->path).fileName() +
                                          "? The texture is replaced with the vanilla version.",
                                      QMessageBox::Discard | QMessageBox::Cancel,
                                      QMessageBox::Cancel) != QMessageBox::Discard)
                return false;
        }
    for (auto canvas : pages) {
        const auto doc = canvas->document;
        const auto relative = QDir(project).relativeFilePath(doc->path);
        recovery->discard(doc->id());
        externalRevisions.remove(doc->id());
        recoveredKeys.remove(doc->id());
        baselineDocs.remove(doc->id());
        clearComparisons(relative);
        session.close(doc);
        tabs->removeTab(tabs->indexOf(canvas));
        delete canvas;
        *reopen << relative;
    }
    if (!pages.isEmpty())
        refresh();
    return true;
}
void Window::baselineReset(const QStringList &assets, ResetKind kind) {
    if (project.isEmpty() || !baseline.ref || assets.isEmpty())
        return;
    const bool reset = kind == ResetKind::ResetToVanilla;
    const auto version = baseline.ref->version;
    const auto name = QFileInfo(assets.first()).fileName();
    if (reset && !baseline.entry) {
        QMessageBox::warning(this, "Reset to Vanilla unavailable",
                             QString("The vanilla Java %1 baseline is not available, so nothing was reset and the "
                                     "override was preserved. Use Resources › Vanilla Baseline › Download / Repair "
                                     "Baseline… first.").arg(version));
        return;
    }
    if (assets.size() > 1 &&
        QMessageBox::question(this, "Reset all overrides to vanilla?",
                              QString("Reset %1 overrides to vanilla Java %2? Their project files are moved into "
                                      ".nova/baseline-resets; Undo Reset restores all of them in one step.")
                                  .arg(assets.size()).arg(version),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    if (!reset &&
        QMessageBox::question(this, "Remove from project?",
                              QString("Remove %1 from the project? It has no vanilla counterpart, so the exported "
                                      "pack will no longer contain it. Its files are kept in .nova/baseline-resets "
                                      "and Undo in the asset browser restores them.").arg(name),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    QStringList reopen;
    if (!closeBaselineAssets(assets, &reopen))
        return;
    const auto root = project;
    const auto check = verifiedBaseline(baseline.entry);
    const auto result = blocking<StepResult>(this, &ioPool, reset ? "Resetting to vanilla…" : "Removing from project…",
                                             [root, assets, kind, check] {
                                                 StepResult r;
                                                 r.ok = applyReset(root, assets, kind, check, &r.id, &r.error);
                                                 return r;
                                             });
    if (root != project)
        return;
    if (!result.ok) {
        QMessageBox::warning(this, reset ? "Reset to Vanilla failed" : "Remove from Project failed",
                             result.error + "\n\nNothing was changed; the project files were preserved.");
        for (const auto &relative : reopen)
            openTexture(project + '/' + relative);
        return;
    }
    resetHistory.erase(resetHistory.begin() + resetCursor, resetHistory.end());
    resetHistory.append({result.id, assets, kind});
    resetCursor = int(resetHistory.size());
    for (const auto &asset : assets)
        pendingChanges << asset << asset + ".mcmeta";
    indexDebounce->start(25);
    if (reset)
        for (const auto &relative : reopen)
            openBaselineTexture(relative);
    statusBar()->showMessage(
        (assets.size() > 1 ? QString("Reset %1 overrides to vanilla").arg(assets.size())
                           : (reset ? "Reset " + name + " to vanilla" : "Removed " + name + " from the project")) +
            " · Undo with Ctrl+Z in the asset browser",
        8000);
    updateBaselineActions();
}
bool Window::baselineHistory(bool redo) {
    if (project.isEmpty() || (redo ? resetCursor >= resetHistory.size() : resetCursor <= 0)) {
        statusBar()->showMessage(redo ? "Nothing to redo in the asset browser" : "Nothing to undo in the asset browser",
                                 4000);
        return false;
    }
    const auto step = resetHistory[redo ? resetCursor : resetCursor - 1];
    const bool reset = step.kind == ResetKind::ResetToVanilla;
    if (redo && reset && !baseline.entry) {
        QMessageBox::warning(this, "Redo Reset unavailable",
                             "The vanilla baseline is not available, so the override was preserved.");
        return false;
    }
    QStringList reopen;
    if (!closeBaselineAssets(step.assets, &reopen))
        return false;
    const auto root = project;
    const auto check = redo && reset ? verifiedBaseline(baseline.entry) : BaselineCheck{};
    const auto result = blocking<StepResult>(this, &ioPool, redo ? "Redoing reset…" : "Undoing reset…",
                                             [root, step, redo, check] {
                                                 StepResult r;
                                                 r.ok = redo ? redoReset(root, step.id, &r.error, check)
                                                             : undoReset(root, step.id, &r.error);
                                                 return r;
                                             });
    if (root != project)
        return false;
    if (!result.ok)
        QMessageBox::warning(this, redo ? "Redo Reset failed" : "Undo Reset failed",
                             result.error + "\n\nNothing was overwritten.");
    else {
        resetCursor += redo ? 1 : -1;
        for (const auto &asset : step.assets)
            pendingChanges << asset << asset + ".mcmeta";
        indexDebounce->start(25);
        const auto what = step.assets.size() > 1 ? QString("%1 textures").arg(step.assets.size())
                                                 : QFileInfo(step.assets.first()).fileName();
        statusBar()->showMessage((redo ? "Redid " : "Undid ") +
                                     QString(reset ? "reset of %1 to vanilla" : "removal of %1").arg(what),
                                 6000);
    }
    // Reopen what was open: the project file if it is back, else the vanilla texture.
    for (const auto &relative : reopen)
        openTexture(project + '/' + relative);
    updateBaselineActions();
    return true;
}
void Window::offerResetRecovery(const QStringList &interrupted) {
    for (const auto &id : interrupted) {
        if (QMessageBox::question(this, "Recover interrupted reset?",
                                  "A Reset to Vanilla or Remove from Project step in this project was interrupted. "
                                  "Recover it now? Nova completes or rolls back the step; no file is deleted.",
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            continue;
        const auto root = project;
        const auto error = blocking<QString>(this, &ioPool, "Recovering reset…", [root, id] {
            QString e;
            recoverReset(root, id, &e);
            return e;
        });
        if (!error.isEmpty())
            QMessageBox::warning(this, "Recovery stopped", error);
        else {
            fullRescan = true;
            indexDebounce->start(50);
        }
    }
}

// ------------------------------------------------------------------ documents
bool Window::openBaselineTexture(const QString &relative, bool activate) {
    const auto path = project + '/' + relative;
    for (int i = 0; i < tabs->count(); ++i)
        if (tabPath(tabs->widget(i)) == path) {
            tabs->setCurrentIndex(i);
            return true;
        }
    if (!baseline.ref)
        return false;
    if (!baseline.entry) {
        if (baseline.loading) {
            if (!baselineRestore.contains(relative))
                baselineRestore << relative;
            return true;
        }
        statusBar()->showMessage(QString("Vanilla Java %1 baseline not available · Resources › Vanilla Baseline › "
                                         "Download / Repair Baseline…").arg(baseline.ref->version), 8000);
        return false;
    }
    if (!baseline.entry->contains(relative)) {
        statusBar()->showMessage("This texture is neither in the project nor in the vanilla baseline", 6000);
        return false;
    }
    if (baselineOpening.contains(relative))
        return true;
    baselineOpening.insert(relative);
    struct Bytes {
        QByteArray png, meta;
        QString error;
    };
    const auto entry = baseline.entry;
    const auto generation = baseline.generation;
    const auto version = baseline.ref->version;
    QApplication::setOverrideCursor(Qt::BusyCursor);
    auto watcher = new QFutureWatcher<Bytes>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, relative, generation, version, activate] {
        QApplication::restoreOverrideCursor();
        const auto bytes = watcher->result();
        watcher->deleteLater();
        if (generation != baseline.generation)
            return;
        baselineOpening.remove(relative);
        if (!bytes.error.isEmpty()) {
            QMessageBox::warning(this, "Vanilla texture unavailable",
                                 bytes.error + "\n\nNothing was opened. Resources › Vanilla Baseline › Download / "
                                               "Repair Baseline… restores the cache.");
            return;
        }
        QString error;
        auto doc = Document::fromBaseline(project + '/' + relative, bytes.png, bytes.meta, version, &error);
        if (!doc) {
            QMessageBox::warning(this, "Vanilla texture not opened", error);
            return;
        }
        if (!session.adopt(doc)) {
            QMessageBox::warning(this, "Vanilla texture not opened",
                                 "Close another texture first (open document limit reached).");
            return;
        }
        baselineDocs.insert(doc->id());
        auto previous = tabs->currentWidget();
        showTextureDocument(doc);
        if (!activate && previous)
            tabs->setCurrentWidget(previous);
        metrics.count("baseline_documents_opened", 1);
    });
    // One small verified read (PNG and, for animations, .mcmeta); never on the GUI thread.
    watcher->setFuture(QtConcurrent::run(&ioPool, [entry, relative] {
        Bytes bytes;
        const BaselineCache cache(entry->root);
        if (!cache.readVerified(*entry, relative, &bytes.png, &bytes.error))
            return bytes;
        if (entry->contains(relative + ".mcmeta"))
            cache.readVerified(*entry, relative + ".mcmeta", &bytes.meta, &bytes.error);
        return bytes;
    }));
    return true;
}
bool Window::prepareAssetFolder(const std::shared_ptr<Document> &doc) {
    if (!doc || !baselineBacked(*doc) || QFileInfo(QFileInfo(doc->path).absolutePath()).isDir())
        return true;
    QString error;
    if (ensureAssetDirectory(project, QDir(project).relativeFilePath(doc->path), &error))
        return true;
    QMessageBox::warning(this, "Save failed",
                         "Nova could not create the texture's folder inside the project. Nothing was saved.\n\n" + error);
    return false;
}
void Window::baselineSaved(const std::shared_ptr<Document> &doc) {
    if (!doc || !baselineDocs.contains(doc->id()) || !doc->revision().exists)
        return;
    // The first save created the override: the row turns from vanilla into a project override.
    baselineDocs.remove(doc->id());
    const auto relative = QDir(project).relativeFilePath(doc->path);
    pendingChanges << relative << relative + ".mcmeta";
    indexDebounce->start(25);
}

// ------------------------------------------------------------------ dialogs
void Window::useVanillaBaseline() {
    if (project.isEmpty())
        return;
    if (baseline.ref) {
        statusBar()->showMessage("This project already uses a vanilla baseline", 5000);
        return;
    }
    auto config = ProjectConfig::load(project);
    if (!config.diagnostic.isEmpty()) {
        QMessageBox::warning(this, "Project settings not changed",
                             config.diagnostic + "\n\n.nova/project.json is left exactly as it is.");
        return;
    }
    const auto detected = Project::inspect(project);
    const auto pack = detected.json.value("pack").toObject();
    const auto format = PackFormat::parse(pack.contains("min_format") ? pack.value("min_format") : pack.value("pack_format"));
    QDialog dialog(this);
    dialog.setObjectName("useVanillaBaselineDialog");
    dialog.setWindowTitle("Use Vanilla Baseline");
    auto v = new QVBoxLayout(&dialog);
    auto intro = new QLabel("Show one Minecraft Java version's vanilla textures under this pack's own files. "
                            "Vanilla textures appear dimmed in Assets; editing one and saving adds it to the pack as "
                            "an override. Nothing is added to the pack otherwise, and Export is unchanged.");
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto form = new QFormLayout;
    auto versions = new QComboBox;
    versions->setObjectName("baselineVersion");
    versions->addItem("Choose a version…", QString());
    for (const auto &g : gameVersions()) {
        versions->addItem("Java " + g.id + " · format " + g.format.text(), g.id);
        if (format && g.format == *format && versions->currentIndex() == 0)
            versions->setCurrentIndex(versions->count() - 1);
    }
    form->addRow("Minecraft version", versions);
    v->addLayout(form);
    auto status = ui::hint({});
    status->setObjectName("baselineVersionStatus");
    status->setWordWrap(true);
    v->addWidget(status);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Use Baseline");
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto ticket = std::make_shared<int>(0);
    auto update = [versions, status, buttons, ticket, &dialog] {
        const auto id = versions->currentData().toString();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!id.isEmpty());
        status->setText(id.isEmpty() ? "pack.mcmeta does not match a supported version; choose one explicitly." : "Checking the local cache…");
        if (id.isEmpty())
            return;
        const int mine = ++*ticket;
        auto watcher = new QFutureWatcher<QString>(&dialog);
        QObject::connect(watcher, &QFutureWatcherBase::finished, &dialog, [watcher, status, mine, ticket, id] {
            watcher->deleteLater();
            if (mine == *ticket)
                status->setText(watcher->result().isEmpty() ? ui::baselineConsentText(id) : watcher->result());
        });
        watcher->setFuture(QtConcurrent::run([id] { return ui::cachedBaselineSummary(id); }));
    };
    connect(versions, &QComboBox::currentIndexChanged, &dialog, update);
    update();
    dialog.resize(520, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted || versions->currentData().toString().isEmpty())
        return;
    const auto version = versions->currentData().toString();
    InstallError kind;
    QString error;
    const auto entry = ui::obtainBaseline(this, version, {}, false, &kind, &error);
    if (!entry) {
        if (kind != InstallError::Cancelled)
            QMessageBox::warning(this, "Vanilla textures not available", error + "\n\nThe project was not changed.");
        return;
    }
    config.baseline = ui::baselineRef(*entry);
    if (!config.save(project, &error)) {
        QMessageBox::warning(this, "Project settings not saved", error);
        return;
    }
    loadBaseline(true);
    statusBar()->showMessage(QString("This project now uses vanilla Java %1 as its baseline").arg(version), 8000);
}
void Window::repairBaseline() {
    if (project.isEmpty() || !baseline.ref) {
        statusBar()->showMessage("This project has no vanilla baseline · Resources › Vanilla Baseline › Use Vanilla "
                                 "Baseline for This Project…", 6000);
        return;
    }
    const auto ref = *baseline.ref;
    if (QMessageBox::question(this, "Download / repair baseline?",
                              QString("Check the vanilla Java %1 textures (identity %2) in the local cache and download "
                                      "any that are missing or damaged from Mojang? Every file is verified; project "
                                      "files are not changed.").arg(ref.version, ref.identity.left(12)),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    InstallError kind;
    QString error;
    auto entry = ui::obtainBaseline(this, ref.version, ref.identity, true, &kind, &error, false);
    if (!entry && kind == InstallError::MetadataChanged) {
        if (QMessageBox::question(this, "Official baseline changed",
                                  error + QString("\n\nUse the current official baseline for Java %1 instead? Nova "
                                                  "downloads it and then updates .nova/project.json; project files "
                                                  "are not changed.").arg(ref.version),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        entry = ui::obtainBaseline(this, ref.version, {}, true, &kind, &error, false);
        if (entry) {
            auto config = ProjectConfig::load(project);
            config.baseline = ui::baselineRef(*entry);
            if (!config.diagnostic.isEmpty() || !config.save(project, &error)) {
                QMessageBox::warning(this, "Project settings not saved",
                                     config.diagnostic.isEmpty() ? error : config.diagnostic);
                return;
            }
        }
    }
    if (!entry) {
        if (kind != InstallError::Cancelled)
            QMessageBox::warning(this, "Vanilla textures not available", error);
        return;
    }
    loadBaseline(true);
    statusBar()->showMessage(QString("Vanilla Java %1 baseline verified").arg(ref.version), 6000);
}
void Window::manageBaselines() {
    QDialog dialog(this);
    dialog.setObjectName("manageBaselinesDialog");
    dialog.setWindowTitle("Manage Cached Baselines");
    auto v = new QVBoxLayout(&dialog);
    v->setSpacing(ui::SpaceL);
    v->addWidget(ui::hint("Vanilla textures downloaded for baseline projects. Projects never contain these files; "
                          "removing a baseline only changes what the asset browser shows until it is downloaded again."));
    auto table = new QTableWidget(0, 8);
    table->setObjectName("baselineCacheTable");
    table->setHorizontalHeaderLabels({"Version", "Identity", "Downloaded", "Files", "Size", "Unique", "Shared", "Status"});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    v->addWidget(table, 1);
    auto total = new QLabel("Reading the cache…");
    total->setObjectName("baselineCacheTotal");
    v->addWidget(total);
    const auto root = BaselineCache::defaultRoot();
    auto location = new QLabel("Location: " + QDir::toNativeSeparators(root));
    location->setObjectName("baselineCacheLocation");
    location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    location->setWordWrap(true);
    v->addWidget(location);
    auto attribution = ui::hint(ui::baselineAttribution());
    attribution->setObjectName("baselineAttribution");
    v->addWidget(attribution);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto verify = buttons->addButton("Verify", QDialogButtonBox::ActionRole);
    verify->setObjectName("baselineVerify");
    auto remove = buttons->addButton("Remove…", QDialogButtonBox::DestructiveRole);
    remove->setObjectName("baselineRemove");
    auto folder = buttons->addButton("Open Folder", QDialogButtonBox::ActionRole);
    folder->setObjectName("baselineOpenFolder");
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    using Summaries = QList<vanilla::BaselineSummary>;
    auto rows = std::make_shared<Summaries>();
    auto selectedRow = [table, rows]() -> std::optional<vanilla::BaselineSummary> {
        const int row = table->currentRow();
        return row >= 0 && row < rows->size() ? std::optional(rows->at(row)) : std::nullopt;
    };
    auto enableButtons = [=] {
        verify->setEnabled(bool(selectedRow()));
        remove->setEnabled(bool(selectedRow()));
        folder->setEnabled(QFileInfo(root).isDir());
    };
    std::function<void()> reload = [=, &dialog] {
        auto watcher = new QFutureWatcher<std::pair<Summaries, qint64>>(&dialog);
        connect(watcher, &QFutureWatcherBase::finished, &dialog, [=] {
            watcher->deleteLater();
            const auto [list, bytes] = watcher->result();
            *rows = list;
            table->setRowCount(0);
            for (const auto &s : list) {
                const int row = table->rowCount();
                table->insertRow(row);
                const bool complete = s.status == vanilla::BaselineSummary::Status::Complete;
                const QStringList cells{"Java " + s.version, s.identity.left(12),
                                        s.created.isValid() ? s.created.toLocalTime().toString("yyyy-MM-dd HH:mm") : "—",
                                        QString::number(s.files), megabytes(s.bytes), megabytes(s.uniqueBytes),
                                        megabytes(s.sharedBytes), complete ? "Complete" : "Damaged: " + s.error};
                for (int c = 0; c < cells.size(); ++c)
                    table->setItem(row, c, new QTableWidgetItem(cells[c]));
            }
            if (!list.isEmpty())
                table->selectRow(0);
            total->setText(list.isEmpty() ? "No vanilla textures are cached."
                                          : QString("%1 cached baselines · %2 on disk").arg(list.size()).arg(megabytes(bytes)));
            enableButtons();
        });
        watcher->setFuture(QtConcurrent::run([] {
            const BaselineCache cache;
            return std::make_pair(cache.list(), cache.totalBytes());
        }));
    };
    connect(table, &QTableWidget::itemSelectionChanged, &dialog, enableButtons);
    connect(folder, &QPushButton::clicked, &dialog, [root] { QDesktopServices::openUrl(QUrl::fromLocalFile(root)); });
    connect(verify, &QPushButton::clicked, &dialog, [=, &dialog, &reload] {
        const auto s = selectedRow();
        if (!s)
            return;
        if (s->status != vanilla::BaselineSummary::Status::Complete) {
            QMessageBox::warning(&dialog, "Baseline damaged",
                                 QString("Vanilla Java %1 is damaged in the cache: %2\n\nRemove it, or open a project "
                                         "that uses it and choose Download / Repair Baseline….").arg(s->version, s->error));
            return;
        }
        auto cancel = std::make_shared<std::atomic_bool>(false);
        QProgressDialog progress(QString("Verifying vanilla Java %1 textures (SHA-256)…").arg(s->version), "Cancel", 0, 0, &dialog);
        progress.setObjectName("baselineVerifyProgress");
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(200);
        connect(&progress, &QProgressDialog::canceled, &dialog, [cancel] { cancel->store(true); });
        QFutureWatcher<std::pair<QStringList, QString>> watcher;
        QEventLoop loop;
        connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
        const auto version = s->version, identity = s->identity;
        watcher.setFuture(QtConcurrent::run([version, identity, cancel] {
            const BaselineCache cache;
            QString error;
            const auto entry = cache.load(version, identity, BaselineCache::Check::Manifest, &error);
            if (!entry)
                return std::make_pair(QStringList{}, error.isEmpty() ? QString("The entry could not be read") : error);
            return std::make_pair(cache.verify(*entry, cancel), QString());
        }));
        if (!watcher.isFinished())
            loop.exec();
        progress.close();
        const auto [bad, error] = watcher.result();
        if (cancel->load())
            return;
        if (!error.isEmpty())
            QMessageBox::warning(&dialog, "Verification failed", error);
        else if (!bad.isEmpty())
            QMessageBox::warning(&dialog, "Baseline damaged",
                                 QString("%1 of %2 files are missing or damaged, for example:\n%3\n\nUse Resources › "
                                         "Vanilla Baseline › Download / Repair Baseline… in a project that uses Java %4.")
                                     .arg(bad.size()).arg(s->files).arg(bad.mid(0, 5).join("\n"), s->version));
        else
            QMessageBox::information(&dialog, "Baseline verified",
                                     QString("All %1 files of vanilla Java %2 match their SHA-256 checksums.")
                                         .arg(s->files).arg(s->version));
        reload();
    });
    connect(remove, &QPushButton::clicked, &dialog, [=, this, &dialog, &reload] {
        const auto s = selectedRow();
        if (!s)
            return;
        const bool used = baseline.ref && baseline.ref->version == s->version && baseline.ref->identity == s->identity;
        auto text = QString("Remove cached Vanilla Java %1 textures (%2)? Projects keep their files; projects using this "
                            "version show only their own files until it is downloaded again.")
                        .arg(s->version, megabytes(s->bytes));
        if (s->sharedBytes > 0)
            text += QString("\n\n%1 shared with other cached versions stays in the cache.").arg(megabytes(s->sharedBytes));
        if (used)
            text += "\n\nThe open project uses this baseline.";
        if (QMessageBox::question(&dialog, "Remove cached baseline?", text, QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        const auto version = s->version, identity = s->identity;
        const auto error = blocking<QString>(&dialog, nullptr, "Removing cached textures…", [version, identity] {
            QString e;
            BaselineCache().remove(version, identity, &e, nullptr, 10000);
            return e;
        });
        if (!error.isEmpty())
            QMessageBox::warning(&dialog, "Baseline not removed", error);
        if (used)
            loadBaseline(true);
        reload();
    });
    enableButtons();
    reload();
    dialog.resize(820, 440);
    dialog.exec();
}
} // namespace nova
