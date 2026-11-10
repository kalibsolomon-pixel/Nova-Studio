// SPDX-License-Identifier: GPL-3.0-or-later
// Saving a document writes it into the pack: one path for Save, Save All and MCP saves.
// Plain textures write their PNG; layered textures write the PNG, then their editable source;
// animations write the PNG + .png.mcmeta pair, then their editable animation. A document is
// reported saved only when every part reached disk; otherwise it stays dirty with its recovery
// checkpoint, and the outcome says exactly what was written.
#include "window.h"
#include "animation_export.h"
#include "edit.h"
#include "minecraft_animation.h"
#include "modelpanel.h"
#include "timeline.h"
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QStatusBar>
#include <QtConcurrent>

namespace nova {
namespace {
// Interactive saves of animations run on the I/O pool, as the export dialog does; agents' saves
// run synchronously so nothing else is processed while a document is being published.
template <typename Work>
auto runIo(QWidget *parent, QThreadPool *pool, bool interactive, const QString &label, Work work) {
    using Result = decltype(work());
    if (!interactive)
        return work();
    QProgressDialog progress(label, QString(), 0, 0, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);
    QFutureWatcher<Result> watcher;
    QEventLoop wait;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(pool, std::move(work)));
    if (!watcher.isFinished())
        wait.exec();
    progress.close();
    return watcher.result();
}
// Write errors may name absolute paths; outcomes reach agents, so they never carry one.
QString withoutPaths(const QString &message, const QString &fallback) {
    static const QRegularExpression absolute(R"(([A-Za-z]:[\\/]|\\\\|(?:^|[\s"'(])/[^\s/]))");
    return message.isEmpty() || message.contains(absolute) ? fallback : message.left(300);
}
} // namespace

bool Window::documentDirty(const Document &doc) const {
    if (doc.modified() || doc.minecraftOutputPending())
        return true;
    // A new document is unsaved until its first save; an untouched vanilla baseline document is not.
    return !doc.revision().exists && !baselineDocs.contains(doc.id());
}

SaveOutcome Window::saveDocumentToPack(const std::shared_ptr<Document> &doc, bool interactive) {
    SaveOutcome outcome;
    auto fail = [&](const QString &code, const QString &message) {
        outcome.code = code;
        outcome.message = message;
        return outcome;
    };
    if (!doc || !session.owns(doc))
        return fail("document_not_open", "The document is no longer open.");
    const auto relative = project.isEmpty() ? QString() : QDir(project).relativeFilePath(doc->path);
    outcome.path = relative.isEmpty() || relative.startsWith("..") || QDir::isAbsolutePath(relative)
        ? QFileInfo(doc->path).fileName() : relative;
    outcome.type = doc->animated() ? "animation" : "texture";
    if (relative.isEmpty() || outcome.path != relative)
        return fail("not_in_project", "This document is outside the current project. Use Save As to choose a file.");
    for (int i = 0; i < tabs->count(); ++i) {
        auto canvas = dynamic_cast<Canvas *>(tabs->widget(i));
        if (!canvas || canvas->document != doc)
            continue;
        if (canvas->hasFloating() && (!interactive || !canvas->commitFloating()))
            return fail("document_busy", "Place or cancel the floating selection first.");
        if (!interactive && canvas->interactionActive())
            return fail("document_busy", "Finish the current canvas interaction first.");
    }
    if (!interactive && doc->strokeOpen())
        return fail("document_busy", "Finish the current stroke first.");
    if (baselineBacked(*doc)) {
        // The first save of a vanilla texture creates its pack folders inside the project (users only).
        QString error;
        if (!interactive)
            return fail("not_available", "Vanilla baseline documents are saved from Nova's editor.");
        if (!QFileInfo(QFileInfo(doc->path).absolutePath()).isDir() && !ensureAssetDirectory(project, relative, &error))
            return fail("missing_directory", "Nova could not create the texture's folder inside the project. "
                                             "Nothing was saved. " + withoutPaths(error, QString()));
    }
    if (!QFileInfo(QFileInfo(doc->path).absolutePath()).isDir())
        return fail("missing_directory", "The folder for this resource path no longer exists. Saving does not "
                                         "create folders; recreate it, or use Save As.");
    auto conflict = [&](const Revision &expected, const QString &file) -> QString {
        const auto disk = Revision::read(file);
        if (expected.sameContent(disk))
            return {};
        return expected.exists ? "external_change" : "path_occupied";
    };
    auto conflictMessage = [](const QString &code) {
        return code == "path_occupied"
            ? QString("Another file now occupies this resource path. Nova does not overwrite it; review the file "
                      "or use Save As.")
            : QString("The file changed on disk after Nova opened it. Nothing was saved; review the external "
                      "change (File › Review External Change).");
    };
    doc->finish();
    QString error;
    const auto root = project;
    if (doc->animated()) {
        if (!pendingAnimationPairs(doc->path).isEmpty())
            return fail("recovery_pending", "An interrupted animation export for this texture must be reviewed "
                                            "first (Animation › Recover Animation Export).");
        for (const auto &[expected, file] : {std::pair{doc->revision(), doc->path},
                                             {doc->mcmetaRevision(), doc->path + ".mcmeta"}})
            if (const auto code = conflict(expected, file); !code.isEmpty())
                return fail(code, conflictMessage(code));
        if (timeline)
            timeline->playback.stop();
        const auto serial = doc->changeSerial();
        const auto snapshot = doc->recoverySnapshot();
        const auto target = doc->animationState().target;
        struct Pair { QString error; bool ok = false; Revision png, meta; };
        const auto destination = doc->path;
        const auto expectedPng = doc->revision(), expectedMeta = doc->mcmetaRevision();
        const auto pair = runIo(this, &ioPool, interactive, "Saving animation to the pack…",
            [snapshot, target, destination, expectedPng, expectedMeta] {
                Pair result;
                const auto plan = planAnimationExport(*snapshot, 1, target, &result.error);
                if (!plan.ok())
                    return result;
                const auto journal = stageAnimationPair(destination, plan.png, plan.mcmeta, expectedPng,
                                                        expectedMeta, &result.error);
                result.ok = !journal.isEmpty() && finishAnimationPair(journal, PairAction::Retry, &result.error);
                if (result.ok) {
                    result.png = Revision::read(destination);
                    result.meta = Revision::read(destination + ".mcmeta");
                }
                return result;
            });
        if (!pair.ok)
            return fail("save_failed", "The PNG and .png.mcmeta pair was not written. " +
                                       withoutPaths(pair.error, "The pack files could not be written."));
        doc->acceptAnimationExport(pair.png, pair.meta, serial == doc->changeSerial());
        outcome.written << "pack";
        externalRevisions.remove(doc->id());
        baselineSaved(doc);
        const auto editableSerial = doc->changeSerial();
        const auto editable = doc->recoverySnapshot();
        const auto editableError = runIo(this, &ioPool, interactive, "Saving editable animation…", [editable, root] {
            QString failure;
            editable->saveNative(root, &failure);
            return failure;
        });
        if (!editableError.isEmpty() || !doc->acceptNativeSave(*editable, editableSerial)) {
            session.notify(doc);
            return fail("editable_not_saved", "The PNG and .png.mcmeta were written to the pack, but the editable "
                        "animation was not saved: " + withoutPaths(editableError, "the editable file could not be written."));
        }
        outcome.written << "editable";
    } else {
        if (const auto code = conflict(doc->revision(), doc->path); !code.isEmpty())
            return fail(code, conflictMessage(code));
        const bool layered = doc->nativeDocument();
        if (!session.dispatch(doc, layered ? Command::ExportPng : Command::Save, {}, false, &error))
            return fail(doc->externalChanged() ? "external_change" : "save_failed",
                        "The PNG was not written to the pack. " + withoutPaths(error, "The file could not be written."));
        outcome.written << "pack";
        externalRevisions.remove(doc->id());
        baselineSaved(doc);
        if (layered) {
            if (!session.dispatch(doc, Command::SaveNative, {}, false, &error))
                return fail("editable_not_saved", "The PNG was written to the pack, but the editable layers were not "
                            "saved: " + withoutPaths(error, "the editable file could not be written."));
            outcome.written << "editable";
        }
    }
    if (!doc->modified())
        recovery->remove(doc->id());
    session.notify(doc);
    if (documentDirty(*doc))
        return fail("save_failed", "The document changed while it was being saved; save it again.");
    return outcome;
}

SaveOutcome Window::saveModelToPack(const std::shared_ptr<mc::ModelEdit> &edit) {
    SaveOutcome outcome{edit ? edit->assetPath() : QString(), "model", {}, {}, {}};
    if (!edit)
        return {outcome.path, "model", "document_not_open", "The model is no longer open.", {}};
    if (modelPanel->authoringModel(edit->assetPath()))
        return {outcome.path, "model", "document_busy", "The model is open in the model authoring window; save it there.", {}};
    if (edit->previewing() || modelPanel->uvInteractionActive())
        return {outcome.path, "model", "document_busy", "Finish the current model or UV preview first.", {}};
    QString error;
    if (!modelPanel->saveModelDocument(edit, &error))
        return {outcome.path, "model", "save_failed",
                "The model JSON was not saved. " + withoutPaths(error, "The file could not be written."), {}};
    outcome.written << "pack";
    updateModelTabs();
    return outcome;
}

QList<SaveOutcome> Window::saveAllDocuments(bool interactive) {
    QList<SaveOutcome> outcomes;
    const auto documents = session.all();
    for (const auto &doc : documents)
        if (documentDirty(*doc))
            outcomes << saveDocumentToPack(doc, interactive);
    for (const auto &asset : modelPanel->openModelAssets())
        if (const auto edit = modelPanel->anyModelDocument(asset); edit && edit->modified())
            outcomes << saveModelToPack(edit);
    refresh();
    return outcomes;
}

void Window::saveAll() {
    if (auto canvas = current(); canvas && canvas->hasFloating() && !canvas->commitFloating())
        return;
    const auto outcomes = saveAllDocuments(true);
    QStringList saved, failed;
    for (const auto &outcome : outcomes)
        (outcome.ok() ? saved : failed) << (outcome.ok() ? outcome.path : outcome.path + " — " + outcome.message);
    if (failed.isEmpty()) {
        statusBar()->showMessage(saved.isEmpty() ? QString("Nothing to save")
                                                 : QString("Saved %1 to the pack").arg(saved.size() == 1
                                                       ? saved.first() : QString("%1 documents").arg(saved.size())),
                                 5000);
        return;
    }
    QMessageBox box(QMessageBox::Warning, "Save All", QString("%1 of %2 documents were not saved. They keep their "
                    "unsaved changes.").arg(failed.size()).arg(outcomes.size()), QMessageBox::Ok, this);
    box.setObjectName("saveAllReport");
    box.setInformativeText((saved.isEmpty() ? QString() : "Saved:\n" + saved.join('\n') + "\n\n") +
                           "Not saved:\n" + failed.join('\n'));
    box.exec();
}

void Window::saveCurrentToPack() {
    auto canvas = current();
    if (!canvas)
        return;
    const auto outcome = saveDocumentToPack(canvas->document, true);
    if (outcome.ok()) {
        statusBar()->showMessage("Saved to the pack · " + outcome.path +
                                 (outcome.written.contains("editable") ? " · editable source saved" : ""), 5000);
    } else if ((outcome.code == "external_change" || outcome.code == "path_occupied") &&
               !canvas->document->animated()) {
        conflictDialog(); // Keep, Reload, Save As or a confirmed Overwrite, as before
    } else {
        QMessageBox::warning(this, "Save failed", outcome.message);
    }
    refresh();
}

QStringList Window::unsavedDocuments(bool projectOnly) const {
    QStringList names;
    for (const auto &doc : session.all()) {
        if (!documentDirty(*doc))
            continue;
        const auto relative = project.isEmpty() ? QString() : QDir(project).relativeFilePath(doc->path);
        const bool inside = !relative.isEmpty() && !relative.startsWith("..") && !QDir::isAbsolutePath(relative);
        if (inside || !projectOnly)
            names << (inside ? relative : QFileInfo(doc->path).fileName());
    }
    if (modelPanel)
        for (const auto &asset : modelPanel->openModelAssets())
            if (const auto edit = modelPanel->anyModelDocument(asset); edit && edit->modified())
                names << asset;
    return names;
}

void Window::updateUnsavedStatus() {
    if (!unsavedLabel) {
        unsavedLabel = new QLabel(this);
        unsavedLabel->setObjectName("unsavedStatus");
        statusBar()->addPermanentWidget(unsavedLabel);
    }
    const auto names = unsavedDocuments(false);
    unsavedLabel->setVisible(!names.isEmpty());
    unsavedLabel->setText(names.size() == 1 ? QString("1 unsaved document") : QString("%1 unsaved documents").arg(names.size()));
    unsavedLabel->setAccessibleName(unsavedLabel->text());
    unsavedLabel->setToolTip(names.isEmpty() ? QString()
        : "Not yet saved to the pack:\n" + names.mid(0, 12).join('\n') +
              (names.size() > 12 ? QString("\n… and %1 more").arg(names.size() - 12) : QString()) +
              "\n\nFile › Save All (Ctrl+Alt+Shift+S) saves them.");
}
} // namespace nova
