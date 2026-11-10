// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "tool_placement.h"
#include "service.h"
#include "branding.h"
#include "fabric_testing.h"
#include "modelpanel.h"
#include "panels.h"
#include "project.h"
#include "projectdialogs.h"
#include "animation_export.h"
#include "theme.h"
#include "timeline.h"
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QtWidgets>
namespace nova {
namespace {
class ModelTab : public QWidget {
  public:
    explicit ModelTab(std::shared_ptr<mc::ModelEdit> document) : doc(std::move(document)) {
        setObjectName("modelDocumentTab");
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
    }
    std::shared_ptr<mc::ModelEdit> doc;
    int face = -1;
};

class EditorTabs : public QTabWidget {
    QScrollArea *welcome;
    QLabel *logo = nullptr;
    void resizeEvent(QResizeEvent *event) override {
        QTabWidget::resizeEvent(event);
        welcome->setGeometry(rect().adjusted(8, 8, -8, -8));
        if (logo)
            logo->setVisible(welcome->height() >= 320);
    }
    // Quiet, theme-aware close buttons replace the style's red close glyph.
    void tabInserted(int index) override {
        QTabWidget::tabInserted(index);
        auto close = ui::iconButton("close", "Close document (Ctrl+W)");
        close->setObjectName("documentTabClose");
        close->setIconSize({14, 14});
        close->setFixedSize(20, 20);
        close->setFocusPolicy(Qt::NoFocus);
        auto previous = tabBar()->tabButton(index, QTabBar::RightSide);
        tabBar()->setTabButton(index, QTabBar::RightSide, close);
        if (previous)
            previous->deleteLater();
        connect(close, &QToolButton::clicked, this, [this, close] {
            for (int i = 0; i < count(); ++i)
                if (tabBar()->tabButton(i, QTabBar::RightSide) == close)
                    emit tabCloseRequested(i);
        });
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        // Middle-click closes a document tab, as in browsers and most editors.
        if (object == tabBar() && event->type() == QEvent::MouseButtonRelease) {
            auto mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::MiddleButton) {
                const int i = tabBar()->tabAt(mouse->position().toPoint());
                if (i >= 0)
                    emit tabCloseRequested(i);
                return true;
            }
        }
        return QTabWidget::eventFilter(object, event);
    }

  public:
    EditorTabs(QAction *create, QAction *open, QAction *folder, QMenu *recent) {
        tabBar()->setObjectName("documentTabs");
        tabBar()->installEventFilter(this);
        tabBar()->setElideMode(Qt::ElideMiddle);
        tabBar()->setExpanding(false);
        welcome = new QScrollArea(this);
        welcome->setObjectName("welcomeActions");
        welcome->setFrameShape(QFrame::NoFrame);
        welcome->setWidgetResizable(true);
        welcome->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto body = new QWidget;
        welcome->setWidget(body);
        auto layout = new QVBoxLayout(body);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->addStretch();
        logo = new QLabel;
        logo->setObjectName("welcomeLogo");
        logo->setPixmap(logoIcon().pixmap(QSize(80, 80), devicePixelRatioF()));
        logo->setAlignment(Qt::AlignCenter);
        logo->setAccessibleName("Nova Studio logo");
        layout->addWidget(logo);
        auto title = new QLabel("Nova Studio");
        title->setAlignment(Qt::AlignCenter);
        title->setWordWrap(true);
        auto font = title->font();
        font.setPointSize(18);
        title->setFont(font);
        layout->addWidget(title);
        auto caption = new QLabel("Minecraft Java resource packs");
        caption->setAlignment(Qt::AlignCenter);
        caption->setWordWrap(true);
        layout->addWidget(caption);
        Q_UNUSED(folder);
        for (auto action : {create, open}) {
            auto row = new QHBoxLayout;
            row->addStretch();
            auto button = new QPushButton(action == create ? "Create pack…" : "Open pack…");
            button->setToolTip(action->text());
            button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            button->setObjectName(action == create ? "welcomeCreate" : "welcomeOpen");
            row->addWidget(button);
            row->addStretch();
            layout->addLayout(row);
            connect(button, &QPushButton::clicked, action, &QAction::trigger);
        }
        auto row = new QHBoxLayout;
        row->addStretch();
        auto button = new QPushButton("Recent Projects");
        button->setMenu(recent);
        button->setObjectName("welcomeRecent");
        row->addWidget(button);
        row->addStretch();
        layout->addLayout(row);
        layout->addStretch();
        connect(this, &QTabWidget::currentChanged, this,
                [this] { welcome->setVisible(count() == 0); });
    }
};
} // namespace

Window::Window(QString p, QString output, QElapsedTimer time)
    : project(p.isEmpty() ? QString() : QDir(p).absolutePath()), telemetry(std::move(output)),
      started(time) {
    QElapsedTimer setup;
    setup.start();
    metrics.samples["application_init"].append(qApp->property("applicationInitMs").toDouble());
    fresh = qApp->arguments().contains("--fresh-layout");
    settings = Settings(qApp->property("settingsFile").toString());
    preferences = settings.load();
    recovery = std::make_unique<RecoveryStore>(QFileInfo(settings.fileName()).absolutePath() +
                                               "/recovery");
    ioPool.setMaxThreadCount(1);
    metrics.record("settings_load", setup);
    setup.restart();
    dark = preferences.dark;
    nav.grid = preferences.gridMode;
    if (!fresh && preferences.projectPath == project) {
        nav.directory = preferences.directory;
        nav.selected = preferences.selected;
        nav.query = preferences.query;
    }
    thumbSize = preferences.thumbnailSize;
    setWindowTitle("Nova Studio");
    setWindowIcon(applicationIcon());
    if (!project.isEmpty()) {
        auto detected = Project::inspect(project);
        setWindowTitle("Nova Studio · " + detected.name +
                       (detected.kind == Project::Kind::ResourcePack ? " · Resource Pack"
                                                                     : " · Folder / needs review"));
    }
    setObjectName("novaWorkspace");
    connect(&session, &DocumentSession::changed, this, [this](QUuid id) {
        refresh();
        for (int i = 0; i < tabs->count(); ++i) {
            auto canvas = dynamic_cast<Canvas *>(tabs->widget(i));
            if (!canvas)
                continue;
            const auto &doc = canvas->document;
            if (canvas == current() || doc->id() != id)
                continue;
            tabs->setTabText(i, QFileInfo(doc->path).fileName() + (documentDirty(*doc) ? " ●" : "") +
                                (doc->nativeDocument() && doc->exportModified() ? " ↑PNG" : ""));
            canvas->update();
        }
    });
    connect(&session, &DocumentSession::closed, this, [this] { updateUnsavedStatus(); });
    resize(1280, 820);
    setMinimumSize(800, 550);
    setDockNestingEnabled(true);
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks);
    setAnimated(false);
    // One application bar: identity, document commands, project location and
    // workspace controls (filled by buildAppBar() once every action exists).
    appBar = addToolBar("Application Bar");
    appBar->setObjectName("commands");
    appBar->setMovable(false);
    appBar->setIconSize({ui::IconSize, ui::IconSize});
    auto brand = new QWidget;
    auto brandLayout = new QHBoxLayout(brand);
    brandLayout->setContentsMargins(6, 0, 10, 0);
    brandLayout->setSpacing(6);
    auto logo = new QLabel;
    logo->setObjectName("toolbarLogo");
    logo->setPixmap(logoIcon().pixmap(QSize(22, 22), devicePixelRatioF()));
    logo->setAccessibleName("Nova Studio logo");
    brandLayout->addWidget(logo);
    auto brandName = new QLabel("Nova Studio");
    brandName->setStyleSheet("font-weight:600");
    brandLayout->addWidget(brandName);
    appBar->addWidget(brand);
    // Undo / Redo exist once: the application bar and Edit menu share them.
    auto undoAction = new QAction("Undo", this);
    undoAction->setObjectName("editUndo");
    // Explicit Ctrl+Z: the Windows standard binding also includes Alt+Backspace,
    // which Nova assigns to Fill with Foreground.
    undoAction->setShortcut(QKeySequence("Ctrl+Z"));
    undoAction->setToolTip("Undo (Ctrl+Z)");
    undoAction->setProperty("novaIcon", "undo");
    connect(undoAction, &QAction::triggered, this, [this] { action("undo"); });
    auto redoAction = new QAction("Redo", this);
    redoAction->setObjectName("editRedo");
    redoAction->setShortcuts({QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")});
    redoAction->setToolTip("Redo (Ctrl+Y)");
    redoAction->setProperty("novaIcon", "redo");
    connect(redoAction, &QAction::triggered, this, [this] { action("redo"); });
    addAction(undoAction);
    addAction(redoAction);
    auto file = menuBar()->addMenu("&File");
    file->setObjectName("menu-File");
    auto newPack = file->addAction("New Resource Pack…", QKeySequence("Ctrl+N"));
    newPack->setObjectName("newProjectCommand");
    connect(newPack, &QAction::triggered, this, [this] {
        auto path = newProjectDialog(this);
        if (!path.isEmpty()) {
            pathText->setText(path);
            emit pathText->returnPressed();
        }
    });
    auto openPack = file->addAction("Open Resource Pack…");
    connect(openPack, &QAction::triggered, this, [this] {
        auto path = QFileDialog::getExistingDirectory(this, "Open Resource Pack", project);
        if (!path.isEmpty()) {
            pathText->setText(path);
            emit pathText->returnPressed();
        }
    });
    auto propertiesAction = file->addAction("Project Properties…");
    propertiesAction->setObjectName("projectPropertiesCommand");
    connect(propertiesAction, &QAction::triggered, this, [this] {
        if (!project.isEmpty())
            projectPropertiesDialog(this, project);
    });
    auto closeProject = file->addAction("Close Project");
    auto exportPack = file->addAction("Export Minecraft Pack…");
    exportPack->setObjectName("exportPackCommand");
    connect(exportPack, &QAction::triggered, this, [this] {
        if (project.isEmpty())
            return;
        for (const auto &doc : session.all())
            if (doc->minecraftOutputPending()) {
                QMessageBox::information(
                    this, "Update textures first",
                    "Save the modified documents first (File › Save All), then export the pack.");
                return;
            }
        const auto target = QFileDialog::getSaveFileName(
            this, "Export Minecraft pack to a new folder", project + "-minecraft");
        if (target.isEmpty())
            return;
        QProgressDialog progress("Exporting Minecraft pack…", QString(), 0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(200);
        QFutureWatcher<QString> watcher;
        QEventLoop loop;
        connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
        const auto root = project;
        watcher.setFuture(QtConcurrent::run(&ioPool, [root, target] {
            QString error;
            Project::exportMinecraft(root, target, &error);
            return error;
        }));
        if (!watcher.isFinished())
            loop.exec();
        progress.close();
        if (!watcher.result().isEmpty())
            QMessageBox::warning(this, "Pack export failed", watcher.result());
        else
            statusBar()->showMessage("Minecraft pack exported without editor metadata", 6000);
    });
    closeProject->setObjectName("closeProjectCommand");
    connect(closeProject, &QAction::triggered, this, [this] {
        if (modelPanel && !modelPanel->confirmClose())
            return;
        for (int i = 0; i < tabs->count(); ++i)
            if (!confirmClose(dynamic_cast<Canvas *>(tabs->widget(i))))
                return;
        modelPanel->discardModelRecovery();
        while (tabs->count()) {
            auto page = dynamic_cast<Canvas *>(tabs->widget(0));
            if (page) {
                recovery->remove(page->document->id());
                session.close(page->document);
            }
            removeDocumentTab(0);
        }
        if (indexCancel)
            indexCancel->store(true);
        ++indexGeneration;
        indexBusy = false;
        monitor.stop();
        indexDebounce->stop();
        reconciliationTimer->stop();
        pendingChanges.clear();
        fullRescan = invalidateFallback = false;
        if (mcpService)
            mcpService->projectChanged();
        endAgentSessionForProjectChange();
        project.clear();
        clearComparisons();
        if (auto related = findChild<QDialog *>("relatedAssetsDialog"))
            related->close();
        session.setProject({});
        if(modelCancel)modelCancel->store(true);
        ++modelGeneration;
        modelLibrary=std::make_shared<mc::Library>();
        modelPanel->setLibrary(modelLibrary,{});
        pathText->clear();
        collection = {};
        ++collectionSerial;
        resetBaselineState();
        nav = {};
        model->setIndex(collection, {});
        cube->setDocument({});
        refresh();
        setWindowTitle("Nova Studio · No project");
    });
    openPack->setShortcut(QKeySequence("Ctrl+Shift+O"));
    openPack->setObjectName("openProjectCommand");
    auto openPng = file->addAction("Open Texture…", QKeySequence("Ctrl+O"));
    openPng->setObjectName("openTextureCommand");
    connect(openPng, &QAction::triggered, this, [this] {
        auto path =
            QFileDialog::getOpenFileName(this, "Open PNG texture", project, "PNG images (*.png)");
        if (!path.isEmpty())
            openTexture(path);
    });
    auto openModel=file->addAction("Open Model / Item Definition…");
    openModel->setObjectName("openMinecraftModel");
    connect(openModel,&QAction::triggered,this,[this]{
        auto path=QFileDialog::getOpenFileName(this,"Inspect pack model, blockstate or item definition",project,"Minecraft JSON (*.json)");
        if(path.isEmpty())return;
        const auto asset=QDir(project).relativeFilePath(path);
        if(mc::Location::fromAsset(asset,"models").isEmpty()&&mc::Location::fromAsset(asset,"items").isEmpty()&&mc::Location::fromAsset(asset,"blockstates").isEmpty()){statusBar()->showMessage("Choose JSON under this pack's assets/<namespace>/models, blockstates or items folder",8000);return;}
        openModelAsset(asset);
    });
    for (auto pair : {std::pair{"Save", "Ctrl+S"}, {"Save As…", "Ctrl+Shift+S"}}) {
        auto a = file->addAction(pair.first, QKeySequence(pair.second));
        a->setObjectName(QString(pair.first).startsWith("Save As") ? "saveAsCommand"
                                                                   : "saveCommand");
        connect(a, &QAction::triggered, this,
                [this, name = QString(pair.first)] { action(name == "Save" ? "save" : "saveAs"); });
    }
    // Save All is new; it takes a free shortcut (Ctrl+Alt+S stays Save Workspace).
    auto saveAllAction = file->addAction("Save All", QKeySequence("Ctrl+Alt+Shift+S"));
    saveAllAction->setObjectName("saveAllCommand");
    saveAllAction->setStatusTip("Save every unsaved document of this project to the pack");
    connect(saveAllAction, &QAction::triggered, this, [this] { saveAll(); });
    auto saveEditable = file->addAction("Save Editable Document");
    saveEditable->setObjectName("saveNativeCommand");
    connect(saveEditable, &QAction::triggered, this, [this] { action("saveNative"); });
    auto updatePng = file->addAction("Update Minecraft PNG", QKeySequence("Ctrl+Alt+E"));
    updatePng->setObjectName("exportPngCommand");
    connect(updatePng, &QAction::triggered, this, [this] { action("exportPng"); });
    auto recent = file->addMenu("Recent Projects");
    recentMenu = recent;
    for (const auto &path : preferences.recentProjects) {
        auto a = recent->addAction(path);
        connect(a, &QAction::triggered, this, [this, path] {
            pathText->setText(path);
            emit pathText->returnPressed();
        });
    }
    recent->setEnabled(!preferences.recentProjects.isEmpty());
    auto review = file->addAction("Review External Change…");
    review->setObjectName("reviewConflictCommand");
    connect(review, &QAction::triggered, this, &Window::conflictDialog);
    auto closeTab = file->addAction("Close Document");
    closeTab->setObjectName("closeDocumentCommand");
    closeTab->setShortcuts({QKeySequence("Ctrl+W"), QKeySequence("Ctrl+F4")});
    connect(closeTab, &QAction::triggered, this, [this] {
        if (tabs->currentWidget())
            emit tabs->tabCloseRequested(tabs->currentIndex());
    });
    auto quit = file->addAction("Exit");
    quit->setObjectName("exitCommand");
    quit->setShortcut(QKeySequence("Alt+F4"));
    connect(quit, &QAction::triggered, this, &QWidget::close);
    // Final File order: project · document · save/export · pack · exit.
    for (auto a : file->actions()) {
        file->removeAction(a);
        if (a->isSeparator())
            delete a;
    }
    for (QAction *a : std::initializer_list<QAction *>{
             newPack, openPack, recent->menuAction(), closeProject, nullptr, openPng, openModel, nullptr,
             findChild<QAction *>("saveCommand"), findChild<QAction *>("saveAsCommand"), saveAllAction, saveEditable,
             updatePng, closeTab, nullptr, exportPack, propertiesAction, review, nullptr, quit})
        if (a)
            file->addAction(a);
        else
            file->addSeparator();
    findChild<QAction *>("saveCommand")->setProperty("novaIcon", "save");
    findChild<QAction *>("saveCommand")->setToolTip("Save to the pack (Ctrl+S)");
    openPack->setProperty("novaIcon", "open");
    auto view = menuBar()->addMenu("&View");
    view->setObjectName("menu-View");
    auto saveLayout = new QAction("Save Workspace", this);
    saveLayout->setObjectName("saveWorkspaceCommand");
    saveLayout->setShortcut(QKeySequence("Ctrl+Alt+S"));
    auto reset = new QAction("Reset Workspace", this);
    reset->setObjectName("resetWorkspaceCommand");
    connect(reset, &QAction::triggered, this, &Window::resetWorkspace);
    connect(saveLayout, &QAction::triggered, this, [this] { persist(); });
    auto theme = new QAction("Dark Theme", this);
    themeAction = theme;
    theme->setObjectName("darkThemeCommand");
    theme->setCheckable(true);
    theme->setChecked(dark);
    connect(theme, &QAction::toggled, this, [this](bool value) {
        dark = value;
        applyTheme();
    });
    auto accentGroup = new QActionGroup(this);
    for (auto name : {"violet", "blue"}) {
        auto a = new QAction(QString(name) == "violet" ? "Violet Accent" : "Blue Accent", this);
        a->setObjectName(QString("accent-") + name);
        a->setCheckable(true);
        accentGroup->addAction(a);
        a->setChecked(preferences.accent == name);
        connect(a, &QAction::triggered, this, [this, name] {
            preferences.accent = name;
            applyTheme();
        });
    }
    panelsMenu = new QMenu("&Panels", this);
    panelsMenu->setObjectName("panelsMenu");
    pathText = new QLineEdit(project);
    pathText->setMinimumWidth(120);
    pathText->setMaximumWidth(460);
    pathText->setObjectName("projectPath");
    pathText->setAccessibleName("Resource pack folder");
    pathText->setPlaceholderText("Resource pack folder…");
    pathText->setToolTip("Resource pack folder · type a path and press Enter, or use Open Resource Pack (Ctrl+Shift+O)");
    auto open = new QAction("Go", this);
    open->setObjectName("openTypedProject");
    open->setToolTip("Open the folder typed in the project field");
    connect(open, &QAction::triggered, pathText, &QLineEdit::returnPressed);
    connect(pathText, &QLineEdit::returnPressed, this, [this] {
        if (!QFileInfo(pathText->text()).isDir()) {
            statusBar()->showMessage("Project directory does not exist");
            return;
        }
        if (modelPanel && !modelPanel->confirmClose())
            return;
        for (int i = 0; i < tabs->count(); ++i)
            if (!confirmClose(dynamic_cast<Canvas *>(tabs->widget(i))))
                return;
        persist();
        modelPanel->discardModelRecovery();
        project = QDir(pathText->text()).canonicalPath();
        if (mcpService)
            mcpService->projectChanged();
        endAgentSessionForProjectChange();
        auto detected = Project::inspect(project);
        setWindowTitle("Nova Studio · " + detected.name +
                       (detected.kind == Project::Kind::ResourcePack ? " · Resource Pack"
                                                                     : " · Folder / needs review"));
        preferences.recentProjects.removeAll(project);
        preferences.recentProjects.prepend(project);
        updateRecent();
        while (tabs->count()) {
            if (auto page = dynamic_cast<Canvas *>(tabs->widget(0))) {
                recovery->remove(page->document->id());
                session.close(page->document);
            }
            removeDocumentTab(0);
        }
        cube->setDocument({});
        nav = {};
        indexProject();
    });
    tabs = new EditorTabs(newPack, openPack, nullptr, recent);
    tabs->setObjectName("editorTabs");
    tabs->setDocumentMode(true);
    tabs->setTabsClosable(true);
    tabs->setMovable(true);
    setCentralWidget(tabs);
    connect(tabs, &QTabWidget::currentChanged, this, [this] {
        QElapsedTimer selection;
        selection.start();
        refresh();
        metrics.record("document_tab_selection", selection);
    });
    connect(tabs, &QTabWidget::tabCloseRequested, this, [this](int i) {
        if (auto page = dynamic_cast<ModelTab *>(tabs->widget(i))) {
            const auto asset = page->doc->assetPath();
            if (!modelPanel->closeModel(page->doc))
                return;
            removeDocumentTab(i);
            model->applyIndex(browserIndex(), {asset});
            setDirectory(nav.directory);
            refresh();
            return;
        }
        auto canvas = dynamic_cast<Canvas *>(tabs->widget(i));
        if (!confirmClose(canvas))
            return;
        recovery->remove(canvas->document->id());
        const bool provisional = !canvas->document->revision().exists;
        const auto path = QDir(project).relativeFilePath(canvas->document->path);
        baselineDocs.remove(canvas->document->id());
        session.close(canvas->document);
        if (provisional) {
            model->applyIndex(browserIndex(), {path});
            setDirectory(nav.directory);
        }
        clearComparisons(path);
        tabs->removeTab(i);
        delete canvas;
        refresh();
    });
    QElapsedTimer thumbnailInitialization;
    thumbnailInitialization.start();
    model = new AssetModel(this);
    // Baseline rows: muted (vanilla, not in the project) and accent (project override), read at paint time.
    model->setOriginForeground([](int origin) {
        const auto &tokens = ui::current();
        return QVariant::fromValue(origin == AssetModel::BaselineRow ? tokens.muted : tokens.accent);
    });
    metrics.record("thumbnail_service_init", thumbnailInitialization);
    proxy = new QSortFilterProxyModel(this);
    proxy->setSourceModel(model);
    proxy->setFilterRole(Qt::UserRole);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy->setRecursiveFilteringEnabled(true);
    tree = new QTreeView;
    tree->setObjectName("assetTree");
    tree->setAccessibleName("Project assets");
    tree->setModel(proxy);
    tree->setHeaderHidden(true);
    tree->setUniformRowHeights(true);
    tree->setIconSize({18, 18});
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    grid = new QListView;
    grid->setObjectName("assetGrid");
    grid->setAccessibleName("Asset thumbnails");
    grid->setModel(proxy);
    grid->setViewMode(QListView::IconMode);
    grid->setResizeMode(QListView::Adjust);
    grid->setMovement(QListView::Static);
    grid->setUniformItemSizes(true);
    grid->setIconSize({thumbSize, thumbSize});
    grid->setGridSize({thumbSize + 28, thumbSize + 42});
    grid->setWordWrap(false);
    grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid->setLayoutMode(QListView::Batched);
    grid->setBatchSize(100);
    browser = new QStackedWidget;
    browser->addWidget(tree);
    browser->addWidget(grid);
    auto assetWidget = new QWidget;
    auto av = new QVBoxLayout(assetWidget);
    av->setContentsMargins(ui::SpaceL, ui::SpaceM, ui::SpaceL, ui::SpaceM);
    av->setSpacing(ui::SpaceM);
    auto modes = new QHBoxLayout;
    modes->setSpacing(0);
    for (auto name : {"Tree", "Grid"}) {
        auto b = new QPushButton(name);
        b->setObjectName(QString("assetMode") + name);
        b->setProperty("role", "segment");
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setChecked((QString(name) == "Grid") == nav.grid);
        b->setToolTip(QString(name) == "Grid" ? "Thumbnail grid of the current folder" : "Folder tree");
        b->setProperty("novaIcon", QString(name).toLower());
        b->setIconSize({14, 14});
        modes->addWidget(b);
        connect(b, &QPushButton::clicked, this,
                [this, g = QString(name) == "Grid"] { setMode(g); });
    }
    modes->addStretch();
    av->addLayout(modes);
    auto categories = new QComboBox;
    categories->setObjectName("assetCategory");
    categories->setAccessibleName("Asset category");
    categories->addItem("Physical folders");
    categories->addItems({"Blocks", "Items", "Entities", "GUI", "Particles", "Environment",
                          "Models", "Blockstates", "Fonts", "Sounds", "Animations",
                          "Other assets"});
    categories->setToolTip("Browse by physical folder or by Minecraft asset category");
    connect(categories, &QComboBox::currentTextChanged, this, [this](const QString &category) {
        model->setCategory(category == "Physical folders" ? QString() : category);
        proxy->setFilterFixedString(search->text());
        proxy->invalidate();
        setDirectory("");
        tree->expandAll();
    });
    search = new QLineEdit;
    search->setObjectName("assetSearch");
    search->setAccessibleName("Search project assets");
    search->setClearButtonEnabled(true);
    search->setPlaceholderText("Search assets…");
    search->addAction(ui::lineIcon("search"), QLineEdit::LeadingPosition)->setProperty("novaIcon", "search");
    av->addWidget(search);
    av->addWidget(categories);
    connect(search, &QLineEdit::textChanged, this, [this](const QString &query) {
        nav.query = query;
        proxy->setFilterFixedString(query);
        setDirectory(nav.directory);
    });
    auto crumbs = new QHBoxLayout;
    crumbs->setSpacing(ui::SpaceS);
    auto up = ui::iconButton("parent", "Go to parent folder", "assetParent");
    up->setAccessibleName("Parent folder");
    crumbs->addWidget(up);
    breadcrumb = new QLabel;
    breadcrumb->setObjectName("assetBreadcrumb");
    breadcrumb->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    crumbs->addWidget(breadcrumb, 1);
    auto sort = new QPushButton("A–Z");
    sort->setObjectName("assetSort");
    sort->setProperty("role", "segment");
    sort->setToolTip("Sort order");
    sort->setAccessibleName("Sort order");
    modes->addWidget(sort);
    av->addLayout(crumbs);
    connect(up, &QPushButton::clicked, this, [this] {
        auto d = nav.directory;
        int slash = d.lastIndexOf('/');
        setDirectory(slash < 0 ? "" : d.left(slash));
    });
    connect(sort, &QPushButton::clicked, this, [this, sort] {
        nav.reverse = !nav.reverse;
        proxy->sort(0, nav.reverse ? Qt::DescendingOrder : Qt::AscendingOrder);
        sort->setText(nav.reverse ? "Z–A" : "A–Z");
    });
    auto slider = new QSlider(Qt::Horizontal);
    slider->setRange(48, 128);
    slider->setValue(thumbSize);
    slider->setObjectName("thumbnailSize");
    slider->setAccessibleName("Thumbnail size");
    slider->setToolTip("Thumbnail size");
    slider->setVisible(nav.grid); // only meaningful for the grid
    av->addWidget(slider);
    connect(slider, &QSlider::valueChanged, this, [this](int size) {
        thumbSize = size;
        grid->setIconSize({size, size});
        grid->setGridSize({size + 28, size + 42});
    });
    av->addWidget(browser, 1);
    assetsDock = new QDockWidget("Assets", this);
    assetsDock->setObjectName("assets");
    assetsDock->setWidget(assetWidget);
    addDockWidget(Qt::LeftDockWidgetArea, assetsDock);
    for (auto itemView :
         {static_cast<QAbstractItemView *>(tree), static_cast<QAbstractItemView *>(grid)}) {
        connect(itemView, &QAbstractItemView::doubleClicked, this,
                [this](const QModelIndex &index) { openAsset(index); });
        // Enter/Return opens the current asset for keyboard and screen-reader users. A
        // double-click also emits activated() with the button still down; that path
        // already opened it above.
        connect(itemView, &QAbstractItemView::activated, this, [this](const QModelIndex &index) {
            if (QApplication::mouseButtons() == Qt::NoButton)
                openAsset(index);
        });
        connect(itemView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex &index) { syncSelection(index); });
    }
    for (auto itemView :
         {static_cast<QAbstractItemView *>(tree), static_cast<QAbstractItemView *>(grid)}) {
        itemView->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(itemView, &QWidget::customContextMenuRequested, this, [this, itemView](QPoint pos) {
            auto index = itemView->indexAt(pos);
            if (!index.isValid())
                return;
            QMenu menu(this);
            auto open = menu.addAction("Open");
            auto related = menu.addAction("Related Assets…");
            related->setEnabled(!model->isDirectory(proxy->mapToSource(index)));
            auto copy = menu.addAction("Copy relative path");
            auto batchAction = menu.addAction("Batch Process…");
            auto vanillaAction = addBaselineContextAction(menu, model->pathFor(proxy->mapToSource(index)));
            auto selected = menu.exec(itemView->viewport()->mapToGlobal(pos));
            if (selected && selected == vanillaAction)
                triggerBaselineContextAction(vanillaAction);
            if (selected == open)
                openAsset(index);
            if (selected == related)
                showRelatedAssets(model->pathFor(proxy->mapToSource(index)));
            if (selected == batchAction) {
                syncSelection(index);
                batchDialog();
            }
            if (selected == copy)
                QApplication::clipboard()->setText(model->pathFor(proxy->mapToSource(index)));
        });
    }
    properties = nullptr; // replaced by the structured inspector (Phase 3-B)
    auto gridButton = new QCheckBox("Pixel grid");
    gridButton->setChecked(preferences.pixelGrid);
    gridButton->setObjectName("pixelGrid");
    gridButton->setToolTip("Show texture pixel boundaries at 600% zoom and above (Ctrl+')");
    pixelGridAction = new QAction("Pixel Grid", this);
    pixelGridAction->setObjectName("pixelGridCommand");
    pixelGridAction->setCheckable(true);
    pixelGridAction->setChecked(preferences.pixelGrid);
    pixelGridAction->setShortcut(QKeySequence("Ctrl+'"));
    connect(pixelGridAction, &QAction::toggled, gridButton, &QCheckBox::setChecked);
    connect(gridButton, &QCheckBox::toggled, this, [this](bool g) {
        preferences.pixelGrid = g;
        { QSignalBlocker block(pixelGridAction); pixelGridAction->setChecked(g); }
        for (int i = 0; i < tabs->count(); ++i) {
            auto c = dynamic_cast<Canvas *>(tabs->widget(i));
            if (!c)
                continue;
            c->grid = g;
            c->update();
        }
    });
    auto center = new QPushButton("Center canvas");
    center->setObjectName("centerCanvas");
    connect(center, &QPushButton::clicked, this, [this] {
        if (current())
            current()->center();
    });
    auto inspector = buildInspector(gridButton, center);
    inspectorDock = new QDockWidget("Inspector", this);
    inspectorDock->setObjectName("inspector");
    auto inspectorScroll = new QScrollArea;
    inspectorScroll->setWidgetResizable(true);
    inspectorScroll->setFrameShape(QFrame::NoFrame);
    inspectorScroll->setWidget(inspector);
    inspectorDock->setWidget(inspectorScroll);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);
    layersDock = new QDockWidget("Layers", this);
    layersDock->setObjectName("layers");
    layersPanel = new LayersPanel(&session);
    auto layersScroll = new QScrollArea;
    layersScroll->setWidgetResizable(true); layersScroll->setFrameShape(QFrame::NoFrame);
    layersScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layersScroll->setWidget(layersPanel); layersDock->setWidget(layersScroll);
    addDockWidget(Qt::RightDockWidgetArea, layersDock);
    splitDockWidget(inspectorDock, layersDock, Qt::Vertical);
    // Build editor widgets before the OpenGL preview exists: reparenting widgets into
    // a window that already contains a QOpenGLWidget costs ~120 ms at startup.
    buildEditor(panelsMenu);
    buildAnimation(panelsMenu);
    cube = new Cube(&metrics);
    connect(cube, &QOpenGLWidget::frameSwapped, this, [this] {
        if (!ready && cube->document && !telemetry.isEmpty()) {
            ready = true;
            auto image = cube->grabFramebuffer();
            if (!image.isNull()) {
                metrics.record("ready_capture_ms", started);
                metrics.write(telemetry);
            }
        }
    });
    previewDock = new QDockWidget("3D Preview", this);
    previewDock->setObjectName("preview");
    previewDock->setToolTip("Drag to orbit · scroll to zoom · click a face to select it");
    cube->setToolTip(previewDock->toolTip());
    auto previewHost = new QWidget;
    auto previewLayout = new QVBoxLayout(previewHost);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->addWidget(cube);
    previewDock->setWidget(previewHost);
    addDockWidget(Qt::BottomDockWidgetArea, previewDock);
    modelPanel = new ModelPanel(cube, &session);
    modelPanel->modelRegistered = [this](const QString &path) {
        QElapsedTimer index;
        index.start();
        model->applyIndex(browserIndex(), {path});
        metrics.record("model_publication_tree_refresh", index);
        nav.selected = path;
        setDirectory(QFileInfo(path).path());
        openModelAsset(path);
    };
    modelPanel->openWorkspaceModel = [this](const QString &asset) { openModelAsset(asset); };
    modelPanel->modelOpened = [this](const auto &doc) { showModelDocument(doc, false); };
    modelPanel->modelChanged = [this] {
        updateModelTabs();
        updateUnsavedStatus();
    };
    modelPanel->modelCheckpointed = [this](const QElapsedTimer &timer) {
        metrics.record("model_recovery_serialize_and_write", timer);
    };
    modelPanel->openTexture = [this](const QString &path) { openTexture(path); };
    modelPanel->navigate = [this](const QString &path) { nav.selected = path; setDirectory(QFileInfo(path).path()); };
    modelsDock = new QDockWidget("Models / UV", this);
    modelsDock->setObjectName("modelsUv");
    auto modelScroll = new QScrollArea;
    modelScroll->setWidgetResizable(true); modelScroll->setFrameShape(QFrame::NoFrame);
    modelScroll->setWidget(modelPanel); modelsDock->setWidget(modelScroll);
    addDockWidget(Qt::RightDockWidgetArea, modelsDock);
    tabifyDockWidget(inspectorDock, modelsDock); inspectorDock->raise();
    buildResourceTools(panelsMenu);
    buildSupportMenu();
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    resizeDocks({assetsDock, inspectorDock}, {270, 300}, Qt::Horizontal);
    resizeDocks({previewDock}, {200}, Qt::Vertical);
    defaultLayout = saveState(1);
    QElapsedTimer stage;
    stage.start();
    buildWorkspaceMenu(view);
    buildAppBar();
    organizeMenus();
    metrics.record("workspace_shell", stage);
    stage.restart();
    applyTheme();
    metrics.record("theme_apply", stage);
    metrics.record("ui_construction", setup);
    restore();
    if (qApp->arguments().contains("--no-preview"))
        previewDock->hide();
    if (qApp->arguments().contains("--float-preview"))
        previewDock->setFloating(true);
    monitor.setParent(this);
    indexDebounce = new QTimer(this);
    indexDebounce->setObjectName("assetChangeDebounce");
    indexDebounce->setSingleShot(true);
    indexDebounce->setInterval(150);
    connect(indexDebounce, &QTimer::timeout, this, &Window::updateIndex);
    connect(&monitor, &DirectoryMonitor::changed, this, [this](QStringList paths) {
        paths.removeIf([](const QString &path) {
            return path.compare(".nova", Qt::CaseInsensitive) == 0 ||
                   path.startsWith(".nova/", Qt::CaseInsensitive);
        });
        if (paths.isEmpty())
            return;
        pendingChanges.append(paths);
        if (pendingChanges.size() > 4096) {
            fullRescan = true;
            pendingChanges.clear();
        }
        indexDebounce->start();
    });
    connect(&monitor, &DirectoryMonitor::uncertain, this, [this] {
        fullRescan = true;
        invalidateFallback = true;
        indexDebounce->start(500);
    });
    indexProject();
    auto reconcile = new QTimer(this);
    reconciliationTimer = reconcile;
    reconcile->setObjectName("assetReconciliation");
    reconcile->setInterval(30000);
    connect(reconcile, &QTimer::timeout, this, [this] {
        if (!project.isEmpty()) {
            if (monitor.active()) {
                metrics.count("native_reconciliation_skips", 1);
                return;
            }
            fullRescan = true;
            indexDebounce->start();
        }
    });
    if (!project.isEmpty())
        reconcile->start();
    auto safetyTimer = new QTimer(this);
    revisionTimer = safetyTimer;
    safetyTimer->setObjectName("recoveryAndRevision");
    safetyTimer->setInterval(2000);
    connect(safetyTimer, &QTimer::timeout, this, &Window::backgroundSafety);
    if (!session.all().empty())
        safetyTimer->start();
    if (!fresh)
        QTimer::singleShot(800, this, &Window::recoverSession);
}
Window::~Window() {
    if (mcpService)
        mcpService->shutdown();
    // QWidget hides child windows during its base destructor, after our preview
    // and members have gone away. Timeline hide/stop must no longer call the
    // Window's displayed/stopped lambdas (which access the preview and tabs).
    if (timeline)
        disconnect(&timeline->playback, nullptr, this, nullptr);
    if (modelCancel) modelCancel->store(true);
    if (indexCancel)
        indexCancel->store(true);
    monitor.stop();
    ioPool.waitForDone();
    // GL cleanup records counters; destroy the widget before the Window's
    // Metrics member, rather than later in QObject's child teardown.
    delete cube;
    cube = nullptr;
}
void Window::qa(const QString &output) {
    qaMode = true;
    QDir().mkpath(output);
    auto timer = new QTimer(this);
    auto step = std::make_shared<int>(-2);
    connect(timer, &QTimer::timeout, this, [this, timer, step, output] {
        if (!current())
            return;
        int s = (*step)++;
        if (s < 0)
            return;
        QStringList names = {
            "01-default.png", "02-tree.png",       "03-grid.png",    "04-high-zoom.png",
            "05-preview.png", "06-rearranged.png", "07-compact.png", "08-native-floating.png",
            "09-light.png",   "10-inspector.png"};
        if (s % 2 == 0) {
            switch (s / 2) {
            case 0:
                if (collection.files.size() > 1) {
                    const auto original = current()->document->path;
                    openTexture(project + '/' + collection.files[1]);
                    openTexture(original);
                }
                break;
            case 1:
                setMode(false);
                setDirectory("");
                tree->expandAll();
                break;
            case 2:
                setMode(true);
                setDirectory(QFileInfo(nav.selected).path());
                break;
            case 3:
                current()->zoom = 40;
                current()->document->draw({2, 2}, {12, 10}, qRgba(130, 141, 255, 255));
                current()->document->finish();
                refresh();
                break;
            case 4:
                cube->setMinimumHeight(300);
                cube->update();
                break;
            case 5:
                addDockWidget(Qt::RightDockWidgetArea, previewDock);
                splitDockWidget(assetsDock, inspectorDock, Qt::Vertical);
                break;
            case 6:
                addDockWidget(Qt::LeftDockWidgetArea, assetsDock);
                addDockWidget(Qt::RightDockWidgetArea, inspectorDock);
                addDockWidget(Qt::BottomDockWidgetArea, previewDock);
                cube->setMinimumHeight(150);
                resizeDocks({previewDock}, {180}, Qt::Vertical);
                current()->zoom = 16;
                resize(960, 640);
                break;
            case 7:
                previewDock->setFloating(true);
                previewDock->resize(440, 320);
                metrics.count("native_float_confirmed", previewDock->isFloating() ? 1 : 0);
                break;
            case 8:
                resetWorkspace();
                resize(1280, 820);
                dark = false;
                themeAction->setChecked(false);
                applyTheme();
                break;
            case 9:
                break;
            default:
                break;
            }
        } else {
            int i = s / 2;
            if (i < names.size()) {
                auto image = i == 7 ? previewDock->grab() : i == 9 ? inspectorDock->grab() : grab();
                image.save(output + '/' + names[i]);
            } else if (i == 7) {
                previewDock->grab().save(output + "/08-native-floating.png");
            }
        }
        if (s >= 20) {
            timer->stop();
            for (int i = 0; i < tabs->count(); ++i) {
                auto canvas = dynamic_cast<Canvas *>(tabs->widget(i));
                if (!canvas)
                    continue;
                auto doc = canvas->document;
                while (doc->modified() && doc->canUndo())
                    doc->undo();
            }
            close();
        }
    });
    timer->start(450);
}
void Window::startExercise() {
    exercise = true;
    auto timer = new QTimer(this);
    auto frame = std::make_shared<int>(0);
    auto last = std::make_shared<QElapsedTimer>();
    last->start();
    connect(timer, &QTimer::timeout, this, [this, timer, frame, last] {
        if (!current())
            return;
        double seconds = started.elapsed() / 1000.0;
        int rate = seconds < 4 ? 60 : seconds < 8 ? 120 : 180;
        metrics.samples[QString("exercise_interval_%1").arg(rate)].append(last->nsecsElapsed() /
                                                                          1e6);
        last->restart();
        int i = ++*frame;
        auto doc = current()->document;
        QPoint p(i % doc->image.width(), (i / doc->image.width()) % doc->image.height());
        QElapsedTimer t;
        t.start();
        doc->draw(p, p, qRgba(i % 255, 130, 210, 255));
        doc->finish();
        metrics.record("brush_event_cpu", t);
        if (i % 10 == 0) {
            doc->undo();
            doc->redo();
        }
        current()->zoom = 12 + std::sin(i * .1) * 5;
        setMode(true);
        refresh();
        if (i % 40 == 0)
            resize(1100 + i % 200, 760);
        if (seconds >= 12) {
            timer->stop();
            for (int k = 0; k < tabs->count(); ++k) {
                auto canvas = dynamic_cast<Canvas *>(tabs->widget(k));
                if (!canvas)
                    continue;
                while (canvas->document->modified() && canvas->document->canUndo())
                    canvas->document->undo();
            }
            close();
        } else
            timer->setInterval(1000 / rate);
    });
    timer->setTimerType(Qt::PreciseTimer);
    timer->start(16);
}
void Window::openModelAsset(const QString &asset) {
    for (int i = 0; i < tabs->count(); ++i)
        if (auto page = dynamic_cast<ModelTab *>(tabs->widget(i)); page && page->doc->assetPath() == asset) {
            tabs->setCurrentIndex(i);
            modelsDock->show();
            modelsDock->raise();
            return;
        }
    if (mc::Location::fromAsset(asset, "models").isEmpty() ||
        !modelPanel->catalog()->editable(asset)) {
        modelPanel->openAsset(asset);
        previewDock->show();
    } else {
        QString error;
        if (auto doc = modelPanel->editableModel(asset, &error))
            showModelDocument(doc);
        else
            statusBar()->showMessage(error, 5000);
    }
    modelsDock->show();
    modelsDock->raise();
}

void Window::showModelDocument(const std::shared_ptr<mc::ModelEdit> &doc, bool activate) {
    ModelTab *page = nullptr;
    for (int i = 0; i < tabs->count(); ++i)
        if (auto candidate = dynamic_cast<ModelTab *>(tabs->widget(i)); candidate && candidate->doc->assetPath() == doc->assetPath())
            page = candidate;
    if (!page) {
        page = new ModelTab(doc);
        tabs->addTab(page, QFileInfo(doc->assetPath()).fileName());
    }
    page->doc = doc;
    if (activate)
        tabs->setCurrentWidget(page);
    updateModelTabs();
}

void Window::updateModelTabs() {
    for (int i = 0; i < tabs->count(); ++i)
        if (auto page = dynamic_cast<ModelTab *>(tabs->widget(i))) {
            tabs->setTabText(i, QFileInfo(page->doc->assetPath()).fileName() +
                                     (page->doc->modified() ? " ●" : ""));
            tabs->setTabToolTip(i, page->doc->assetPath());
        }
}

QString Window::tabPath(QWidget *page) const {
    if (auto canvas = dynamic_cast<Canvas *>(page))
        return canvas->document->path;
    if (auto modelTab = dynamic_cast<ModelTab *>(page))
        return project + '/' + modelTab->doc->assetPath();
    return {};
}

void Window::removeDocumentTab(int index) {
    auto page = tabs->widget(index);
    clearComparisons(QDir(project).relativeFilePath(tabPath(page)));
    if (cube->parentWidget() == page) {
        previewDock->widget()->layout()->addWidget(cube);
        cube->show();
        previewDock->show();
    }
    tabs->removeTab(index);
    delete page;
}

Canvas *Window::current() const { return dynamic_cast<Canvas *>(tabs->currentWidget()); }
void Window::applyTheme() {
    if (qApp->style()->objectName().compare("fusion", Qt::CaseInsensitive) != 0)
        qApp->setStyle("Fusion");
    const auto tokens = ui::tokens(dark, preferences.accent);
    ui::setCurrent(tokens);
    qApp->setPalette(ui::palette(tokens));
    qApp->setStyleSheet(ui::styleSheet(tokens));
    if (themeAction && themeAction->isChecked() != dark) {
        QSignalBlocker block(themeAction);
        themeAction->setChecked(dark);
    }
    QElapsedTimer icons;
    icons.start();
    refreshThemeIcons();
    metrics.record("theme_icons", icons);
}
void Window::indexProject() {
    if (reconciliationTimer) {
        if (project.isEmpty())
            reconciliationTimer->stop();
        else if (!reconciliationTimer->isActive())
            reconciliationTimer->start();
    }
    session.setProject(project);
    clearComparisons();
    if (auto related = findChild<QDialog *>("relatedAssetsDialog"))
        related->close();
    // Invalidate the previous project's model worker immediately. Waiting for the
    // new asset scan left the old editable catalog active during project switches.
    if (modelCancel)
        modelCancel->store(true);
    ++modelGeneration;
    modelLibrary = std::make_shared<mc::Library>();
    modelPanel->setLibrary(modelLibrary, project);
    if (indexCancel)
        indexCancel->store(true);
    indexBusy = true;
    auto generation = ++indexGeneration;
    indexCancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = indexCancel;
    pendingChanges.clear();
    fullRescan = false;
    monitor.stop();
    loadBaseline();
    collection = {};
    ++collectionSerial;
    model->setIndex(collection, project);
    if (project.isEmpty()) {
        indexBusy = false;
        statusBar()->showMessage("Open a resource-pack folder or PNG texture to begin");
        return;
    }
    statusBar()->showMessage("Indexing in background…");
    QString root = project;
    if (!monitor.watch(root))
        statusBar()->showMessage("Change monitor unavailable; periodic reconciliation enabled");
    auto watcher = new QFutureWatcher<std::pair<Index, double>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, root, generation] {
        auto [index, ms] = watcher->result();
        watcher->deleteLater();
        if (root != project || generation != indexGeneration)
            return;
        indexBusy = false;
        if (!index.error.isEmpty()) {
            statusBar()->showMessage(index.error);
            return;
        }
        collection = std::move(index);
        ++collectionSerial;
        restoreModelWorkspacePending = true;
        updateModelIndex();
        metrics.samples["index"].append(ms);
        QElapsedTimer install;
        install.start();
        model->setIndex(browserIndex(), project);
        metrics.record("initial_model_install", install);
        statusBar()->showMessage(
            QString("%1 assets indexed in %2 ms").arg(collection.files.size()).arg(ms, 0, 'f', 1));
        if (nav.directory.isEmpty() && !collection.files.isEmpty()) {
            nav.directory = QFileInfo(collection.files.first()).path();
            nav.selected = collection.files.first();
        }
        setDirectory(nav.directory);
        search->setText(nav.query);
        setMode(nav.grid);
        if (!fresh && preferences.projectPath == project) {
            for (const auto &path : preferences.openDocuments) {
                const auto asset = QDir(project).relativeFilePath(path);
                if (!mc::Location::fromAsset(asset, "models").isEmpty()) {
                    continue; // Restored after the model catalog is installed.
                } else if (QFileInfo::exists(path) || QFileInfo::exists(nativeDocumentPath(project, path)))
                    openTexture(path);
                else if (baselineCandidate(path)) // a vanilla texture opened but not yet saved
                    openBaselineTexture(QDir(project).relativeFilePath(path), path == preferences.activeDocument);
            }
            for (int i = 0; i < tabs->count(); ++i)
                if (tabPath(tabs->widget(i)) ==
                    preferences.activeDocument)
                    tabs->setCurrentIndex(i);
        }
        if (tabs->count() == 0)
            for (auto path : collection.files)
                if (path.endsWith(".png", Qt::CaseInsensitive)) {
                    openTexture(project + '/' + path);
                    break;
                }
    });
    watcher->setFuture(QtConcurrent::run([root, cancel] {
        QElapsedTimer t;
        t.start();
        auto index = Index::scan(root, cancel);
        return std::pair{std::move(index), t.nsecsElapsed() / 1e6};
    }));
}
void Window::setDirectory(QString d) {
    nav.directory = d == "." ? QString() : std::move(d);
    if (baseline.index ? !model->hasDirectory(nav.directory) : !collection.directories.contains(nav.directory))
        nav.directory.clear();
    QSignalBlocker a(tree->selectionModel()), b(grid->selectionModel());
    auto index = proxy->mapFromSource(model->indexFor(nav.directory));
    tree->setRootIndex({});
    grid->setRootIndex(index);
    breadcrumb->setText(nav.directory.isEmpty() ? "Project" : QFileInfo(nav.directory).fileName());
    breadcrumb->setToolTip(nav.directory);
    auto selected = proxy->mapFromSource(model->indexFor(nav.selected));
    if (selected.isValid()) {
        for (auto parent = selected.parent(); parent.isValid(); parent = parent.parent())
            tree->expand(parent);
        tree->setCurrentIndex(selected);
        grid->setCurrentIndex(selected);
        tree->scrollTo(selected);
    }
}
void Window::setMode(bool g) {
    nav.grid = g;
    browser->setCurrentIndex(g ? 1 : 0);
    for (auto button : assetsDock->findChildren<QPushButton *>())
        if (button->text() == "Tree" || button->text() == "Grid")
            button->setChecked((button->text() == "Grid") == g);
    if (auto slider = assetsDock->findChild<QSlider *>("thumbnailSize"))
        slider->setVisible(g);
    auto selected = proxy->mapFromSource(model->indexFor(nav.selected));
    QSignalBlocker a(tree->selectionModel()), b(grid->selectionModel());
    if (selected.isValid()) {
        for (auto parent = selected.parent(); parent.isValid(); parent = parent.parent())
            tree->expand(parent);
        tree->setCurrentIndex(selected);
        grid->setCurrentIndex(selected);
        tree->scrollTo(selected);
    }
}
void Window::syncSelection(const QModelIndex &index) {
    if (!index.isValid())
        return;
    auto source = proxy->mapToSource(index);
    nav.selected = model->pathFor(source);
    auto directory = model->isDirectory(source) ? nav.selected : QFileInfo(nav.selected).path();
    setDirectory(directory == "." ? "" : directory);
}
void Window::openAsset(const QModelIndex &index) {
    auto source = proxy->mapToSource(index);
    if (model->isDirectory(source))
        setDirectory(model->pathFor(source));
    else if (model->pathFor(source).endsWith(".png", Qt::CaseInsensitive))
        openTexture(project + '/' + model->pathFor(source));
    else if (!mc::Location::fromAsset(model->pathFor(source), "models").isEmpty() ||
             !mc::Location::fromAsset(model->pathFor(source), "items").isEmpty() ||
             !mc::Location::fromAsset(model->pathFor(source), "blockstates").isEmpty()) {
        openModelAsset(model->pathFor(source));
    } else
        statusBar()->showMessage(
            "This asset type is indexed; editing is limited to PNG and pack metadata");
}
void Window::openTexture(const QString &path) {
    if (!pendingAnimationPairs(path).isEmpty()) recoverAnimationExport(path);
    for (int i = 0; i < tabs->count(); ++i)
        if (tabPath(tabs->widget(i)) == path) {
            tabs->setCurrentIndex(i);
            nav.selected = QDir(project).relativeFilePath(path);
            if (model->indexFor(nav.selected).isValid())
                setDirectory(QFileInfo(nav.selected).path());
            return;
        }
    // A vanilla texture the project does not contain opens as a virtual baseline document.
    if (baselineCandidate(path) && !QFileInfo::exists(path) && !QFileInfo(path).isSymLink() &&
        !QFileInfo::exists(nativeDocumentPath(project, path))) {
        openBaselineTexture(QDir(project).relativeFilePath(path));
        return;
    }
    QString error;
    auto doc = session.open(path, &error);
    if (!doc) {
        statusBar()->showMessage(error);
        return;
    }
    showTextureDocument(doc);
}
void Window::showTextureDocument(const std::shared_ptr<Document> &doc) {
    const auto path = doc->path;
    for (int i = 0; i < tabs->count(); ++i)
        if (auto canvas = dynamic_cast<Canvas *>(tabs->widget(i)); canvas && canvas->document == doc) {
            tabs->setCurrentIndex(i);
            return;
        }
    doc->setHistoryLimits(qint64(preferences.historyMiB) * 1024 * 1024, 2048);
    auto canvas = new Canvas(doc, &metrics, nullptr, &toolState);
    configureCanvas(canvas);
    canvas->grid = preferences.pixelGrid;
    tabs->setTabToolTip(tabs->addTab(canvas, QFileInfo(path).fileName()), path);
    canvas->changed = [this, doc] { session.notify(doc); };

    tabs->setCurrentWidget(canvas);
    // Once laid out, shrink the initial zoom if the texture would overflow the
    // visible canvas (the default 384 px target ignores the window size).
    QTimer::singleShot(0, canvas, [this, canvas] {
        const auto size = canvas->document->image.size();
        double fit = std::min((canvas->width() - 32.0) / std::max(1, size.width()),
                              (canvas->height() - 56.0) / std::max(1, size.height()));
        if (fit >= 1)
            fit = std::floor(fit); // whole multiples keep texture pixels uniform
        if (fit > 0 && canvas->zoom > fit) {
            canvas->zoom = std::max(0.25, fit);
            canvas->center();
            if (zoomLabel && current() == canvas)
                zoomLabel->setText(QString("%1%").arg(qRound(canvas->zoom * 100)));
        }
    });
    nav.selected = QDir(project).relativeFilePath(doc->path);
    if (model->indexFor(nav.selected).isValid())
        setDirectory(QFileInfo(nav.selected).path());
    metrics.count("documents_opened", 1);
    refresh();
}
void Window::refresh() {
    if (revisionTimer) {
        if (session.all().empty())
            revisionTimer->stop();
        else if (!revisionTimer->isActive())
            revisionTimer->start();
    }
    auto canvas = current();
    layersPanel->setDocument(canvas ? canvas->document : nullptr);
    editorRefresh();
    QElapsedTimer animationStage;
    animationStage.start();
    animationRefresh();
    if (canvas && canvas->document->animated())
        metrics.record("animation_timeline_sync", animationStage);
    QElapsedTimer viewportStage;
    viewportStage.start();
    if (canvas) {
        cube->setDocument(canvas->document);
        tabs->setTabText(
            tabs->indexOf(canvas),
            QFileInfo(canvas->document->path).fileName() +
                (documentDirty(*canvas->document) ? " ●" : "") +
                (canvas->document->nativeDocument() && canvas->document->exportModified() ? " ↑PNG"
                                                                                          : ""));
        tabs->setTabToolTip(tabs->indexOf(canvas), canvas->document->path);
        if (auto save = findChild<QAction *>("saveCommand"))
            save->setText("Save");
        if (auto saveAs = findChild<QAction *>("saveAsCommand"))
            saveAs->setText(canvas->document->nativeDocument() ? "Export PNG As…" : "Save As…");
        canvas->update();
    } else {
        cube->setDocument({});
    }
    updateInspector();
    updateUnsavedStatus();
    auto page = dynamic_cast<ModelTab *>(tabs->currentWidget());
    if (auto previous = dynamic_cast<ModelTab *>(cube->parentWidget()); previous && previous != page)
        previous->face = cube->selectedFace;
    QElapsedTimer dockSync;
    dockSync.start();
    if (modelPanel)
        modelPanel->setDocument(canvas ? canvas->document : nullptr, canvas);
    metrics.record("document_model_dock_sync", dockSync);
    if (modelPanel)
        modelPanel->setModelTabActive(page != nullptr);
    if (toolBox)
        toolBox->setEnabled(page == nullptr);
    if (toolOptions)
        toolOptions->setEnabled(page == nullptr);
    for (const auto name : {"saveAsCommand", "exportPngCommand"})
        if (auto command = findChild<QAction *>(name))
            command->setEnabled(page == nullptr);
    if (page) {
        const bool moveViewport = cube->parentWidget() != page;
        const bool selectModel = modelPanel->modelAsset() != page->doc->assetPath();
        QElapsedTimer activation;
        if (moveViewport || selectModel)
            activation.start();
        if (moveViewport) {
            page->layout()->addWidget(cube);
            cube->show();
            previewDock->hide();
        }
        if (selectModel) {
            QElapsedTimer modelSync;
            modelSync.start();
            cube->selectedFace = page->face;
            modelPanel->openAsset(page->doc->assetPath(), false);
            metrics.record("model_activation_dock_rebuild", modelSync);
        }
        if (nav.selected != page->doc->assetPath()) {
            nav.selected = page->doc->assetPath();
            setDirectory(QFileInfo(nav.selected).path());
        }
        if (auto save = findChild<QAction *>("saveCommand")) {
            save->setText("Save Model JSON");
            save->setEnabled(true);
        }
        updateModelTabs();
        if (activation.isValid())
            metrics.record("model_tab_activation", activation);
    } else if (previewDock && cube->parentWidget() != previewDock->widget()) {
        previewDock->widget()->layout()->addWidget(cube);
        cube->show();
        previewDock->show();
    }
    if (canvas && canvas->document->animated())
        metrics.record("animation_viewport_refresh", viewportStage);
}
void Window::action(const QString &name) {
    // Reset to Vanilla history when the asset browser has focus (baseline projects only).
    if ((name == "undo" || name == "redo") && baseline.ref && browserHasFocus() &&
        (name == "redo" ? resetCursor < resetHistory.size() : resetCursor > 0)) {
        baselineHistory(name == "redo");
        return;
    }
    if (auto page = dynamic_cast<ModelTab *>(tabs->currentWidget())) {
        if (name == "undo" || name == "redo") {
            if (!modelPanel->authoringModel(page->doc->assetPath()))
                modelPanel->modelHistory(name == "redo", page->doc);
        } else if (name == "save" || name == "saveNative")
            modelPanel->saveWorkspaceModel(page->doc);
        return;
    }
    // Model JSON history when keyboard focus is in the Models / UV panel (C8).
    if ((name == "undo" || name == "redo") && modelPanel && modelPanel->historyShortcut(name == "redo"))
        return;
    if (current() && current()->uvEditing && (name == "undo" || name == "redo") &&
        current()->uvUndo) {
        current()->uvUndo(name == "redo");
        return;
    }
    if (!current())
        return;
    QElapsedTimer t;
    t.start();
    auto doc = current()->document;
    if ((name.startsWith("save") || name == "exportPng") && current()->hasFloating() &&
        !current()->commitFloating())
        return;
    // Save writes the document into the pack (saving.cpp), including layered and animated documents.
    if (name == "save") {
        saveCurrentToPack();
        metrics.record(name, t);
        return;
    }
    // The first export of a vanilla texture creates its pack folders inside the project.
    if (name == "exportPng" && !doc->animated() && !prepareAssetFolder(doc))
        return;
    if (doc->animated() && name == "saveNative") {
        saveAnimationNative();
        return;
    }
    if (doc->animated() && (name == "exportPng" || name == "saveAs")) {
        exportAnimation(name == "saveAs");
        return;
    }
    if ((name == "undo" || name == "redo") && current()->interactionActive()) {
        // Undo first cancels an unfinished placement, polygon or stroke.
        current()->cancelInteraction();
        refresh();
        return;
    }
    QString error, target;
    Command command = Command::Save;
    if (name == "saveNative")
        command = Command::SaveNative;
    else if (name == "exportPng")
        command = Command::ExportPng;
    else if (name == "undo")
        command = Command::Undo;
    else if (name == "redo")
        command = Command::Redo;
    else if (name == "saveAs") {
        command = Command::SaveAs;
        QFileDialog dialog(this,
                           doc->nativeDocument() ? "Export composite PNG as" : "Save texture as",
                           doc->path, "PNG images (*.png)");
        dialog.setOption(QFileDialog::DontConfirmOverwrite);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix("png");
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
            return;
        target = dialog.selectedFiles().first();
    }
    bool ok = false;
    if (command == Command::SaveAs) {
        auto expected = Revision::read(target);
        for (auto &other : session.all())
            if (other != doc && pathIdentity(other->path) == pathIdentity(target)) {
                QMessageBox::warning(this, "Save As",
                                     "That file is already open in another document");
                return;
            }
        if (expected.exists && QMessageBox::question(this, "Replace destination PNG?",
                                                     "Replace the disk version at " + target +
                                                         " with Nova Studio's current pixels?",
                                                     QMessageBox::Yes | QMessageBox::Cancel,
                                                     QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        ok = doc->saveAsRevision(target, expected, &error);
    } else
        ok = session.dispatch(doc, command, target, false, &error);
    if (!ok) {
        qWarning().noquote() << "Document command failed:" << name << doc->path << error;
        if ((command == Command::ExportPng ||
             (command == Command::Save && !doc->nativeDocument())) &&
            doc->externalChanged())
            conflictDialog();
        else
            QMessageBox::warning(this, "Command failed", error);
    } else if (command == Command::Save || command == Command::SaveAs ||
               command == Command::SaveNative || command == Command::ExportPng) {
        if (!doc->modified())
            recovery->remove(doc->id());
        if (command == Command::ExportPng || !doc->nativeDocument())
            externalRevisions.remove(doc->id());
        baselineSaved(doc);
        statusBar()->showMessage(
            (command == Command::SaveNative || (command == Command::Save && doc->nativeDocument()))
                ? "Editable layers saved · Update Minecraft PNG to export"
                : "Composite PNG saved atomically",
            4000);
    }
    metrics.record(name, t);
    refresh();
}
void Window::persist() {
    if (fresh || qaMode || exercise)
        return;
    if (modelPanel)
        modelPanel->savePreviewState();
    preferences.dark = dark;
    preferences.workspacePreset = preset;
    if (timeline) preferences.animationView = QJsonDocument(timeline->viewSettings()).toJson(QJsonDocument::Compact);
    preferences.gridMode = nav.grid;
    preferences.thumbnailSize = thumbSize;
    preferences.geometry = saveGeometry();
    preferences.layout = currentLayout();
    rememberPresetLayout();
    preferences.projectPath = project;
    preferences.directory = nav.directory;
    preferences.selected = nav.selected;
    preferences.query = nav.query;
    preferences.openDocuments.clear();
    for (int i = 0; i < tabs->count(); ++i)
        preferences.openDocuments.append(tabPath(tabs->widget(i)));
    preferences.activeDocument = tabPath(tabs->currentWidget());
    preferences.recentColors = toolState.recent;
    if (colorPanel)
        preferences.activePalette = colorPanel->activePaletteName();
    {
        QJsonObject brushes;
        for (const auto &info : toolInfos())
            if (toolState.brushes.contains(info.tool))
                brushes[info.id] = toolState.brushPreset(info.tool);
        preferences.toolState = QJsonDocument(QJsonObject{{"schema", 1},
                                                          {"foreground", ops::colorName(toolState.foreground)},
                                                          {"background", ops::colorName(toolState.background)},
                                                          {"tolerance", toolState.tolerance},
                                                          {"smooth_shapes", toolState.smoothShape},
                                                          {"symmetry", int(toolState.symmetry)},
                                                          {"brushes", brushes}})
                                    .toJson(QJsonDocument::Compact);
    }
    if (!project.isEmpty() && QFileInfo(project).isDir()) {
        preferences.recentProjects.removeAll(project);
        preferences.recentProjects.prepend(project);
    }
    statusBar()->showMessage(settings.save(preferences) ? "Workspace saved"
                                                        : "Workspace save failed");
    updateRecent();
}
void Window::updateRecent() {
    if (!recentMenu)
        return;
    recentMenu->clear();
    for (const auto &path : preferences.recentProjects.mid(0, 10)) {
        auto a = recentMenu->addAction(path);
        connect(a, &QAction::triggered, this, [this, path] {
            pathText->setText(path);
            emit pathText->returnPressed();
        });
    }
    recentMenu->setEnabled(!preferences.recentProjects.isEmpty());
}
void Window::restore() {
    if (fresh)
        return;
    QElapsedTimer timer;
    timer.start();
    if (!preferences.geometry.isEmpty())
        restoreGeometry(preferences.geometry);
    // The saved arrangement wins (it includes the user's own adjustments to the
    // last preset); a missing or invalid one falls back to that preset.
    const auto name = presetBox && presetBox->findData(preferences.workspacePreset) >= 0
                          ? preferences.workspacePreset
                          : QString("essentials");
    bool ok = false;
    if (!preferences.layout.isEmpty()) {
        // An unknown tool position falls back to the left edge; the rest of the layout stays.
        ok = applyLayout(preferences.layout);
        metrics.count("layout_restored", ok ? 1 : 0);
    }
    if (!ok && name != "essentials")
        applyPreset(name, false);
    else if (!ok) {
        if (toolPlacement)
            toolPlacement->place(ToolPlacement::LeftEdge);
        restoreState(defaultLayout, 1);
    }
    preset = name;
    // The restored arrangement is this workspace's baseline once it has settled.
    QTimer::singleShot(0, this, [this, name] {
        if (preset == name && presetBaseline.isEmpty())
            presetBaseline = preferences.presetLayouts.value(name, currentLayout());
    });
    watchScreens();
    if (presetGroup)
        for (auto a : presetGroup->actions())
            a->setChecked(a->data().toString() == preset);
    if (presetBox) {
        QSignalBlocker block(presetBox);
        presetBox->setCurrentIndex(std::max(0, presetBox->findData(preset)));
    }
    keepOnScreen();
    metrics.record("workspace_restore", timer);
}
void Window::keepOnScreen() {
    QList<QRect> available;
    for (auto screen : QGuiApplication::screens())
        available.append(screen->availableGeometry());
    // Only move when needed: an unconditional setGeometry() forces a native
    // resize round-trip (~125 ms with the OpenGL preview) on every call.
    if (!isMaximized())
        if (const auto target = Settings::visibleGeometry(geometry(), available); target != geometry())
            setGeometry(target);
    for (auto dock : {assetsDock, inspectorDock, previewDock, layersDock, colorsDock, historyDock, animationDock,
                      modelsDock, problemsDock})
        if (dock && dock->isFloating())
            if (const auto target = Settings::visibleGeometry(dock->geometry(), available); target != dock->geometry())
                dock->setGeometry(target);
    if (toolPlacement)
        toolPlacement->keepReachable();
}
// Screens added, removed, resized or rescaled (DPI): keep the window and floating
// panels, including a floating tool strip, on an available screen.
void Window::watchScreens() {
    auto check = [this] {
        metrics.count("screen_change_rechecks", 1);
        QTimer::singleShot(0, this, [this] { keepOnScreen(); });
    };
    auto watch = [this, check](QScreen *screen) {
        connect(screen, &QScreen::availableGeometryChanged, this, check);
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, check);
    };
    if (property("novaScreensWatched").toBool())
        return;
    setProperty("novaScreensWatched", true);
    for (auto screen : QGuiApplication::screens())
        watch(screen);
    connect(qApp, &QGuiApplication::screenAdded, this, [watch, check](QScreen *screen) {
        watch(screen);
        check();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, check);
}
void Window::resetWorkspace() {
    for (auto dock : {assetsDock, inspectorDock, previewDock, layersDock, colorsDock, historyDock, animationDock,
                      modelsDock, problemsDock}) {
        dock->setFloating(false);
        dock->show();
    }
    for (auto bar : {static_cast<QToolBar *>(toolBox), static_cast<QToolBar *>(toolOptions), appBar})
        if (bar)
            bar->show();
    // Product default: every workspace forgets the user's arrangement, tools on the left.
    preferences.presetLayouts.clear();
    if (toolPlacement)
        toolPlacement->place(ToolPlacement::LeftEdge);
    applyPreset("essentials", false);
    statusBar()->showMessage("Workspace reset to Essentials");
}
bool Window::confirmClose(Canvas *canvas) {
    if (!canvas)
        return true;
    // A pasted/moved selection is pending user work even on a previously saved
    // document. Commit it before deciding whether a save prompt is necessary.
    if (canvas->hasFloating() && !canvas->commitFloating())
        return false;
    if (!canvas->document->modified())
        return true;
    tabs->setCurrentWidget(canvas);
    auto choice = QMessageBox::question(
        this, "Unsaved texture",
        "Save changes to " + QFileInfo(canvas->document->path).fileName() + "?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Save) {
        if (canvas->document->animated()) {
            saveAnimationNative();
            return !canvas->document->modified();
        }
        QString error;
        if (!canvas->document->nativeDocument() && !prepareAssetFolder(canvas->document))
            return false;
        if (!session.dispatch(canvas->document, Command::Save, {}, false, &error)) {
            QMessageBox::warning(this, "Save failed", error);
            return false;
        }
    }
    return true;
}
void Window::closeEvent(QCloseEvent *e) {
    if (modelPanel && !modelPanel->confirmClose()) {
        e->ignore();
        return;
    }
    for (int i = 0; i < tabs->count(); ++i)
        if (!confirmClose(dynamic_cast<Canvas *>(tabs->widget(i)))) {
            e->ignore();
            return;
        }
    // The Fabric test client is never stopped without the user's decision.
    if (fabricTest && !fabricTest->confirmShutdown()) {
        e->ignore();
        return;
    }
    if (mcpService)
        mcpService->shutdown();
    ioPool.waitForDone();
    for (auto &doc : session.all())
        recovery->remove(doc->id());
    modelPanel->discardModelRecovery();
    persist();
    if (!telemetry.isEmpty()) {
        qint64 history = 0;
        for (auto &doc : session.all())
            history += doc->historyBytes();
        metrics.count("history_bytes", history);
        metrics.count("thumbnail_cache_bytes", model->cachedBytes());
        metrics.write(telemetry);
    }
    e->accept();
}
void Window::showEvent(QShowEvent *e) {
    QMainWindow::showEvent(e);
    if (first) {
        first = false;
        QTimer::singleShot(0, this, [this] {
            // With a project open, start keyboard focus in the asset tree rather than the
            // project path field: navigation starts where work starts, and an idle window
            // no longer repaints a blinking caret in the toolbar (C8 idle analysis).
            if (!focusWidget() || focusWidget() == pathText) {
                if (!project.isEmpty() && tree)
                    tree->setFocus(Qt::OtherFocusReason);
                else if (auto create = findChild<QPushButton *>("welcomeCreate"))
                    create->setFocus(Qt::OtherFocusReason);
            }
            metrics.samples["first_ui_ms"].append(started.nsecsElapsed() / 1e6);
            if (!telemetry.isEmpty())
                metrics.write(telemetry);
        });
    }
}
} // namespace nova
