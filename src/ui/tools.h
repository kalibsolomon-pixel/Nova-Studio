// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Tool state shared by every canvas, the grouped tool box and the contextual
// options bar. Tools translate pointer input into DocumentSession commands;
// they never edit pixels themselves.
#include "core.h"
#include <QIcon>
#include <QMap>
#include <QObject>
#include <QToolBar>

class QStackedWidget;
class QToolButton;
class QActionGroup;
namespace nova {
enum class Tool {
    Pencil, Brush, Eraser, ColorReplace,
    RectSelect, EllipseSelect, Lasso, PolygonLasso, MagicWand, ColorRange,
    Move, Transform,
    Line, Shape,
    Fill, Gradient,
    Eyedropper
};
struct ToolInfo {
    Tool tool;
    QString id, name, group, shortcut, tip;
};
const QList<ToolInfo> &toolInfos();
const ToolInfo &toolInfo(Tool tool);
bool isPaintTool(Tool t);
bool isSelectTool(Tool t);
QIcon toolIcon(Tool tool, const QColor &ink);

class ToolState : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    Tool tool = Tool::Pencil;
    QRgb foreground = qRgba(130, 141, 255, 255), background = qRgba(255, 255, 255, 255);
    QMap<Tool, ops::Brush> brushes;
    ops::Symmetry symmetry = ops::Symmetry::None;
    int radial = 6, smoothing = 0;
    bool paletteLock = false, lockAlpha = false;
    std::vector<QRgb> palette;
    // selection / colour matching
    ops::Combine selectMode = ops::Combine::Replace;
    int tolerance = 0;
    ops::Distance metric = ops::Distance::Rgb;
    bool sampleComposite = false, includeAlpha = true;
    // shapes
    ops::ShapeKind shape = ops::ShapeKind::Rectangle;
    bool filled = false, fromCentre = false, smoothShape = false;
    int thickness = 1, sides = 5, shapeOpacity = 255;
    int snap = 1; // shape / rectangle-selection grid snapping in pixels
    double radius = 2, innerRatio = .5, rotation = 0;
    bool flipH = false, flipV = false;
    // fill / gradient / replace. Fill mode: true = Contiguous (the default for every new
    // session; not persisted), false = Universal Fill (ops::universalFillMatches()).
    bool contiguous = true;
    int fillOpacity = 255, replaceTolerance = 32;
    QString gradientType = "Linear";
    int gradientSteps = 0;
    bool gradientDither = false;
    // transform
    int scaleX = 100, scaleY = 100;
    double angle = 0;
    QString resampling = "Nearest neighbour";
    bool transformHidden = false, transformFlipH = false, transformFlipV = false;
    QStringList recent; // recent colours "#AARRGGBB"

    ops::Brush &brush(Tool t);
    ops::Brush brush(Tool t) const;
    StrokeStyle strokeStyle(Tool t) const;
    void setTool(Tool t);
    void setForeground(QRgb c);
    void setBackground(QRgb c);
    void swapColors();
    void resetColors();
    void rememberColor(QRgb c);
    void touch() { emit changed(); }
    QJsonObject brushPreset(Tool t) const;
    void applyBrushPreset(Tool t, const QJsonObject &preset);
  signals:
    void changed();
    void toolChanged(nova::Tool tool);
    void colorsChanged();
    void transformChanged();
    void commitRequested();
    void cancelRequested();
    void shapeSettingsRequested();
    void resetTransformRequested();
};

// Left tool box: one button per group showing the last-used variant; the
// button's menu lists the other variants (expandable groups, not 17 buttons).
class ToolBox : public QToolBar {
    Q_OBJECT
    ToolState *state;
    QMap<QString, QToolButton *> groups;
    QMap<Tool, QAction *> actions;
    QActionGroup *exclusive;
    QAction *spacerItem = nullptr, *chipItem = nullptr;
    // Wrapping mode: the strip's own widgets move into one row-wrapping container.
    QWidget *flow = nullptr; // a plain child laid over the toolbar's padded area
    QList<QAction *> wrappedActions;
    void sync();
    void placeFlow();

  protected:
    void resizeEvent(QResizeEvent *) override;

  public:
    ToolBox(ToolState *state, QWidget *parent = nullptr);
    QAction *actionFor(Tool t) const { return actions.value(t); }
    QToolButton *effectsButton = nullptr;
    void refreshIcons(const QColor &ink);
    // In a panel (above Colours or Assets, floating) the strip wraps onto further rows
    // instead of hiding tools behind QToolBar's overflow button. The same buttons, actions,
    // shortcuts, tooltips and accessible names are kept, in the same order. Edge
    // placements use the plain toolbar layout.
    void setWrapping(bool wrap);
    bool wrapping() const { return flow != nullptr; }
    // Width of the strip on one row in wrapping mode (0 otherwise).
    int singleRowWidth() const;
  signals:
    void colorsRequested();
};

// Context options for the active tool (top bar).
class ToolOptionsBar : public QToolBar {
    Q_OBJECT
    ToolState *state;
    QStackedWidget *pages;
    QMap<QString, int> pageIndex;
    bool updating = false;
    QWidget *paintPage();
    QWidget *selectPage();
    QWidget *shapePage();
    QWidget *fillPage();
    QWidget *gradientPage();
    QWidget *transformPage();
    QWidget *pickPage();
    void showFor(Tool t);
    int ensurePage(const QString &page);

  public:
    ToolOptionsBar(ToolState *state, QWidget *parent = nullptr);
    void reload(); // pull values from state into controls
  signals:
    void saveBrushPreset();
    void loadBrushPreset(const QString &name);
};
} // namespace nova
