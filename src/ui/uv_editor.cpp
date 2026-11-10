// SPDX-License-Identifier: GPL-3.0-or-later
#include "modelpanel.h"
#include "modelauthor.h"
#include "creation.h"
#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtWidgets>
namespace nova {
void ModelPanel::authorModel() {
    if (!library || !target.startsWith("model:")) {
        QMessageBox::information(this, "Choose a model", "Choose a block/item model in Models / UV first.");
        return;
    }
    if (uvInteraction || (uvDocument && uvDocument->previewing())) finishUvEdit(false);
    auto asset = modelAsset();
    if (!library->editable(asset)) {
        QMessageBox::information(this, "Read-only fallback", "Import a project-local override through Dependencies first.");
        return;
    }
    QString error;
    if (authorWindow && authorWindow->property("asset").toString() == asset) {
        authorWindow->raise();
        authorWindow->activateWindow();
        return;
    }
    auto edit = editableModel(asset, &error);
    if (!edit) {
        QMessageBox::warning(this, "Open model", error);
        return;
    }
    if (!edit->hasLocalGeometry() && !library->model(target.mid(6)).generated) {
        const auto inherited=library->model(target.mid(6)).resolved["elements"].toArray();
        if(!inherited.isEmpty()) {
            if(QMessageBox::question(this,"Local geometry override","Copy inherited elements into this editable model? Parent geometry updates will no longer apply.",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;
            if(!edit->localGeometry(inherited,&error)){QMessageBox::warning(this,"Override blocked",error);return;}
        }
    }
    if (displayDocument && displayDocument->previewing()) finishDisplayEdit(false);
    // One non-modal authoring window per panel.
    if (authorWindow && !authorWindow->close())
        return;
    uvDocument=edit;uvAsset=asset;
    // Non-modal (C8): the texture editor, main preview and Problems stay usable while
    // authoring. The panel's own UV/display editors defer to this window for the model.
    auto dialog = new ModelAuthorDialog(edit, library, root, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setProperty("asset", asset);
    dialog->changed = [this, edit] {
        library->previewSource(edit->assetPath(), edit->source());
        rebuild();
        if (modelChanged)
            modelChanged();
    };
    connect(dialog, &QDialog::finished, this, [this, edit, asset] {
        auto current = asset;
        if (edit->assetPath() != current) {
            modelEdits.remove(current);
            modelEdits[edit->assetPath()] = edit;
            current = edit->assetPath();
            uvAsset = current;
            target = "model:" + mc::Location::fromAsset(current, "models");
            choices();
        }
        library->previewSource(current, edit->source());
        rebuild();
        if (modelChanged)
            modelChanged();
    });
    authorWindow = dialog;
    dialog->show();
}
void ModelPanel::restoreModel() {
    if(root.isEmpty() || !library)return;
    auto file=QFileDialog::getOpenFileName(this,"Restore model recovery snapshot",root+"/.nova/model-recovery","Snapshot (*.json)");if(file.isEmpty())return;
    auto edit=std::make_shared<mc::ModelEdit>();QString error;
    if(!edit->restoreRecovery(root,file,&error)){QMessageBox::warning(this,"Recovery blocked",error);return;}
    if (edit->provisional()) {
        if (!registerProvisionalModel(edit, &error))
            QMessageBox::warning(this, "Recovery blocked", error);
        return;
    }
    if(modelEdits.contains(edit->assetPath()) && modelEdits[edit->assetPath()]->modified()) {
        if(QMessageBox::question(this,"Replace in-memory model edits?","Restore this snapshot in memory? Current unsaved model edits will be replaced; original disk JSON stays unchanged.",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;
    }
    if(modelEdits.size()>=16 && !modelEdits.contains(edit->assetPath())){QMessageBox::information(this,"History limit","Save pending models first.");return;}
    modelEdits[edit->assetPath()]=edit;if(modelOpened)modelOpened(edit);library->previewSource(edit->assetPath(),edit->source());openAsset(edit->assetPath());authorModel();
}
void ModelPanel::offerModelRecovery() {
    const auto project = root;
    if (project.isEmpty())
        return;
    QString folder;
    if (!mc::safeDestination(project, ".nova/model-recovery", &folder))
        return;
    QDirIterator snapshots(folder, {"*.json"}, QDir::Files | QDir::NoSymLinks);
    for (int inspected = 0; inspected < 64 && snapshots.hasNext(); ++inspected) {
        const auto path = snapshots.next();
        auto edit = std::make_shared<mc::ModelEdit>();
        QString error;
        if (!edit->restoreRecovery(project, path, &error) || !edit->modified() ||
            (modelEdits.contains(edit->assetPath()) && modelEdits.value(edit->assetPath())->modified()))
            continue;
        QMessageBox box(QMessageBox::Question, "Recover Unsaved Model",
                        "Restore the unsaved model " + edit->assetPath() + "?",
                        QMessageBox::NoButton, this);
        box.setObjectName("modelRecoveryDialog");
        auto restore = box.addButton("Restore", QMessageBox::AcceptRole);
        box.setEscapeButton(box.addButton("Later", QMessageBox::RejectRole));
        box.exec();
        if (root != project)
            return;
        if (box.clickedButton() != restore) {
            deferredModelRecovery.insert(edit->assetPath());
            continue;
        }
        if (edit->provisional()) {
            if (!registerProvisionalModel(edit, &error))
                QMessageBox::warning(this, "Recovery blocked", error);
        } else if (modelEdits.size() < 16 || modelEdits.contains(edit->assetPath())) {
            modelEdits[edit->assetPath()] = edit;
            library->previewSource(edit->assetPath(), edit->source());
            if (modelOpened)
                modelOpened(edit);
            if (openWorkspaceModel)
                openWorkspaceModel(edit->assetPath());
        }
    }
}
void ModelPanel::buildUvEditor(QVBoxLayout *layout) {
    uvControls = new QGroupBox("Face UV");
    uvControls->setObjectName("uvEditor");
    uvControls->hide();
    auto grid = new QGridLayout(uvControls);
    grid->setContentsMargins(4, 6, 4, 4);
    const char *names[] = {"U1", "V1", "U2", "V2"};
    for (int i = 0; i < 4; ++i) {
        auto s = new QDoubleSpinBox;
        s->setObjectName("uv" + QString(names[i]));
        s->setDecimals(4);
        s->setRange(-4096, 4096);
        s->setSingleStep(1);
        s->setKeyboardTracking(false);
        uvCoords[i] = s;
        grid->addWidget(new QLabel(names[i]), i, 0);
        grid->addWidget(s, i, 1);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { previewUv(); });
    }
    uvRotation = new QComboBox;
    uvRotation->setObjectName("uvRotation");
    uvRotation->addItems({"0°", "90°", "180°", "270°"});
    grid->addWidget(uvRotation, 4, 0);
    connect(uvRotation, &QComboBox::currentIndexChanged, this, [this] { previewUv(); });
    uvBegin = new QPushButton("Edit UV");
    uvBegin->setObjectName("uvBegin");
    grid->addWidget(uvBegin, 4, 1);
    connect(uvBegin, &QPushButton::clicked, this, &ModelPanel::beginUvEdit);
    uvSnap = new QCheckBox("Pixel snap");
    uvSnap->setChecked(true);
    uvSnap->setObjectName("uvSnap");
    uvLock = new QCheckBox("Lock ratio");
    uvLock->setObjectName("uvLock");
    grid->addWidget(uvSnap, 5, 0);
    grid->addWidget(uvLock, 5, 1);
    auto updateInteraction = [this] {
        if (canvas) {
            canvas->uvSnap = uvSnap->isChecked();
            canvas->uvAspect = uvLock->isChecked();
        }
    };
    connect(uvSnap, &QCheckBox::toggled, this, updateInteraction);
    connect(uvLock, &QCheckBox::toggled, this, updateInteraction);
    auto button = [&](const QString &label, const QString &name, int row, int col, auto callback) {
        auto b = new QPushButton(label);
        b->setObjectName(name);
        grid->addWidget(b, row, col);
        connect(b, &QPushButton::clicked, this, callback);
    };
    button("Commit", "uvCommit", 6, 0, [this] { finishUvEdit(true); });
    button("Cancel", "uvCancel", 6, 1, [this] { finishUvEdit(false); });
    button("Undo model", "uvUndo", 7, 0, [this] {
        if (uvDocument) {
            uvDocument->undo();
            refreshUvModel();
        }
    });
    button("Redo model", "uvRedo", 7, 1, [this] {
        if (uvDocument) {
            uvDocument->redo();
            refreshUvModel();
        }
    });
    button("Reset UV", "uvReset", 8, 0, [this] {
        if (uvDocument) {
            QString error;
            if (!uvDocument->resetUv(uvElement, uvName, &error))
                uvStatus->setText(error);
            else
                refreshUvModel();
        }
    });
    button("Save JSON", "uvSave", 8, 1, [this] {
        if (uvDocument)
            saveModel(uvDocument);
    });
    uvStatus = new QLabel;
    uvStatus->setWordWrap(true);
    uvStatus->setTextFormat(Qt::PlainText);
    uvStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    grid->addWidget(uvStatus, 9, 0, 1, 2);
    layout->addWidget(uvControls);
}
void ModelPanel::updateUvControls() {
    if (!uvControls)
        return;
    const int selected = cube->selectedFace;
    const bool selectedValid = mesh && selected >= 0 && selected < mesh->faces.size();
    uvControls->setVisible(selectedValid);
    if (!selectedValid)
        return;
    const auto &f = mesh->faces[selected];
    uvSyncing = true;
    auto raw = mc::mesh(library->model(f.modelId));
    for (const auto &face : raw.faces)
        if (face.elementIndex == f.elementIndex && face.name == f.name) {
            int start = (4 - face.rotation / 90) % 4;
            const auto a = face.uv[start] * 16, b = face.uv[(start + 2) % 4] * 16;
            const double values[] = {a.x(), a.y(), b.x(), b.y()};
            for (int i = 0; i < 4; ++i)
                uvCoords[i]->setValue(values[i]);
            uvRotation->setCurrentIndex(face.rotation / 90);
            break;
        }
    const bool editing = uvInteraction;
    for (auto s : uvCoords)
        s->setEnabled(editing);
    uvRotation->setEnabled(editing);
    uvBegin->setEnabled(f.elementIndex >= 0 && !editing);
    for (auto name : {"uvCommit", "uvCancel", "uvReset"})
        if (auto b = uvControls->findChild<QPushButton *>(name))
            b->setEnabled(editing);
    for (auto name : {"uvUndo", "uvRedo", "uvSave"})
        if (auto b = uvControls->findChild<QPushButton *>(name))
            b->setEnabled(bool(uvDocument));
    if (f.elementIndex < 0)
        uvStatus->setText("Generated geometry has no editable JSON face.");
    else if (uvDocument)
        uvStatus->setText(uvDocument->previewing() ? "UV preview · Enter commits, Escape cancels"
                          : uvDocument->modified() ? "Model changes pending · Save model JSON"
                                                   : "Model JSON matches saved revision");
    else
        uvStatus->setText(
            "UV units are 0–16 per texture. Edit UV enables handles and arrow nudging.");
    uvSyncing = false;
}
void ModelPanel::beginUvEdit() {
    const int index = cube->selectedFace;
    if (!mesh || index < 0 || index >= mesh->faces.size())
        return;
    const auto f = mesh->faces[index];
    if (f.elementIndex < 0)
        return;
    const auto asset = mc::Location::parse(f.modelId).asset("models");
    if (authorWindow && authorWindow->property("asset").toString() == asset) {
        uvStatus->setText("This model is open in the authoring window; edit its faces there.");
        return;
    }
    if (!library->editable(asset)) {
        uvStatus->setText(
            "Read-only fallback: import an override from Dependencies before editing.");
        return;
    }
    QString error;
    auto edit = editableModel(asset, &error);
    if (!edit) {
        uvStatus->setText(error);
        return;
    }
    if (!edit->hasLocalGeometry()) {
        if (QMessageBox::question(this, "Create local geometry override?",
                                  "This face is inherited. Copy every inherited element into this "
                                  "editable model? Future parent geometry changes will no longer "
                                  "apply. Unknown fields are preserved.",
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        if (!edit->localGeometry(library->model(f.modelId).resolved["elements"].toArray(),
                                 &error)) {
            uvStatus->setText(error);
            return;
        }
    }
    uvDocument = edit;
    uvAsset = asset;
    uvElement = f.elementIndex;
    uvName = f.name;
    uvInteraction = true;
    if (canvas) {
        canvas->uvEditing = true;
        canvas->uvSnap = uvSnap->isChecked();
        canvas->uvAspect = uvLock->isChecked();
        canvas->uvChanged = [this](QRectF r) { previewUvRect(r); };
        canvas->uvCommit = [this] { finishUvEdit(true); };
        canvas->uvCancel = [this] { finishUvEdit(false); };
        canvas->uvUndo = [this](bool redo) {
            if (uvDocument) {
                if (redo)
                    uvDocument->redo();
                else
                    uvDocument->undo();
                refreshUvModel();
            }
        };
        canvas->setFocus();
        canvas->update();
    }
    refreshUvModel();
}
void ModelPanel::previewUv() {
    if (uvSyncing || !uvDocument || !uvInteraction)
        return;
    QString error;
    if (!uvDocument->previewUv(uvElement, uvName,
                               QVector4D(float(uvCoords[0]->value()), float(uvCoords[1]->value()),
                                         float(uvCoords[2]->value()), float(uvCoords[3]->value())),
                               uvRotation->currentIndex() * 90, &error)) {
        uvStatus->setText(error);
        return;
    }
    refreshUvModel();
}
void ModelPanel::previewUvRect(QRectF r) {
    if (!document)
        return;
    const double sx = 16. / document->image.width(), sy = 16. / document->image.height();
    const bool flipX = uvCoords[0]->value() > uvCoords[2]->value(),
               flipY = uvCoords[1]->value() > uvCoords[3]->value();
    uvSyncing = true;
    uvCoords[0]->setValue((flipX ? r.right() : r.left()) * sx);
    uvCoords[2]->setValue((flipX ? r.left() : r.right()) * sx);
    uvCoords[1]->setValue((flipY ? r.bottom() : r.top()) * sy);
    uvCoords[3]->setValue((flipY ? r.top() : r.bottom()) * sy);
    uvSyncing = false;
    previewUv();
}
void ModelPanel::refreshUvModel() {
    if (!uvDocument || !library)
        return;
    if (!uvDocument->previewing() && uvDocument->modified())
        checkpointModel(uvDocument);
    if (modelChanged)
        modelChanged();
    library->previewSource(uvAsset, uvDocument->source());
    rebuild();
    if (canvas)
        canvas->update();
}
void ModelPanel::finishUvEdit(bool commit) {
    if (!uvDocument)
        return;
    uvInteraction = false;
    if (commit)
        uvDocument->commitPreview();
    else
        uvDocument->cancelPreview();
    if (canvas)
        canvas->uvEditing = false;
    refreshUvModel();
}
bool ModelPanel::saveModelDocument(const std::shared_ptr<mc::ModelEdit> &edit, QString *error) {
    if (!edit->save(error))
        return false;
    removeModelRecovery(edit);
    library->releaseModelSource(edit->assetPath());
    if (modelChanged)
        modelChanged();
    updateUvControls();
    return true;
}
bool ModelPanel::saveModel(const std::shared_ptr<mc::ModelEdit> &edit) {
    QString error;
    if (saveModelDocument(edit, &error))
        return true;
    QMessageBox box(this);
    box.setObjectName("modelJsonConflict");
    box.setWindowTitle("Model JSON save blocked");
    box.setText(error);
    box.setInformativeText("Keep preserves local edits. Reload discards local model edits. Save "
                           "copy leaves the externally changed original intact.");
    auto keep = box.addButton("Keep edits", QMessageBox::RejectRole);
    auto reload = box.addButton("Reload disk…", QMessageBox::ActionRole);
    auto copy = box.addButton("Save copy…", QMessageBox::ActionRole);
    box.setDefaultButton(keep);
    box.exec();
    if (box.clickedButton() == reload) {
        if (QMessageBox::question(
                this, "Discard local model edits?",
                "Reload the current disk model and discard this model's local history?",
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes) {
            if (edit->open(root, edit->assetPath(), &error)) {
                uvInteraction = false;
                if (canvas)
                    canvas->uvEditing = false;
                removeModelRecovery(edit);
                library->releaseModelSource(edit->assetPath());
                rebuild();
                if (modelChanged)
                    modelChanged();
                return true;
            }
            QMessageBox::warning(this, "Reload failed", error);
        }
    } else if (box.clickedButton() == copy) {
        const auto path = QFileDialog::getSaveFileName(this, "Save model copy",
                                                       root + "/model-copy.json", "JSON (*.json)");
        if (!path.isEmpty() && pathIdentity(path) != pathIdentity(edit->path()) &&
            !atomicWrite(path, QJsonDocument(edit->source()).toJson(), Revision::read(path),
                         &error))
            QMessageBox::warning(this, "Copy blocked", error);
    }
    return false;
}
void ModelPanel::removeModelRecovery(const std::shared_ptr<mc::ModelEdit> &edit) {
    if (deferredModelRecovery.contains(edit->assetPath()))
        return;
    const auto key = QString::fromLatin1(
        QCryptographicHash::hash(edit->assetPath().toUtf8(), QCryptographicHash::Sha256).toHex());
    QString path;
    if (mc::safeDestination(root, ".nova/model-recovery/" + key + ".json", &path))
        QFile::remove(path);
}

void ModelPanel::discardModelRecovery() {
    for (const auto &edit : modelEdits)
        removeModelRecovery(edit);
}

void ModelPanel::saveWorkspaceModel(const std::shared_ptr<mc::ModelEdit> &edit) {
    if (authorWindow && authorWindow->property("asset").toString() == edit->assetPath()) {
        authorWindow->raise();
        authorWindow->activateWindow();
        return;
    }
    saveModel(edit);
}

bool ModelPanel::confirmModelClose(const std::shared_ptr<mc::ModelEdit> &edit) {
    if (authorWindow && authorWindow->property("asset").toString() == edit->assetPath() &&
        !authorWindow->close())
        return false;
    if (uvDocument == edit && (uvInteraction || edit->previewing()))
        finishUvEdit(false);
    if (displayDocument == edit && edit->previewing())
        finishDisplayEdit(false);
    if (!edit->modified())
        return true;
    const auto choice = QMessageBox::question(
        this, "Unsaved model JSON", "Save changes to " + edit->assetPath() + "?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return choice == QMessageBox::Discard || (choice == QMessageBox::Save && saveModel(edit));
}

bool ModelPanel::closeModel(const std::shared_ptr<mc::ModelEdit> &edit) {
    if (!confirmModelClose(edit))
        return false;
    const auto asset = edit->assetPath();
    removeModelRecovery(edit);
    if (uvDocument == edit) {
        uvDocument.reset();
        uvAsset.clear();
    }
    if (displayDocument == edit)
        displayDocument.reset();
    modelEdits.remove(asset);
    library->releaseModelSource(asset);
    if (modelAsset() == asset) {
        pinned = false;
        target.clear();
    }
    choices();
    rebuild();
    return true;
}

bool ModelPanel::confirmClose() {
    const auto documents = modelEdits;
    for (const auto &edit : documents)
        if (!confirmModelClose(edit))
            return false;
    return true;
}
} // namespace nova
