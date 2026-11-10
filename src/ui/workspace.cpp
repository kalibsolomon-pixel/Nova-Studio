// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 3-B workspace shell: application bar, structured inspector, task
// workspaces (presets), the View menu, canvas zoom commands, Preferences,
// the generated keyboard-shortcut reference and the activity log.
// Layout only: nothing here edits documents or changes their data.
#include "layers.h"
#include "modelpanel.h"
#include "theme.h"
#include "tool_placement.h"
#include "timeline.h"
#include "window.h"
#include "vanilla/cache.h"
#include <QtConcurrent>
#include <QtWidgets>

namespace nova {
namespace {
struct PresetInfo {
    const char *id, *name, *tip;
};
const PresetInfo workspacePresets[] = {
    {"essentials", "Essentials", "Balanced default: assets, canvas, inspector, layers and 3D preview"},
    {"texture", "Texture Editing", "Largest canvas; colours and layers beside it, preview in the side column"},
    {"animation", "Animation", "Timeline across the bottom with layers and preview in the side column"},
    {"model", "Model Editing", "Models / UV inspector with a large 3D preview"},
    {"uv", "UV Editing", "Models / UV inspector beside a large texture canvas for UV regions"},
};
QString presetName(const QString &id) {
    for (const auto &p : workspacePresets)
        if (id == p.id)
            return p.name;
    return {};
}
QString cleanText(QString text) { return text.remove('&').section('\t', 0, 0).trimmed(); }
} // namespace

// ------------------------------------------------------------------ inspector
QWidget *Window::buildInspector(QCheckBox *gridBox, QPushButton *center) {
    auto root = new QWidget;
    root->setObjectName("inspectorPanel");
    auto v = new QVBoxLayout(root);
    v->setContentsMargins(ui::SpaceXL, ui::SpaceL, ui::SpaceXL, ui::SpaceL);
    v->setSpacing(ui::SpaceM);
    inspectorPages = new QStackedWidget;
    inspectorPages->setObjectName("inspectorPages");
    // Empty state: what to do next, not a blank panel.
    auto empty = new QWidget;
    auto ev = new QVBoxLayout(empty);
    ev->setContentsMargins(0, 0, 0, 0);
    ev->addWidget(ui::sectionTitle("No document"));
    auto emptyHint = ui::hint("Double-click a PNG in Assets, or use File › Open Texture (Ctrl+O). "
                              "Model JSON opens in the Models / UV panel.");
    emptyHint->setObjectName("inspectorEmptyHint");
    ev->addWidget(emptyHint);
    ev->addStretch();
    inspectorPages->addWidget(empty);
    auto page = new QWidget;
    auto pv = new QVBoxLayout(page);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(ui::SpaceM);
    pv->addWidget(ui::sectionTitle("Document"));
    auto form = new QFormLayout;
    form->setObjectName("inspectorDocumentForm");
    form->setHorizontalSpacing(ui::SpaceL);
    form->setVerticalSpacing(ui::SpaceS);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    for (const auto &[key, label] : std::initializer_list<std::pair<const char *, const char *>>{
             {"name", "Name"}, {"size", "Size"}, {"layers", "Layers"}, {"frames", "Frames"}, {"state", "Status"},
             {"source", "Editable source"}, {"export", "Minecraft PNG"}, {"history", "Undo memory"},
             {"origin", "Origin"}}) {
        auto value = new QLabel;
        value->setObjectName(QString("inspector-") + key);
        value->setProperty("role", "value");
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto name = new QLabel(label);
        name->setProperty("role", "hint");
        form->addRow(name, value);
        inspectorValues.insert(key, value);
    }
    pv->addLayout(form);
    pv->addWidget(ui::sectionTitle("View"));
    pv->addWidget(gridBox);
    // Zoom buttons join this row once buildWorkspaceMenu() creates the actions.
    auto zoomBox = new QWidget;
    zoomBox->setObjectName("inspectorZoomRow");
    auto zoomRow = new QHBoxLayout(zoomBox);
    zoomRow->setContentsMargins(0, 0, 0, 0);
    zoomRow->setSpacing(ui::SpaceXS);
    zoomLabel = new QLabel("100%");
    zoomLabel->setObjectName("inspectorZoom");
    zoomLabel->setAlignment(Qt::AlignCenter);
    zoomLabel->setMinimumWidth(48);
    zoomLabel->setAccessibleName("Canvas zoom");
    zoomRow->addWidget(zoomLabel);
    zoomRow->addStretch();
    pv->addWidget(zoomBox);
    pv->addWidget(center);
    pv->addWidget(ui::sectionTitle("Active tool"));
    inspectorTool = new QLabel;
    inspectorTool->setObjectName("inspectorTool");
    inspectorTool->setStyleSheet("font-weight:600");
    pv->addWidget(inspectorTool);
    inspectorToolTip = ui::hint({});
    inspectorToolTip->setObjectName("inspectorToolTip");
    inspectorToolTip->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pv->addWidget(inspectorToolTip);
    inspectorTips = ui::hint("Right-click the canvas for editing commands. Wheel zooms; Space+drag or "
                             "middle-drag pans. Help › Keyboard Shortcuts (Ctrl+/) lists every key.");
    inspectorTips->setObjectName("inspectorTips");
    inspectorTips->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pv->addSpacing(ui::SpaceS);
    pv->addWidget(inspectorTips);
    pv->addStretch();
    inspectorPages->addWidget(page);
    v->addWidget(inspectorPages, 1);
    connect(&toolState, &ToolState::toolChanged, this, [this] { updateInspector(); });
    return root;
}
void Window::updateInspector() {
    if (!inspectorPages)
        return;
    auto canvas = current();
    inspectorPages->setCurrentIndex(canvas ? 1 : 0);
    if (!canvas)
        return;
    const auto doc = canvas->document;
    auto set = [this](const char *key, const QString &text, const char *role = "value") {
        auto label = inspectorValues.value(key);
        label->setText(text);
        if (label->property("role").toString() != role) {
            label->setProperty("role", role);
            label->style()->unpolish(label);
            label->style()->polish(label);
        }
        if (auto form = inspectorPages->findChild<QFormLayout *>("inspectorDocumentForm"))
            form->setRowVisible(label, !text.isEmpty());
    };
    set("name", QFileInfo(doc->path).fileName());
    set("size", QString("%1 × %2 px").arg(doc->image.width()).arg(doc->image.height()));
    set("layers", QString::number(doc->layers().size()));
    set("frames", doc->animated() ? QString("%1 source · %2 playback")
                                         .arg(doc->animationState().frames.size())
                                         .arg(doc->animationState().entries.size())
                                  : QString());
    set("state", doc->modified() ? "Unsaved changes" : "Saved", doc->modified() ? "warning" : "value");
    if (doc->nativeDocument()) {
        set("source", doc->modified() ? "Unsaved" : "Saved");
        if (externalRevisions.contains(doc->id()))
            set("export", "Changed on disk — review before export", "error");
        else
            set("export", doc->exportModified() ? "Update available (Ctrl+Alt+E)" : "Matches editable source",
                doc->exportModified() ? "warning" : "value");
    } else {
        set("source", {});
        set("export", {});
    }
    set("history", QString("%1 KiB of %2 MiB").arg(doc->historyBytes() / 1024).arg(preferences.historyMiB));
    set("origin", baselineOrigin(*doc)); // baseline projects only
    zoomLabel->setText(QString("%1%").arg(qRound(canvas->zoom * 100)));
    const auto &info = toolInfo(toolState.tool);
    inspectorTool->setText(info.shortcut.isEmpty() ? info.name : QString("%1  (%2)").arg(info.name, info.shortcut));
    inspectorToolTip->setText(info.tip);
    inspectorTips->setVisible(preferences.showTips);
}

// ------------------------------------------------------------------ menus
void Window::buildWorkspaceMenu(QMenu *view) {
    // Zoom and view commands (canvas). Created first: the inspector reuses them.
    auto command = [this](const QString &text, const QString &name, const QList<QKeySequence> &keys,
                          const QString &icon, auto callback) {
        auto a = new QAction(text, this);
        a->setObjectName(name);
        a->setShortcuts(keys);
        if (!icon.isEmpty())
            a->setProperty("novaIcon", icon);
        a->setToolTip(keys.isEmpty() ? text : QString("%1 (%2)").arg(text, keys.first().toString(QKeySequence::NativeText)));
        connect(a, &QAction::triggered, this, callback);
        addAction(a);
        return a;
    };
    auto zoomIn = command("Zoom In", "zoomInCommand", {QKeySequence("Ctrl+="), QKeySequence("Ctrl++")}, "zoom-in",
                          [this] { zoomCanvas(1); });
    auto zoomOut = command("Zoom Out", "zoomOutCommand", {QKeySequence("Ctrl+-")}, "zoom-out", [this] { zoomCanvas(-1); });
    auto fit = command("Fit on Screen", "fitCanvasCommand", {QKeySequence("Ctrl+0")}, "fit", [this] { zoomCanvas(0); });
    auto actual = command("Actual Pixels (100%)", "actualPixelsCommand", {QKeySequence("Ctrl+1")}, "actual",
                          [this] { zoomCanvas(2); });
    auto centerCommand = command("Center Canvas", "centerCanvasCommand", {}, "center", [this] {
        if (current())
            current()->center();
    });
    // Phase 4-G: the command palette became application-wide Search (Ctrl+K; Ctrl+Shift+P still works).
    auto palette = command("Search…", "commandPaletteCommand", {QKeySequence("Ctrl+K"), QKeySequence("Ctrl+Shift+P")},
                           "search", [this] { commandPalette(); });
    palette->setStatusTip("Find commands, tools, settings, workspaces, panels and assets");
    if (auto row = findChild<QWidget *>("inspectorZoomRow")) {
        auto layout = static_cast<QHBoxLayout *>(row->layout());
        int at = 0;
        for (auto a : {zoomOut, (QAction *)nullptr, zoomIn, fit, actual}) {
            if (!a) {
                ++at; // the zoom label sits between Zoom Out and Zoom In
                continue;
            }
            auto button = new QToolButton;
            button->setDefaultAction(a);
            button->setAutoRaise(true);
            button->setIconSize({ui::IconSize, ui::IconSize});
            layout->insertWidget(at++, button);
        }
    }
    // Workspace presets.
    auto workspace = view->addMenu("&Workspace");
    workspace->setObjectName("workspaceMenu");
    presetGroup = new QActionGroup(this);
    presetGroup->setExclusive(true);
    for (const auto &p : workspacePresets) {
        auto a = workspace->addAction(p.name);
        a->setObjectName(QString("workspacePreset-") + p.id);
        a->setCheckable(true);
        a->setData(QString(p.id));
        a->setStatusTip(p.tip);
        presetGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, id = QString(p.id)] { applyPreset(id); });
    }
    workspace->addSeparator();
    layoutsMenu = workspace->addMenu("Saved &Layouts");
    layoutsMenu->setObjectName("workspaceLayouts");
    updateLayouts();
    workspace->addAction(findChild<QAction *>("saveWorkspaceCommand"));
    workspace->addAction(findChild<QAction *>("resetWorkspaceCommand"));
    // Panels in a stable, task-oriented order.
    for (auto a : panelsMenu->actions())
        panelsMenu->removeAction(a);
    for (auto dock : {assetsDock, inspectorDock, modelsDock, colorsDock, layersDock, historyDock, previewDock,
                      animationDock, problemsDock})
        panelsMenu->addAction(dock->toggleViewAction());
    panelsMenu->addSeparator();
    toolBox->toggleViewAction()->setText("Tool Strip");
    toolOptions->toggleViewAction()->setText("Tool Options Bar");
    panelsMenu->addAction(toolBox->toggleViewAction());
    panelsMenu->addMenu(toolPlacement->menu());
    panelsMenu->addAction(toolPlacement->dockWidget()->toggleViewAction());
    panelsMenu->addAction(toolOptions->toggleViewAction());
    view->addMenu(panelsMenu);
    view->addSeparator();
    for (auto a : {zoomIn, zoomOut, fit, actual, centerCommand})
        view->addAction(a);
    view->addAction(pixelGridAction);
    addAction(pixelGridAction);
    view->addSeparator();
    auto theme = view->addMenu("&Theme");
    theme->setObjectName("themeMenu");
    theme->addAction(themeAction);
    theme->addSeparator();
    for (auto a : findChildren<QAction *>())
        if (a->objectName().startsWith("accent-"))
            theme->addAction(a);
    view->addSeparator();
    view->addAction(palette);
}
void Window::organizeMenus() {
    // Standard creative-application order: document, editing, content, view, help.
    if (auto edit = findChild<QMenu *>("menu-Edit")) {
        edit->addSeparator();
        auto preferencesAction = edit->addAction("Preferences…");
        preferencesAction->setObjectName("preferencesCommand");
        preferencesAction->setShortcut(QKeySequence("Ctrl+,"));
        preferencesAction->setProperty("novaIcon", "settings");
        preferencesAction->setMenuRole(QAction::PreferencesRole);
        connect(preferencesAction, &QAction::triggered, this, [this] { preferencesDialog(); });
    }
    // Search is also the first Help command, where people look for it.
    if (auto help = findChild<QMenu *>("menu-Help"); help && !help->actions().isEmpty()) {
        help->insertAction(help->actions().first(), findChild<QAction *>("commandPaletteCommand"));
        help->insertSeparator(help->actions().at(1));
    }
    if (auto resources = findChild<QMenu *>("menu-Resources"))
        resources->insertAction(findChild<QAction *>("processModelTextures"), findChild<QAction *>("batchProcessorCommand"));
    for (const char *name : {"menu-File", "menu-Edit", "menu-Image", "menu-Transform", "menu-Select", "menu-Effects",
                             "animationMenu", "menu-Resources", "menu-View", "menu-Help"})
        if (auto menu = findChild<QMenu *>(name)) {
            menuBar()->removeAction(menu->menuAction());
            menuBar()->addAction(menu->menuAction());
        }
}

// ------------------------------------------------------------------ application bar
void Window::buildAppBar() {
    appBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    appBar->addSeparator();
    for (const char *name : {"saveCommand", "editUndo", "editRedo"})
        appBar->addAction(findChild<QAction *>(name));
    appBar->addSeparator();
    auto projectBox = new QWidget;
    projectBox->setObjectName("appBarProject");
    auto pl = new QHBoxLayout(projectBox);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(ui::SpaceXS);
    auto openButton = new QToolButton;
    openButton->setDefaultAction(findChild<QAction *>("openProjectCommand"));
    openButton->setAutoRaise(true);
    openButton->setIconSize({ui::IconSize, ui::IconSize});
    openButton->setToolTip("Open Resource Pack… (Ctrl+Shift+O)");
    pl->addWidget(openButton);
    pl->addWidget(pathText, 1);
    auto go = new QToolButton;
    go->setDefaultAction(findChild<QAction *>("openTypedProject"));
    go->setToolButtonStyle(Qt::ToolButtonTextOnly);
    go->setAutoRaise(true);
    pl->addWidget(go);
    appBar->addWidget(projectBox);
    auto spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    appBar->addWidget(spacer);
    auto workspaceLabel = new QLabel("Workspace");
    workspaceLabel->setProperty("role", "hint");
    appBar->addWidget(workspaceLabel);
    presetBox = new QComboBox;
    presetBox->setObjectName("workspacePresetBox");
    presetBox->setAccessibleName("Workspace");
    presetBox->setToolTip("Arrange panels for a task. Switching never changes documents.");
    for (const auto &p : workspacePresets) {
        presetBox->addItem(p.name, QString(p.id));
        presetBox->setItemData(presetBox->count() - 1, QString(p.tip), Qt::ToolTipRole);
    }
    presetBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    workspaceLabel->setBuddy(presetBox);
    connect(presetBox, &QComboBox::activated, this, [this](int index) { applyPreset(presetBox->itemData(index).toString()); });
    appBar->addWidget(presetBox);
    appBar->addSeparator();
    appBar->addAction(findChild<QAction *>("commandPaletteCommand"));
    // Status bar: the activity log keeps every transient message reviewable.
    activityButton = ui::iconButton("activity", "Activity: recent status messages", "activityButton");
    activityButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    activityButton->setPopupMode(QToolButton::InstantPopup);
    activityButton->setIconSize({14, 14});
    activityMenu = new QMenu(activityButton);
    activityMenu->setObjectName("activityMenu");
    activityButton->setMenu(activityMenu);
    connect(activityMenu, &QMenu::aboutToShow, this, [this] {
        activityMenu->clear();
        if (activity.isEmpty())
            activityMenu->addAction("No messages yet")->setEnabled(false);
        for (const auto &entry : activity.mid(0, 40))
            activityMenu->addAction(entry)->setEnabled(false);
        activityMenu->addSeparator();
        activityMenu->addAction("Copy All", this, [this] { QApplication::clipboard()->setText(activity.join('\n')); });
        activityMenu->addAction("Clear", this, [this] { activity.clear(); });
        unseenActivity = 0;
        activityButton->setText({});
    });
    statusBar()->addPermanentWidget(activityButton);
    connect(statusBar(), &QStatusBar::messageChanged, this, [this](const QString &message) { recordActivity(message); });
}
void Window::recordActivity(const QString &message) {
    if (message.trimmed().isEmpty())
        return;
    const auto entry = QTime::currentTime().toString("HH:mm:ss") + "  " + message;
    if (!activity.isEmpty() && activity.first().mid(10) == entry.mid(10))
        return;
    activity.prepend(entry);
    while (activity.size() > 200)
        activity.removeLast();
    ++unseenActivity;
    if (activityButton)
        activityButton->setText(QString::number(std::min(unseenActivity, 99)));
}
void Window::refreshThemeIcons() {
    if (toolBox)
        toolBox->refreshIcons(ui::current().text);
    for (auto a : findChildren<QAction *>())
        if (const auto icon = a->property("novaIcon"); icon.isValid())
            a->setIcon(ui::lineIcon(icon.toString()));
    for (auto b : findChildren<QAbstractButton *>())
        if (const auto icon = b->property("novaIcon"); icon.isValid())
            b->setIcon(ui::lineIcon(icon.toString()));
}

// ------------------------------------------------------------------ presets
QByteArray Window::currentLayout() {
    return packLayout(toolPlacement ? toolPlacement->placement() : ToolPlacement::LeftEdge, saveState(1));
}
// Restores a packed layout: the tool strip goes to its saved position first, so the
// saved state then finds it in the right container (main-window toolbar or tools dock).
bool Window::applyLayout(const QByteArray &packed) {
    ToolPlacement placement;
    QByteArray state;
    unpackLayout(packed, &placement, &state); // unknown positions read as the left edge
    if (state.isEmpty())
        return false;
    const auto previous = toolPlacement ? toolPlacement->placement() : ToolPlacement::LeftEdge;
    if (toolPlacement)
        toolPlacement->place(placement);
    if (!restoreState(state, 1)) {
        if (toolPlacement)
            toolPlacement->place(previous);
        return false;
    }
    return true;
}
// Keeps the outgoing workspace's arrangement only if the user changed it.
void Window::rememberPresetLayout() {
    if (preset.isEmpty() || presetBaseline.isEmpty())
        return;
    const auto now = currentLayout();
    if (now != presetBaseline)
        preferences.presetLayouts.insert(preset, now);
}
void Window::applyPreset(const QString &requested, bool useSaved) {
    QElapsedTimer timer;
    timer.start();
    const auto name = presetName(requested).isEmpty() ? QString("essentials") : requested;
    if (useSaved && name != preset)
        rememberPresetLayout();
    preset = name;
    if (presetBox) {
        QSignalBlocker block(presetBox);
        presetBox->setCurrentIndex(std::max(0, presetBox->findData(name)));
    }
    if (presetGroup)
        for (auto a : presetGroup->actions())
            a->setChecked(a->data().toString() == name);
    // Problems are task-independent: keep that panel as the user left it.
    const bool problems = problemsDock && problemsDock->isVisible() && !problemsDock->isFloating();
    // Visibility changes made while arranging are not user choices: without this
    // guard, hiding the timeline during restoreState() was recorded as the
    // user's timeline preference and later switches silently dropped it.
    QScopedValueRollback<bool> arranging(animationSyncing, true);
    // One repaint for the final arrangement instead of one per intermediate step.
    setUpdatesEnabled(false);
    // The user's own arrangement of this workspace, if any; otherwise the product default.
    if (useSaved && preferences.presetLayouts.contains(name) && applyLayout(preferences.presetLayouts.value(name))) {
        if (problems)
            problemsDock->show();
        animationRefresh();
        keepOnScreen();
        setUpdatesEnabled(true);
        QTimer::singleShot(0, this, [this, name] {
            if (preset == name)
                presetBaseline = preferences.presetLayouts.value(name);
        });
        metrics.record("workspace_switch", timer);
        statusBar()->showMessage("Workspace: " + presetName(name) + " (your layout)", 3000);
        return;
    }
    if (toolPlacement)
        toolPlacement->place(ToolPlacement::LeftEdge);
    restoreState(defaultLayout, 1);
    const int h = std::max(height(), 550), w = std::max(width(), 800);
    resizeDocks({assetsDock}, {std::clamp(w / 6, 180, 300)}, Qt::Horizontal);
    resizeDocks({inspectorDock}, {std::clamp(w / 5, 220, 340)}, Qt::Horizontal);
    if (name == "essentials") {
        resizeDocks({previewDock}, {std::clamp(h / 4, 150, 260)}, Qt::Vertical);
    } else if (name == "texture" || name == "animation" || name == "uv") {
        // Preview moves into the side column so the canvas spans the full height.
        // Short windows tab it with Layers (splitDockWidget tabs into a tabbed
        // group); taller ones stack it below so it stays visible while painting.
        if (h < 720) {
            splitDockWidget(layersDock, previewDock, Qt::Vertical);
        } else {
            removeDockWidget(previewDock);
            addDockWidget(Qt::RightDockWidgetArea, previewDock, Qt::Vertical);
            previewDock->show();
        }
        if (name == "texture")
            colorsDock->raise();
        else
            modelsDock->raise();
        if (name == "uv")
            resizeDocks({inspectorDock}, {std::clamp(w / 4, 300, 380)}, Qt::Horizontal);
        if (name == "animation") {
            inspectorDock->raise();
            preferences.timelineVisible = true;
        }
        resizeDocks({previewDock}, {std::clamp(h / 5, 140, 220)}, Qt::Vertical);
    } else if (name == "model") {
        modelsDock->raise();
        resizeDocks({inspectorDock}, {std::clamp(w / 4, 300, 380)}, Qt::Horizontal);
        resizeDocks({previewDock}, {std::clamp(int(h * 0.42), 200, 520)}, Qt::Vertical);
    }
    layersDock->raise();
    if (problems)
        problemsDock->show();
    animationRefresh(); // the timeline follows the active document, as before
    if (name == "animation" && animationDock->isVisible())
        resizeDocks({animationDock}, {std::clamp(h / 3, 200, 300)}, Qt::Vertical);
    keepOnScreen();
    setUpdatesEnabled(true);
    // Baseline after the deferred relayout, so settling sizes do not count as user changes.
    presetBaseline.clear();
    QTimer::singleShot(0, this, [this, name] {
        if (preset == name)
            presetBaseline = currentLayout();
    });
    metrics.record("workspace_switch", timer);
    statusBar()->showMessage("Workspace: " + presetName(name), 3000);
}

// ------------------------------------------------------------------ zoom
void Window::zoomCanvas(int direction) {
    auto canvas = current();
    if (!canvas)
        return;
    static constexpr double levels[] = {0.25, 0.5, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
    const auto size = canvas->document->image.size();
    double zoom = canvas->zoom;
    if (direction == 1) {
        for (double level : levels)
            if (level > zoom * 1.001) {
                zoom = level;
                break;
            }
    } else if (direction == -1) {
        for (auto it = std::rbegin(levels); it != std::rend(levels); ++it)
            if (*it < zoom * 0.999) {
                zoom = *it;
                break;
            }
    } else if (direction == 0) {
        zoom = std::min((canvas->width() - 32.0) / std::max(1, size.width()),
                        (canvas->height() - 56.0) / std::max(1, size.height()));
        if (zoom >= 1)
            zoom = std::floor(zoom); // whole multiples keep every texture pixel the same size
        canvas->center();
    } else {
        zoom = 1;
    }
    canvas->zoom = std::clamp(zoom, 0.25, 64.0);
    canvas->update();
    if (zoomLabel)
        zoomLabel->setText(QString("%1%").arg(qRound(canvas->zoom * 100)));
}

// ------------------------------------------------------------------ preferences
void Window::preferencesDialog(const QString &focus) {
    QDialog dialog(this);
    dialog.setObjectName("preferencesDialog");
    dialog.setWindowTitle("Preferences");
    auto v = new QVBoxLayout(&dialog);
    v->setSpacing(ui::SpaceL);
    auto form = [&](const QString &title) {
        v->addWidget(ui::sectionTitle(title));
        auto f = new QFormLayout;
        f->setHorizontalSpacing(ui::SpaceXL);
        f->setContentsMargins(ui::SpaceL, 0, 0, 0);
        v->addLayout(f);
        return f;
    };
    auto appearance = form("Appearance");
    auto theme = new QComboBox;
    theme->setObjectName("preferenceTheme");
    theme->addItems({"Dark", "Light"});
    theme->setCurrentIndex(dark ? 0 : 1);
    appearance->addRow("Theme", theme);
    auto accent = new QComboBox;
    accent->setObjectName("preferenceAccent");
    accent->addItem("Violet", "violet");
    accent->addItem("Blue", "blue");
    accent->setCurrentIndex(preferences.accent == "blue" ? 1 : 0);
    appearance->addRow("Accent", accent);
    auto tips = new QCheckBox("Show usage tips in the Inspector");
    tips->setObjectName("preferenceTips");
    tips->setChecked(preferences.showTips);
    appearance->addRow(tips);
    auto editing = form("Editing");
    auto gridCheck = new QCheckBox("Show the pixel grid at high zoom");
    gridCheck->setObjectName("preferencePixelGrid");
    gridCheck->setChecked(preferences.pixelGrid);
    editing->addRow(gridCheck);
    auto history = new QSpinBox;
    history->setObjectName("preferenceHistory");
    history->setRange(1, 64);
    history->setSuffix(" MiB");
    history->setValue(preferences.historyMiB);
    history->setToolTip("Undo memory per open document. Applies to open documents immediately.");
    editing->addRow("Undo history per document", history);
    auto startup = form("Startup");
    auto reopen = new QCheckBox("Reopen the last project and its documents");
    reopen->setObjectName("preferenceReopen");
    reopen->setChecked(preferences.reopenLast);
    startup->addRow(reopen);
    auto workspace = form("Workspace");
    auto reset = new QPushButton("Reset Workspace");
    reset->setObjectName("preferenceResetWorkspace");
    reset->setToolTip("Restore the Essentials arrangement now (documents are not affected)");
    connect(reset, &QPushButton::clicked, this, &Window::resetWorkspace);
    workspace->addRow("Panels", reset);
    auto textures = form("Vanilla textures");
    auto cacheLocation = new QLineEdit(QDir::toNativeSeparators(vanilla::BaselineCache::defaultRoot()));
    cacheLocation->setObjectName("preferenceBaselineLocation");
    cacheLocation->setReadOnly(true);
    cacheLocation->setToolTip("Shared per-user cache of vanilla textures downloaded for baseline projects");
    textures->addRow("Cache location", cacheLocation);
    auto cacheSize = new QLabel("Calculating…");
    cacheSize->setObjectName("preferenceBaselineSize");
    textures->addRow("Cache size", cacheSize);
    auto sizeWatcher = new QFutureWatcher<qint64>(&dialog);
    connect(sizeWatcher, &QFutureWatcherBase::finished, &dialog, [sizeWatcher, cacheSize] {
        cacheSize->setText(QString("%1 MB").arg(sizeWatcher->result() / 1048576.0, 0, 'f', 1));
    });
    sizeWatcher->setFuture(QtConcurrent::run([] { return vanilla::BaselineCache().totalBytes(); }));
    auto manageCache = new QPushButton("Manage…");
    manageCache->setObjectName("preferenceBaselineManage");
    manageCache->setToolTip("Review, verify or remove cached vanilla textures");
    connect(manageCache, &QPushButton::clicked, this, [this] { manageBaselines(); });
    textures->addRow("Cached baselines", manageCache);
    auto experimental = form("Experimental");
    auto agentSession = new QCheckBox("Show Agent Session (ACP) in the Resources menu");
    agentSession->setObjectName("preferenceExperimentalAgentSession");
    agentSession->setChecked(preferences.experimentalAgentSession);
    agentSession->setToolTip("Experimental: tested only with a scripted agent and OpenCode start-up. Conversations "
                             "with a real AI model and other agents have not been tested.");
    experimental->addRow(agentSession);
    v->addStretch();
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    v->addWidget(buttons);
    dialog.resize(440, dialog.sizeHint().height());
    // Opened from Search: put keyboard focus on the setting that was found.
    if (auto target = focus.isEmpty() ? nullptr : dialog.findChild<QWidget *>(focus))
        QTimer::singleShot(0, target, [target] { target->setFocus(Qt::OtherFocusReason); });
    if (dialog.exec() != QDialog::Accepted)
        return;
    preferences.reopenLast = reopen->isChecked();
    preferences.showTips = tips->isChecked();
    preferences.experimentalAgentSession = agentSession->isChecked();
    updateExperimentalActions();
    if (preferences.historyMiB != history->value()) {
        preferences.historyMiB = history->value();
        for (auto &doc : session.all())
            doc->setHistoryLimits(qint64(preferences.historyMiB) * 1024 * 1024, 2048);
    }
    if (auto box = findChild<QCheckBox *>("pixelGrid"))
        box->setChecked(gridCheck->isChecked());
    const auto accentName = accent->currentData().toString();
    if (accentName != preferences.accent) {
        preferences.accent = accentName;
        if (auto a = findChild<QAction *>("accent-" + accentName))
            a->setChecked(true);
    }
    dark = theme->currentIndex() == 0;
    themeAction->setChecked(dark);
    applyTheme();
    updateInspector();
    statusBar()->showMessage("Preferences applied", 3000);
}

// ------------------------------------------------------------------ shortcuts reference
void Window::shortcutsDialog() {
    if (auto open = findChild<QDialog *>("keyboardShortcutsDialog")) {
        open->raise();
        open->activateWindow();
        return;
    }
    auto dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("keyboardShortcutsDialog");
    dialog->setWindowTitle("Keyboard Shortcuts");
    auto v = new QVBoxLayout(dialog);
    auto filter = new QLineEdit;
    filter->setObjectName("shortcutFilter");
    filter->setPlaceholderText("Filter by command or key…");
    filter->setClearButtonEnabled(true);
    filter->setAccessibleName("Filter shortcuts");
    v->addWidget(filter);
    auto shortcutTree = new QTreeWidget;
    shortcutTree->setObjectName("shortcutTree");
    shortcutTree->setColumnCount(2);
    shortcutTree->setHeaderLabels({"Command", "Shortcut"});
    shortcutTree->setRootIsDecorated(false);
    shortcutTree->setUniformRowHeights(true);
    shortcutTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    shortcutTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    v->addWidget(shortcutTree, 1);
    QHash<QString, QList<QAction *>> byKey; // one entry per action, even if listed twice
    QSet<QAction *> listed; // Search sits in View and Help; the reference lists it once
    std::function<void(QMenu *, QTreeWidgetItem *, const QString &)> collect = [&](QMenu *menu, QTreeWidgetItem *group,
                                                                                const QString &prefix) {
        for (auto a : menu->actions()) {
            if (a->isSeparator())
                continue;
            if (a->menu()) {
                collect(a->menu(), group, prefix + cleanText(a->text()) + " › ");
                continue;
            }
            if (a->shortcut().isEmpty() || listed.contains(a))
                continue;
            listed.insert(a);
            QStringList keys;
            for (const auto &key : a->shortcuts())
                keys << key.toString(QKeySequence::NativeText);
            new QTreeWidgetItem(group, {prefix + cleanText(a->text()), keys.join("  or  ")});
            for (const auto &key : keys)
                if (!byKey[key].contains(a))
                    byKey[key] << a;
        }
    };
    for (auto a : menuBar()->actions())
        if (a->menu()) {
            auto group = new QTreeWidgetItem(shortcutTree, {cleanText(a->text())});
            group->setFirstColumnSpanned(true);
            group->setFlags(Qt::ItemIsEnabled);
            auto font = group->font(0);
            font.setBold(true);
            group->setFont(0, font);
            collect(a->menu(), group, {});
            if (!group->childCount())
                delete group;
        }
    auto tools = new QTreeWidgetItem(shortcutTree, {"Tools"});
    tools->setFirstColumnSpanned(true);
    for (const auto &info : toolInfos())
        new QTreeWidgetItem(tools, {info.name, info.shortcut});
    auto gestures = new QTreeWidgetItem(shortcutTree, {"Canvas and preview"});
    gestures->setFirstColumnSpanned(true);
    for (const auto &[what, keys] : std::initializer_list<std::pair<const char *, const char *>>{
             {"Zoom canvas", "Mouse wheel"},
             {"Pan canvas", "Space+drag · middle or right drag"},
             {"Pick colour while painting", "Alt+click"},
             {"Straight line from last point", "Shift+click"},
             {"Add / subtract / intersect selection", "Shift · Alt · Shift+Alt"},
             {"Apply placement or transform", "Enter"},
             {"Cancel placement, polygon or preview", "Esc"},
             {"Nudge placement", "Arrow keys"},
             {"Cycle overlapping UV faces", "Ctrl+Alt+click"},
             {"Orbit / zoom 3D preview", "Drag · mouse wheel"},
             {"Close document tab", "Middle-click tab"}})
        new QTreeWidgetItem(gestures, {what, keys});
    for (auto group : {tools, gestures}) {
        group->setFlags(Qt::ItemIsEnabled);
        auto font = group->font(0);
        font.setBold(true);
        group->setFont(0, font);
    }
    shortcutTree->expandAll();
    QStringList conflicts;
    for (auto it = byKey.begin(); it != byKey.end(); ++it)
        if (it.value().size() > 1) {
            QStringList names;
            for (auto a : it.value())
                names << cleanText(a->text());
            conflicts << it.key() + ": " + names.join(", ");
        }
    auto note = ui::hint(conflicts.isEmpty()
                             ? "Single-letter tool keys work while the canvas or a panel has focus and are "
                               "ignored while typing in a text field. Delete and Alt+Backspace act only when "
                               "the document area has focus."
                             : "Shared shortcuts: " + conflicts.join("; "));
    note->setObjectName("shortcutNote");
    v->addWidget(note);
    connect(filter, &QLineEdit::textChanged, dialog, [shortcutTree](const QString &text) {
        for (int i = 0; i < shortcutTree->topLevelItemCount(); ++i) {
            auto group = shortcutTree->topLevelItem(i);
            int shown = 0;
            for (int j = 0; j < group->childCount(); ++j) {
                auto item = group->child(j);
                const bool match = text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive) ||
                                   item->text(1).contains(text, Qt::CaseInsensitive);
                item->setHidden(!match);
                shown += match;
            }
            group->setHidden(!shown);
        }
    });
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    v->addWidget(buttons);
    dialog->resize(620, 640);
    dialog->show();
    filter->setFocus();
}
} // namespace nova
