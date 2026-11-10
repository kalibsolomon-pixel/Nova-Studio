// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 3-B workspace and interaction regression tests: task workspaces never
// touch documents, layouts persist and recover, every existing feature stays
// reachable from the menu bar (and therefore the command palette), shortcuts
// are unambiguous and destructive keys are scoped to the document area.
#include "layers.h"
#include "modelpanel.h"
#include "creation.h"
#include "imageops.h"
#include "window.h"
#include <QtTest>
#include <QtWidgets>

namespace nova {
class EditorTestAccess {
  public:
    static bool busy(Window &w) { return w.indexBusy; }
    static QDockWidget *dock(Window &w, const char *name) { return w.findChild<QDockWidget *>(name); }
    static Preferences &preferences(Window &w) { return w.preferences; }
    static ModelPanel *models(Window &w) { return w.modelPanel; }
};
} // namespace nova
using namespace nova;

namespace {
QString makePack(const QString &root) {
    QDir().mkpath(root + "/assets/nova/textures/block");
    QDir().mkpath(root + "/assets/nova/models/block");
    QFile meta(root + "/pack.mcmeta");
    if (!meta.open(QIODevice::WriteOnly))
        qFatal("Cannot create the 3-B fixture pack.mcmeta");
    meta.write(R"({"pack":{"pack_format":46,"description":"3-B"}})");
    for (int i = 0; i < 3; ++i) {
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(QColor::fromHsv(i * 90, 160, 200));
        savePng(root + QString("/assets/nova/textures/block/t%1.png").arg(i), image);
    }
    QFile model(root + "/assets/nova/models/block/m.json");
    if (!model.open(QIODevice::WriteOnly))
        qFatal("Cannot create the 3-B fixture model");
    model.write(R"({"parent":"block/cube_all","textures":{"all":"nova:block/t0"}})");
    return root;
}
class Answer : public QTimer {
  public:
    Answer(QMessageBox::StandardButton choice) {
        setInterval(20);
        connect(this, &QTimer::timeout, this, [choice] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                if (auto button = box->button(choice))
                    button->click();
        });
        start();
    }
};
// Raised panels only: Qt parks non-current tabbed docks off-screen but visible.
bool shown(QDockWidget *dock) { return dock->isVisible() && !dock->visibleRegion().isEmpty(); }
bool reachable(QMenu *menu, QAction *target) {
    for (auto a : menu->actions()) {
        if (a == target)
            return true;
        if (a->menu() && reachable(a->menu(), target))
            return true;
    }
    return false;
}
bool inMenuBar(QMainWindow &window, QAction *target) {
    for (auto a : window.menuBar()->actions())
        if (a == target || (a->menu() && reachable(a->menu(), target)))
            return true;
    return false;
}
} // namespace

class Phase3BTests : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    std::unique_ptr<Window> open(const QString &settings, const QString &project = {}) {
        // Each test gets its own settings folder (and therefore its own recovery store).
        qApp->setProperty("settingsFile", temporary.filePath(QFileInfo(settings).baseName() + "/" + settings));
        QElapsedTimer timer;
        timer.start();
        auto window = std::make_unique<Window>(project, QString(), timer);
        window->resize(1280, 800);
        window->show();
        if (!QTest::qWaitForWindowExposed(window.get()))
            return {};
        if (!project.isEmpty()) {
            QElapsedTimer wait;
            wait.start();
            while ((EditorTestAccess::busy(*window) || !window->activeCanvas()) && wait.elapsed() < 10000)
                QTest::qWait(20);
            QTest::qWait(30); // initial fit-to-view runs on the next event-loop turn
        }
        return window;
    }

  private slots:
    void teardownWithAnimationDoesNotRefreshDestroyedViews() {
        const auto pack = makePack(temporary.filePath("teardown"));
        auto window = open("teardown.ini", pack);
        QVERIFY(window);
        window->findChild<QAction *>("convertAnimation")->trigger();
        QVERIFY(window->activeCanvas()->document->animated());
        // Exercise direct QObject ownership teardown (also used on exceptional
        // exits), while an animation tab/timeline still owns document views.
        window.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void modelAndUvSourcesSurviveWorkspaceSwitches() {
        const auto pack = makePack(temporary.filePath("model-workspaces"));
        const auto asset = QString("assets/nova/models/block/m.json");
        auto source = mc::modelTemplate(mc::ModelTemplate::MultiElement, "nova:block/t0");
        source["unknown_root"] = QJsonObject{{"retained", 73}};
        QFile file(pack + '/' + asset);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(source).toJson()); file.close();
        const auto diskRevision = Revision::read(file.fileName());
        auto window = open("model-workspaces.ini", pack);
        QVERIFY(window);
        auto panel = EditorTestAccess::models(*window);
        panel->openAsset(asset);
        panel->authorModel();
        auto author = window->findChild<QDialog *>("modelAuthorDialog");
        QVERIFY(author);
        auto fromX = author->findChild<QDoubleSpinBox *>("authorNumber0");
        QVERIFY(fromX); fromX->setValue(fromX->value() + .25);
        auto directions = author->findChild<QListWidget *>("authorDirections");
        QVERIFY(directions); directions->setCurrentRow(0);
        author->findChild<QDoubleSpinBox *>("authorBulkOffsetU")->setValue(.5);
        author->findChild<QPushButton *>("authorBulkOffset")->click();
        author->findChild<QPushButton *>("authorApply")->click();
        const auto sources = panel->documentSources();
        QVERIFY(!sources.isEmpty());
        QCOMPARE(sources.value(asset)["unknown_root"], source["unknown_root"]);
        const auto pixels = window->activeCanvas()->document->image;
        const auto target = panel->selectedTarget();
        for (const auto &name : {"texture", "animation", "model", "uv", "essentials"}) {
            window->findChild<QAction *>(QString("workspacePreset-") + name)->trigger();
            QApplication::processEvents();
            QCOMPARE(panel->documentSources(), sources);
            QCOMPARE(panel->selectedTarget(), target);
            QCOMPARE(window->activeCanvas()->document->image, pixels);
            QVERIFY(diskRevision.sameContent(Revision::read(file.fileName())));
        }
        author->findChild<QPushButton *>("authorUndo")->click();
        QVERIFY(panel->documentSources() != sources);
        author->findChild<QPushButton *>("authorRedo")->click();
        QCOMPARE(panel->documentSources(), sources);
        Answer discard(QMessageBox::Discard);
        QVERIFY(window->close());
    }
    void presetsArrangePanelsWithoutChangingDocuments() {
        const auto pack = makePack(temporary.filePath("presets"));
        auto window = open("presets.ini", pack);
        QVERIFY(window);
        auto doc = window->activeCanvas()->document;
        doc->draw({1, 1}, {3, 3}, qRgba(1, 2, 3, 255));
        doc->finish();
        const auto pixels = doc->image;
        const auto operations = doc->historyOperations();
        const bool modified = doc->modified();
        auto box = window->findChild<QComboBox *>("workspacePresetBox");
        QVERIFY(box);
        QCOMPARE(box->count(), 5);
        auto preview = EditorTestAccess::dock(*window, "preview");
        for (const auto &name : {"texture", "animation", "model", "uv", "essentials"}) {
            QElapsedTimer switching;
            switching.start();
            window->findChild<QAction *>(QString("workspacePreset-") + name)->trigger();
            const double arranged = switching.nsecsElapsed() / 1e6;
            QApplication::processEvents(); // deferred relayout and repaint of the new arrangement
            qInfo() << "workspace switch" << name << "arranged" << arranged << "ms; laid out and painted"
                    << switching.nsecsElapsed() / 1e6 << "ms";
            QTest::qWait(30);
            QCOMPARE(window->workspacePreset(), QString(name));
            QCOMPARE(box->currentData().toString(), QString(name));
            QCOMPARE(doc->image, pixels);
            QCOMPARE(doc->historyOperations(), operations);
            QCOMPARE(doc->modified(), modified);
            QVERIFY(!doc->animated()); // switching never converts or edits
            QVERIFY(!EditorTestAccess::dock(*window, "animationDock")->isVisible());
            if (QString(name) == "texture" || QString(name) == "uv")
                QCOMPARE(window->dockWidgetArea(preview), Qt::RightDockWidgetArea);
            if (QString(name) == "essentials" || QString(name) == "model")
                QCOMPARE(window->dockWidgetArea(preview), Qt::BottomDockWidgetArea);
            if (QString(name) == "texture")
                QVERIFY(shown(EditorTestAccess::dock(*window, "colors")));
            if (QString(name) == "model" || QString(name) == "uv")
                QVERIFY(shown(EditorTestAccess::dock(*window, "modelsUv")));
            for (auto dock : window->findChildren<QDockWidget *>())
                if (shown(dock) && !dock->isFloating())
                    QVERIFY2(window->rect().contains(QRect(dock->mapTo(window.get(), QPoint()), dock->size())),
                             qPrintable(QString("%1 %2: %3,%4 %5x%6 in %7x%8").arg(name, dock->objectName())
                                            .arg(dock->mapTo(window.get(), QPoint()).x()).arg(dock->mapTo(window.get(), QPoint()).y())
                                            .arg(dock->width()).arg(dock->height()).arg(window->width()).arg(window->height())));
        }
        // The combo box drives the same presets.
        box->setCurrentIndex(box->findData("texture"));
        emit box->activated(box->currentIndex());
        QCOMPARE(window->workspacePreset(), QString("texture"));
        // Animation documents keep their timeline across workspace switches.
        window->findChild<QAction *>("convertAnimation")->trigger();
        QTRY_VERIFY(doc->animated());
        const auto frames = doc->animationState().frames.size();
        window->findChild<QAction *>("workspacePreset-animation")->trigger();
        QTRY_VERIFY(EditorTestAccess::dock(*window, "animationDock")->isVisible());
        window->findChild<QAction *>("workspacePreset-model")->trigger();
        QTRY_VERIFY(EditorTestAccess::dock(*window, "animationDock")->isVisible());
        QCOMPARE(doc->animationState().frames.size(), frames);
        QVERIFY(doc->editFrame(FrameCommand::Duplicate));
        QVERIFY(doc->editFrame(FrameCommand::Duplicate));
        const auto animation = doc->animationState();
        const auto animationHistory = doc->historyOperations(), animationCursor = doc->historyCursor();
        const auto identity = doc->id();
        const bool animationModified = doc->modified();
        for (const auto &name : {"essentials", "texture", "animation", "model", "uv"}) {
            window->findChild<QAction *>(QString("workspacePreset-") + name)->trigger();
            QApplication::processEvents();
            QCOMPARE(window->activeCanvas()->document->id(), identity);
            const auto after = doc->animationState();
            QCOMPARE(after.active, animation.active);
            QCOMPARE(after.defaultTicks, animation.defaultTicks);
            QCOMPARE(after.interpolate, animation.interpolate);
            QCOMPARE(after.metadata, animation.metadata);
            QCOMPARE(after.target, animation.target);
            QCOMPARE(after.entries.size(), animation.entries.size());
            for (size_t i = 0; i < after.entries.size(); ++i) {
                QCOMPARE(after.entries[i].frame, animation.entries[i].frame);
                QCOMPARE(after.entries[i].ticks, animation.entries[i].ticks);
            }
            QCOMPARE(after.frames.size(), animation.frames.size());
            for (size_t i = 0; i < after.frames.size(); ++i) {
                const auto &a = after.frames[i], &b = animation.frames[i];
                QCOMPARE(a.id, b.id); QCOMPARE(a.name, b.name); QCOMPARE(a.activeLayer, b.activeLayer);
                QCOMPARE(a.layers.size(), b.layers.size());
                for (size_t j = 0; j < a.layers.size(); ++j) {
                    QCOMPARE(a.layers[j].id, b.layers[j].id);
                    QCOMPARE(a.layers[j].name, b.layers[j].name);
                    QCOMPARE(a.layers[j].pixels, b.layers[j].pixels);
                    QCOMPARE(a.layers[j].visible, b.layers[j].visible);
                    QCOMPARE(a.layers[j].opacity, b.layers[j].opacity);
                    QCOMPARE(a.layers[j].extensions, b.layers[j].extensions);
                }
            }
            QCOMPARE(doc->historyOperations(), animationHistory);
            QCOMPARE(doc->historyCursor(), animationCursor);
            QCOMPARE(doc->modified(), animationModified);
        }
        Answer discard(QMessageBox::Discard);
        QVERIFY(window->close());
    }
    void presetAndLayoutPersistAndRecover() {
        {
            auto window = open("persist.ini");
            QVERIFY(window);
            window->findChild<QAction *>("workspacePreset-model")->trigger();
            QVERIFY(window->close());
        }
        Settings settings(temporary.filePath("persist/persist.ini"));
        QCOMPARE(settings.load().workspacePreset, QString("model"));
        {
            auto window = open("persist.ini");
            QCOMPARE(window->workspacePreset(), QString("model"));
            QCOMPARE(window->findChild<QComboBox *>("workspacePresetBox")->currentData().toString(), QString("model"));
            QVERIFY(window->findChild<QAction *>("workspacePreset-model")->isChecked());
            QVERIFY(window->close());
        }
        // Unknown preset names and corrupt layouts fall back safely.
        QSettings raw(temporary.filePath("persist/persist.ini"), QSettings::IniFormat);
        raw.setValue("workspace/preset", "not-a-workspace");
        raw.setValue("workspace/layout", QByteArray("corrupt"));
        raw.setValue("workspace/geometry", QByteArray("corrupt"));
        raw.sync();
        auto window = open("persist.ini");
        QCOMPARE(window->workspacePreset(), QString("essentials"));
        QCOMPARE(window->dockWidgetArea(EditorTestAccess::dock(*window, "preview")), Qt::BottomDockWidgetArea);
        // Reset restores Essentials and every panel.
        EditorTestAccess::dock(*window, "assets")->hide();
        EditorTestAccess::dock(*window, "layers")->setFloating(true);
        window->findChild<QAction *>("resetWorkspaceCommand")->trigger();
        QVERIFY(EditorTestAccess::dock(*window, "assets")->isVisible());
        QVERIFY(!EditorTestAccess::dock(*window, "layers")->isFloating());
        QCOMPARE(window->workspacePreset(), QString("essentials"));
        QVERIFY(window->close());
    }
    void everyFeatureIsReachableFromTheMenuBar() {
        auto window = open("reach.ini");
        QStringList names = {
            "newProjectCommand", "openProjectCommand", "openTextureCommand", "openMinecraftModel",
            "closeProjectCommand", "saveCommand", "saveAsCommand", "saveNativeCommand", "exportPngCommand",
            "closeDocumentCommand", "exportPackCommand", "projectPropertiesCommand", "reviewConflictCommand",
            "exitCommand", "editUndo", "editRedo", "editCut", "editCopy", "editPaste", "editPasteLayer",
            "editDuplicateSelection", "editFill", "editClear", "defineStamp", "stampLibrary", "numericShape",
            "swapColors", "defaultColors", "brushSmaller", "brushLarger", "preferencesCommand", "selectAll",
            "selectNone", "selectInvert", "imageFlipH", "imageFlipV", "imageRotateCW", "imageRotate180",
            "imageRotateCCW", "imageResize", "imageCanvas", "imageCropSelection", "imageCrop", "imageTrim",
            "imageFlatten", "imageTilePreview", "layerAdd", "layerDuplicate", "layerDelete", "layerRename",
            "layerRaise", "layerLower", "layerTransformSelected", "layerFlipH", "layerFlipV", "layerRotate90",
            "layerRotate180", "layerRotate", "layerScale", "layerOffset", "layerCentre", "layerAlign",
            "layerMergeDown", "generatorBrowser", "newAnimation", "convertAnimation", "importAnimation",
            "playAnimation", "stopAnimation", "previousFrame", "nextFrame", "firstFrame", "lastFrame",
            "addAnimationFrame", "duplicateAnimationFrame", "toggleOnion", "animationMetadata", "animationScope",
            "exportAnimation", "recoverAnimationExport", "animationLimits", "flattenAnimation",
            "createResourceAsset", "editResourceModel", "restoreModelSnapshot", "validateResources",
            "processModelTextures", "selectModelFacePixels", "recoverAssetCreation", "batchProcessorCommand",
            "saveLayoutCommand", "saveWorkspaceCommand", "resetWorkspaceCommand", "zoomInCommand",
            "zoomOutCommand", "fitCanvasCommand", "actualPixelsCommand", "centerCanvasCommand",
            "pixelGridCommand", "darkThemeCommand", "accent-violet", "accent-blue", "commandPaletteCommand",
            "gettingStartedCommand", "keyboardShortcutsCommand", "exportDiagnosticsCommand", "aboutCommand"};
        for (const auto &info : toolInfos())
            names << "tool-" + info.id;
        // Effects-menu families; other registered operations (fill, flip, resize, crop, …)
        // are reached through their dedicated Image/Layer/Edit commands and tools.
        for (const auto &info : ops::registry())
            if (QStringList{"Adjust", "Filters", "Shading", "Generate"}.contains(info.category))
                names << "op-" + info.id;
        for (const auto &p : {"essentials", "texture", "animation", "model", "uv"})
            names << QString("workspacePreset-") + p;
        QStringList missing;
        for (const auto &name : names) {
            auto action = window->findChild<QAction *>(name);
            if (!action || !inMenuBar(*window, action))
                missing << name;
        }
        QVERIFY2(missing.isEmpty(), qPrintable("Not reachable from the menu bar: " + missing.join(", ")));
        for (auto dock : window->findChildren<QDockWidget *>())
            QVERIFY2(inMenuBar(*window, dock->toggleViewAction()), qPrintable(dock->objectName()));
        // Standard menu order and unique mnemonics.
        QStringList titles, mnemonics;
        for (auto a : window->menuBar()->actions()) {
            titles << a->text().remove('&');
            const int at = a->text().indexOf('&');
            QVERIFY2(at >= 0, qPrintable(a->text()));
            mnemonics << a->text().mid(at + 1, 1).toLower();
        }
        QCOMPARE(titles, (QStringList{"File", "Edit", "Image", "Layer", "Select", "Effects", "Animation",
                                      "Resources", "View", "Help"}));
        QCOMPARE(QSet<QString>(mnemonics.begin(), mnemonics.end()).size(), mnemonics.size());
        QVERIFY(window->close());
    }
    void shortcutsAreUnambiguous() {
        auto window = open("shortcuts.ini");
        QHash<QKeySequence, QStringList> owners;
        for (auto action : window->findChildren<QAction *>())
            for (const auto &key : action->shortcuts())
                if (!key.isEmpty() && action->shortcutContext() != Qt::WidgetShortcut)
                    owners[key] << (action->objectName().isEmpty() ? action->text() : action->objectName());
        QStringList conflicts;
        for (auto it = owners.begin(); it != owners.end(); ++it)
            if (it.value().size() > 1)
                conflicts << it.key().toString() + " → " + it.value().join(" / ");
        QVERIFY2(conflicts.isEmpty(), qPrintable(conflicts.join("; ")));
        // The generated reference lists the same commands and reports no conflicts.
        window->findChild<QAction *>("keyboardShortcutsCommand")->trigger();
        auto dialog = window->findChild<QDialog *>("keyboardShortcutsDialog");
        QVERIFY(dialog && dialog->isVisible() && !dialog->isModal());
        auto tree = dialog->findChild<QTreeWidget *>("shortcutTree");
        QVERIFY(!tree->findItems("Fit on Screen", Qt::MatchRecursive).isEmpty());
        QVERIFY(!dialog->findChild<QLabel *>("shortcutNote")->text().startsWith("Shared"));
        auto filter = dialog->findChild<QLineEdit *>("shortcutFilter");
        filter->setText("Ctrl+Shift+P");
        int shown = 0;
        for (auto item : tree->findItems("*", Qt::MatchWildcard | Qt::MatchRecursive))
            shown += item->parent() && !item->isHidden();
        QCOMPARE(shown, 1);
        dialog->close();
        QVERIFY(window->close());
    }
    void destructiveKeysNeedDocumentFocus() {
        const auto pack = makePack(temporary.filePath("focus"));
        auto window = open("focus.ini", pack);
        auto canvas = window->activeCanvas();
        const auto pixels = canvas->document->image;
        window->findChild<QAction *>("selectAll")->trigger(); // Clear needs a selection
        QVERIFY(canvas->document->hasSelection());
        auto tree = window->findChild<QTreeView *>("assetTree");
        tree->setFocus();
        QTRY_VERIFY(tree->hasFocus());
        QTest::keyClick(tree, Qt::Key_Delete);
        QTest::keyClick(tree, Qt::Key_Backspace, Qt::AltModifier);
        QCOMPARE(canvas->document->image, pixels);
        auto search = window->findChild<QLineEdit *>("assetSearch");
        search->setFocus();
        QTest::keyClicks(search, "be");    // tool letters type text
        QTest::keyClick(search, Qt::Key_A, Qt::ControlModifier); // select-all text, not pixels
        QCOMPARE(search->text(), QString("be"));
        QCOMPARE(search->selectedText(), QString("be"));
        QCOMPARE(ops::maskBounds(canvas->document->selection()), QRect(0, 0, 16, 16)); // unchanged
        QCOMPARE(window->tools().tool, Tool::Pencil);
        search->clear();
        canvas->setFocus();
        QTRY_VERIFY(canvas->hasFocus());
        QTest::keyClick(canvas, Qt::Key_Delete);
        QVERIFY(canvas->document->image != pixels); // Clear applies in the document area
        window->findChild<QAction *>("editUndo")->trigger();
        QCOMPARE(canvas->document->image, pixels);
        Answer discard(QMessageBox::Discard);
        QVERIFY(window->close());
    }
    void zoomLayerAndActivityCommands() {
        const auto pack = makePack(temporary.filePath("commands"));
        auto window = open("commands.ini", pack);
        auto canvas = window->activeCanvas();
        window->findChild<QAction *>("fitCanvasCommand")->trigger();
        const double fit = canvas->zoom;
        QVERIFY(16 * fit <= canvas->width() && 16 * fit <= canvas->height());
        window->findChild<QAction *>("zoomInCommand")->trigger();
        QVERIFY(canvas->zoom > fit);
        window->findChild<QAction *>("zoomOutCommand")->trigger();
        window->findChild<QAction *>("zoomOutCommand")->trigger();
        QVERIFY(canvas->zoom < fit);
        window->findChild<QAction *>("actualPixelsCommand")->trigger();
        QCOMPARE(canvas->zoom, 1.0);
        QCOMPARE(window->findChild<QLabel *>("inspectorZoom")->text(), QString("100%"));
        // Layer menu reuses the panel commands and history.
        auto doc = canvas->document;
        window->findChild<QAction *>("layerAdd")->trigger();
        QCOMPARE(doc->layers().size(), size_t(2));
        const auto top = doc->activeLayer();
        window->findChild<QAction *>("layerLower")->trigger();
        QCOMPARE(doc->layers().front().id, top);
        window->findChild<QAction *>("layerRaise")->trigger();
        QCOMPARE(doc->layers().back().id, top);
        window->findChild<QAction *>("layerDelete")->trigger();
        QCOMPARE(doc->layers().size(), size_t(1));
        window->findChild<QAction *>("editUndo")->trigger();
        QCOMPARE(doc->layers().size(), size_t(2));
        QCOMPARE(window->findChild<QLabel *>("inspector-layers")->text(), QString("2"));
        // Transient messages stay reviewable in the activity log.
        window->statusBar()->showMessage("Phase 3-B activity check", 10);
        QTest::qWait(40);
        auto activity = window->findChild<QToolButton *>("activityButton");
        QVERIFY(!activity->text().isEmpty());
        auto menu = window->findChild<QMenu *>("activityMenu");
        emit menu->aboutToShow();
        bool found = false;
        for (auto a : menu->actions())
            found |= a->text().contains("Phase 3-B activity check");
        QVERIFY(found);
        QVERIFY(activity->text().isEmpty());
        Answer discard(QMessageBox::Discard);
        QVERIFY(window->close());
    }
    void preferencesApplyAndPersist() {
        {
            auto window = open("preferences.ini");
            QTimer::singleShot(150, window.get(), [] {
                auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog && dialog->objectName() == "preferencesDialog");
                dialog->findChild<QComboBox *>("preferenceTheme")->setCurrentIndex(1);
                dialog->findChild<QComboBox *>("preferenceAccent")->setCurrentIndex(1);
                dialog->findChild<QSpinBox *>("preferenceHistory")->setValue(8);
                dialog->findChild<QCheckBox *>("preferenceReopen")->setChecked(false);
                dialog->findChild<QCheckBox *>("preferenceTips")->setChecked(false);
                dialog->findChild<QCheckBox *>("preferencePixelGrid")->setChecked(false);
                dialog->accept();
            });
            window->findChild<QAction *>("preferencesCommand")->trigger();
            QVERIFY(!window->findChild<QAction *>("darkThemeCommand")->isChecked());
            QVERIFY(window->findChild<QAction *>("accent-blue")->isChecked());
            QVERIFY(!window->findChild<QCheckBox *>("pixelGrid")->isChecked());
            QVERIFY(!window->findChild<QAction *>("pixelGridCommand")->isChecked());
            QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#f0f2f6"));
            QVERIFY(window->close());
        }
        const auto saved = Settings(temporary.filePath("preferences/preferences.ini")).load();
        QVERIFY(!saved.dark);
        QCOMPARE(saved.accent, QString("blue"));
        QCOMPARE(saved.historyMiB, 8);
        QVERIFY(!saved.reopenLast);
        QVERIFY(!saved.showTips);
        QVERIFY(!saved.pixelGrid);
        // Placeholder text stays legible in the light theme (a baseline defect).
        const auto placeholder = qApp->palette().color(QPalette::PlaceholderText);
        const auto base = qApp->palette().color(QPalette::Base);
        QVERIFY(qAbs(placeholder.lightness() - base.lightness()) > 80);
        auto window = open("preferences.ini");
        QVERIFY(!window->findChild<QAction *>("darkThemeCommand")->isChecked());
        window->findChild<QAction *>("darkThemeCommand")->setChecked(true);
        QVERIFY(window->close());
    }
    void compactWindowKeepsWorkspaceUsable() {
        const auto pack = makePack(temporary.filePath("compact"));
        auto window = open("compact.ini", pack);
        window->resize(window->minimumSize());
        QTest::qWait(60);
        QCOMPARE(window->size(), QSize(800, 550));
        for (const auto &name : {"essentials", "texture", "animation", "model", "uv"}) {
            qInfo() << "compact preset" << name;
            window->findChild<QAction *>(QString("workspacePreset-") + name)->trigger();
            QTest::qWait(30);
            auto central = window->centralWidget();
            QVERIFY2(central->width() >= 200 && central->height() >= 150,
                     qPrintable(QString("%1: %2x%3").arg(name).arg(central->width()).arg(central->height())));
            for (auto dock : window->findChildren<QDockWidget *>())
                if (shown(dock) && !dock->isFloating())
                    QVERIFY2(dock->width() > 40 && dock->height() > 40, qPrintable(dock->objectName()));
            // Toolbars overflow into extension menus/scroll rather than clipping off-window.
            for (auto bar : window->findChildren<QToolBar *>())
                if (bar->isVisible())
                    QVERIFY(window->rect().contains(QRect(bar->mapTo(window.get(), QPoint()), bar->size())));
        }
        QVERIFY(window->close());
    }
};
QTEST_MAIN(Phase3BTests)
#include "phase3b_test.moc"
