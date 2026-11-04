#pragma once
#include "assets.h"
#include "canvas.h"
#include "core.h"
#include "cube.h"
#include "metrics.h"
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMainWindow>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTreeView>
namespace nova {
class Window : public QMainWindow {
    QString project, telemetry;
    QElapsedTimer started;
    bool first = true, dark = true;
    bool ready = false;
    bool fresh = false, qaMode = false, exercise = false;
    Metrics metrics;
    Index collection;
    Navigation nav;
    QLineEdit *pathText, *search;
    QLabel *breadcrumb, *properties;
    AssetModel *model;
    QSortFilterProxyModel *proxy;
    QTreeView *tree;
    QListView *grid;
    QStackedWidget *browser;
    QTabWidget *tabs;
    Cube *cube;
    QDockWidget *assetsDock, *inspectorDock, *previewDock;
    QRgb color = qRgba(130, 141, 255, 255);
    bool eraser = false;
    int thumbSize = 72;
    Canvas *current() const;
    void indexProject();
    void setDirectory(QString directory);
    void setMode(bool grid);
    void syncSelection(const QModelIndex &index);
    void openAsset(const QModelIndex &index);
    void openTexture(const QString &path);
    void refresh();
    void applyTheme();
    void persist();
    void restore();
    void action(const QString &name);

  protected:
    void closeEvent(QCloseEvent *) override;
    void showEvent(QShowEvent *) override;

  public:
    Window(QString project, QString telemetry, QElapsedTimer started);
    void qa(const QString &output);
    void startExercise();
};
} // namespace nova
