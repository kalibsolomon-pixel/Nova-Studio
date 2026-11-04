// SPDX-License-Identifier: GPL-3.0-or-later
// Window integration for the Phase 2-C3 editor: tool box, context options,
// colour/history docks, editing menus, clipboard, command palette and canvas
// context menu. Every action reaches pixels through DocumentSession commands.
#include "batchdialog.h"
#include "opdialog.h"
#include "panels.h"
#include "theme.h"
#include "window.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QtWidgets>

namespace nova {
namespace {
QJsonObject brushJson(const ops::Brush &b) {
    return {{"shape", int(b.shape)}, {"size", b.size}, {"opacity", b.opacity}, {"hardness", b.hardness},
            {"spacing", b.spacing}};
}
} // namespace

void Window::configureCanvas(Canvas *canvas) {
    canvas->session = &session;
    canvas->transformLayers = [this] { return layersPanel->selectedLayers(); };
    canvas->error = [this](const QString &message) { statusBar()->showMessage(message, 5000); };
    canvas->contextMenu = [this](QPoint global) { canvasMenu(global); };
    ui::afterEvents(canvas, {QEvent::Wheel}, [this, canvas] {
        if (zoomLabel && current() == canvas)
            zoomLabel->setText(QString("%1%").arg(qRound(canvas->zoom * 100)));
    });
}
QMenu *Window::operationMenu(const QString &title, const QString &category) {
    auto menu = menuBar()->addMenu(title);
    menu->setObjectName("menu-" + category);
    for (const auto &info : ops::registry())
        if (info.category == category) {
            auto a = menu->addAction(info.name + "…");
            a->setObjectName("op-" + info.id);
            a->setStatusTip(info.description);
            connect(a, &QAction::triggered, this, [this, id = info.id] { openOperation({id}); });
        }
    return menu;
}
void Window::openOperation(const QStringList &ids) {
    auto canvas = current();
    if (!canvas) {
        statusBar()->showMessage("Open a texture first", 4000);
        return;
    }
    if (canvas->document->animated() && !ids.isEmpty() && ops::find(ids[0]) && ops::find(ids[0])->changesSize) {
        processAnimationFrames(ids[0]);
        return;
    }
    canvas->commitFloating();
    OperationDialog dialog(&session, canvas->document, canvas, &presets, colorPanel->activePalette(), ids,
                           [this] { return colorPanel->activePaletteText(); }, this);
    dialog.setSelectedLayers(layersPanel->selectedLayers(), layersPanel->selectedLayers().size()>1);
    dialog.exec();
    canvas->clearExternalPreview();
    refresh();
}
void Window::documentOperation(const ops::Spec &spec) {
    auto canvas = current();
    if (!canvas)
        return;
    if (canvas->document->animated() && ops::find(spec.id) && ops::find(spec.id)->changesSize) {
        processAnimationFrames(spec.id);
        return;
    }
    canvas->commitFloating();
    QString error;
    if (!session.apply(canvas->document, canvas->document->activeLayer(), spec, &error))
        statusBar()->showMessage(error, 6000);
    refresh();
}
void Window::layerOperation(const ops::Spec &spec) { documentOperation(spec); }
void Window::copySelection(bool cut) {
    auto canvas = current();
    if (!canvas)
        return;
    canvas->commitFloating();
    auto doc = canvas->document;
    const auto pixels = doc->selectedPixels(doc->activeLayer());
    if (pixels.isNull())
        return;
    const QRect bounds = doc->hasSelection() ? ops::maskBounds(doc->selection()) : pixels.rect();
    if (bounds.isEmpty()) {
        statusBar()->showMessage("The selection contains no pixels to copy", 4000);
        return;
    }
    clip.image = pixels.copy(bounds);
    clip.origin = bounds.topLeft();
    // System clipboard receives PNG data with alpha; the internal copy keeps the origin.
    auto mime = new QMimeData;
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    clip.image.save(&buffer, "PNG");
    mime->setData("image/png", png);
    mime->setImageData(clip.image);
    QApplication::clipboard()->setMimeData(mime);
    clip.key = QApplication::clipboard()->image().cacheKey();
    if (cut) {
        QString error;
        if (!session.clear(doc, doc->activeLayer(), &error))
            statusBar()->showMessage(error, 5000);
    }
    statusBar()->showMessage(QString("%1 %2×%3 pixels").arg(cut ? "Cut" : "Copied").arg(clip.image.width()).arg(clip.image.height()), 3000);
}
void Window::paste(bool asLayer) {
    auto canvas = current();
    if (!canvas)
        return;
    canvas->commitFloating();
    QImage image = clip.image;
    QPoint origin = clip.origin;
    const auto mime = QApplication::clipboard()->mimeData();
    if (mime && mime->hasFormat("image/png")) {
        QImage system;
        system.loadFromData(mime->data("image/png"), "PNG");
        if (!system.isNull() && (clip.image.isNull() || system.convertToFormat(QImage::Format_RGBA8888) !=
                                                            clip.image.convertToFormat(QImage::Format_RGBA8888))) {
            image = system; // copied elsewhere: place at the top-left
            origin = {};
        }
    } else if (mime && mime->hasImage() && clip.image.isNull()) {
        image = qvariant_cast<QImage>(mime->imageData());
        origin = {};
    }
    if (image.isNull()) {
        statusBar()->showMessage("The clipboard has no image", 3000);
        return;
    }
    image = image.convertToFormat(QImage::Format_RGBA8888);
    auto doc = canvas->document;
    if (image.width() > doc->image.width() || image.height() > doc->image.height())
        statusBar()->showMessage("Pasted image is larger than the canvas; pixels outside are clipped on commit", 6000);
    origin.setX(std::clamp(origin.x(), 0, std::max(0, doc->image.width() - image.width())));
    origin.setY(std::clamp(origin.y(), 0, std::max(0, doc->image.height() - image.height())));
    if (asLayer) {
        QImage placed(doc->image.size(), QImage::Format_RGBA8888);
        placed.fill(Qt::transparent);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (placed.rect().contains(origin + QPoint(x, y)))
                    placed.setPixel(origin + QPoint(x, y), image.pixel(x, y));
        QString error;
        if (!session.addLayer(doc, "Pasted", placed, {}, &error))
            statusBar()->showMessage(error, 5000);
        return;
    }
    toolState.setTool(Tool::Move);
    canvas->beginFloating(image, origin, {}, "Paste");
    statusBar()->showMessage("Drag or use arrow keys to place · Enter applies · Esc cancels", 6000);
}
void Window::duplicateSelection() {
    auto canvas = current();
    if (!canvas)
        return;
    canvas->commitFloating();
    auto doc = canvas->document;
    if (doc->hasSelection() && ops::maskEmpty(doc->selection())) {
        statusBar()->showMessage("The selection contains no pixels to duplicate", 4000);
        return;
    }
    QString error;
    if (!session.addLayer(doc, "Selection copy", doc->selectedPixels(doc->activeLayer()), {}, &error))
        statusBar()->showMessage(error, 5000);
}
void Window::batchDialog() {
    if (project.isEmpty()) {
        statusBar()->showMessage("Open a resource pack to batch process its textures", 5000);
        return;
    }
    BatchContext context;
    context.root = project;
    context.files = collection.files;
    context.selected = nav.selected.endsWith(".png", Qt::CaseInsensitive) ? nav.selected : QString();
    context.directory = nav.directory;
    context.query = nav.query;
    context.presets = &presets;
    context.palette = colorPanel->activePalette();
    context.paletteText = [this] { return colorPanel->activePaletteText(); };
    context.openDocuments = [this] {
        QStringList open;
        for (const auto &doc : session.all())
            if (doc->modified() || doc->exportModified())
                open << QFileInfo(doc->path).absoluteFilePath();
        return open;
    };
    BatchDialog dialog(context, this);
    connect(&dialog, &BatchDialog::finished, this, [this](const batch::Report &report) {
        statusBar()->showMessage(QString("Batch finished: %1 done, %2 failed").arg(report.done).arg(report.failed), 8000);
    });
    dialog.exec();
}
void Window::canvasMenu(QPoint global) {
    QMenu menu(this);
    for (const char *name : std::initializer_list<const char *>{"editUndo", "editRedo", nullptr, "editCut", "editCopy", "editPaste", "editPasteLayer",
                             nullptr, "selectAll", "selectNone", "selectInvert", nullptr, "editFill", "editClear",
                             "imageCropSelection", nullptr, "tool-transform"}) {
        if (!name) {
            menu.addSeparator();
            continue;
        }
        if (auto a = findChild<QAction *>(name))
            menu.addAction(a);
    }
    menu.exec(global);
}
void Window::updateLayouts() {
    if (!layoutsMenu)
        return;
    layoutsMenu->clear();
    auto save = layoutsMenu->addAction("Save Layout As…");
    save->setObjectName("saveLayoutCommand");
    connect(save, &QAction::triggered, this, [this] {
        bool ok = false;
        const auto name = QInputDialog::getText(this, "Save workspace layout", "Layout name", QLineEdit::Normal, "My layout", &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        preferences.layouts.insert(name.left(64), saveState(1));
        persist();
        updateLayouts();
    });
    if (!preferences.layouts.isEmpty())
        layoutsMenu->addSeparator();
    for (auto it = preferences.layouts.begin(); it != preferences.layouts.end(); ++it) {
        auto sub = layoutsMenu->addMenu(it.key());
        sub->addAction("Apply", this, [this, name = it.key()] {
            if (!restoreState(preferences.layouts.value(name), 1))
                statusBar()->showMessage("Layout could not be restored", 4000);
            keepOnScreen();
        });
        sub->addAction("Delete", this, [this, name = it.key()] {
            preferences.layouts.remove(name);
            persist();
            updateLayouts();
        });
    }
    layoutsMenu->addSeparator();
    layoutsMenu->addAction("Restore Default Layout", this, &Window::resetWorkspace);
}
void Window::editorRefresh() {
    auto canvas = current();
    if (historyPanel)
        historyPanel->setDocument(canvas ? canvas->document : nullptr);
    const bool open = canvas != nullptr;
    const bool selection = open && canvas->document->hasSelection();
    for (const char *name : {"editCut", "editCopy", "editClear", "selectNone", "selectInvert", "imageCropSelection",
                             "editDuplicateSelection"})
        if (auto a = findChild<QAction *>(name))
            a->setEnabled(selection || (open && (QString(name) == "editCopy" || QString(name) == "editCut")));
    for (const char *menuName : {"menu-Adjust", "menu-Filters", "menu-Transform", "menu-Image", "menu-Shading", "menu-Generate"})
        if (auto m = findChild<QMenu *>(menuName))
            m->setEnabled(open);
}
void Window::buildEditor(QMenu *view) {
    QElapsedTimer stage;
    stage.start();
    const auto dir = QFileInfo(settings.fileName()).absolutePath();
    palettes = PaletteStore(dir + "/palettes.json");
    presets = PresetStore(dir + "/presets.json");
    QString loadError;
    if (!palettes.load(&loadError) || !presets.load(&loadError))
        statusBar()->showMessage(loadError, 8000);
    // Restore tool settings (colours, brushes, matching options).
    toolState.recent = preferences.recentColors;
    if (!preferences.toolState.isEmpty()) {
        const auto o = QJsonDocument::fromJson(preferences.toolState).object();
        if (o.value("schema").toInt() == 1) {
            toolState.foreground = ops::parseColor(o.value("foreground").toString("#FF828DFF"));
            toolState.background = ops::parseColor(o.value("background").toString("#FFFFFFFF"));
            toolState.tolerance = std::clamp(o.value("tolerance").toInt(), 0, 255);
            toolState.smoothShape = o.value("smooth_shapes").toBool(false);
            toolState.symmetry = ops::Symmetry(std::clamp(o.value("symmetry").toInt(), 0, 4));
            const auto brushes = o.value("brushes").toObject();
            for (const auto &info : toolInfos())
                if (brushes.contains(info.id))
                    toolState.applyBrushPreset(info.tool, brushes.value(info.id).toObject());
            toolState.symmetry = ops::Symmetry(std::clamp(o.value("symmetry").toInt(), 0, 4));
            toolState.smoothing = 0;
        }
    }
    metrics.record("editor_settings_libraries", stage);
    stage.restart();
    toolBox = new ToolBox(&toolState, this);
    addToolBar(Qt::LeftToolBarArea, toolBox);
    connect(toolBox, &ToolBox::colorsRequested, this, [this] {
        colorsDock->show();
        colorsDock->raise();
    });
    metrics.record("editor_toolbox", stage);
    stage.restart();
    addToolBarBreak(Qt::TopToolBarArea);
    toolOptions = new ToolOptionsBar(&toolState, this);
    connect(&toolState,&ToolState::shapeSettingsRequested,this,&Window::shapeParameters);
    layersPanel->transformSelected=[this]{openOperation({"offset","flip","rotate","scale_content","replace_color"});};
    addToolBar(Qt::TopToolBarArea, toolOptions);
    connect(toolOptions, &ToolOptionsBar::saveBrushPreset, this, [this] {
        bool ok = false;
        const auto name = QInputDialog::getText(this, "Save brush preset", "Preset name", QLineEdit::Normal,
                                                toolInfo(toolState.tool).name + " preset", &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        presets.put({"brush", name, {}, toolState.brushPreset(toolState.tool)});
        QString error;
        if (!presets.save(&error))
            statusBar()->showMessage(error, 6000);
    });
    connect(toolOptions, &ToolOptionsBar::loadBrushPreset, this, [this](const QString &name) {
        if (name.isEmpty()) {
            toolOptions->setProperty("brushPresetNames", presets.names("brush"));
            return;
        }
        if (auto p = presets.find("brush", name))
            toolState.applyBrushPreset(toolState.tool, p->data);
    });
    metrics.record("editor_options", stage);
    stage.restart();
    colorPanel = new ColorPanel(&toolState, &palettes);
    colorPanel->selectPalette(preferences.activePalette);
    colorPanel->currentImage = [this] { return current() ? current()->document->image : QImage(); };
    colorPanel->mapImage = [this](const QString &palette) {
        if (current())
            openOperation({"palette_map"});
        (void)palette;
    };
    colorPanel->replaceInImage = [this](QRgb from, QRgb to) {
        if (current())
            documentOperation({"replace_color", {{"from", ops::colorName(from)}, {"to", ops::colorName(to)},
                                                 {"tolerance", toolState.tolerance}, {"preserve_shading", false}}});
    };
    colorsDock = new QDockWidget("Colours", this);
    colorsDock->setObjectName("colors");
    auto colorScroll = new QScrollArea;
    colorScroll->setWidgetResizable(true);
    colorScroll->setFrameShape(QFrame::NoFrame);
    colorScroll->setWidget(colorPanel);
    colorsDock->setWidget(colorScroll);
    addDockWidget(Qt::RightDockWidgetArea, colorsDock);
    historyPanel = new HistoryPanel(&session);
    historyDock = new QDockWidget("History", this);
    historyDock->setObjectName("history");
    historyDock->setWidget(historyPanel);
    addDockWidget(Qt::RightDockWidgetArea, historyDock);
    // Right column: Colours | Inspector tabs above Layers | History tabs.
    tabifyDockWidget(inspectorDock, colorsDock);
    tabifyDockWidget(layersDock, historyDock);
    colorsDock->raise();
    layersDock->raise();
    connect(&toolState, &ToolState::changed, this, [this] {
        if (current())
            current()->update();
    });
    metrics.record("editor_docks", stage);
    stage.restart();
    // ---------------------------------------------------------------- Edit
    auto edit = menuBar()->addMenu("&Edit");
    edit->setObjectName("menu-Edit");
    auto add = [this](QMenu *menu, const QString &text, const char *name, const QString &shortcut, auto fn) {
        auto a = menu->addAction(text);
        a->setObjectName(name);
        if (!shortcut.isEmpty())
            a->setShortcut(QKeySequence(shortcut));
        connect(a, &QAction::triggered, this, fn);
        return a;
    };
    edit->addAction(findChild<QAction *>("editUndo"));
    edit->addAction(findChild<QAction *>("editRedo"));
    edit->addSeparator();
    add(edit, "Cut", "editCut", "Ctrl+X", [this] { copySelection(true); });
    add(edit, "Copy", "editCopy", "Ctrl+C", [this] { copySelection(false); });
    add(edit, "Paste", "editPaste", "Ctrl+V", [this] { paste(false); });
    add(edit, "Paste as New Layer", "editPasteLayer", "Ctrl+Shift+V", [this] { paste(true); });
    add(edit, "Duplicate Selection to Layer", "editDuplicateSelection", "Ctrl+J", [this] { duplicateSelection(); });
    edit->addSeparator();
    add(edit, "Fill with Foreground", "editFill", "Alt+Backspace", [this] {
        layerOperation({"fill", {{"color", ops::colorName(toolState.foreground)}}});
    });
    add(edit, "Clear", "editClear", "Delete", [this] {
        if (auto c = current()) {
            QString error;
            if (!session.clear(c->document, c->document->activeLayer(), &error))
                statusBar()->showMessage(error, 4000);
        }
    });
    add(edit, "Define Brush Stamp from Selection", "defineStamp", {}, [this] {
        auto c = current();
        if (!c)
            return;
        const auto doc = c->document;
        const auto pixels = doc->selectedPixels(doc->activeLayer());
        const QRect b = doc->hasSelection() ? ops::maskBounds(doc->selection()) : ops::alphaBounds(pixels, 0);
        if (b.isEmpty() || b.width() > ops::Brush::MaxSize || b.height() > ops::Brush::MaxSize) {
            statusBar()->showMessage("Select 1–256 visible pixels to use as a stamp", 5000);
            return;
        }
        for (auto t : {Tool::Pencil, Tool::Brush, Tool::Eraser}) {
            auto &brush = toolState.brush(t);
            brush.stamp = pixels.copy(b);
        }
        auto &brush = toolState.brush(Tool::Brush);
        brush.shape = ops::BrushShape::Custom;
        brush.sourceColors = true;
        toolState.setTool(Tool::Brush);
        toolState.touch();
        statusBar()->showMessage(QString("Brush stamp defined (%1×%2); choose “Custom stamp” on any brush").arg(b.width()).arg(b.height()), 6000);
    });
    // Every tool, grouped as on the tool strip, so the palette can find them.
    auto toolMenu = edit->addMenu("&Tool");
    toolMenu->setObjectName("toolMenu");
    QString lastGroup;
    for (const auto &info : toolInfos()) {
        if (!lastGroup.isEmpty() && lastGroup != info.group)
            toolMenu->addSeparator();
        lastGroup = info.group;
        toolMenu->addAction(toolBox->actionFor(info.tool));
    }
    edit->addSeparator();
    add(edit,"Original Stamp Library…","stampLibrary",{},[this]{stampLibrary();});
    add(edit,"Numeric Shape / Presets…","numericShape",{},[this]{shapeParameters();});
    edit->addSeparator();
    add(edit, "Swap Colours", "swapColors", "X", [this] { toolState.swapColors(); });
    add(edit, "Default Colours", "defaultColors", "D", [this] { toolState.resetColors(); });
    add(edit, "Decrease Brush Size", "brushSmaller", "[", [this] {
        auto &b = toolState.brush(toolState.tool);
        b.size = std::max(1, b.size - 1);
        toolState.touch();
    });
    add(edit, "Increase Brush Size", "brushLarger", "]", [this] {
        auto &b = toolState.brush(toolState.tool);
        b.size = std::min(ops::Brush::MaxSize, b.size + 1);
        toolState.touch();
    });
    for (auto shortcutAction : edit->actions())
        if (shortcutAction->shortcut().count() && shortcutAction->shortcut()[0].keyboardModifiers() == Qt::NoModifier)
            shortcutAction->setShortcutContext(Qt::WindowShortcut);
    // Pixel-destroying keys act only while the document area has focus, so
    // Delete in the asset tree, layer list or a panel never clears pixels.
    for (const char *name : {"editClear", "editFill"})
        if (auto a = findChild<QAction *>(name)) {
            a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            tabs->addAction(a);
        }
    // ---------------------------------------------------------------- Select
    auto select = menuBar()->addMenu("&Select");
    select->setObjectName("menu-Select");
    auto selection = [this](DocumentSession::SelectionCommand c) {
        if (auto canvas = current()) {
            canvas->commitFloating();
            session.selection(canvas->document, c);
        }
    };
    add(select, "Select All", "selectAll", "Ctrl+A", [=] { selection(DocumentSession::SelectionCommand::All); });
    add(select, "Deselect", "selectNone", "Ctrl+D", [=] { selection(DocumentSession::SelectionCommand::None); });
    add(select, "Invert Selection", "selectInvert", "Ctrl+Shift+I", [=] { selection(DocumentSession::SelectionCommand::Invert); });
    select->addSeparator();
    for (auto t : {Tool::RectSelect, Tool::EllipseSelect, Tool::Lasso, Tool::PolygonLasso, Tool::MagicWand, Tool::ColorRange})
        select->addAction(toolBox->actionFor(t));
    // ---------------------------------------------------------------- Image
    auto image = menuBar()->addMenu("&Image");
    image->setObjectName("menu-Image");
    add(image, "Flip Image Horizontally", "imageFlipH", {}, [this] { documentOperation({"image_flip", {}}); });
    add(image, "Flip Image Vertically", "imageFlipV", {}, [this] { documentOperation({"image_flip", {{"direction", "Vertical"}}}); });
    add(image, "Rotate 90° Clockwise", "imageRotateCW", {}, [this] { documentOperation({"image_rotate", {}}); });
    add(image, "Rotate 180°", "imageRotate180", {}, [this] { documentOperation({"image_rotate", {{"turns", "180°"}}}); });
    add(image, "Rotate 90° Counter-clockwise", "imageRotateCCW", {}, [this] {
        documentOperation({"image_rotate", {{"turns", "90° counter-clockwise"}}});
    });
    image->addSeparator();
    add(image, "Resize Image…", "imageResize", "Ctrl+Alt+I", [this] { openOperation({"resize_image"}); });
    add(image, "Resize Canvas…", "imageCanvas", "Ctrl+Alt+C", [this] { openOperation({"resize_canvas"}); });
    add(image, "Crop to Selection", "imageCropSelection", {}, [this] {
        if (auto c = current(); c && c->document->hasSelection()) {
            const auto b = ops::maskBounds(c->document->selection());
            documentOperation({"crop", {{"x", b.x()}, {"y", b.y()}, {"width", b.width()}, {"height", b.height()}}});
        }
    });
    add(image, "Crop…", "imageCrop", {}, [this] { openOperation({"crop"}); });
    add(image, "Trim Transparent Borders", "imageTrim", {}, [this] { documentOperation({"trim", {}}); });
    add(image, "Flatten Image", "imageFlatten", {}, [this] {
        if (auto c = current()) {
            QString error;
            if (!session.flatten(c->document, &error))
                statusBar()->showMessage(error, 4000);
        }
    });
    image->addSeparator();
    add(image, "Tile Preview && Seam Check…", "imageTilePreview", {}, [this] {
        if (current())
            showTilePreview(this, current()->document->image);
    });
    // ---------------------------------------------------------------- Layer
    // Layer commands (formerly only panel buttons) followed by layer transforms.
    auto transform = menuBar()->addMenu("&Layer");
    transform->setObjectName("menu-Transform");
    add(transform, "New Layer", "layerAdd", "Ctrl+Shift+N", [this] { layersPanel->addLayer(); });
    add(transform, "Duplicate Layer", "layerDuplicate", {}, [this] { layersPanel->duplicateLayer(); });
    add(transform, "Delete Layer", "layerDelete", {}, [this] { layersPanel->deleteLayer(); });
    add(transform, "Rename Layer", "layerRename", {}, [this] {
        layersDock->show();
        layersDock->raise();
        layersPanel->renameLayer();
    });
    add(transform, "Raise Layer", "layerRaise", "Ctrl+]", [this] { layersPanel->raiseLayer(); });
    add(transform, "Lower Layer", "layerLower", "Ctrl+[", [this] { layersPanel->lowerLayer(); });
    transform->addSeparator();
    add(transform, "Transform Selected Layers…", "layerTransformSelected", {}, [this] {
        if (layersPanel->transformSelected)
            layersPanel->transformSelected();
    });
    add(transform, "Flip Layer Horizontally", "layerFlipH", {}, [this] { layerOperation({"flip", {}}); });
    add(transform, "Flip Layer Vertically", "layerFlipV", {}, [this] { layerOperation({"flip", {{"direction", "Vertical"}}}); });
    add(transform, "Rotate Layer 90° Clockwise", "layerRotate90", {}, [this] { layerOperation({"rotate", {{"angle", 90.0}}}); });
    add(transform, "Rotate Layer 180°", "layerRotate180", {}, [this] { layerOperation({"rotate", {{"angle", 180.0}}}); });
    add(transform, "Rotate Layer…", "layerRotate", {}, [this] { openOperation({"rotate"}); });
    add(transform, "Scale Content…", "layerScale", {}, [this] { openOperation({"scale_content"}); });
    add(transform, "Offset / Wrap…", "layerOffset", {}, [this] { openOperation({"offset"}); });
    add(transform, "Centre Content", "layerCentre", {}, [this] { layerOperation({"align_content", {}}); });
    add(transform, "Align Content…", "layerAlign", {}, [this] { openOperation({"align_content"}); });
    transform->addSeparator();
    transform->addAction(toolBox->actionFor(Tool::Move));
    transform->addAction(toolBox->actionFor(Tool::Transform));
    transform->addSeparator();
    add(transform, "Merge Layer Down", "layerMergeDown", "Ctrl+E", [this] {
        if (auto c = current()) {
            QString error;
            if (!session.mergeDown(c->document, c->document->activeLayer(), &error))
                statusBar()->showMessage(error, 4000);
        }
    });
    if (auto flatten = findChild<QAction *>("imageFlatten"))
        transform->addAction(flatten);
    // ---------------------------------------------------------------- operation menus
    auto adjust = operationMenu("&Adjust", "Adjust");
    auto filters = operationMenu("&Filters", "Filters");
    auto shading = operationMenu("&Shading", "Shading");
    shading->addSeparator();
    for (const auto &preset : ops::builtInPresets())
        if (preset.operation == "directional_shading") {
            auto a = shading->addAction(preset.name + " preset…");
            a->setObjectName("shadingPreset-" + preset.name);
            connect(a, &QAction::triggered, this, [this, preset] {
                auto c = current();
                if (!c)
                    return;
                OperationDialog dialog(&session, c->document, c, &presets, colorPanel->activePalette(),
                                       {"directional_shading"}, [this] { return colorPanel->activePaletteText(); }, this);
                dialog.parameters()->setParams(preset.params);
                emit dialog.parameters()->edited();
                dialog.exec();
                c->clearExternalPreview();
            });
        }
    auto generate = operationMenu("&Generate", "Generate");
    // Batch processing lives in the Resources menu (organizeMenus places it).
    auto batchAction = new QAction("Batch Process Textures…", this);
    batchAction->setObjectName("batchProcessorCommand");
    batchAction->setShortcut(QKeySequence("Ctrl+Alt+B"));
    addAction(batchAction);
    connect(batchAction, &QAction::triggered, this, [this] { batchDialog(); });
    generate->addSeparator();
    add(generate, "All Generators…", "generatorBrowser", {}, [this] {
        QStringList ids;
        for (const auto &info : ops::registry())
            if (info.generator)
                ids.append(info.id);
        openOperation(ids);
    });
    // One Effects menu groups the four operation families (menu bar and fx button).
    effectsMenu = menuBar()->addMenu("Effe&cts");
    effectsMenu->setObjectName("menu-Effects");
    for (auto m : {adjust, filters, shading, generate}) {
        menuBar()->removeAction(m->menuAction());
        effectsMenu->addMenu(m);
    }
    toolBox->effectsButton->setMenu(effectsMenu);
    Q_UNUSED(view); // panel toggles are arranged by buildWorkspaceMenu()
    metrics.record("editor_menus_icons", stage);
}
} // namespace nova
