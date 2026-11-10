// SPDX-License-Identifier: GPL-3.0-or-later
#include "modelpanel.h"
#include "display.h"
#include "minecraft_animation.h"
#include "modelauthor.h"
#include "project.h"
#include "settings.h"
#include "theme.h"
#include <QCryptographicHash>
#include <QImageReader>
#include <QJsonDocument>
#include <QtConcurrent>
#include <QtWidgets>
#include <cmath>
namespace nova {
ModelPanel::ModelPanel(Cube *c, DocumentSession *s, QWidget *parent)
    : QWidget(parent), cube(c), session(s) {
    auto v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(5);
    versions = new QComboBox;
    versions->setObjectName("modelVersion");
    versions->addItems({"1.21.11", "26.1", "26.3"});
    versions->setCurrentText("26.1");
    versions->setAccessibleName("Minecraft version profile");
    targets = new QComboBox;
    targets->setObjectName("modelTarget");
    targets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    targets->setMinimumContentsLength(12);
    targets->setAccessibleName("Model or item definition");
    v->setContentsMargins(ui::SpaceL, ui::SpaceM, ui::SpaceL, ui::SpaceM);
    v->setSpacing(ui::SpaceM);
    v->addWidget(ui::sectionTitle("Model"));
    auto resources = new QPushButton("Resource stack…");
    resources->setObjectName("resourceStack");
    resources->setToolTip("Choose resource packs below this one for fallback textures and models");
    versions->setToolTip("Minecraft Java version profile used to resolve and validate this model");
    auto resourceRow = new QHBoxLayout;
    resourceRow->addWidget(versions);
    resourceRow->addWidget(resources, 1);
    v->addLayout(resourceRow);
    connect(resources, &QPushButton::clicked, this, &ModelPanel::configureResources);
    v->addWidget(targets);
    auto author = new QPushButton("Edit model…");
    author->setObjectName("authorModel");
    author->setToolTip("Open the non-modal model editor: elements, faces, UVs and textures");
    auto modelRow = new QHBoxLayout;
    modelRow->addWidget(author, 1);
    v->addLayout(modelRow);
    connect(author, &QPushButton::clicked, this, &ModelPanel::authorModel);
    v->addWidget(ui::sectionTitle("Preview context"));
    contexts = new QComboBox;
    contexts->setObjectName("modelContext");
    contexts->setAccessibleName("Display context");
    contexts->setToolTip("raw shows the model untransformed; other entries apply that display context");
    contexts->addItems({"raw", "gui", "firstperson_righthand", "firstperson_lefthand",
                        "thirdperson_righthand", "thirdperson_lefthand", "ground", "fixed", "head",
                        "on_shelf"});
    v->addWidget(contexts);
    inputs = new QLineEdit;
    inputs->setObjectName("modelInputs");
    inputs->setAccessibleName("Preview inputs JSON");
    inputs->setPlaceholderText("Preview inputs as JSON…");
    inputs->setToolTip("Manual properties, e.g. "
                       "{\"facing\":\"north\",\"minecraft:using_item\":true,\"_choice\":1}. "
                       "Weighted choice is zero-based.");
    v->addWidget(inputs);
    buildDisplayEditor(v);
    materialPlayback = new MaterialPlayback(this);
    materialControls = new QWidget;
    auto materialLayout = new QVBoxLayout(materialControls);
    materialLayout->setContentsMargins(0, 0, 0, 0);
    v->addWidget(materialControls);
    materialControls->hide();
    animateMaterials = new QCheckBox("Animate materials");
    animateMaterials->setObjectName("animateMaterials");
    materialLayout->addWidget(animateMaterials);
    auto playbackRow = new QHBoxLayout;
    auto stopMaterials = new QPushButton("Stop");
    stopMaterials->setObjectName("stopMaterials");
    auto seekMaterials = new QSpinBox;
    seekMaterials->setObjectName("seekMaterials");
    seekMaterials->setRange(0, 3600000);
    seekMaterials->setSuffix(" ms");
    seekMaterials->setKeyboardTracking(false);
    playbackRow->addWidget(stopMaterials);
    playbackRow->addWidget(seekMaterials, 1);
    materialLayout->addLayout(playbackRow);
    connect(animateMaterials, &QCheckBox::toggled, this, [this](bool play) {
        if (play) {
            cube->setMaterialPreview(true);
            materialPlayback->play();
        } else
            materialPlayback->pause();
    });
    connect(stopMaterials, &QPushButton::clicked, this, [this] {
        materialPlayback->seek(0);
        materialPlayback->stop();
        cube->setMaterialPreview(false);
    });
    connect(seekMaterials, &QSpinBox::valueChanged, this, [this](int ms) {
        cube->setMaterialPreview(true);
        materialPlayback->seek(ms);
    });
    connect(materialPlayback, &MaterialPlayback::frame, cube, &Cube::setMaterialImage);
    connect(materialPlayback, &MaterialPlayback::statusChanged, this, [this] {
        QSignalBlocker block(animateMaterials);
        animateMaterials->setChecked(materialPlayback->playing());
        animateMaterials->setEnabled(materialPlayback->count() > 0);
        materialControls->setVisible(materialPlayback->count() > 0);
    });
    connect(cube, &Cube::visibilityChanged, materialPlayback, &MaterialPlayback::setVisible);
    materialPlayback->setVisible(cube->isVisible());
    connect(session, &DocumentSession::changed, this, [this](QUuid id) {
        for (const auto &doc : session->all())
            if (doc->id() == id) {
                const auto path = QDir(root).relativeFilePath(doc->path);
                if (!materials.contains(path))
                    continue;
                liveMaterialBindings[id] = path;
                materials[path] = doc->image;
                cube->setMaterialImage(path, doc->image);
                if (doc->animated()) {
                    materialDocuments[path] = doc;
                    materialPlayback->updateDocument(path, doc);
                }
                if (contextModel.generated)
                    rebuild();
            }
    });
    connect(session, &DocumentSession::closed, this, [this](QUuid id) {
        const auto path = liveMaterialBindings.take(id);
        if (!path.isEmpty()) {
            materialDocuments.remove(path);
            materials.remove(path);
            rebuild();
        }
    });
    auto viewToggle = new QToolButton;
    viewToggle->setText("Preview display options");
    viewToggle->setCheckable(true);
    viewToggle->setObjectName("previewControlsToggle");
    viewToggle->setProperty("role", "disclosure");
    viewToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    viewToggle->setArrowType(Qt::RightArrow);
    viewToggle->setAccessibleName("Show preview display options");
    v->addWidget(viewToggle);
    auto viewControls = new QWidget;
    viewControls->setObjectName("previewControls");
    auto viewLayout = new QVBoxLayout(viewControls);
    viewLayout->setContentsMargins(ui::SpaceL, 0, 0, 0);
    v->addWidget(viewControls);
    viewControls->hide();
    connect(viewToggle, &QToolButton::toggled, this, [viewToggle, viewControls](bool open) {
        viewToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        viewControls->setVisible(open);
    });
    auto controls = new QGridLayout;
    int row = 0, col = 0;
    auto toggle = [&](const QString &text, bool *field) {
        auto b = new QCheckBox(text);
        b->setObjectName("preview" + text);
        controls->addWidget(b, row, col);
        if (++col == 2) {
            col = 0;
            ++row;
        }
        connect(b, &QCheckBox::toggled, this, [=, this](bool value) {
            *field = value;
            cube->update();
        });
    };
    toggle("Perspective", &cube->perspective);
    toggle("Wireframe", &cube->wireframe);
    toggle("Outlines", &cube->outlines);
    toggle("Axes", &cube->axes);
    toggle("Grid", &cube->groundGrid);
    toggle("Neutral", &cube->neutral);
    viewLayout->addLayout(controls);
    auto lighting = new QVBoxLayout;
    auto ambient = new QSpinBox;
    ambient->setRange(0, 100);
    ambient->setValue(55);
    ambient->setSuffix("% ambient");
    ambient->setObjectName("previewAmbient");
    auto light = new QComboBox;
    light->setObjectName("previewLight");
    light->addItems({"Light: upper left", "Light: upper right", "Light: front", "Light: below"});
    lighting->addWidget(ambient);
    lighting->addWidget(light);
    viewLayout->addLayout(lighting);
    connect(ambient, &QSpinBox::valueChanged, this, [this](int n) {
        cube->ambient = float(n) / 100;
        cube->update();
    });
    connect(light, &QComboBox::currentIndexChanged, this, [this](int n) {
        const QVector3D directions[] = {{-.4f, .8f, .6f}, {.4f, .8f, .6f}, {0, 0, 1}, {0, -1, 0}};
        cube->lightDirection = directions[n];
        cube->update();
    });
    auto camera = new QHBoxLayout;
    for (const auto &name : {"Reset view", "Fit model"}) {
        auto b = new QPushButton(name);
        camera->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, name] {
            if (QString(name) == "Reset view")
                cube->resetView();
            else
                cube->fitModel();
        });
    }
    viewLayout->addLayout(camera);
    auto resetPreview = new QPushButton("Reset Preview");
    resetPreview->setObjectName("resetPreview");
    viewLayout->addWidget(resetPreview);
    connect(resetPreview, &QPushButton::clicked, this, [this] {
        target.clear();
        pinned = false;
        contexts->setCurrentText("raw");
        uv->setChecked(false);
        cube->restoreView({});
        for (auto name : {"Perspective", "Wireframe", "Outlines", "Axes", "Grid", "Neutral"})
            if (auto b = findChild<QCheckBox *>("preview" + QString(name))) {
                QSignalBlocker block(b);
                b->setChecked(false);
            }
        if (auto ambient = findChild<QSpinBox *>("previewAmbient"))
            ambient->setValue(55);
        if (auto light = findChild<QComboBox *>("previewLight"))
            light->setCurrentIndex(0);
        choices();
        rebuild();
    });
    auto source = new QPushButton("Inspect JSON…");
    source->setObjectName("inspectModelJson");
    source->setToolTip("Read-only snapshot of the selected model's source JSON");
    modelRow->addWidget(source, 1);
    connect(source, &QPushButton::clicked, this, [this] {
        if (!library || target.isEmpty() || target == "cube:")
            return;
        const auto path = target.startsWith("model:")
                              ? mc::Location::parse(target.mid(6)).asset("models")
                              : target.mid(11);
        auto dialog = new QDialog(this); // non-modal read-only snapshot (C8)
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName("modelJsonInspector");
        dialog->setWindowTitle(path + " · read-only snapshot");
        auto layout = new QVBoxLayout(dialog);
        auto text = new QPlainTextEdit;
        text->setReadOnly(true);
        text->setPlainText(QString::fromUtf8(
            QJsonDocument(library->source(path)).toJson(QJsonDocument::Indented)));
        layout->addWidget(text);
        dialog->resize(680, 520);
        dialog->show();
    });
    v->addWidget(ui::sectionTitle("Faces & UV"));
    uv = new QCheckBox("Show UV regions on the texture");
    uv->setToolTip("Ctrl+Alt-click on a texture UV region to cycle overlapping model faces");
    uv->setObjectName("uvOverlay");
    v->addWidget(uv);
    connect(uv, &QCheckBox::toggled, this, [this] { overlay(); });
    faces = new QListWidget;
    faces->setObjectName("modelFaces");
    faces->setAccessibleName("Model faces");
    faces->setMaximumHeight(90);
    v->addWidget(faces);
    faceInfo = new QLabel;
    faceInfo->setWordWrap(true);
    faceInfo->setTextFormat(Qt::PlainText);
    v->addWidget(faceInfo);
    faceInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    buildUvEditor(v);
    v->addWidget(ui::sectionTitle("Diagnostics"));
    diagnostics = new QLabel;
    diagnostics->setObjectName("modelDiagnostics");
    diagnostics->setTextFormat(Qt::PlainText);
    diagnostics->setWordWrap(true);
    diagnostics->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(diagnostics);
    diagnostics->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto dependencyTitle = ui::hint("Dependencies · double-click to open");
    v->addWidget(dependencyTitle);
    dependencies = new QListWidget;
    dependencies->setObjectName("modelDependencies");
    dependencies->setAccessibleName("Model dependencies");
    dependencies->setMaximumHeight(130);
    v->addWidget(dependencies);
    v->addStretch();
    connect(targets, &QComboBox::currentIndexChanged, this, [this] {
        if (syncing)
            return;
        target = targets->currentData().toString();
        pinned = true;
        rebuild();
    });
    connect(versions, &QComboBox::currentTextChanged, this, [this](const QString &) {
        if (library) {
            loadResources();
            choices();
            rebuild();
        }
    });
    connect(contexts, &QComboBox::currentTextChanged, this, [this](const QString &) { rebuild(); });
    connect(inputs, &QLineEdit::returnPressed, this, [this] { rebuild(); });
    connect(faces, &QListWidget::currentRowChanged, this, [this](int face) {
        if (!syncing)
            selectFace(face);
    });
    connect(cube, &Cube::faceSelected, this, [this](int face) { selectFace(face); });
    connect(dependencies, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const auto path = item->data(Qt::UserRole).toString();
        if (path.isEmpty())
            return;
        const auto providers =
            library ? library->providers(path) : QVector<mc::ResourceCandidate>{};
        if (library && (!library->editable(path) || providers.size() > 1)) {
            const auto inspectedLibrary = library;
            QDialog dialog(this);
            dialog.setObjectName("resourceInspector");
            dialog.setWindowTitle("Read-only resource inspection");
            QVBoxLayout layout(&dialog);
            auto assetLabel = new QLabel(path);
            assetLabel->setWordWrap(true);
            layout.addWidget(assetLabel);
            auto sourceChoice = new QComboBox;
            sourceChoice->setObjectName("resourceProvider");
            sourceChoice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            sourceChoice->setMinimumContentsLength(12);
            for (int i = 0; i < providers.size(); ++i) {
                sourceChoice->addItem(providers[i].label + (i == 0 ? " · winner" : " · shadowed"));
                sourceChoice->setItemData(i, providers[i].path, Qt::ToolTipRole);
            }
            layout.addWidget(sourceChoice);
            auto status = new QLabel;
            status->setWordWrap(true);
            layout.addWidget(status);
            auto views = new QStackedWidget;
            auto text = new QPlainTextEdit;
            text->setObjectName("resourceContents");
            text->setReadOnly(true);
            views->addWidget(text);
            auto picture = new QLabel;
            picture->setObjectName("resourceImage");
            picture->setAlignment(Qt::AlignCenter);
            views->addWidget(picture);
            layout.addWidget(views, 1);
            auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
            auto import = buttons->addButton("Import override", QDialogButtonBox::ActionRole);
            auto edit = buttons->addButton("Open editable", QDialogButtonBox::ActionRole);
            bool openEditable = false;
            layout.addWidget(buttons);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            auto present = [&] {
                const int index = sourceChoice->currentIndex();
                const auto fileName = inspectedLibrary->assetPath(path, index);
                text->clear();
                picture->clear();
                views->setCurrentWidget(text);
                import->setEnabled(!fileName.isEmpty() && index >= 0 &&
                                   !providers[index].editable &&
                                   !QFileInfo::exists(root + '/' + path));
                edit->setEnabled(!fileName.isEmpty() && index >= 0 && providers[index].editable);
                status->setText(
                    "Read-only inspection. Source priority stays unchanged. Import creates "
                    "a local override only when the destination does not exist.");
                if (fileName.isEmpty()) {
                    text->setPlainText("Resource is missing, unsafe or failed hash verification.");
                    return;
                }
                if (path.endsWith(".png")) {
                    QImageReader reader(fileName, "PNG");
                    const auto size = reader.size();
                    if (!size.isValid() || size.width() > 32768 || size.height() > 32768 ||
                        qint64(size.width()) * size.height() * 4 > 64 * 1024 * 1024) {
                        text->setPlainText("Image exceeds the 64 MiB inspection budget.");
                        return;
                    }
                    reader.setScaledSize(
                        size.scaled(600, 360, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)));
                    const auto image = reader.read();
                    if (image.isNull()) {
                        text->setPlainText(reader.errorString());
                        return;
                    }
                    picture->setPixmap(QPixmap::fromImage(image));
                    views->setCurrentWidget(picture);
                } else {
                    QFile file(fileName);
                    if (file.open(QIODevice::ReadOnly)) {
                        text->setPlainText(QString::fromUtf8(file.read(2 * 1024 * 1024)));
                        if (file.size() > 2 * 1024 * 1024)
                            status->setText("Read-only inspection · first 2 MiB shown.");
                    } else
                        text->setPlainText("Resource could not be opened.");
                }
            };
            connect(sourceChoice, &QComboBox::currentIndexChanged, &dialog, present);
            present();
            connect(edit, &QPushButton::clicked, &dialog, [&] {
                openEditable = true;
                dialog.accept();
            });
            connect(import, &QPushButton::clicked, &dialog, [&] {
                QFile source(inspectedLibrary->assetPath(path, sourceChoice->currentIndex()));
                QString error;
                if (!source.open(QIODevice::ReadOnly) || source.size() > 16 * 1024 * 1024 ||
                    !QDir().mkpath(QFileInfo(root + '/' + path).absolutePath()) ||
                    !atomicWrite(root + '/' + path, source.readAll(), Revision{}, &error)) {
                    QMessageBox::warning(&dialog, "Import blocked", error);
                    return;
                }
                dialog.accept();
            });
            dialog.resize(650, 500);
            dialog.exec();
            if (openEditable) {
                if (navigate)
                    navigate(path);
                if (path.endsWith(".png")) {
                    if (openTexture)
                        openTexture(root + '/' + path);
                } else
                    openWorkspaceModel ? openWorkspaceModel(path) : openAsset(path);
            }
            return;
        }
        if (navigate)
            navigate(path);
        if (path.endsWith(".png")) {
            if (openTexture)
                openTexture(root + '/' + path);
        } else if (!mc::Location::fromAsset(path, "models").isEmpty() ||
                   !mc::Location::fromAsset(path, "items").isEmpty() ||
                   !mc::Location::fromAsset(path, "blockstates").isEmpty())
            openWorkspaceModel ? openWorkspaceModel(path) : openAsset(path);
        else {
            QDialog dialog(this);
            dialog.setWindowTitle(path);
            auto layout = new QVBoxLayout(&dialog);
            auto text = new QPlainTextEdit;
            text->setReadOnly(true);
            QFile f(root + '/' + path);
            if (f.open(QIODevice::ReadOnly))
                text->setPlainText(QString::fromUtf8(f.read(2 * 1024 * 1024)));
            layout->addWidget(text);
            dialog.resize(650, 500);
            dialog.exec();
        }
    });
}
namespace {
QString resourceSettingsKey(const QString &root, const QString &version) {
    return "modelResources/" +
           QString::fromLatin1(
               QCryptographicHash::hash(QFileInfo(root).canonicalFilePath().toCaseFolded().toUtf8(),
                                        QCryptographicHash::Sha256)
                   .toHex()) +
           '/' + version;
}
} // namespace
void ModelPanel::configureResources() {
    if (!library || root.isEmpty())
        return;
    QSettings prefs(Settings(qApp->property("settingsFile").toString()).fileName(),
                    QSettings::IniFormat);
    const auto key = resourceSettingsKey(root, library->version());
    bool ok = false;
    const auto value = QInputDialog::getMultiLineText(
        this, "Resource stack",
        "Editable pack has highest priority. Add folders below in descending priority, one per "
        "line.\n"
        "For vanilla choose a verified cache made with prepare-vanilla-cache.py for this Java "
        "version.\n"
        "Fallback files remain read-only; double-click a dependency to inspect or import it.",
        prefs.value(key).toStringList().join('\n'), &ok);
    if (!ok)
        return;
    auto paths = value.split('\n', Qt::SkipEmptyParts);
    for (auto &p : paths)
        p = p.trimmed();
    if (paths.size() > 8) {
        QMessageBox::warning(this, "Resource stack", "At most eight fallback sources");
        return;
    }
    const auto old = prefs.value(key);
    prefs.setValue(key, paths);
    prefs.sync();
    loadResources();
    choices();
    rebuild();
    Q_UNUSED(old);
}
void ModelPanel::loadResources() {
    if (!library || root.isEmpty())
        return;
    QSettings prefs(Settings(qApp->property("settingsFile").toString()).fileName(),
                    QSettings::IniFormat);
    const auto version = versions->currentText();
    const auto paths = prefs.value(resourceSettingsKey(root, version)).toStringList().mid(0, 8);
    if (paths.isEmpty() && !library->hasFallbacks() && library->version() == version)
        return;
    const auto originalLibrary = library;
    const auto originalRoot = root;
    QFutureWatcher<std::shared_ptr<mc::Library>> watcher;
    QEventLoop loop;
    QProgressDialog progress("Loading resource sources…", QString(), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(250);
    connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    auto previous = *library;
    watcher.setFuture(QtConcurrent::run([paths, version, previous = std::move(previous)]() mutable {
        QVector<mc::ResourceSource> sources;
        QStringList errors;
        for (const auto &path : paths) {
            QString error;
            auto source = mc::ResourceSource::open(path, version, &error);
            if (!error.isEmpty())
                errors << path + ": " + error;
            else
                sources << source;
        }
        // Additional packs precede vanilla regardless of an accidentally misplaced cache.
        std::stable_sort(sources.begin(), sources.end(),
                         [](const auto &a, const auto &b) { return !a.vanilla && b.vanilla; });
        previous.setFallbacks(sources, version);
        previous.diagnostics += errors;
        return std::make_shared<mc::Library>(std::move(previous));
    }));
    if (!watcher.isFinished())
        loop.exec();
    progress.close();
    if (root != originalRoot || library != originalLibrary) {
        diagnostics->setText(
            "Resource loading was superseded by a catalog change; reapply Resource stack.");
        return;
    }
    *library = std::move(*watcher.result());
}
QStringList ModelPanel::resourceStackPaths() const {
    if (!library || root.isEmpty())
        return {};
    QSettings prefs(Settings(qApp->property("settingsFile").toString()).fileName(), QSettings::IniFormat);
    return prefs.value(resourceSettingsKey(root, library->version())).toStringList().mid(0, 8);
}
void ModelPanel::setLibrary(std::shared_ptr<mc::Library> value, const QString &project) {
    if (library != value)
        materialDocuments.clear();
    const bool newProject = root != project;
    const auto stored =
        newProject ? Settings(qApp->property("settingsFile").toString()).projectPreview(project)
                   : QJsonObject();
    if (newProject) {
        liveMaterialBindings.clear();
        modelEdits.clear();
        deferredModelRecovery.clear();
        uvInteraction = false;
        uvDocument.reset();
        displayDocument.reset();
        uvAsset.clear();
        uvElement = -1;
        target.clear();
        pinned = false;
        const auto pack = Project::inspect(project).json["pack"].toObject();
        const auto format = PackFormat::parse(pack.contains("min_format") ? pack["min_format"]
                                                                          : pack["pack_format"]);
        if (format)
            for (const auto &v : gameVersions())
                if (v.format == *format && mc::Profile::forVersion(v.id).valid()) {
                    QSignalBlocker block(versions);
                    versions->setCurrentText(v.id);
                    break;
                }
    }
    library = std::move(value);
    root = project;
    if (newProject && mc::Profile::forVersion(stored["version"].toString()).valid()) {
        QSignalBlocker block(versions);
        versions->setCurrentText(stored["version"].toString());
    }
    if (library && (newProject || library->version() != versions->currentText()))
        loadResources();
    if (newProject && !stored.isEmpty()) {
        target = stored["target"].toString().left(2048);
        pinned = !target.isEmpty();
        QSignalBlocker block(contexts);
        contexts->setCurrentText(stored["context"].toString("raw"));
        inputs->setText(stored["inputs"].toString().left(4096));
        uv->setChecked(stored["uv"].toBool());
    }
    for (const auto &edit : modelEdits)
        if (edit->provisional())
            library->setProvisionalSource(edit->assetPath(), edit->source());
        else if (edit->modified() || edit->previewing())
            library->previewSource(edit->assetPath(), edit->source());
    choices();
    rebuild();
    if (newProject && !stored.isEmpty()) {
        cube->restoreView(stored["camera"].toObject());
        const QPair<const char *, bool> flags[] = {
            {"Perspective", cube->perspective}, {"Wireframe", cube->wireframe},
            {"Outlines", cube->outlines},       {"Axes", cube->axes},
            {"Grid", cube->groundGrid},         {"Neutral", cube->neutral}};
        for (const auto &flag : flags)
            if (auto b = findChild<QCheckBox *>("preview" + QString(flag.first))) {
                QSignalBlocker block(b);
                b->setChecked(flag.second);
            }
        if (auto ambient = findChild<QSpinBox *>("previewAmbient")) {
            QSignalBlocker block(ambient);
            ambient->setValue(int(cube->ambient * 100));
        }
    }
}
void ModelPanel::savePreviewState() {
    if (root.isEmpty())
        return;
    Settings(qApp->property("settingsFile").toString())
        .saveProjectPreview(root, {{"schema", 1},
                                   {"target", target},
                                   {"version", versions->currentText()},
                                   {"context", contexts->currentText()},
                                   {"inputs", inputs->text().left(4096)},
                                   {"uv", uv->isChecked()},
                                   {"camera", cube->viewSettings()}});
}
void ModelPanel::choices() {
    syncing = true;
    targets->clear();
    targets->addItem("No model · texture only", QString());
    targets->addItem("Generic cube (not a Minecraft model)", QStringLiteral("cube:"));
    fallbackReason.clear();
    if (!document)
        fallbackReason = "No texture is open";
    else if (!library)
        fallbackReason = "No resource pack is open";
    if (library) {
        // The model named like the texture first, then other valid models using it.
        const auto choice = activeAsset.isEmpty() ? mc::PreviewChoice{} : mc::previewModelFor(*library, activeAsset);
        const auto associated = choice.candidates.mid(0, 256);
        for (const auto &id : associated)
            targets->addItem("Using texture · " + id, "model:" + id);
        if (document)
            fallbackReason = choice.reason;
        if (!pinned) {
            target = choice.model.isEmpty() ? QString() : "model:" + choice.model;
        }
        const auto all = library->modelIds();
        const auto definitions = library->definitionPaths();
        // Editable resources and definitions must remain reachable even when a
        // vanilla catalog supplies thousands of lower-priority model names.
        for (bool editable : {true, false}) {
            int models = 0, dispatches = 0;
            const int modelLimit = editable ? 2048 : 1024, definitionLimit = editable ? 512 : 256;
            for (const auto &id : all)
                if (!associated.contains(id) &&
                    library->editable(mc::Location::parse(id).asset("models")) == editable &&
                    models++ < modelLimit)
                    targets->addItem(id, "model:" + id);
            for (const auto &path : definitions)
                if (library->editable(path) == editable && dispatches++ < definitionLimit)
                    targets->addItem(path, "definition:" + path);
        }
    }
    int index = targets->findData(target);
    if (index < 0 && library && target.startsWith("model:") &&
        library->editable(mc::Location::parse(target.mid(6)).asset("models"))) {
        index = targets->count();
        targets->addItem(target.mid(6), target);
    }
    if (index < 0) {
        target.clear();
        index = 0;
    }
    targets->setCurrentIndex(index);
    targets->setToolTip(target.isEmpty() && !fallbackReason.isEmpty()
                            ? "No Minecraft model is shown: " + fallbackReason
                            : QString("Model shown in the 3D preview"));
    syncing = false;
}
void ModelPanel::showFallback(const QString &notice, const QString &details) {
    cube->setNotice(notice);
    diagnostics->setText(notice + (details.isEmpty() ? QString() : "\n" + details));
    diagnostics->setToolTip(notice);
}
void ModelPanel::setDocument(std::shared_ptr<Document> doc, Canvas *view) {
    if (document == doc && canvas == view &&
        (!doc || activeAsset == QDir(root).relativeFilePath(doc->path)))
        return;
    if (uvInteraction)
        finishUvEdit(false);
    if (canvas) {
        canvas->uvEditing = false;
        canvas->uvChanged = {};
        canvas->uvCommit = {};
        canvas->uvCancel = {};
        canvas->uvUndo = {};
        canvas->uvRegions.clear();
        canvas->uvVisible = false;
        canvas->uvSelected = {};
        canvas->update();
    }
    document = std::move(doc);
    canvas = view;
    activeAsset = document ? QDir(root).relativeFilePath(document->path) : QString();
    const auto previous = target;
    choices();
    if (previous == target && mesh) {
        cube->setModel(mesh, activeAsset, materials);
        for (const auto &path : materials.keys()) {
            const auto image = materialPlayback->image(path);
            if (!image.isNull())
                cube->setMaterialImage(path, image);
        }
        overlay();
    } else
        rebuild();
    if (canvas)
        canvas->uvSelected = [this](int face) { selectFace(face); };
}
void ModelPanel::setModelTabActive(bool active) {
    if (modelTabActive && !active && uvInteraction)
        finishUvEdit(false);
    modelTabActive = active;
    targets->setEnabled(!active);
}

void ModelPanel::openAsset(const QString &asset, bool showMaterial) {
    if (modelAsset() != asset) {
        if (uvInteraction || (uvDocument && uvDocument->previewing()))
            finishUvEdit(false);
        if (displayDocument && displayDocument->previewing())
            finishDisplayEdit(false);
        uvDocument.reset();
        displayDocument.reset();
        uvAsset.clear();
    }
    if (!library)
        return;
    pinned = true;
    const auto id = mc::Location::fromAsset(asset, "models");
    target = id.isEmpty() ? "definition:" + asset : "model:" + id;
    choices();
    rebuild();
    cube->fitModel();
    // Opening a model also offers its first editable material through the existing tab engine.
    if (showMaterial && mesh && !mesh->faces.isEmpty() && (!document || !materials.contains(activeAsset)))
        for (const auto &f : mesh->faces)
            if (!f.texture.isEmpty() && QFileInfo::exists(root + '/' + f.texture)) {
                if (openTexture)
                    openTexture(root + '/' + f.texture);
                break;
            }
}
void ModelPanel::rebuild() {
    inputs->setVisible(target.startsWith("definition:"));
    const int previousFace = cube->selectedFace;
    faces->clear();
    dependencies->clear();
    faceInfo->clear();
    contextModel = {};
    mesh.reset();
    materials.clear();
    if (!library || target.isEmpty() || target == "cube:") {
        materialPlayback->setMaterials({});
        cube->setMaterialPreview(false);
        if (uvControls)
            uvControls->hide();
        if (canvas)
            canvas->uvEditing = false;
        if (target == "cube:") {
            cube->setModel({});
            showFallback("Generic cube: not a Minecraft model. Choose a model to preview it.",
                         "The texture is drawn on all six faces of a plain cube.");
        } else {
            // Texture only: one flat, double-sided square. Never a cube that could be
            // mistaken for the texture's real model.
            mc::Model flat;
            flat.valid = true;
            flat.version = library ? library->version() : QString("26.1");
            flat.resolved = QJsonDocument::fromJson(
                R"json({"elements":[{"from":[0,0,8],"to":[16,16,8],"faces":{"north":{"uv":[0,0,16,16],"texture":"nova:active"},"south":{"uv":[0,0,16,16],"texture":"nova:active"}}}]})json")
                                .object();
            mesh = std::make_shared<mc::Mesh>(mc::mesh(flat));
            cube->setModel(mesh, mesh->faces.isEmpty() ? QString() : mesh->faces.first().texture);
            const auto reason = fallbackReason.isEmpty() ? QString("No model chosen") : fallbackReason;
            showFallback("No Minecraft model · " + reason + ".\nShowing the texture flat; this is not a model render.",
                         library ? QString("Choose a model above, or add one to the pack or the resource stack.")
                                 : QString());
            mesh.reset(); // no faces/UV inspection for the fallback square
        }
        overlay();
        updateDisplayControls();
        return;
    }
    cube->setNotice({});
    auto combined = std::make_shared<mc::Mesh>();
    QStringList messages = library->diagnostics;
    QStringList previewLimitations;
    QString asset;
    QVector<mc::Instance> instances;
    messages << "Isolated preview · world AO, atlas shaders and runtime tints omitted";
    if (target.startsWith("model:")) {
        auto id = target.mid(6);
        instances << mc::Instance{id, "raw model"};
        asset = mc::Location::parse(id).asset("models");
    } else {
        asset = target.mid(11);
        QJsonParseError error;
        QJsonObject values;
        if (!inputs->text().trimmed().isEmpty()) {
            auto json = QJsonDocument::fromJson(inputs->text().toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !json.isObject()) {
                diagnostics->setText("Preview inputs must be a JSON object");
                cube->setModel(combined, activeAsset);
                overlay();
                return;
            }
            values = json.object();
        }
        auto dispatch = library->dispatch(asset, values, values["_choice"].toInt());
        instances = dispatch.instances;
        messages += dispatch.diagnostics;
    }
    for (const auto &instance : instances) {
        const auto model = library->model(instance.model);
        if (contextModel.id.isEmpty())
            contextModel = model;
        if (model.generated)
            for (const auto &path : model.textures) {
                if (materials.contains(path) || materials.size() >= 64)
                    continue;
                materials[path] = loadMaterial(path, messages);
            }
        auto generatedSprites = materials;
        if (model.generated)
            for (const auto &path : model.textures)
                if (auto doc = materialDocuments.value(path)) {
                    auto silhouette = materials.value(path).copy();
                    silhouette.fill(Qt::transparent);
                    for (const auto &frame : doc->animationState().frames) {
                        const auto pixels = doc->frameComposite(frame.id);
                        if (pixels.size() != silhouette.size())
                            continue;
                        for (int y = 0; y < pixels.height(); ++y)
                            for (int x = 0; x < pixels.width(); ++x)
                                if (qAlpha(pixels.pixel(x, y)))
                                    silhouette.setPixelColor(x, y, Qt::white);
                    }
                    generatedSprites[path] = silhouette;
                }
        auto part = mc::transformed(mc::generatedMesh(model, generatedSprites), instance);
        if (!part.previewLimitation.isEmpty())
            previewLimitations << instance.model + ": " + part.previewLimitation;
        // Minecraft order: display * translate(-0.5) * item-definition transformation.
        part = mc::applyTransform(
            std::move(part),
            mc::itemPose(mc::displayTransform(model, displayContext()), instance.transformation));
        for (auto &face : part.faces)
            if (face.tint >= 0 && face.tint < instance.tints.size())
                face.color = instance.tints[face.tint];
        messages += part.diagnostics;
        if (combined->faces.size() + part.faces.size() > 24576) {
            messages << "Combined geometry budget (24576 faces)";
            break;
        }
        for (auto &face : part.faces) {
            face.element = instance.model + " / " + face.element;
            combined->faces << face;
        }
        if (!instance.label.isEmpty())
            messages << instance.label;
    }
    if (!combined->faces.isEmpty()) {
        combined->minimum = combined->maximum = combined->faces[0].points[0];
        for (const auto &f : combined->faces)
            for (const auto &p : f.points)
                for (int i = 0; i < 3; ++i) {
                    combined->minimum[i] = std::min(combined->minimum[i], p[i]);
                    combined->maximum[i] = std::max(combined->maximum[i], p[i]);
                }
    }
    qint64 bytes = 0;
    for (const auto &face : combined->faces)
        if (!face.texture.isEmpty() && !materials.contains(face.texture)) {
            if (materials.size() >= 64) {
                messages << "Material count limit (64)";
                break;
            }
            const auto canonical = library->assetPath(face.texture);
            if (canonical.isEmpty()) {
                messages << "Missing/unsafe texture: " + face.texture;
                continue;
            }
            auto image = loadMaterial(face.texture, messages);
            if (image.isNull()) {
                messages << "Missing/unsupported texture: " + face.texture;
                continue;
            }
            if (bytes + image.sizeInBytes() > 64 * 1024 * 1024) {
                messages << "Material pixel budget (64 MiB)";
                break;
            }
            bytes += image.sizeInBytes();
            materials[face.texture] = image;
        }
    previewLimitations.removeDuplicates();
    combined->previewLimitation = previewLimitations.join("; ");
    mesh = combined;
    cube->setModel(mesh, activeAsset, materials);
    QMap<QString, std::shared_ptr<Document>> animations;
    for (const auto &path : materials.keys())
        if (materialDocuments.contains(path))
            animations[path] = materialDocuments[path];
    materialPlayback->setMaterials(animations);
    if (animations.isEmpty())
        cube->setMaterialPreview(false);
    cube->setContextTransform(QMatrix4x4());
    syncing = true;
    for (const auto &face : mesh->faces)
        faces->addItem(face.element + " · " + face.name + QString(" · %1°").arg(face.rotation));
    syncing = false;
    QStringList paths = library->dependencies(asset) + library->dependents(asset) +
                        library->dependents(activeAsset);
    for (const auto &p : contextModel.parents)
        if (!p.startsWith("minecraft:builtin/"))
            paths << mc::Location::parse(p).asset("models");
    for (const auto &p : contextModel.textures)
        paths << p;
    paths.removeDuplicates();
    paths.sort();
    for (const auto &path : paths.mid(0, 512)) {
        const auto providers = library->providers(path);
        QStringList names;
        for (const auto &provider : providers)
            names << provider.label;
        auto item = new QListWidgetItem(
            (library->assetPath(path).isEmpty() ? "Missing · " : names.value(0) + " · ") + path,
            dependencies);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path + "\nPriority: " + names.join(" > "));
    }
    messages.removeDuplicates();
    diagnostics->setText(
        QString("%1 faces · Java %2\n").arg(mesh->faces.size()).arg(library->version()) +
        messages.mid(0, 20).join('\n'));
    diagnostics->setToolTip(messages.join('\n'));
    if (mesh->faces.isEmpty()) {
        QString problem;
        for (const auto &instance : instances)
            if (const auto m = library->model(instance.model); !m.valid) {
                problem = m.id + " could not be resolved: " + m.diagnostics.value(0);
                break;
            }
        cube->setNotice("Model not shown · " +
                        (problem.isEmpty() ? QString("no supported geometry") : problem) +
                        ".\nThe texture remains editable.");
    } else if (!mesh->previewLimitation.isEmpty()) {
        cube->setNotice("Generated item preview uses a flat approximation.\n" +
                        mesh->previewLimitation + ".\nThe texture remains editable.");
    }
    overlay();
    if (mesh && previousFace >= 0 && previousFace < mesh->faces.size()) {
        cube->selectedFace = previousFace;
        QSignalBlocker block(faces);
        faces->setCurrentRow(previousFace);
        if (canvas)
            canvas->uvFace = previousFace;
    }
    updateUvControls();
    updateDisplayControls();
}
QImage ModelPanel::modelPreviewImage(const QString &asset, bool uvView, int faceIndex, QString *error,
                                     bool comparison) {
    if (target != "model:" + mc::Location::fromAsset(asset, "models") || !mesh || mesh->faces.isEmpty()) {
        *error = "Select the model in the native preview. It must have supported geometry.";
        return {};
    }
    if (mesh->faces.size() > 1536) {
        *error = "The model exceeds the preview face budget.";
        return {};
    }
    if (!uvView) {
        for (const auto &face : mesh->faces)
            if (face.texture.isEmpty() || materials.value(face.texture).isNull()) {
                *error = "The model has an unresolved or unsupported face texture.";
                return {};
            }
        auto image = cube->captureModel(comparison);
        if (image.isNull())
            *error = "The native model viewport is unavailable.";
        return image;
    }
    if (faceIndex < 0 || faceIndex >= mesh->faces.size()) {
        *error = "The face index is outside the native mesh.";
        return {};
    }
    const auto &face = mesh->faces[faceIndex];
    const auto texture = materials.value(face.texture);
    if (texture.isNull()) {
        *error = "The face has no available texture.";
        return {};
    }
    const auto size = texture.size().scaled(QSize(256, 256), Qt::KeepAspectRatio);
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.drawImage(image.rect(), texture);
    QPolygonF corners;
    for (const auto &uv : face.uv)
        corners << QPointF(uv.x() * (size.width() - 1), uv.y() * (size.height() - 1));
    if (comparison) {
        // Revision comparisons dim texels outside the face so the mapped region is clear.
        QPainterPath outside, inside;
        outside.addRect(QRectF(image.rect()));
        inside.addPolygon(corners);
        inside.closeSubpath();
        painter.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 150));
    }
    painter.setPen(QPen(QColor(80, 220, 240, 220), comparison ? 2 : 1));
    painter.drawPolygon(corners);
    painter.setPen(QPen(QColor(255, 210, 80), 2));
    painter.drawLine(corners[0], corners[0] * .75 + corners[1] * .25);
    painter.end();
    return image;
}
void ModelPanel::overlay() {
    if (!canvas)
        return;
    canvas->uvRegions.clear();
    canvas->uvVisible = uv->isChecked();
    if (mesh)
        for (int i = 0; i < mesh->faces.size(); ++i) {
            const auto &f = mesh->faces[i];
            if (f.texture != activeAsset)
                continue;
            const auto b = f.uvRect();
            const QSize size = document->image.size();
            canvas->uvRegions << Canvas::UvRegion{
                i,
                {b.x() * size.width() / 16, b.y() * size.height() / 16,
                 b.width() * size.width() / 16, b.height() * size.height() / 16},
                f.name};
        }
    canvas->update();
}
void ModelPanel::selectFace(int index) {
    if (!mesh || index < 0 || index >= mesh->faces.size())
        return;
    const auto face = mesh->faces[index];
    if (uvInteraction || (uvDocument && uvDocument->previewing()))
        finishUvEdit(false);
    pinned = true; // Face navigation must retain this model across material tabs.
    if (!modelTabActive && face.texture != activeAsset && openTexture &&
        QFileInfo::exists(root + '/' + face.texture))
        openTexture(root + '/' + face.texture);
    cube->selectedFace = index;
    cube->update();
    syncing = true;
    faces->setCurrentRow(index);
    syncing = false;
    faceInfo->setText(face.element + " · " + face.name +
                      QString("\nUV %1,%2 → %3,%4 · %5°\n")
                          .arg(face.uvRect().left())
                          .arg(face.uvRect().top())
                          .arg(face.uvRect().right())
                          .arg(face.uvRect().bottom())
                          .arg(face.rotation) +
                      face.texture);
    if (canvas && document && face.texture == activeAsset) {
        uv->setChecked(true);
        overlay();
        canvas->uvFace = index;
        const auto r = face.uvRect();
        const double sx = document->image.width() / 16., sy = document->image.height() / 16.;
        const int left = int(std::floor(r.left() * sx)), top = int(std::floor(r.top() * sy));
        QRect pixels(left, top, int(std::ceil(r.right() * sx)) - left,
                     int(std::ceil(r.bottom() * sy)) - top);
        session->select(document, ops::rectMask(document->image.size(), pixels),
                        ops::Combine::Replace, "Select model face UV");
        canvas->update();
    }
    updateUvControls();
}
void ModelPanel::selectTextureFaces() {
    if (!mesh || !canvas || !document)
        return;
    const double sx = document->image.width() / 16., sy = document->image.height() / 16.;
    auto mask = ops::emptyMask(document->image.size());
    int count = 0, overlaps = 0;
    QSet<QString> regions;
    for (const auto &face : mesh->faces)
        if (face.texture == activeAsset) {
            const auto r = face.uvRect();
            const int x = int(std::floor(r.left() * sx)), y = int(std::floor(r.top() * sy));
            QRect pixels(x, y, int(std::ceil(r.right() * sx)) - x,
                         int(std::ceil(r.bottom() * sy)) - y);
            mask = ops::combine(mask, ops::rectMask(document->image.size(), pixels),
                                ops::Combine::Add);
            const auto key =
                QString("%1,%2,%3,%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
            if (regions.contains(key))
                ++overlaps;
            regions.insert(key);
            ++count;
        }
    session->select(document, mask, ops::Combine::Replace, "Select model texture faces");
    uv->setChecked(true);
    overlay();
    canvas->update();
    faceInfo->setText(QString("%1 faces use this texture · %2 shared UV regions. Image operations "
                              "use the selected pixels.")
                          .arg(count)
                          .arg(overlaps));
}
} // namespace nova
