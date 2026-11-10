// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "modelpanel.h"
#include <QStatusBar>
#include <QTabWidget>
#include <QtConcurrent>
namespace nova {
void Window::updateIndex() {
    if (project.isEmpty())
        return;
    if (indexBusy) {
        indexDebounce->start(250);
        return;
    }
    auto paths = pendingChanges;
    pendingChanges.clear();
    paths.removeDuplicates();
    auto full = fullRescan;
    fullRescan = false;
    auto invalidate = invalidateFallback;
    invalidateFallback = false;
    if (paths.isEmpty() && !full)
        return;
    if (indexCancel)
        indexCancel->store(true);
    auto cancel = std::make_shared<std::atomic_bool>(false);
    indexCancel = cancel;
    auto generation = ++indexGeneration;
    auto root = project;
    auto previous = collection;
    indexBusy = true;
    auto watcher = new QFutureWatcher<Index>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, root, generation, paths, full, invalidate] {
                auto result = watcher->result();
                watcher->deleteLater();
                if (generation != indexGeneration || root != project)
                    return;
                indexBusy = false;
                if (!result.error.isEmpty()) {
                    statusBar()->showMessage(result.error + " · waiting for project to reconnect");
                    monitor.watch(project);
                    return;
                }
                bool identical = full && !invalidate &&
                                 result.directories == collection.directories &&
                                 result.records.size() == collection.records.size();
                if (identical)
                    for (auto it = result.records.cbegin(); it != result.records.cend(); ++it) {
                        auto old = collection.records.constFind(it.key());
                        if (old == collection.records.cend() || old->size != it->size ||
                            old->modified != it->modified) {
                            identical = false;
                            break;
                        }
                    }
                if (identical) {
                    monitor.watch(project);
                    return;
                }
                QElapsedTimer timer;
                timer.start();
                collection = std::move(result);
                ++collectionSerial;
                model->applyIndex(browserIndex(), full ? QStringList{""} : paths);
                updateModelIndex(full ? QStringList{""} : paths);
                metrics.record("incremental_model_install", timer);
                metrics.count("index_updates", 1);
                setDirectory(nav.directory);
                statusBar()->showMessage(QString("%1 assets · %2 update")
                                             .arg(collection.files.size())
                                             .arg(full ? "reconciled" : "incremental"),
                                         4000);
                if (full)
                    monitor.watch(project);
            });
    watcher->setFuture(QtConcurrent::run([root, previous, paths, full, cancel] {
        return full ? Index::scan(root, cancel) : Index::refresh(root, previous, paths, cancel);
    }));
}
void Window::updateModelIndex(const QStringList &paths) {
    if (modelCancel) modelCancel->store(true);
    auto cancel = std::make_shared<std::atomic_bool>(false); modelCancel = cancel;
    const auto generation = ++modelGeneration;
    const auto root = project;
    auto previous = *modelLibrary;
    const auto index = collection;
    auto watcher = new QFutureWatcher<std::shared_ptr<mc::Library>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, root, generation] {
        const auto result = watcher->result(); watcher->deleteLater();
        if (root != project || generation != modelGeneration) return;
        modelLibrary = result; modelPanel->setLibrary(result, root);
        if (current()) modelPanel->setDocument(current()->document, current());
        if (std::exchange(restoreModelWorkspacePending, false)) {
            auto active = tabs->currentWidget();
            if (!fresh && preferences.projectPath == project) {
                for (const auto &path : preferences.openDocuments) {
                    const auto asset = QDir(project).relativeFilePath(path);
                    if (!mc::Location::fromAsset(asset, "models").isEmpty() &&
                        collection.records.contains(asset))
                        openModelAsset(asset);
                }
                for (int i = 0; i < tabs->count(); ++i)
                    if (tabPath(tabs->widget(i)) == preferences.activeDocument)
                        active = tabs->widget(i);
                if (active)
                    tabs->setCurrentWidget(active);
            }
            modelPanel->offerModelRecovery();
        }
    });
    watcher->setFuture(QtConcurrent::run([previous = std::move(previous), root, index, paths, cancel]() mutable {
        previous.update(root, index, paths, cancel); return std::make_shared<mc::Library>(std::move(previous));
    }));
}
} // namespace nova
