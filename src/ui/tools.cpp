// SPDX-License-Identifier: GPL-3.0-or-later
#include "tools.h"
#include <QFormLayout>
#include "theme.h"
#include <QActionGroup>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QKeyEvent>
#include <QWidgetAction>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QImageReader>
#include <QMenu>
#include <QPainter>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>

namespace nova {
const QList<ToolInfo> &toolInfos() {
    static const QList<ToolInfo> list = {
        {Tool::Pencil, "pencil", "Pencil", "Draw", "B", "Exact hard-edged pixels (Alt-click picks a colour, Shift-click draws a line)"},
        {Tool::Brush, "brush", "Brush", "Draw", "Shift+B", "Sized brush with opacity, hardness and spacing (Alt-click picks a colour, Shift-click draws a line)"},
        {Tool::Eraser, "eraser", "Eraser", "Draw", "E", "Erase to transparency with the brush footprint (Shift-click erases a line)"},
        {Tool::ColorReplace, "replace", "Colour Replace", "Draw", "Shift+E", "Paints only over pixels matching the background colour (Alt-click picks a colour)"},
        {Tool::RectSelect, "rect-select", "Rectangle Select", "Select", "M", "Drag a rectangle (Shift add, Alt subtract, Shift+Alt intersect, Ctrl square)"},
        {Tool::EllipseSelect, "ellipse-select", "Ellipse Select", "Select", "Shift+M", "Drag an ellipse (same modifiers as rectangle)"},
        {Tool::Lasso, "lasso", "Lasso", "Select", "L", "Freehand selection"},
        {Tool::PolygonLasso, "polygon-lasso", "Polygon Lasso", "Select", "Shift+L", "Click points; double-click or Enter closes, Backspace removes a point"},
        {Tool::MagicWand, "magic-wand", "Magic Wand", "Select", "W", "Select contiguous similar colours"},
        {Tool::ColorRange, "color-range", "Colour Range", "Select", "Shift+W", "Select every pixel within the tolerance"},
        {Tool::Move, "move", "Move", "Transform", "V", "Lift the selection (or layer) and place it; Enter applies, Esc cancels"},
        {Tool::Transform, "transform", "Transform", "Transform", "T", "Scale and rotate the selection or layer with a live preview"},
        {Tool::Line, "line", "Line", "Shapes", "Shift+U", "Straight line with the brush (Shift snaps to 45°, Alt-click picks a colour)"},
        {Tool::Shape, "shape", "Shape", "Shapes", "U", "Rectangles, ellipses, polygons and stars (Shift keeps aspect, Alt from centre)"},
        {Tool::Fill, "fill", "Fill Bucket", "Fill", "G", "Contiguous or Universal Fill with tolerance (Alt-click picks a colour)"},
        {Tool::Gradient, "gradient", "Gradient", "Fill", "Shift+G", "Drag a foreground→background gradient (Shift snaps to 45°, Alt-click picks a colour)"},
        {Tool::Eyedropper, "eyedropper", "Eyedropper", "Colour", "I", "Pick the foreground (Shift or right-click: background)"},
    };
    return list;
}
const ToolInfo &toolInfo(Tool tool) {
    for (const auto &info : toolInfos())
        if (info.tool == tool)
            return info;
    return toolInfos().first();
}
bool isPaintTool(Tool t) {
    return t == Tool::Pencil || t == Tool::Brush || t == Tool::Eraser || t == Tool::ColorReplace;
}
bool isSelectTool(Tool t) { return t >= Tool::RectSelect && t <= Tool::ColorRange; }

QIcon toolIcon(Tool tool, const QColor &ink) {
    return ui::lineIcon(toolInfo(tool).id, ink, ui::current().disabled);
}

// ---------------------------------------------------------------- state
ops::Brush &ToolState::brush(Tool t) {
    if (!brushes.contains(t)) {
        ops::Brush b;
        if (t == Tool::Brush) {
            b.size = 4;
            b.shape = ops::BrushShape::Circle;
        }
        brushes.insert(t, b);
    }
    return brushes[t];
}
ops::Brush ToolState::brush(Tool t) const {
    return brushes.contains(t) ? brushes.value(t) : const_cast<ToolState *>(this)->brush(t);
}
StrokeStyle ToolState::strokeStyle(Tool t) const {
    StrokeStyle s;
    s.brush = brush(t);
    s.color = foreground;
    s.erase = t == Tool::Eraser;
    s.symmetry = symmetry;
    s.radial = radial;
    s.lockAlpha = lockAlpha;
    s.wrap = wrap;
    if (paletteLock)
        s.palette = palette;
    if (t == Tool::ColorReplace) {
        s.replaceOnly = true;
        s.replaceColor = background;
        s.replaceTolerance = replaceTolerance;
    }
    return s;
}
void ToolState::setTool(Tool t) {
    if (tool == t)
        return;
    emit commitRequested(); // finish floating placements before switching tools
    tool = t;
    emit toolChanged(t);
    emit changed();
}
void ToolState::setForeground(QRgb c) {
    foreground = c;
    rememberColor(c);
    emit colorsChanged();
    emit changed();
}
void ToolState::setBackground(QRgb c) {
    background = c;
    emit colorsChanged();
    emit changed();
}
void ToolState::swapColors() {
    std::swap(foreground, background);
    emit colorsChanged();
    emit changed();
}
void ToolState::resetColors() {
    foreground = qRgba(0, 0, 0, 255);
    background = qRgba(255, 255, 255, 255);
    emit colorsChanged();
    emit changed();
}
void ToolState::rememberColor(QRgb c) {
    const auto name = ops::colorName(c);
    recent.removeAll(name);
    recent.prepend(name);
    while (recent.size() > 16)
        recent.removeLast();
}
QJsonObject ToolState::brushPreset(Tool t) const {
    const auto b = brush(t);
    QJsonObject preset{{"schema", 3}, {"source_colors", b.sourceColors}, {"shape", int(b.shape)}, {"size", b.size}, {"height", b.height}, {"opacity", b.opacity},
            {"hardness", b.hardness}, {"spacing", b.spacing}, {"smoothing", smoothing},
            {"symmetry", int(symmetry)}, {"radial", radial}};
    if (!b.stamp.isNull()) {
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        if (b.stamp.scaled(b.stamp.size().boundedTo({ops::Brush::MaxSize, ops::Brush::MaxSize}),
                           Qt::IgnoreAspectRatio, Qt::FastTransformation).save(&buffer, "PNG"))
            preset["stamp_png"] = QString::fromLatin1(data.toBase64());
    }
    return preset;
}
void ToolState::applyBrushPreset(Tool t, const QJsonObject &p) {
    auto &b = brush(t);
    b.sourceColors = p["schema"].toInt() >= 3 && p["source_colors"].toBool();
    b.shape = ops::BrushShape(std::clamp(p["shape"].toInt(), 0, 5));
    b.size = std::clamp(p["size"].toInt(1), 1, ops::Brush::MaxSize);
    b.height = std::clamp(p["height"].toInt(0), 0, ops::Brush::MaxSize);
    b.stamp = {};
    const auto encoded = p["stamp_png"].toString();
    if (!encoded.isEmpty() && encoded.size() <= 512 * 1024) {
        auto data = QByteArray::fromBase64(encoded.toLatin1());
        QBuffer buffer(&data);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "PNG");
        const auto size = reader.size();
        if (size.width() > 0 && size.height() > 0 && size.width() <= ops::Brush::MaxSize && size.height() <= ops::Brush::MaxSize)
            b.stamp = reader.read().convertToFormat(QImage::Format_RGBA8888);
    }
    // A missing/corrupt stamp must never silently turn into a solid square dab.
    if (b.shape == ops::BrushShape::Custom && b.stamp.isNull())
        b.shape = ops::BrushShape::Square;
    b.opacity = std::clamp(p["opacity"].toInt(255), 0, 255);
    b.hardness = std::clamp(p["hardness"].toInt(100), 0, 100);
    b.spacing = std::clamp(p["spacing"].toInt(0), 0, 1000);
    smoothing = std::clamp(p["smoothing"].toInt(0), 0, 100);
    symmetry = ops::Symmetry(std::clamp(p["symmetry"].toInt(0), 0, 4));
    radial = std::clamp(p["radial"].toInt(6), 2, 32);
    emit changed();
}

// ---------------------------------------------------------------- tool box
namespace {
// A rail button whose menu holds more tools. As in Photoshop, a small triangle in the
// bottom-right corner marks it instead of a separate arrow segment, so every rail button is
// the same square. Press and hold, right-click, Alt+Down or the menu key opens the menu.
class ToolGroupButton : public QToolButton {
  public:
    using QToolButton::QToolButton;

  protected:
    void paintEvent(QPaintEvent *e) override {
        QToolButton::paintEvent(e);
        if (!menu())
            return;
        const auto &t = ui::current();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(!isEnabled() ? t.disabled : isChecked() ? t.accent : t.muted);
        const QPointF corner = QRectF(rect()).bottomRight() - QPointF(ui::SpaceXS + 1, ui::SpaceXS + 1);
        const qreal size = 6;
        p.drawPolygon(QPolygonF{corner, corner - QPointF(size, 0), corner - QPointF(0, size)});
    }
    void contextMenuEvent(QContextMenuEvent *e) override {
        if (!menu())
            return QToolButton::contextMenuEvent(e);
        showMenu();
    }
    void keyPressEvent(QKeyEvent *e) override {
        if (menu() && (e->key() == Qt::Key_Menu || (e->key() == Qt::Key_Down && e->modifiers() & Qt::AltModifier))) {
            showMenu();
            return;
        }
        QToolButton::keyPressEvent(e);
    }
};
// Foreground/background chip at the foot of the tool strip (fast colour access).
class ColorChip : public QWidget {
    ToolState *state;

  public:
    std::function<void()> requested;
    ColorChip(ToolState *s, QWidget *parent) : QWidget(parent), state(s) {
        setObjectName("toolColorChip");
        setFixedSize(ui::ToolHitTarget, ui::ToolHitTarget);
        setCursor(Qt::PointingHandCursor);
        setToolTip("Foreground / background colour\nClick, Enter or Space: Colours panel · X swaps · D resets");
        setAccessibleName("Foreground and background colours");
        setFocusPolicy(Qt::TabFocus); // reachable with Tab like the tool buttons
        connect(state, &ToolState::colorsChanged, this, qOverload<>(&QWidget::update));
    }
    void keyPressEvent(QKeyEvent *e) override {
        if ((e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && requested) {
            requested();
            return;
        }
        QWidget::keyPressEvent(e);
    }
    void focusInEvent(QFocusEvent *e) override {
        QWidget::focusInEvent(e);
        update();
    }
    void focusOutEvent(QFocusEvent *e) override {
        QWidget::focusOutEvent(e);
        update();
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        auto swatch = [&](QRect r, QRgb c) {
            p.fillRect(r, QColor(120, 120, 120));
            p.fillRect(r.adjusted(0, 0, -r.width() / 2, -r.height() / 2), QColor(170, 170, 170));
            p.fillRect(r.adjusted(r.width() / 2, r.height() / 2, 0, 0), QColor(170, 170, 170));
            p.fillRect(r, QColor::fromRgba(c));
            p.setPen(palette().color(QPalette::WindowText));
            p.drawRect(r.adjusted(0, 0, -1, -1));
        };
        swatch(QRect(11, 11, 14, 14), state->background);
        swatch(QRect(4, 4, 14, 14), state->foreground);
        if (hasFocus()) {
            p.setPen(QPen(ui::current().accent, 1));
            p.drawRect(rect().adjusted(0, 0, -1, -1));
        }
    }
    void mousePressEvent(QMouseEvent *) override {
        if (requested)
            requested();
    }
};
} // namespace

ToolBox::ToolBox(ToolState *s, QWidget *parent) : QToolBar("Tools", parent), state(s) {
    setObjectName("toolBox");
    setOrientation(Qt::Vertical);
    // A fixed rail on the left edge, as in other pixel editors: the tools never move, so
    // muscle memory and every workspace agree on where they are.
    setMovable(false);
    setFloatable(false);
    setAllowedAreas(Qt::LeftToolBarArea);
    setIconSize({ui::ToolIconSize, ui::ToolIconSize});
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    exclusive = new QActionGroup(this);
    for (const auto &info : toolInfos()) {
        // Each split button already groups its variants. Keep rules only around
        // the Effects and colour controls so the tool choices scan as one cluster.
        auto a = new QAction(info.name, this);
        a->setObjectName("tool-" + info.id);
        a->setCheckable(true);
        a->setShortcut(QKeySequence(info.shortcut));
        a->setShortcutContext(Qt::WindowShortcut);
        a->setToolTip(QString("%1 (%2)\n%3").arg(info.name, info.shortcut, info.tip));
        a->setStatusTip(info.tip);
        a->setData(int(info.tool));
        exclusive->addAction(a);
        actions.insert(info.tool, a);
        connect(a, &QAction::triggered, this, [this, t = info.tool] { state->setTool(t); });
        auto &button = groups[info.group];
        if (!button) {
            button = new ToolGroupButton(this);
            button->setObjectName("toolGroup-" + info.group);
            button->setPopupMode(QToolButton::DelayedPopup);
            button->setAutoRaise(true);
            button->setDefaultAction(a);
            button->setMenu(new QMenu(button));
            addWidget(button);
        }
        button->menu()->addAction(a);
    }
    for (auto button : std::as_const(groups))
        if (button->menu()->actions().size() < 2)
            button->setMenu(nullptr);
    addSeparator();
    effectsButton = new ToolGroupButton(this);
    effectsButton->setObjectName("effectsButton");
    effectsButton->setText("Effects");
    effectsButton->setProperty("novaIcon", "effects");
    effectsButton->setIcon(ui::lineIcon("effects"));
    effectsButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    effectsButton->setToolTip("Effects: adjustments, filters, shading and generators");
    effectsButton->setPopupMode(QToolButton::InstantPopup);
    effectsButton->setAutoRaise(true);
    effectsButton->setAccessibleName("Effects");
    addWidget(effectsButton);
    addSeparator();
    auto spacer = new QWidget(this);
    spacer->setFixedHeight(ui::SpaceS);
    spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    spacerItem = addWidget(spacer);
    // Keep colour wells close to the tools in a vertical strip too.
    connect(this, &QToolBar::orientationChanged, this,
            [this](Qt::Orientation o) { spacerItem->setVisible(o == Qt::Vertical); });
    auto chip = new ColorChip(state, this);
    chip->requested = [this] { emit colorsRequested(); };
    chipItem = addWidget(chip);
    connect(state, &ToolState::toolChanged, this, &ToolBox::sync);
    sync();
}
void ToolBox::refreshIcons(const QColor &ink) {
    for (auto it = actions.begin(); it != actions.end(); ++it)
        it.value()->setIcon(toolIcon(it.key(), ink));
}
void ToolBox::sync() {
    auto a = actions.value(state->tool);
    if (!a)
        return;
    a->setChecked(true);
    if (auto button = groups.value(toolInfo(state->tool).group))
        button->setDefaultAction(a); // the group shows its last-used variant
}

// ---------------------------------------------------------------- options bar
namespace {
QSpinBox *spin(const QString &tip, int lo, int hi, int value, const QString &suffix = {}) {
    auto s = new QSpinBox;
    s->setRange(lo, hi);
    s->setValue(value);
    s->setToolTip(tip);
    s->setAccessibleName(tip);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    // Options-bar fields are compact value fields (wheel, arrow keys or typing), as in
    // other creative tools' option bars; step buttons would push the row off screen.
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setAlignment(Qt::AlignRight);
    return s;
}
// Pages are created directly under their stacked widget: building a parentless
// (top-level) container and reparenting it later cost ~120 ms at startup.
QWidget *pageParent = nullptr;
// Options row sized to its controls (the former fixed 58 px row wasted a third
// of its height). A horizontal scroll bar appears only when the window is too
// narrow, and the row grows by exactly its height while it is shown.
// Fades the trailing edge of a scrolled options row into the bar, so a row that
// continues past the window edge reads as scrollable rather than clipped.
class EdgeFade : public QWidget {
  public:
    using QWidget::QWidget;
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        QLinearGradient fade(0, 0, width(), 0);
        QColor clear = ui::current().window;
        clear.setAlpha(0);
        fade.setColorAt(0, clear);
        fade.setColorAt(1, ui::current().window);
        p.fillRect(rect(), fade);
    }
};
class OptionsRow : public QScrollArea {
    EdgeFade *fade = nullptr;
    void settleHeight() {
        setFixedHeight(sizeHint().height());
        if (auto stack = qobject_cast<QStackedWidget *>(parentWidget()); stack && stack->currentWidget() == this)
            stack->setFixedHeight(height());
    }
    void placeFade() {
        if (!fade) {
            fade = new EdgeFade(this);
            fade->setAttribute(Qt::WA_TransparentForMouseEvents);
            connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { placeFade(); });
            connect(horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this] { placeFade(); });
        }
        const auto bar = horizontalScrollBar();
        fade->setVisible(bar->isVisible() && bar->value() < bar->maximum());
        fade->setGeometry(viewport()->geometry().right() - 40, viewport()->y(), 41, viewport()->height());
        fade->raise();
    }

  public:
    using QScrollArea::QScrollArea;
    QSize sizeHint() const override {
        const int bar = horizontalScrollBar()->isVisible() ? horizontalScrollBar()->sizeHint().height() : 0;
        return {QScrollArea::sizeHint().width(), (widget() ? widget()->sizeHint().height() : 28) + bar};
    }
    QSize minimumSizeHint() const override { return {80, sizeHint().height()}; }
    // The leading tool name is a courtesy: it steps aside before the row would need to scroll.
    void fitToolName(int width) {
        auto name = widget() ? widget()->findChild<QLabel *>("toolOptionsName") : nullptr;
        if (!name)
            return;
        const bool shown = name->isVisibleTo(widget());
        const int extra = shown ? 0 : name->sizeHint().width() + 2 * ui::SpaceM + 2 * ui::GroupGap + 1;
        const bool fits = widget()->sizeHint().width() + extra <= width;
        if (shown != fits) {
            name->setVisible(fits);
            // The gap after the name goes with it.
            const auto layout = widget()->layout();
            const int at = layout->indexOf(name);
            if (at >= 0 && at + 1 < layout->count() && layout->itemAt(at + 1)->widget())
                layout->itemAt(at + 1)->widget()->setVisible(fits);
            widget()->adjustSize();
        }
    }
    void resizeEvent(QResizeEvent *e) override {
        if (widget() && widget()->layout())
            widget()->layout()->setSpacing(e->size().width() < ui::OptionsCompactWidth ? ui::SpaceS : ui::SpaceM);
        fitToolName(e->size().width());
        const bool before = horizontalScrollBar()->isVisible();
        QScrollArea::resizeEvent(e);
        placeFade();
        if (before != horizontalScrollBar()->isVisible() || height() != sizeHint().height())
            QTimer::singleShot(0, this, [this] { settleHeight(); });
    }
    // Pages pre-built during idle time sit hidden in the stacked widget, which never
    // resizes them, and scrollbar children report isVisible() == false while their page
    // is hidden. Such a page keeps the bar-less fixed height from construction and its
    // controls end up clipped by exactly the scrollbar height once it is shown. Re-evaluate
    // the height after the page reaches the screen, when the scrollbar state is real.
    void showEvent(QShowEvent *e) override {
        QScrollArea::showEvent(e);
        fitToolName(width());
        QTimer::singleShot(0, this, [this] { settleHeight(); });
    }
};
// Extra whitespace, rather than another rule, separates related option groups.
QWidget *groupGap() {
    auto gap = new QWidget;
    auto layout = new QHBoxLayout(gap);
    layout->setContentsMargins(ui::GroupGap, 0, ui::GroupGap, 0);
    auto rule = new QFrame;
    rule->setProperty("role", "vrule");
    rule->setFixedSize(1, ui::IconSize);
    layout->addWidget(rule);
    return gap;
}
QWidget *row(std::initializer_list<QWidget *> widgets) {
    auto scroll = new OptionsRow(pageParent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto w = new QWidget;
    auto l = new QHBoxLayout(w);
    l->setContentsMargins(ui::SpaceXS, ui::SpaceXS, ui::SpaceXS, ui::SpaceXS);
    l->setSpacing(ui::SpaceM);
    for (auto x : widgets)
        l->addWidget(x ? x : groupGap());
    l->addStretch();
    scroll->setWidget(w);
    scroll->setFixedHeight(scroll->sizeHint().height());
    return scroll;
}
QLabel *label(const QString &text) { return new QLabel(text); }
} // namespace
ToolOptionsBar::ToolOptionsBar(ToolState *s, QWidget *parent) : QToolBar("Tool Options", parent), state(s) {
    setObjectName("toolOptions");
    setMovable(false);
    pages = new QStackedWidget;
    // Pages are built lazily the first time their tool is selected.
    addWidget(pages);
    connect(state, &ToolState::toolChanged, this, [this](Tool t) { showFor(t); });
    connect(state, &ToolState::changed, this, [this] { reload(); });
    // Off the startup path: the first page is added after the window's first event-loop
    // turn (Qt's first stacked-page insertion costs ~120 ms here) and the remaining pages
    // are pre-built during idle time so the first tool switch does not hitch.
    QTimer::singleShot(0, this, [this] {
        showFor(state->tool);
        QTimer::singleShot(2000, this, [this] {
            for (const char *name : {"paint", "select", "shape", "fill", "gradient", "transform", "pick"})
                ensurePage(name);
        });
    });
}
int ToolOptionsBar::ensurePage(const QString &page) {
    if (!pageIndex.contains(page)) {
        pageParent = pages;
        QWidget *w = page == "select"      ? selectPage()
                     : page == "shape"     ? shapePage()
                     : page == "fill"      ? fillPage()
                     : page == "gradient"  ? gradientPage()
                     : page == "transform" ? transformPage()
                     : page == "pick"      ? pickPage()
                                           : paintPage();
        const int current = pages->currentIndex();
        pageIndex[page] = pages->addWidget(w);
        if (current >= 0)
            pages->setCurrentIndex(current);
    }
    return pageIndex.value(page);
}
void ToolOptionsBar::showFor(Tool t) {
    QString page = "paint";
    if (isSelectTool(t))
        page = "select";
    else if (t == Tool::Shape)
        page = "shape";
    else if (t == Tool::Fill)
        page = "fill";
    else if (t == Tool::Gradient)
        page = "gradient";
    else if (t == Tool::Move || t == Tool::Transform)
        page = "transform";
    else if (t == Tool::Eyedropper)
        page = "pick";
    pages->setCurrentIndex(ensurePage(page));
    reload();
    // Hidden pages may need scrollbars, but must not make the active page taller.
    pages->setFixedHeight(pages->currentWidget()->sizeHint().height());
}
namespace {
// Every options row opens with the active tool's name, so the row always says what it edits.
QLabel *toolName() {
    auto name = new QLabel;
    name->setObjectName("toolOptionsName");
    return name;
}
} // namespace
QWidget *ToolOptionsBar::paintPage() {
    auto name = new QLabel;
    name->setObjectName("paintToolName");
    auto shape = new QComboBox;
    shape->setObjectName("brushShape");
    shape->addItems(ops::brushShapeNames());
    // Pickers size to a short label, not their longest item, so the row fits 1280 px.
    shape->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    shape->setMinimumContentsLength(7);
    shape->setToolTip("Brush shape (Custom uses the stamp captured from a selection)");
    auto size = spin("Brush size ([ / ] to change)", 1, ops::Brush::MaxSize, 1, " px");
    size->setObjectName("brushSize");
    auto height = spin("Brush height (Auto keeps a square; custom stamps retain their original size)", 0, ops::Brush::MaxSize, 0, " px");
    height->setSpecialValueText("Auto");
    height->setObjectName("brushHeight");
    auto opacity = spin("Opacity (0–255; keys 1–9 and 0 set 10–100%)", 0, 255, 255);
    opacity->setObjectName("brushOpacity");
    auto hardness = spin("Hardness: 100 keeps exact pixel edges", 0, 100, 100, "%");
    hardness->setObjectName("brushHardness");
    auto spacing = spin("Spacing between dabs (percent of size, 0 = every pixel)", 0, 1000, 0, "%");
    spacing->setObjectName("brushSpacing");
    auto smoothing = spin("Stroke smoothing (0 = raw pointer)", 0, 100, 0, "%");
    smoothing->setObjectName("strokeSmoothing");
    auto symmetry = new QComboBox;
    symmetry->setObjectName("symmetry");
    symmetry->addItems(ops::symmetryNames());
    symmetry->setItemText(0, "No symmetry"); // the closed picker names its setting
    symmetry->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    symmetry->setMinimumContentsLength(8);
    symmetry->setToolTip("Symmetry painting (guides shown on the canvas)");
    auto radial = spin("Radial segments", 2, 32, 6);
    radial->setObjectName("radialCount");
    // Lock toggles are compact icon checkboxes; their names live in tooltips and for assistive tech.
    auto palette = new QCheckBox;
    palette->setIcon(ui::lineIcon("palette"));
    palette->setProperty("novaIcon", "palette");
    palette->setAccessibleName("Palette lock");
    palette->setProperty("role", "toggle");
    palette->setIconSize({ui::IconSize, ui::IconSize});
    auto stampColors = new QCheckBox("Stamp colours");
    stampColors->setObjectName("stampSourceColors");
    stampColors->setToolTip("Use source RGBA colours; off recolours stamp coverage with the foreground. Palette lock maps source colours.");
    palette->setObjectName("paletteLock");
    palette->setToolTip("Palette lock: snap stroke colours to the nearest colour of the active palette");
    auto lockAlpha = new QCheckBox;
    lockAlpha->setIcon(ui::lineIcon("opacity"));
    lockAlpha->setProperty("novaIcon", "opacity");
    lockAlpha->setAccessibleName("Lock alpha");
    lockAlpha->setProperty("role", "toggle");
    lockAlpha->setIconSize({ui::IconSize, ui::IconSize});
    lockAlpha->setToolTip("Lock alpha: paint only over existing pixels, preserving their transparency");
    lockAlpha->setObjectName("lockAlpha");
    auto wrap = new QCheckBox;
    wrap->setIcon(ui::lineIcon("grid"));
    wrap->setProperty("novaIcon", "grid");
    wrap->setAccessibleName("Wrap around edges");
    wrap->setProperty("role", "toggle");
    wrap->setIconSize({ui::IconSize, ui::IconSize});
    wrap->setObjectName("wrapPaint");
    wrap->setToolTip("Wrap around edges: strokes past one edge continue on the opposite edge, for seamless tiling textures");
    auto pixelPerfect = new QCheckBox("Pixel perfect");
    pixelPerfect->setObjectName("pixelPerfect");
    pixelPerfect->setToolTip("Removes the extra corner pixel where a 1 px freehand line turns (L-shapes become clean diagonals)");
    auto tolerance = spin("Colour replace tolerance", 0, 255, 32);
    tolerance->setObjectName("replaceTolerance");
    // A menu command like Brush settings beside it: frameless text with a drop-down arrow.
    auto presets = new QToolButton;
    presets->setText("Presets");
    presets->setPopupMode(QToolButton::InstantPopup);
    presets->setObjectName("brushPresets");
    presets->setToolTip("Save or apply brush presets");
    auto menu = new QMenu(presets);
    presets->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        menu->addAction("Save current brush as preset…", this, [this] { emit saveBrushPreset(); });
        menu->addSeparator();
        emit loadBrushPreset({}); // window repopulates via property
        for (const auto &n : property("brushPresetNames").toStringList())
            menu->addAction(n, this, [this, n] { emit loadBrushPreset(n); });
    });
    auto toleranceLabel = label("Tolerance");
    toleranceLabel->setObjectName("replaceToleranceLabel");
    auto settings = new QToolButton;
    settings->setText("Brush settings");
    settings->setObjectName("brushSettings");
    settings->setToolTip("Brush height, edge hardness, spacing and stroke smoothing");
    auto formBody = new QWidget;
    auto form = new QFormLayout(formBody);
    form->setContentsMargins(ui::PanelPadding, ui::PanelPadding, ui::PanelPadding, ui::PanelPadding);
    form->setHorizontalSpacing(ui::SpaceXL);
    form->setVerticalSpacing(ui::SpaceL);
    form->addRow("Height", ui::sliderField(height, "Height"));
    form->addRow("Hardness", ui::sliderField(hardness, "Hardness"));
    form->addRow("Spacing", ui::sliderField(spacing, "Spacing"));
    form->addRow("Smoothing", ui::sliderField(smoothing, "Smoothing"));
    height->setFixedWidth(ui::SettingsValueWidth);
    form->itemAt(0, QFormLayout::FieldRole)->setAlignment(Qt::AlignRight);
    ui::settingsMenu(settings, formBody);
    auto w = row({name, nullptr, shape, label("Size"), size, label("Opacity"), opacity,
                  settings, pixelPerfect, nullptr, symmetry, radial, nullptr, palette, stampColors, lockAlpha, wrap,
                  toleranceLabel, tolerance, nullptr, presets});
    auto brush = [this] { return &state->brush(state->tool); };
    connect(stampColors, &QCheckBox::toggled, this, [=, this](bool value) { if (!updating) { brush()->sourceColors = value; state->touch(); } });
    connect(shape, &QComboBox::currentIndexChanged, this, [=, this](int i) {
        if (!updating) { brush()->shape = ops::BrushShape(i); state->touch(); }
    });
    connect(size, &QSpinBox::valueChanged, this, [=, this](int v) { if (!updating) { brush()->size = v; state->touch(); } });
    connect(height, &QSpinBox::valueChanged, this, [=, this](int v) { if (!updating) { brush()->height = v; state->touch(); } });
    connect(opacity, &QSpinBox::valueChanged, this, [=, this](int v) { if (!updating) { brush()->opacity = v; state->touch(); } });
    connect(hardness, &QSpinBox::valueChanged, this, [=, this](int v) { if (!updating) { brush()->hardness = v; state->touch(); } });
    connect(spacing, &QSpinBox::valueChanged, this, [=, this](int v) { if (!updating) { brush()->spacing = v; state->touch(); } });
    connect(smoothing, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->smoothing = v; state->touch(); } });
    connect(symmetry, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!updating) { state->symmetry = ops::Symmetry(i); state->touch(); }
    });
    connect(radial, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->radial = v; state->touch(); } });
    connect(palette, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->paletteLock = v; state->touch(); } });
    connect(lockAlpha, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->lockAlpha = v; state->touch(); } });
    connect(wrap, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->wrap = v; state->touch(); } });
    connect(pixelPerfect, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->pixelPerfect = v; state->touch(); } });
    connect(tolerance, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->replaceTolerance = v; state->touch(); } });
    return w;
}
QWidget *ToolOptionsBar::selectPage() {
    auto modes = new QWidget;
    auto ml = new QHBoxLayout(modes);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(2);
    auto group = new QActionGroup(modes);
    const QStringList names = {"Replace", "Add (Shift)", "Subtract (Alt)", "Intersect (Shift+Alt)"};
    const QStringList icons = {"select-replace", "select-add", "select-subtract", "select-intersect"};
    for (int i = 0; i < 4; ++i) {
        auto a = new QAction(names[i].section(" (", 0, 0), group);
        a->setProperty("novaIcon", icons[i]);
        a->setIcon(ui::lineIcon(icons[i]));
        a->setCheckable(true);
        a->setToolTip("Selection mode: " + names[i]);
        a->setObjectName("selectMode" + QString::number(i));
        auto b = new QToolButton;
        b->setDefaultAction(a);
        b->setObjectName("selectModeButton" + QString::number(i));
        ml->addWidget(b);
        connect(a, &QAction::triggered, this, [this, i] {
            if (!updating) { state->selectMode = ops::Combine(i); state->touch(); }
        });
    }
    auto tolerance = spin("Colour tolerance (0 = exact)", 0, 255, 0);
    tolerance->setObjectName("selectTolerance");
    auto metric = new QComboBox;
    metric->setObjectName("selectMetric");
    metric->addItems(ops::distanceNames());
    metric->setToolTip("Colour distance: RGB/RGBA channel difference, perceptual redmean, or hue");
    auto composite = new QCheckBox("Sample all layers");
    composite->setObjectName("sampleComposite");
    composite->setToolTip("Match colours on the visible image instead of the selected layer");
    auto alpha = new QCheckBox("Match alpha");
    alpha->setObjectName("includeAlpha");
    alpha->setToolTip("Compare transparency too; off matches colours whatever their alpha");
    auto snap = spin("Snap rectangle/ellipse selections to a grid of N pixels", 1, 64, 1, " px");
    snap->setObjectName("selectSnap");
    auto w = row({toolName(), nullptr, label("Mode"), modes, nullptr, label("Tolerance"), tolerance, metric, composite, alpha, nullptr,
                  label("Grid"), snap});
    connect(snap, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->snap = v; state->touch(); } });
    connect(tolerance, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->tolerance = v; state->touch(); } });
    connect(metric, &QComboBox::currentIndexChanged, this, [this](int i) { if (!updating) { state->metric = ops::Distance(i); state->touch(); } });
    connect(composite, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->sampleComposite = v; state->touch(); } });
    connect(alpha, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->includeAlpha = v; state->touch(); } });
    return w;
}
QWidget *ToolOptionsBar::shapePage() {
    auto kind = new QComboBox;
    kind->setObjectName("shapeKind");
    auto names = ops::shapeNames();
    names.removeLast(); // Line has its own tool
    kind->addItems(names);
    kind->setToolTip("Shape kind");
    auto filled = new QCheckBox("Filled");
    filled->setObjectName("shapeFilled");
    filled->setToolTip("Fill the shape; off draws its outline at the given thickness");
    auto smooth = new QCheckBox("Smooth edges");
    smooth->setObjectName("shapeSmooth");
    smooth->setToolTip("Optional anti-aliased coverage; leave off for exact pixel-art edges");
    auto thickness = spin("Outline thickness", 1, 64, 1, " px");
    thickness->setObjectName("shapeThickness");
    auto radius = new QDoubleSpinBox;
    radius->setRange(0, 512);
    radius->setValue(2);
    radius->setToolTip("Corner radius (rounded rectangle)");
    radius->setObjectName("shapeRadius");
    auto sides = spin("Sides / star points", 3, 64, 5);
    sides->setObjectName("shapeSides");
    auto inner = new QDoubleSpinBox;
    inner->setRange(.05, .95);
    inner->setSingleStep(.05);
    inner->setValue(.5);
    inner->setToolTip("Star inner radius ratio");
    inner->setObjectName("shapeInner");
    auto rotation = new QDoubleSpinBox;
    rotation->setRange(-360, 360);
    rotation->setSuffix("°");
    rotation->setToolTip("Rotation around the shape centre");
    rotation->setObjectName("shapeRotation");
    auto flipH = new QCheckBox("Flip H"), flipV = new QCheckBox("Flip V");
    flipH->setObjectName("shapeFlipH");
    flipH->setToolTip("Mirror the shape horizontally");
    flipV->setObjectName("shapeFlipV");
    flipV->setToolTip("Mirror the shape vertically");
    auto centre = new QCheckBox("From centre");
    centre->setObjectName("shapeFromCentre");
    centre->setToolTip("Drag from the centre (also hold Alt)");
    auto opacity = spin("Opacity (0–255)", 0, 255, 255);
    opacity->setObjectName("shapeOpacity");
    // Shape-specific values show only for the shapes that use them.
    auto radiusLabel = label("Radius"), sidesLabel = label("Sides"), innerLabel = label("Inner");
    radiusLabel->setObjectName("shapeRadiusLabel");
    sidesLabel->setObjectName("shapeSidesLabel");
    innerLabel->setObjectName("shapeInnerLabel");
    auto snap = spin("Snap shape corners to a grid of N pixels (1 = every pixel)", 1, 64, 1, " px");
    auto numeric = new QPushButton("Parameters / presets…");
    numeric->setObjectName("shapeParameters");
    connect(numeric,&QPushButton::clicked,state,&ToolState::shapeSettingsRequested);
    snap->setObjectName("shapeSnap");
    auto w = row({toolName(), nullptr, kind, filled, smooth, nullptr, label("Thickness"), thickness, radiusLabel, radius, sidesLabel, sides,
                  innerLabel, inner, nullptr, label("Rotate"), rotation, flipH, flipV, centre, nullptr, label("Opacity"),
                  opacity, label("Grid"), snap, nullptr, numeric});
    connect(snap, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->snap = v; state->touch(); } });
    connect(kind, &QComboBox::currentIndexChanged, this, [this](int i) { if (!updating) { state->shape = ops::ShapeKind(i); state->touch(); } });
    connect(filled, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->filled = v; state->touch(); } });
    connect(smooth, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->smoothShape = v; state->touch(); } });
    connect(thickness, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->thickness = v; state->touch(); } });
    connect(radius, &QDoubleSpinBox::valueChanged, this, [this](double v) { if (!updating) { state->radius = v; state->touch(); } });
    connect(sides, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->sides = v; state->touch(); } });
    connect(inner, &QDoubleSpinBox::valueChanged, this, [this](double v) { if (!updating) { state->innerRatio = v; state->touch(); } });
    connect(rotation, &QDoubleSpinBox::valueChanged, this, [this](double v) { if (!updating) { state->rotation = v; state->touch(); } });
    connect(flipH, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->flipH = v; state->touch(); } });
    connect(flipV, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->flipV = v; state->touch(); } });
    connect(centre, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->fromCentre = v; state->touch(); } });
    connect(opacity, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->shapeOpacity = v; state->touch(); } });
    return w;
}
namespace {
// Mode help shown on the mode buttons and on the tolerance box. The two modes compare
// colours differently on purpose; Contiguous keeps its original rule.
const char *contiguousFillHelp =
    "Contiguous: fills the connected area (edge neighbours) around the clicked pixel.\n"
    "Tolerance 0–255 uses the chosen colour metric; alpha may differ by up to the tolerance;\n"
    "fully transparent pixels match each other.";
const char *universalFillHelp =
    "Universal Fill: replaces every matching pixel in the image, connected or not.\n"
    "A pixel matches when R, G and B each differ by at most the tolerance (0–255) and alpha\n"
    "is exactly equal. Fully transparent pixels match each other whatever their hidden colour.\n"
    "Tolerance 0 = exact colour. The selection and Sample all layers still apply.";
} // namespace
QWidget *ToolOptionsBar::fillPage() {
    auto modes = new QWidget;
    modes->setObjectName("fillMode");
    modes->setAccessibleName("Fill mode");
    auto ml = new QHBoxLayout(modes);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(ui::SpaceXS);
    auto group = new QActionGroup(modes);
    for (bool contiguous : {true, false}) {
        auto a = new QAction(contiguous ? "Contiguous" : "Universal Fill", group);
        a->setCheckable(true);
        a->setChecked(contiguous);
        a->setObjectName(contiguous ? "fillModeContiguous" : "fillModeUniversal");
        a->setToolTip(contiguous ? contiguousFillHelp : universalFillHelp);
        auto b = new QToolButton;
        b->setDefaultAction(a);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setObjectName(a->objectName() + "Button");
        b->setAccessibleName("Fill mode: " + a->text());
        ml->addWidget(b);
        connect(a, &QAction::triggered, this, [this, contiguous] {
            if (!updating) { state->contiguous = contiguous; state->touch(); }
        });
    }
    auto tolerance = spin("Tolerance", 0, 255, 0);
    tolerance->setObjectName("fillTolerance");
    tolerance->setToolTip(contiguousFillHelp);
    auto metric = new QComboBox;
    metric->setObjectName("fillMetric");
    metric->setAccessibleName("Contiguous fill colour metric");
    metric->setToolTip("Contiguous fill colour distance. Universal Fill always compares R, G and B.");
    metric->addItems(ops::distanceNames());
    auto composite = new QCheckBox("Sample all layers");
    composite->setObjectName("fillSampleComposite");
    composite->setToolTip("Match colours on the visible image; only the selected layer changes");
    auto opacity = spin("Opacity (0–255)", 0, 255, 255);
    opacity->setObjectName("fillOpacity");
    auto w = row({toolName(), nullptr, modes, nullptr, label("Tolerance"), tolerance, metric, composite, label("Opacity"), opacity});
    connect(tolerance, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->tolerance = v; state->touch(); } });
    connect(metric, &QComboBox::currentIndexChanged, this, [this](int i) { if (!updating) { state->metric = ops::Distance(i); state->touch(); } });
    connect(composite, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->sampleComposite = v; state->touch(); } });
    connect(opacity, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->fillOpacity = v; state->touch(); } });
    return w;
}
QWidget *ToolOptionsBar::gradientPage() {
    auto type = new QComboBox;
    type->setObjectName("gradientType");
    type->addItems({"Linear", "Radial", "Reflected"});
    type->setToolTip("Gradient type: Linear, Radial from the start point, or Reflected around it");
    auto steps = spin("Discrete steps (0 = smooth)", 0, 64, 0);
    steps->setObjectName("gradientSteps");
    auto dither = new QCheckBox("Ordered dither");
    dither->setObjectName("gradientDither");
    dither->setToolTip("Bayer 4×4 dithering between steps, a classic pixel-art blend");
    auto opacity = spin("Opacity (0–255)", 0, 255, 255);
    opacity->setObjectName("gradientOpacity");
    auto w = row({toolName(), nullptr, label("Foreground → background"), type, label("Steps"), steps, dither, label("Opacity"), opacity});
    connect(type, &QComboBox::currentTextChanged, this, [this](const QString &v) { if (!updating) { state->gradientType = v; state->touch(); } });
    connect(steps, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->gradientSteps = v; state->touch(); } });
    connect(dither, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->gradientDither = v; state->touch(); } });
    connect(opacity, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->fillOpacity = v; state->touch(); } });
    return w;
}
QWidget *ToolOptionsBar::transformPage() {
    auto sx = spin("Horizontal scale", 1, 1600, 100, "%");
    sx->setObjectName("transformScaleX");
    auto sy = spin("Vertical scale", 1, 1600, 100, "%");
    sy->setObjectName("transformScaleY");
    auto angle = new QDoubleSpinBox;
    angle->setObjectName("transformAngle");
    angle->setRange(-360, 360);
    angle->setSuffix("°");
    angle->setToolTip("Rotation (degrees clockwise)");
    auto resampling = new QComboBox;
    resampling->setObjectName("transformResampling");
    resampling->addItems({"Nearest neighbour", "Bilinear", "Bicubic"});
    resampling->setToolTip("Nearest neighbour keeps pixel art crisp");
    auto apply = new QPushButton("Apply ⏎");
    apply->setObjectName("transformApply");
    auto cancel = new QPushButton("Cancel Esc");
    cancel->setObjectName("transformCancel");
    auto reset = new QPushButton("Reset");
    reset->setObjectName("transformReset");
    auto hint = new QLabel("Drag inside to move · drag corners to scale (Shift keeps aspect) · arrows nudge");
    hint->setEnabled(false);
    auto flipH = new QCheckBox("Flip H");
    flipH->setObjectName("transformFlipH");
    auto flipV = new QCheckBox("Flip V");
    flipV->setObjectName("transformFlipV");
    auto hidden = new QCheckBox("Include hidden");
    hidden->setObjectName("transformHidden");
    hidden->setToolTip("Also move or transform hidden layers among the selected layers");
    auto w = row({toolName(), nullptr, label("Selected layers"), hidden, nullptr, label("Scale"), sx, sy, label("Rotate"), angle,
                  resampling, flipH, flipV, nullptr, apply, cancel, reset, hint});
    connect(flipH, &QCheckBox::toggled, this, [this](bool value) {
        if (!updating) {
            state->transformFlipH = value;
            emit state->transformChanged();
        }
    });
    connect(flipV, &QCheckBox::toggled, this, [this](bool value) {
        if (!updating) {
            state->transformFlipV = value;
            emit state->transformChanged();
        }
    });
    connect(hidden, &QCheckBox::toggled, this, [this](bool value) {
        if (!updating)
            state->transformHidden = value;
    });
    connect(sx, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->scaleX = v; emit state->transformChanged(); } });
    connect(sy, &QSpinBox::valueChanged, this, [this](int v) { if (!updating) { state->scaleY = v; emit state->transformChanged(); } });
    connect(angle, &QDoubleSpinBox::valueChanged, this, [this](double v) { if (!updating) { state->angle = v; emit state->transformChanged(); } });
    connect(resampling, &QComboBox::currentTextChanged, this, [this](const QString &v) {
        if (!updating) { state->resampling = v; emit state->transformChanged(); }
    });
    connect(apply, &QPushButton::clicked, state, &ToolState::commitRequested);
    connect(cancel, &QPushButton::clicked, state, &ToolState::cancelRequested);
    connect(reset, &QPushButton::clicked, state, &ToolState::resetTransformRequested);
    return w;
}
QWidget *ToolOptionsBar::pickPage() {
    auto composite = new QCheckBox("Sample all layers");
    composite->setObjectName("pickComposite");
    composite->setToolTip("Pick from the visible image instead of the selected layer");
    auto w = row({toolName(), nullptr, label("Click: foreground · Shift-click / right-click: background"), composite});
    connect(composite, &QCheckBox::toggled, this, [this](bool v) { if (!updating) { state->sampleComposite = v; state->touch(); } });
    return w;
}
void ToolOptionsBar::reload() {
    updating = true;
    auto page = pages->currentWidget();
    if (!page) { // pages are built after startup
        updating = false;
        return;
    }
    const auto b = state->brush(state->tool);
    auto set = [&](const char *name, int v) {
        if (auto s = page->findChild<QSpinBox *>(name))
            s->setValue(v);
    };
    if (auto n = page->findChild<QLabel *>("paintToolName"))
        n->setText("<b>" + toolInfo(state->tool).name + "</b>");
    if (auto n = page->findChild<QLabel *>("toolOptionsName"))
        n->setText("<b>" + toolInfo(state->tool).name + "</b>"); // same weight as the paint row
    if (auto c = page->findChild<QComboBox *>("brushShape"))
        c->setCurrentIndex(int(b.shape));
    set("brushSize", b.size);
    set("brushHeight", b.height);
    set("brushOpacity", b.opacity);
    set("brushHardness", b.hardness);
    set("brushSpacing", b.spacing);
    set("strokeSmoothing", state->smoothing);
    set("radialCount", state->radial);
    set("replaceTolerance", state->replaceTolerance);
    if (auto c = page->findChild<QComboBox *>("symmetry"))
        c->setCurrentIndex(int(state->symmetry));
    auto check = [&](const char *name, bool v) {
        if (auto c = page->findChild<QCheckBox *>(name))
            c->setChecked(v);
    };
    auto shown = [&](const char *name, bool v) {
        if (auto w = page->findChild<QWidget *>(name))
            w->setVisible(v);
    };
    auto setReal = [&](const char *name, double v) {
        if (auto s = page->findChild<QDoubleSpinBox *>(name))
            s->setValue(v);
    };
    check("paletteLock", state->paletteLock);
    check("lockAlpha", state->lockAlpha);
    check("wrapPaint", state->wrap);
    check("pixelPerfect", state->pixelPerfect);
    shown("pixelPerfect", state->tool == Tool::Pencil);
    if (auto c = page->findChild<QCheckBox *>("stampSourceColors")) {
        c->setChecked(b.sourceColors); c->setVisible(b.shape == ops::BrushShape::Custom);
    }
    if (auto r = page->findChild<QSpinBox *>("replaceTolerance"))
        r->setVisible(state->tool == Tool::ColorReplace);
    if (auto r = page->findChild<QLabel *>("replaceToleranceLabel"))
        r->setVisible(state->tool == Tool::ColorReplace);
    if (auto r = page->findChild<QSpinBox *>("radialCount"))
        r->setVisible(state->symmetry == ops::Symmetry::Radial);
    if (auto a = page->findChild<QAction *>("selectMode" + QString::number(int(state->selectMode))))
        a->setChecked(true);
    set("selectTolerance", state->tolerance);
    set("fillTolerance", state->tolerance);
    if (auto a = page->findChild<QAction *>(state->contiguous ? "fillModeContiguous" : "fillModeUniversal"))
        a->setChecked(true);
    if (auto t = page->findChild<QSpinBox *>("fillTolerance"))
        t->setToolTip(state->contiguous ? contiguousFillHelp : universalFillHelp);
    if (auto c = page->findChild<QComboBox *>("fillMetric")) {
        c->setCurrentIndex(int(state->metric));
        c->setEnabled(state->contiguous);
    }
    for (const char *n : {"fillSampleComposite", "sampleComposite", "pickComposite"})
        check(n, state->sampleComposite);
    check("includeAlpha", state->includeAlpha);
    if (auto c = page->findChild<QComboBox *>("selectMetric"))
        c->setCurrentIndex(int(state->metric));
    set("fillOpacity", state->fillOpacity);
    set("gradientOpacity", state->fillOpacity);
    set("gradientSteps", state->gradientSteps);
    check("gradientDither", state->gradientDither);
    if (auto c = page->findChild<QComboBox *>("gradientType"))
        c->setCurrentText(state->gradientType);
    set("transformScaleX", state->scaleX);
    set("transformScaleY", state->scaleY);
    if (auto c = page->findChild<QCheckBox *>("transformFlipH"))
        c->setChecked(state->transformFlipH);
    if (auto c = page->findChild<QCheckBox *>("transformFlipV"))
        c->setChecked(state->transformFlipV);
    if (auto c = page->findChild<QCheckBox *>("transformHidden"))
        c->setChecked(state->transformHidden);
    if (auto a = page->findChild<QDoubleSpinBox *>("transformAngle"))
        a->setValue(state->angle);
    if (auto c = page->findChild<QComboBox *>("transformResampling"))
        c->setCurrentText(state->resampling);
    if (auto c = page->findChild<QComboBox *>("shapeKind"))
        c->setCurrentIndex(int(state->shape));
    if (auto c = page->findChild<QCheckBox *>("shapeFilled"))
        c->setChecked(state->filled);
    if (auto c = page->findChild<QCheckBox *>("shapeSmooth"))
        c->setChecked(state->smoothShape);
    set("shapeThickness", state->thickness);
    set("shapeSides", state->sides);
    set("shapeOpacity", state->shapeOpacity);
    setReal("shapeRadius", state->radius);
    setReal("shapeInner", state->innerRatio);
    setReal("shapeRotation", state->rotation);
    check("shapeFlipH", state->flipH);
    check("shapeFlipV", state->flipV);
    check("shapeFromCentre", state->fromCentre);
    const bool rounded = state->shape == ops::ShapeKind::RoundedRectangle;
    const bool pointed = state->shape == ops::ShapeKind::RegularPolygon || state->shape == ops::ShapeKind::Star;
    for (const char *n : {"shapeRadius", "shapeRadiusLabel"})
        shown(n, rounded);
    for (const char *n : {"shapeSides", "shapeSidesLabel"})
        shown(n, pointed);
    for (const char *n : {"shapeInner", "shapeInnerLabel"})
        shown(n, state->shape == ops::ShapeKind::Star);
    set("shapeSnap", state->snap);
    set("selectSnap", state->snap);
    // Transform-only controls are hidden for Move.
    for (const char *n : {"transformScaleX", "transformScaleY", "transformAngle", "transformResampling", "transformReset"})
        if (auto w = page->findChild<QWidget *>(n))
            w->setVisible(state->tool == Tool::Transform);
    updating = false;
}
} // namespace nova
