#include "window.h"
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QtWidgets>
namespace nova {
Window::Window(QString p, QString output, QElapsedTimer time)
    : project(QDir(p).absolutePath()), telemetry(std::move(output)), started(time) {
    fresh = qApp->arguments().contains("--fresh-layout");
    setWindowTitle("Nova Studio · Qt");
    resize(1280, 820);
    setMinimumSize(800, 550);
    setDockNestingEnabled(true);
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks);
    setAnimated(false);
    auto commands = addToolBar("Commands");
    commands->setObjectName("commands");
    commands->setMovable(false);
    commands->addWidget(new QLabel("  ✦  NOVA STUDIO   "));
    for (auto pair : {std::pair{"Save", "Ctrl+S"}, {"Undo", "Ctrl+Z"}, {"Redo", "Ctrl+Y"}}) {
        auto a = commands->addAction(pair.first);
        a->setShortcut(QKeySequence(pair.second));
        connect(a, &QAction::triggered, this,
                [this, name = QString(pair.first).toLower()] { action(name); });
    }
    auto view = menuBar()->addMenu("View");
    auto saveLayout = view->addAction("Save workspace");
    saveLayout->setShortcut(QKeySequence("Ctrl+Shift+S"));
    connect(saveLayout, &QAction::triggered, this, [this] { persist(); });
    auto theme = view->addAction("Dark theme");
    theme->setCheckable(true);
    theme->setChecked(true);
    connect(theme, &QAction::toggled, this, [this](bool value) {
        dark = value;
        applyTheme();
    });
    auto projectBar = addToolBar("Project");
    projectBar->setObjectName("project");
    projectBar->setMovable(false);
    addToolBarBreak();
    projectBar->addWidget(new QLabel(" Project  "));
    pathText = new QLineEdit(project);
    pathText->setMinimumWidth(400);
    projectBar->addWidget(pathText);
    auto open = projectBar->addAction("Open");
    connect(open, &QAction::triggered, this, [this] {
        for (int i = 0; i < tabs->count(); ++i)
            if (static_cast<Canvas *>(tabs->widget(i))->document->modified()) {
                statusBar()->showMessage("Save modified documents before switching projects");
                return;
            }
        if (!QFileInfo(pathText->text()).isDir()) {
            statusBar()->showMessage("Project directory does not exist");
            return;
        }
        persist();
        project = QDir(pathText->text()).absolutePath();
        while (tabs->count()) {
            auto page = tabs->widget(0);
            tabs->removeTab(0);
            delete page;
        }
        cube->setDocument({});
        nav = {};
        indexProject();
    });
    tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    tabs->setTabsClosable(true);
    tabs->setMovable(true);
    setCentralWidget(tabs);
    connect(tabs, &QTabWidget::currentChanged, this, [this] { refresh(); });
    connect(tabs, &QTabWidget::tabCloseRequested, this, [this](int i) {
        auto canvas = static_cast<Canvas *>(tabs->widget(i));
        if (canvas->document->modified()) {
            statusBar()->showMessage("Save this document before closing its tab");
            return;
        }
        tabs->removeTab(i);
        delete canvas;
        refresh();
    });
    model = new AssetModel(this);
    proxy = new QSortFilterProxyModel(this);
    proxy->setSourceModel(model);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy->setRecursiveFilteringEnabled(true);
    tree = new QTreeView;
    tree->setModel(proxy);
    tree->setHeaderHidden(true);
    tree->setUniformRowHeights(true);
    tree->setIconSize({18, 18});
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    grid = new QListView;
    grid->setModel(proxy);
    grid->setViewMode(QListView::IconMode);
    grid->setResizeMode(QListView::Adjust);
    grid->setMovement(QListView::Static);
    grid->setUniformItemSizes(true);
    grid->setIconSize({thumbSize, thumbSize});
    grid->setGridSize({thumbSize + 28, thumbSize + 36});
    grid->setWordWrap(false);
    grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid->setLayoutMode(QListView::Batched);
    grid->setBatchSize(100);
    browser = new QStackedWidget;
    browser->addWidget(tree);
    browser->addWidget(grid);
    auto assetWidget = new QWidget;
    auto av = new QVBoxLayout(assetWidget);
    av->setContentsMargins(6, 6, 6, 6);
    av->setSpacing(5);
    auto modes = new QHBoxLayout;
    modes->addWidget(new QLabel("ASSETS"));
    for (auto name : {"Tree", "Grid"}) {
        auto b = new QPushButton(name);
        modes->addWidget(b);
        connect(b, &QPushButton::clicked, this,
                [this, g = QString(name) == "Grid"] { setMode(g); });
    }
    av->addLayout(modes);
    search = new QLineEdit;
    search->setPlaceholderText("Search textures…");
    av->addWidget(search);
    connect(search, &QLineEdit::textChanged, this, [this](const QString &query) {
        nav.query = query;
        if (query.isEmpty()) {
            model->setIndex(collection, project);
        } else {
            Index filtered;
            filtered.files = nav.entries(collection);
            filtered.directories.insert("", filtered.files);
            model->setIndex(filtered, project);
        }
        setDirectory(nav.directory);
    });
    auto crumbs = new QHBoxLayout;
    auto up = new QPushButton("↑");
    up->setMaximumWidth(28);
    crumbs->addWidget(up);
    breadcrumb = new QLabel;
    crumbs->addWidget(breadcrumb, 1);
    auto sort = new QPushButton("A–Z");
    sort->setMaximumWidth(40);
    crumbs->addWidget(sort);
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
    slider->setValue(72);
    slider->setToolTip("Thumbnail size");
    av->addWidget(slider);
    connect(slider, &QSlider::valueChanged, this, [this](int size) {
        thumbSize = size;
        grid->setIconSize({size, size});
        grid->setGridSize({size + 28, size + 36});
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
        connect(itemView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex &index) { syncSelection(index); });
    }
    auto inspector = new QWidget;
    auto iv = new QVBoxLayout(inspector);
    iv->setContentsMargins(10, 10, 10, 10);
    iv->addWidget(new QLabel("PIXEL TOOLS"));
    auto toolRow = new QHBoxLayout;
    for (auto pair : {std::pair{"Pencil B", false}, {"Eraser E", true}}) {
        auto b = new QPushButton(pair.first);
        toolRow->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, value = pair.second] {
            eraser = value;
            refresh();
        });
    }
    iv->addLayout(toolRow);
    for (auto pair : {std::pair{"B", false}, {"E", true}}) {
        auto key = new QShortcut(QKeySequence(pair.first), this);
        connect(key, &QShortcut::activated, this, [this, value = pair.second] {
            eraser = value;
            refresh();
        });
    }
    iv->addWidget(new QLabel("Foreground · RGBA"));
    auto choose = new QPushButton("Choose color…");
    choose->setStyleSheet("background:#828dff;color:#10121b");
    iv->addWidget(choose);
    connect(choose, &QPushButton::clicked, this, [this, choose] {
        auto c = QColorDialog::getColor(QColor::fromRgba(color), this, "Foreground",
                                        QColorDialog::ShowAlphaChannel);
        if (c.isValid()) {
            color = c.rgba();
            choose->setStyleSheet("background:" + c.name() + ";color:#10121b");
            refresh();
        }
    });
    properties = new QLabel;
    properties->setWordWrap(true);
    iv->addWidget(properties);
    auto gridButton = new QCheckBox("Pixel grid");
    gridButton->setChecked(true);
    iv->addWidget(gridButton);
    connect(gridButton, &QCheckBox::toggled, this, [this](bool g) {
        for (int i = 0; i < tabs->count(); ++i) {
            auto c = static_cast<Canvas *>(tabs->widget(i));
            c->grid = g;
            c->update();
        }
    });
    auto center = new QPushButton("Center canvas");
    iv->addWidget(center);
    connect(center, &QPushButton::clicked, this, [this] {
        if (current())
            current()->center();
    });
    iv->addWidget(new QLabel("Layers\nSingle RGBA layer · Phase 1"));
    auto help = new QLabel("Ctrl+S   Save\nCtrl+Z   Undo\nCtrl+Y   Redo\nWheel   "
                           " Zoom\nMiddle / right drag    Pan\nDrag panel titles "
                           "to rearrange\nDouble-click a title to float");
    help->setWordWrap(true);
    iv->addWidget(help);
    iv->addStretch();
    inspectorDock = new QDockWidget("Inspector", this);
    inspectorDock->setObjectName("inspector");
    auto inspectorScroll = new QScrollArea;
    inspectorScroll->setWidgetResizable(true);
    inspectorScroll->setFrameShape(QFrame::NoFrame);
    inspectorScroll->setWidget(inspector);
    inspectorDock->setWidget(inspectorScroll);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);
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
    previewDock = new QDockWidget("3D Preview · Drag to orbit / Scroll to zoom", this);
    previewDock->setObjectName("preview");
    previewDock->setWidget(cube);
    addDockWidget(Qt::BottomDockWidgetArea, previewDock);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    for (auto dock : {assetsDock, inspectorDock, previewDock})
        view->addAction(dock->toggleViewAction());
    resizeDocks({assetsDock, inspectorDock}, {290, 225}, Qt::Horizontal);
    resizeDocks({previewDock}, {220}, Qt::Vertical);
    applyTheme();
    restore();
    indexProject();
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
        QStringList names = {"01-default.png",   "02-tree.png",    "03-grid.png",
                             "04-high-zoom.png", "05-preview.png", "06-rearranged.png",
                             "07-compact.png"};
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
            default:
                break;
            }
        } else {
            int i = s / 2;
            if (i < names.size()) {
                auto image = grab();
                image.save(output + '/' + names[i]);
            } else if (i == 7) {
                previewDock->grab().save(output + "/08-native-floating.png");
            }
        }
        if (s >= 16) {
            timer->stop();
            for (int i = 0; i < tabs->count(); ++i) {
                auto doc = static_cast<Canvas *>(tabs->widget(i))->document;
                while (doc->modified())
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
                auto canvas = static_cast<Canvas *>(tabs->widget(k));
                canvas->document =
                    std::make_shared<Document>(canvas->document->path, canvas->document->image);
            }
            close();
        } else
            timer->setInterval(1000 / rate);
    });
    timer->setTimerType(Qt::PreciseTimer);
    timer->start(16);
}
Canvas *Window::current() const { return static_cast<Canvas *>(tabs->currentWidget()); }
void Window::applyTheme() {
    qApp->setStyle("Fusion");
    QPalette palette;
    if (dark) {
        palette.setColor(QPalette::Window, QColor(29, 31, 40));
        palette.setColor(QPalette::WindowText, QColor(222, 225, 235));
        palette.setColor(QPalette::Base, QColor(22, 24, 32));
        palette.setColor(QPalette::AlternateBase, QColor(31, 34, 44));
        palette.setColor(QPalette::Text, QColor(222, 225, 235));
        palette.setColor(QPalette::Button, QColor(42, 45, 59));
        palette.setColor(QPalette::ButtonText, QColor(222, 225, 235));
        palette.setColor(QPalette::Highlight, QColor(66, 72, 121));
        palette.setColor(QPalette::HighlightedText, Qt::white);
    } else
        palette = qApp->style()->standardPalette();
    qApp->setPalette(palette);
    qApp->setStyleSheet("QToolBar{spacing:5px;padding:4px;border:0;border-bottom:1px solid "
                        "#41434e} QDockWidget::title{padding:5px} QTreeView::item{height:23px} "
                        "QLineEdit{padding:3px} QTabBar::tab{padding:5px 10px} "
                        "QPushButton{padding:4px 7px} QStatusBar{font-size:11px}");
}
void Window::indexProject() {
    statusBar()->showMessage("Indexing in background…");
    QString root = project;
    auto watcher = new QFutureWatcher<std::pair<Index, double>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, root] {
        auto [index, ms] = watcher->result();
        watcher->deleteLater();
        if (root != project)
            return;
        if (!index.error.isEmpty()) {
            statusBar()->showMessage(index.error);
            return;
        }
        collection = std::move(index);
        metrics.samples["index"].append(ms);
        model->setIndex(collection, project);
        statusBar()->showMessage(QString("%1 textures indexed in %2 ms")
                                     .arg(collection.files.size())
                                     .arg(ms, 0, 'f', 1));
        if (nav.directory.isEmpty() && !collection.files.isEmpty()) {
            nav.directory = QFileInfo(collection.files.first()).path();
            nav.selected = collection.files.first();
        }
        setDirectory(nav.directory);
        search->setText(nav.query);
        setMode(nav.grid);
        if (tabs->count() == 0 && !collection.files.isEmpty())
            openTexture(project + '/' + collection.files.first());
    });
    watcher->setFuture(QtConcurrent::run([root] {
        QElapsedTimer t;
        t.start();
        auto index = Index::scan(root);
        return std::pair{std::move(index), t.nsecsElapsed() / 1e6};
    }));
}
void Window::setDirectory(QString d) {
    nav.directory = std::move(d);
    auto index = proxy->mapFromSource(model->indexFor(nav.directory));
    tree->setRootIndex(index);
    grid->setRootIndex(index);
    breadcrumb->setText(nav.directory.isEmpty() ? "Project" : QFileInfo(nav.directory).fileName());
    breadcrumb->setToolTip(nav.directory);
    auto selected = proxy->mapFromSource(model->indexFor(nav.selected));
    if (selected.isValid()) {
        QSignalBlocker a(tree->selectionModel()), b(grid->selectionModel());
        tree->setCurrentIndex(selected);
        grid->setCurrentIndex(selected);
    }
}
void Window::setMode(bool g) {
    nav.grid = g;
    browser->setCurrentIndex(g ? 1 : 0);
    auto selected = proxy->mapFromSource(model->indexFor(nav.selected));
    QSignalBlocker a(tree->selectionModel()), b(grid->selectionModel());
    if (selected.isValid()) {
        tree->setCurrentIndex(selected);
        grid->setCurrentIndex(selected);
    }
}
void Window::syncSelection(const QModelIndex &index) {
    if (index.isValid())
        nav.selected = model->pathFor(proxy->mapToSource(index));
}
void Window::openAsset(const QModelIndex &index) {
    auto source = proxy->mapToSource(index);
    if (model->isDirectory(source))
        setDirectory(model->pathFor(source));
    else
        openTexture(project + '/' + model->pathFor(source));
}
void Window::openTexture(const QString &path) {
    for (int i = 0; i < tabs->count(); ++i)
        if (static_cast<Canvas *>(tabs->widget(i))->document->path == path) {
            tabs->setCurrentIndex(i);
            return;
        }
    QString error;
    auto im = loadPng(path, &error);
    if (im.isNull()) {
        statusBar()->showMessage(error);
        return;
    }
    auto doc = std::make_shared<Document>(path, im);
    auto canvas = new Canvas(doc, &metrics);
    canvas->changed = [this] { refresh(); };
    tabs->addTab(canvas, QFileInfo(path).fileName());
    tabs->setCurrentWidget(canvas);
    nav.selected = QDir(project).relativeFilePath(path);
    refresh();
}
void Window::refresh() {
    auto canvas = current();
    if (canvas) {
        canvas->eraser = eraser;
        canvas->color = color;
        cube->setDocument(canvas->document);
        tabs->setTabText(tabs->indexOf(canvas), QFileInfo(canvas->document->path).fileName() +
                                                    (canvas->document->modified() ? " ●" : ""));
        properties->setText(QString("DOCUMENT\n%1\n%2 × %3 pixels\n%4")
                                .arg(QFileInfo(canvas->document->path).fileName())
                                .arg(canvas->document->image.width())
                                .arg(canvas->document->image.height())
                                .arg(canvas->document->modified() ? "Modified" : "Saved"));
        canvas->update();
    } else {
        cube->setDocument({});
        properties->setText("Open a PNG texture");
    }
}
void Window::action(const QString &name) {
    if (!current())
        return;
    QElapsedTimer t;
    t.start();
    auto doc = current()->document;
    if (name == "save") {
        QString error;
        statusBar()->showMessage(doc->save(&error) ? "PNG saved atomically"
                                                   : "Save failed: " + error);
    } else if (name == "undo")
        doc->undo();
    else if (name == "redo")
        doc->redo();
    metrics.record(name, t);
    refresh();
}
void Window::persist() {
    if (fresh || qaMode || exercise)
        return;
    if (!QFileInfo(project).isDir())
        return;
    QDir(project).mkpath(".nova");
    QSettings settings(project + "/.nova/qt-layout.ini", QSettings::IniFormat);
    settings.setValue("geometry", saveGeometry());
    settings.setValue("layout", saveState(1));
    settings.setValue("directory", nav.directory);
    settings.setValue("selected", nav.selected);
    settings.setValue("query", nav.query);
    settings.setValue("grid", nav.grid);
    settings.setValue("dark", dark);
    QStringList open;
    for (int i = 0; i < tabs->count(); ++i)
        open.append(static_cast<Canvas *>(tabs->widget(i))->document->path);
    settings.setValue("tabs", open);
    settings.sync();
    statusBar()->showMessage(settings.status() == QSettings::NoError ? "Workspace saved"
                                                                     : "Workspace save failed");
}
void Window::restore() {
    if (fresh)
        return;
    metrics.count("layout_restored", QFileInfo::exists(project + "/.nova/qt-layout.ini") ? 1 : 0);
    QSettings settings(project + "/.nova/qt-layout.ini", QSettings::IniFormat);
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("layout").toByteArray(), 1);
    nav.directory = settings.value("directory").toString();
    nav.selected = settings.value("selected").toString();
    nav.query = settings.value("query").toString();
    nav.grid = settings.value("grid", false).toBool();
    dark = settings.value("dark", true).toBool();
    applyTheme();
    for (auto p : settings.value("tabs").toStringList())
        openTexture(p);
}
void Window::closeEvent(QCloseEvent *e) {
    for (int i = 0; i < tabs->count(); ++i) {
        auto doc = static_cast<Canvas *>(tabs->widget(i))->document;
        if (doc->modified()) {
            auto choice = QMessageBox::question(
                this, "Unsaved textures",
                "Save changes to " + QFileInfo(doc->path).fileName() + "?",
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
            if (choice == QMessageBox::Cancel) {
                e->ignore();
                return;
            }
            if (choice == QMessageBox::Save) {
                QString error;
                if (!doc->save(&error)) {
                    QMessageBox::warning(this, "Save failed", error);
                    e->ignore();
                    return;
                }
            }
        }
    }
    persist();
    if (!telemetry.isEmpty())
        metrics.write(telemetry);
    e->accept();
}
void Window::showEvent(QShowEvent *e) {
    QMainWindow::showEvent(e);
    if (first) {
        first = false;
        QTimer::singleShot(0, this, [this] {
            metrics.samples["first_ui_ms"].append(started.nsecsElapsed() / 1e6);
            if (!telemetry.isEmpty())
                metrics.write(telemetry);
        });
    }
}
} // namespace nova
