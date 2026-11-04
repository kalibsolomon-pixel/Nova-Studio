// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "canvas.h"
#include "cube.h"
#include "display.h"
#include "edit.h"
#include "material_playback.h"
#include "models.h"
#include "session.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>
class QComboBox;
class QLineEdit;
class QLabel;
class QListWidget;
class QCheckBox;
class QGroupBox;
class QDoubleSpinBox;
class QPushButton;
class QVBoxLayout;
namespace nova {
class ModelPanel : public QWidget {
    Cube *cube;
    DocumentSession *session;
    std::shared_ptr<mc::Library> library;
    QString root, activeAsset, target;
    QComboBox *targets, *versions, *contexts;
    QLineEdit *inputs;
    QLabel *diagnostics, *faceInfo;
    QListWidget *faces, *dependencies;
    QCheckBox *uv;
    std::shared_ptr<const mc::Mesh> mesh;
    mc::Model contextModel;
    QMap<QString, QImage> materials;
    QMap<QString, std::shared_ptr<Document>> materialDocuments;
    QMap<QUuid, QString> liveMaterialBindings;
    MaterialPlayback *materialPlayback = nullptr;
    QCheckBox *animateMaterials = nullptr;
    QWidget *materialControls = nullptr;
    QImage loadMaterial(const QString &asset, QStringList &diagnostics);
    std::shared_ptr<Document> document;
    QPointer<Canvas> canvas;
    bool syncing = false, pinned = false;
    void rebuild();
    void choices();
    void overlay();
    void configureResources();
    void loadResources();
    QGroupBox *uvControls = nullptr;
    QDoubleSpinBox *uvCoords[4]{};
    QComboBox *uvRotation = nullptr;
    QCheckBox *uvSnap = nullptr, *uvLock = nullptr;
    QLabel *uvStatus = nullptr;
    QPushButton *uvBegin = nullptr;
    QMap<QString, std::shared_ptr<mc::ModelEdit>> modelEdits;
    std::shared_ptr<mc::ModelEdit> uvDocument;
    QString uvAsset, uvName;
    int uvElement = -1;
    bool uvSyncing = false;
    void buildUvEditor(QVBoxLayout *);
    void updateUvControls();
    void beginUvEdit();
    void previewUv();
    void previewUvRect(QRectF);
    void refreshUvModel();
    void finishUvEdit(bool commit);
    bool saveModel(const std::shared_ptr<mc::ModelEdit> &);
    // Item display transforms (display_editor.cpp)
    QWidget *displayControls = nullptr;
    QDoubleSpinBox *displayValues[9]{};
    QCheckBox *displayUniform = nullptr, *displaySnap = nullptr, *displayLinked = nullptr,
              *contextFraming = nullptr;
    QLabel *displaySource = nullptr, *displayStatus = nullptr;
    QPushButton *displayApply = nullptr, *displayCancel = nullptr;
    std::shared_ptr<mc::ModelEdit> displayDocument;
    QJsonObject displayClipboard;
    QString framedContext;
    bool displaySyncing = false;
    void buildDisplayEditor(QVBoxLayout *);
    void updateDisplayControls();
    void updateDisplayFraming();
    void previewDisplayValues();
    void refreshDisplayModel();
    void finishDisplayEdit(bool commit);
    mc::DisplayTransform displayValuesNow() const;
    void setDisplayValues(const mc::DisplayTransform &);
    QString displayModelAsset() const;
    bool confirmModelClose(const std::shared_ptr<mc::ModelEdit> &);
    void removeModelRecovery(const std::shared_ptr<mc::ModelEdit> &);
    QString checkpointModel(const std::shared_ptr<mc::ModelEdit> &);
    // Non-modal authoring window (one per panel)
    bool uvInteraction = false;
    bool modelTabActive = false;
    QSet<QString> deferredModelRecovery;
    QPointer<QWidget> authorWindow;

  public:
    std::function<void(const QString &)> openTexture;
    std::function<void(const QString &)> navigate;
    ModelPanel(Cube *, DocumentSession *, QWidget *parent = nullptr);
    void setLibrary(std::shared_ptr<mc::Library>, const QString &project);
    void setDocument(std::shared_ptr<Document>, Canvas *);
    void openAsset(const QString &asset, bool showMaterial = true);
    std::shared_ptr<mc::ModelEdit> editableModel(const QString &asset, QString *error);
    std::function<void(const std::shared_ptr<mc::ModelEdit> &)> modelOpened;
    std::function<void()> modelChanged;
    std::function<void(const QElapsedTimer &)> modelCheckpointed;
    bool closeModel(const std::shared_ptr<mc::ModelEdit> &);
    void discardModelRecovery();
    void saveWorkspaceModel(const std::shared_ptr<mc::ModelEdit> &);
    std::function<void(const QString &)> modelRegistered;
    std::function<void(const QString &)> openWorkspaceModel;
    void setModelTabActive(bool active);
    bool registerProvisionalModel(const std::shared_ptr<mc::ModelEdit> &, QString *error);
    void offerModelRecovery();
    QStringList previewMaterials() const { return materials.keys(); }
    QStringList previewParents() const { return contextModel.parents; }
    QImage modelPreviewImage(const QString &asset, bool uvView, int face, QString *error,
                             bool comparison = false);
    QStringList openModelAssets() const { return modelEdits.keys(); }
    std::shared_ptr<mc::ModelEdit> openModelDocument(const QString &asset) const;
    QString publishModelEdit(const std::shared_ptr<mc::ModelEdit> &edit);
    void authorModel();
    void restoreModel();
    QString modelAsset() const {
        return target.startsWith("model:") ? mc::Location::parse(target.mid(6)).asset("models")
                                          : target.startsWith("definition:") ? target.mid(11) : QString();
    }
    std::shared_ptr<mc::Library> catalog() const { return library; }
    QMap<QString,QJsonObject> documentSources() const {
        QMap<QString,QJsonObject> sources;
        for(auto it=modelEdits.begin();it!=modelEdits.end();++it)if(it.value()->modified() || it.value()->previewing())sources[it.key()]=it.value()->source();
        return sources;
    }
    QString displayContext() const; // empty for the raw (untransformed) view
    bool modelHistory(bool redo, std::shared_ptr<mc::ModelEdit> edit = {});
    bool historyShortcut(bool redo);
    bool uvInteractionActive() const { return uvInteraction; }
    bool authoringModel(const QString &asset) const {
        return authorWindow && authorWindow->property("asset").toString() == asset;
    }
    bool authoringOpen() const { return authorWindow; }
    void selectFace(int face);
    void selectTextureFaces();
    bool confirmClose();
    void savePreviewState();
    QString selectedTarget() const { return target; }
};
} // namespace nova
