// SPDX-License-Identifier: GPL-3.0-or-later
#include "batchdialog.h"
#include "fabric_testing.h"
#include "minecraft_testing.h"
#include "modelpanel.h"
#include "panels.h"
#include "resourcetools.h"
#include "window.h"
#include <QtWidgets>
namespace nova {
void Window::buildResourceTools(QMenu *view) {
    problemsDock = new QDockWidget("Problems", this);
    problemsDock->setObjectName("resourceProblems");
    addDockWidget(Qt::BottomDockWidgetArea, problemsDock);
    problemsDock->hide();
    connect(problemsDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible && !resourceTools)
            showProblems(false);
    });
    view->addAction(problemsDock->toggleViewAction());
    auto menu = menuBar()->addMenu("&Resources");
    menu->setObjectName("menu-Resources");
    auto action = [&](const QString &text, const QString &name, auto callback) {
        auto a = menu->addAction(text);
        a->setObjectName(name);
        connect(a, &QAction::triggered, this, callback);
        return a;
    };
    action("Create Minecraft Asset…", "createResourceAsset", [this] { createResourceAsset(); });
    menu->addSeparator();
    action("Edit Selected Model…", "editResourceModel", [this] { modelPanel->authorModel(); });
    action("Select Pixels Used by Model Faces", "selectModelFacePixels",
           [this] { modelPanel->selectTextureFaces(); });
    action("Restore Model Snapshot…", "restoreModelSnapshot",
           [this] { modelPanel->restoreModel(); });
    menu->addSeparator();
    action("Validate Resources", "validateResources", [this] { showProblems(true); })
        ->setShortcut(QKeySequence("F8"));
    action("Process Model Textures…", "processModelTextures", [this] { modelBatch(); });
    action("Related Assets…", "relatedAssets", [this] { showRelatedAssets(); });
    action("MCP Integration…", "mcpAccess", [this] { showMcpAccess(); });
    menu->addSeparator();
    auto test = action("Test in Minecraft…", "testInMinecraft", [this] { minecraftTesting(false); });
    test->setShortcut(QKeySequence("Ctrl+Shift+M"));
    action("Sync Test Pack", "syncTestPack", [this] { minecraftTesting(true); })
        ->setShortcut(QKeySequence("Ctrl+Alt+M"));
    menu->addSeparator();
    action("Recover Partial Creation…", "recoverAssetCreation", [this] {
        auto path =
            QFileDialog::getOpenFileName(this, "Choose creation journal",
                                         project + "/.nova/creation", "Journal (manifest.json)");
        if (path.isEmpty())
            return;
        auto relative = QDir(project).relativeFilePath(QFileInfo(path).path());
        if (QMessageBox::question(this, "Recover created files",
                                  "Remove only newly created files whose content still matches "
                                  "this journal? Externally changed files are preserved.",
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        QString e;
        if (!mc::rollbackCreation(project, relative, &e))
            QMessageBox::warning(this, "Recovery stopped", e);
        else {
            fullRescan = true;
            indexDebounce->start(50);
        }
    });
}
QStringList Window::unsavedMinecraftOutputs() {
    QStringList names;
    for (const auto &doc : session.all())
        if (doc->minecraftOutputPending())
            names << QDir(project).relativeFilePath(doc->path);
    for (const auto &asset : modelPanel->openModelAssets())
        if (const auto doc = modelPanel->openModelDocument(asset); doc && doc->modified())
            names << asset;
    return names;
}
// Phase 3-D launcher hand-off (Aurora / official launcher) is dormant: kept,
// tested and reachable only with NOVA_ENABLE_LAUNCHER_HANDOFF=1. Phase 3-D.1's
// Fabric development runtime is the single active pipeline.
bool Window::launcherHandoffEnabled() {
    return qEnvironmentVariableIntValue("NOVA_ENABLE_LAUNCHER_HANDOFF") == 1;
}
void Window::minecraftTesting(bool syncOnly) {
    if (project.isEmpty()) {
        statusBar()->showMessage("Open a resource pack to test it in Minecraft", 5000);
        return;
    }
    if (launcherHandoffEnabled())
        return launcherHandoffTesting(syncOnly);
    if (!fabricTest) {
        FabricTestDialog::Context context;
        context.projectRoot = [this] { return project; };
        context.packName = [this] { return QFileInfo(project).fileName(); };
        context.unsavedDocuments = [this] { return unsavedMinecraftOutputs(); };
        context.activity = [this](const QString &message) { recordActivity(message); };
        context.preferences = &preferences;
        fabricTest = new FabricTestDialog(context, this);
    }
    if (!syncOnly) {
        fabricTest->testInMinecraft();
        fabricTest->activateWindow();
        return;
    }
    if (fabricTest->syncPack())
        statusBar()->showMessage(fabricTest->runtime()->running()
                                     ? "Test pack synced — press F3+T in Minecraft to reload"
                                     : "Test pack synced to the Fabric test environment",
                                 6000);
}
void Window::launcherHandoffTesting(bool syncOnly) {
    if (!minecraftTest) {
        MinecraftTestDialog::Context context;
        context.projectRoot = [this] { return project; };
        context.packName = [this] { return QFileInfo(project).fileName(); };
        context.unsavedDocuments = [this] { return unsavedMinecraftOutputs(); };
        context.activity = [this](const QString &message) { recordActivity(message); };
        context.rememberedTarget = &preferences.minecraftTestTarget;
        minecraftTest = new MinecraftTestDialog(context, this);
    }
    if (syncOnly && !minecraftTest->isVisible()) {
        if (minecraftTest->targets().isEmpty())
            return minecraftTesting(false);
        if (minecraftTest->syncPack())
            statusBar()->showMessage("Test pack synced — press F3+T in Minecraft to reload", 6000);
        return;
    }
    if (syncOnly) {
        minecraftTest->syncPack();
        return;
    }
    minecraftTest->rescan();
    minecraftTest->show();
    minecraftTest->raise();
    minecraftTest->activateWindow();
}
void Window::showProblems(bool validate) {
    if (!resourceTools) {
        resourceTools = new ResourceTools;
        problemsDock->setWidget(resourceTools);
        resourceTools->context = [this] {
            return ResourceContext{project,
                                   modelPanel->modelAsset().isEmpty() ? nav.selected
                                                                      : modelPanel->modelAsset(),
                                   nav.directory,
                                   collection,
                                   modelPanel->catalog(),
                                   modelPanel->documentSources()};
        };
        resourceTools->navigate = [this](const QString &path) {
            nav.selected = path;
            setDirectory(QFileInfo(path).path());
            if (path.endsWith(".png"))
                openTexture(project + '/' + path);
            else if (!mc::Location::fromAsset(path, "models").isEmpty() ||
                     !mc::Location::fromAsset(path, "items").isEmpty() ||
                     !mc::Location::fromAsset(path, "blockstates").isEmpty()) {
                openModelAsset(path);
                modelsDock->show();
                modelsDock->raise();
            } else {
                QDialog dialog(this);
                dialog.setWindowTitle(path);
                auto v = new QVBoxLayout(&dialog);
                auto text = new QPlainTextEdit;
                text->setReadOnly(true);
                QFile f(project + '/' + path);
                if (f.open(QIODevice::ReadOnly))
                    text->setPlainText(QString::fromUtf8(f.read(2 * 1024 * 1024)));
                v->addWidget(text);
                dialog.resize(650, 500);
                dialog.exec();
            }
        };
        resourceTools->changed = [this](const QStringList &paths) {
            pendingChanges += paths;
            indexDebounce->start(50);
        };
    }
    problemsDock->show();
    problemsDock->raise();
    if (validate)
        resourceTools->validate();
}
void Window::createResourceAsset() {
    ResourceContext c{project, nav.selected, nav.directory, collection, modelPanel->catalog()};
    createAssetDialog(c, this, [this](const QStringList &paths) {
        pendingChanges += paths;
        indexDebounce->start(25);
        const auto modelPath = std::find_if(paths.begin(), paths.end(), [](const QString &path) {
            return !mc::Location::fromAsset(path, "models").isEmpty();
        });
        if (modelPath != paths.end()) {
            modelPanel->catalog()->refreshLocal(paths);
            openModelAsset(*modelPath);
        }
        statusBar()->showMessage("Assets created", 6000);
    });
}
void Window::modelBatch() {
    auto library = modelPanel->catalog();
    auto asset = modelPanel->modelAsset();
    if (!library || asset.isEmpty()) {
        statusBar()->showMessage("Select a model in Models / UV first", 5000);
        return;
    }
    QStringList scopes{"Textures used by this model",
                       "Textures used by model and dependent family"};
    bool ok = false;
    auto selected = QInputDialog::getItem(
        this, "Model texture scope", "Choose explicit texture dependencies", scopes, 0, false, &ok);
    if (!ok)
        return;
    auto textures = mc::modelTextureScope(*library, asset, selected == scopes[1]);
    QStringList editable;
    for (const auto &t : textures)
        if (library->editable(t))
            editable << t;
    if (editable.isEmpty()) {
        statusBar()->showMessage("No editable local textures in this dependency scope", 5000);
        return;
    }
    BatchContext c;
    c.root = project;
    c.files = editable;
    c.fixedScopeName =
        selected == scopes[1] ? "Selected model family textures" : "Selected model textures";
    c.presets = &presets;
    c.palette = colorPanel->activePalette();
    c.paletteText = [this] { return colorPanel->activePaletteText(); };
    c.sourceGuard = [this, editable] {
        for (const auto &doc : session.all())
            if (editable.contains(QDir(project).relativeFilePath(doc->path)) &&
                (doc->modified() || doc->exportModified()))
                return QString("Save editable changes and update Minecraft PNGs before processing "
                               "this model's saved textures.");
        return QString();
    };
    BatchDialog dialog(c, this);
    dialog.setWindowTitle("Model textures · " + asset);
    dialog.setScope(batch::Scope::Pack, {});
    dialog.exec();
}
} // namespace nova
