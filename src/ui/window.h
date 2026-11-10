// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "assets.h"
#include "canvas.h"
#include "core.h"
#include "cube.h"
#include "layers.h"
#include "metrics.h"
#include "monitor.h"
#include "palette.h"
#include "presets.h"
#include "tools.h"
#include "baseline_reset.h"
#include "project_config.h"
#include "recovery.h"
#include "session.h"
#include "settings.h"
#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QPushButton>
#include <QPointer>
#include <QToolButton>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMainWindow>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTreeView>
namespace nova {
namespace mcp { class Service; struct Snapshot; struct Request; }
namespace vanilla { struct BaselineEntry; }
// The result of saving one document into the pack. `code` is empty on success; `written` lists
// what reached disk ("pack": PNG, PNG + .png.mcmeta or model JSON; "editable": layered source).
struct SaveOutcome {
    QString path, type, code, message;
    QStringList written;
    bool ok() const { return code.isEmpty(); }
};
class ColorPanel;
struct SearchIndex;
class HistoryPanel;
class TimelinePanel;
class ModelPanel;
namespace mc { class ModelEdit; }
class ResourceTools;
class Window : public QMainWindow {
    mcp::Service *mcpService = nullptr;
    void showMcpAccess(int page = 0); // 0 Access, 1 Agent Setup
    void ensureMcpService();
    void createMcpAccessDialog(int page);
    void showAgentSession(); // ACP prototype: a Nova-launched agent with Nova's MCP bridge attached
    void updateExperimentalActions(); // shows or hides the experimental Agent Session command
    void endAgentSessionForProjectChange();
    struct Comparison {
        QString path, beforeRevision, afterRevision, detail, owner;
        QUuid instance;
        quint64 serial = 0;
        QImage before, after, uvBefore, uvAfter;
    };
    QList<Comparison> comparisons;
    QPointer<QDialog> comparisonDialog;
    void rememberComparison(Comparison comparison);
    void clearComparisons(const QString &path = {});
    void showComparison(const QString &path, const QString &revision);
    bool comparisonCurrent(const Comparison &comparison) const;
    bool openRelatedAsset(const QString &path);
    void showRelatedAssets(QString path = {});
    bool isLocalAsset(const QString &path) const;
    std::shared_ptr<Document> reviewRaster(const QString &path) const;
    QString project, telemetry;
    DocumentSession session;
    std::unique_ptr<RecoveryStore> recovery;
    QThreadPool ioPool;
    bool ioBusy = false;
    QMap<QUuid, qint64> recoveredKeys;
    QMap<QUuid, Revision> externalRevisions;
    DirectoryMonitor monitor;
    QTimer *indexDebounce = nullptr;
    QTimer *reconciliationTimer = nullptr, *revisionTimer = nullptr;
    QStringList pendingChanges;
    bool fullRescan = false;
    bool invalidateFallback = false;
    bool indexBusy = false;
    quint64 indexGeneration = 0;
    std::shared_ptr<std::atomic_bool> indexCancel;
    void updateIndex();
    void backgroundSafety();
    void recoverSession();
    void conflictDialog();
    Settings settings;
    Preferences preferences;
    QByteArray defaultLayout;
    // Tool strip position (tool_placement.h) and the arrangement right after the current
    // workspace preset was applied, to tell the user's own changes from the preset's.
    class ToolStripPlacement *toolPlacement = nullptr;
    QByteArray presetBaseline;
    QByteArray currentLayout();
    bool applyLayout(const QByteArray &packed);
    void rememberPresetLayout();
    void watchScreens();
    QAction *themeAction = nullptr;
    QMenu *recentMenu = nullptr;
    void updateRecent();
    bool confirmClose(Canvas *canvas);
    void openModelAsset(const QString &asset);
    void showModelDocument(const std::shared_ptr<mc::ModelEdit> &doc, bool activate = true);
    void updateModelTabs();
    QString tabPath(QWidget *page) const;
    void removeDocumentTab(int index);
    void resetWorkspace();
    void keepOnScreen();
    QElapsedTimer started;
    bool first = true, dark = true;
    bool ready = false;
    bool fresh = false, qaMode = false, exercise = false;
    Metrics metrics;
    Index collection;
    quint64 collectionSerial = 0; // bumped whenever `collection` is replaced
    Navigation nav;
    // ---- Vanilla baseline (baseline_ui.cpp, docs/vanilla-baseline/DESIGN.md) ----
    // `collection` and workspaceIndex() stay project-only (MCP, search, validation, batch);
    // the asset browser alone shows browserIndex() = workspaceIndex() merged over the baseline.
    struct BaselineState {
        std::optional<BaselineRef> ref; // .nova/project.json; unset for legacy/blank projects
        std::shared_ptr<const vanilla::BaselineEntry> entry; // loaded cache entry (Check::Sizes)
        std::shared_ptr<const Index> index;                   // baselineIndex(*entry)
        QString unavailable, diagnostic;
        quint64 generation = 0;
        bool loading = false;
    } baseline;
    struct BaselineStep {
        QString id;
        QStringList assets;
        ResetKind kind = ResetKind::ResetToVanilla;
    };
    QList<BaselineStep> resetHistory; // project-level Reset to Vanilla / Remove history
    int resetCursor = 0;
    QSet<QUuid> baselineDocs;         // documents opened from the baseline (not yet saved)
    QSet<QString> baselineOpening;
    QStringList baselineRestore;      // tabs to reopen once the baseline entry is loaded
    Index browserCache;
    quint64 browserSerial = ~0ull;
    QStringList browserFiles;
    QLabel *baselineLabel = nullptr;
    QMenu *baselineMenu = nullptr;
    Index browserIndex();
    void loadBaseline(bool sameProject = false);
    void resetBaselineState(bool sameProject = false);
    void updateBaselineStatus();
    void updateBaselineActions();
    void buildBaselineMenu(QMenu *resources);
    QAction *addBaselineContextAction(QMenu &menu, const QString &relative);
    void triggerBaselineContextAction(QAction *action);
    bool canResetToVanilla(const QString &relative) const;
    bool canRemoveFromProject(const QString &relative) const;
    bool baselineCandidate(const QString &path) const;
    QString baselineOrigin(const Document &doc) const;
    bool baselineBacked(const Document &doc) const;
    bool openBaselineTexture(const QString &relative, bool activate = true);
    bool prepareAssetFolder(const std::shared_ptr<Document> &doc);
    void baselineSaved(const std::shared_ptr<Document> &doc);
    QString baselineSelection() const;
    QStringList baselineOverrides() const;
    void baselineReset(const QStringList &assets, ResetKind kind);
    bool baselineHistory(bool redo);
    bool browserHasFocus() const;
    bool closeBaselineAssets(const QStringList &assets, QStringList *reopen);
    void useVanillaBaseline();
    void repairBaseline();
    void manageBaselines();
    void offerResetRecovery(const QStringList &interrupted);
    QLineEdit *pathText, *search;
    QLabel *breadcrumb, *properties;
    AssetModel *model;
    QSortFilterProxyModel *proxy;
    QTreeView *tree;
    QListView *grid;
    QStackedWidget *browser;
    QTabWidget *tabs;
    Cube *cube;
    ModelPanel *modelPanel = nullptr;
    QDockWidget *modelsDock = nullptr;
    QDockWidget *problemsDock = nullptr;
    ResourceTools *resourceTools = nullptr;
    class MinecraftTestDialog *minecraftTest = nullptr; // dormant launcher hand-off (Phase 3-D)
    class FabricTestDialog *fabricTest = nullptr;       // default backend (Phase 3-D.1)
    void minecraftTesting(bool syncOnly);
    void launcherHandoffTesting(bool syncOnly);
    static bool launcherHandoffEnabled();
    QStringList unsavedMinecraftOutputs();
    void buildResourceTools(QMenu *view);
    void showProblems(bool validate);
    void createResourceAsset();
    void modelBatch();
    std::shared_ptr<mc::Library> modelLibrary = std::make_shared<mc::Library>();
    std::shared_ptr<std::atomic_bool> modelCancel;
    quint64 modelGeneration = 0;
    bool restoreModelWorkspacePending = false;
    void updateModelIndex(const QStringList &paths = {});
    QDockWidget *assetsDock, *inspectorDock, *previewDock = nullptr;
    QDockWidget *layersDock;
    LayersPanel *layersPanel;
    // ---- Phase 2-C3 editor ----
    ToolState toolState;
    ToolBox *toolBox = nullptr;
    ToolOptionsBar *toolOptions = nullptr;
    ColorPanel *colorPanel = nullptr;
    HistoryPanel *historyPanel = nullptr;
    QDockWidget *colorsDock = nullptr, *historyDock = nullptr;
    QDockWidget *animationDock = nullptr;
    TimelinePanel *timeline = nullptr;
    bool animationSyncing = false;
    void buildAnimation(QMenu *view);
    void ensureTimeline();
    void animationRefresh();
    void newAnimation();
    void animationProperties();
    void processAnimationFrames(const QString &operation = {});
    void exportAnimation(bool as = false);
    void recoverAnimationExport(const QString &asset = {});
    void saveAnimationNative();
    QMenu *layoutsMenu = nullptr, *effectsMenu = nullptr;
    PaletteStore palettes;
    PresetStore presets;
    struct Clip {
        QImage image;
        QPoint origin;
        qint64 key = 0;
    } clip;
    void buildEditor(QMenu *view);
    QMenu *operationMenu(const QString &title, const QString &category);
    void openOperation(const QStringList &ids);
    void documentOperation(const ops::Spec &spec);
    void layerOperation(const ops::Spec &spec);
    void copySelection(bool cut);
    void paste(bool asLayer);
    void duplicateSelection();
    void commandPalette(); // Phase 4-G application search (search.cpp)
  public:
    SearchIndex searchIndex(); // built from the live menus each time Search opens
  private:
    void canvasMenu(QPoint global);
    void batchDialog();
    void shapeParameters();
    void stampLibrary();
    void updateLayouts();
    void editorRefresh();
    void configureCanvas(Canvas *canvas);
    int thumbSize = 72;
    Canvas *current() const;
    void indexProject();
    void setDirectory(QString directory);
    void setMode(bool grid);
    void syncSelection(const QModelIndex &index);
    void openAsset(const QModelIndex &index);
    void openTexture(const QString &path);
    Index workspaceIndex(QSet<QString> *provisional = nullptr) const;
    void showTextureDocument(const std::shared_ptr<Document> &doc);
    void refresh();
    void applyTheme();
    void buildSupportMenu();
    void persist();
    void restore();
    void action(const QString &name);
    // ---- Saving (saving.cpp): one path for Save, Save All and MCP saves ----
    QLabel *unsavedLabel = nullptr;
    QByteArray draftKey; // per-process key for stable MCP document identifiers
    bool documentDirty(const Document &doc) const;
    SaveOutcome saveDocumentToPack(const std::shared_ptr<Document> &doc, bool interactive);
    SaveOutcome saveModelToPack(const std::shared_ptr<mc::ModelEdit> &edit);
    QList<SaveOutcome> saveAllDocuments(bool interactive);
    void saveAll();
    void saveCurrentToPack();
    QStringList unsavedDocuments(bool projectOnly) const;
    void updateUnsavedStatus();
    QString documentIdentifier(QUuid instance);
    QString draftRevisionToken(QUuid instance, quint64 serial, const QString &suffix = {});
    QJsonObject mcpDraftRequest(const mcp::Snapshot &snapshot, const mcp::Request &request, bool editing,
                                std::function<QJsonObject()> &commit);
    // ---- Phase 3-B workspace shell (workspace.cpp) ----
    QToolBar *appBar = nullptr;
    QString preset = "essentials";
    QComboBox *presetBox = nullptr;
    QActionGroup *presetGroup = nullptr;
    QToolButton *activityButton = nullptr;
    QMenu *activityMenu = nullptr;
    QStringList activity;
    int unseenActivity = 0;
    QMenu *panelsMenu = nullptr;
    QAction *pixelGridAction = nullptr;
    QStackedWidget *inspectorPages = nullptr;
    QMap<QString, QLabel *> inspectorValues;
    QLabel *inspectorTool = nullptr, *inspectorToolTip = nullptr, *inspectorTips = nullptr;
    QLabel *zoomLabel = nullptr;
    void buildAppBar();
    QWidget *buildInspector(QCheckBox *grid, QPushButton *center);
    void updateInspector();
    void buildWorkspaceMenu(QMenu *view);
    void organizeMenus();
    void applyPreset(const QString &name, bool useSaved = true);
    void zoomCanvas(int direction); // 1 in, -1 out, 0 fit, 2 actual pixels
    void preferencesDialog(const QString &focus = {}); // focus: object name of a control to select
    void shortcutsDialog();
    void recordActivity(const QString &message);
    void refreshThemeIcons();
    friend class EditorTestAccess;

  protected:
    void closeEvent(QCloseEvent *) override;
    void showEvent(QShowEvent *) override;

  public:
    ToolState &tools() { return toolState; }
    DocumentSession &documents() { return session; }
    Canvas *activeCanvas() const { return current(); }
    QString workspacePreset() const { return preset; }
    Window(QString project, QString telemetry, QElapsedTimer started);
    ~Window() override;
    void qa(const QString &output);
    void startExercise();
    void performanceCycle(const QString &root);
};
} // namespace nova
