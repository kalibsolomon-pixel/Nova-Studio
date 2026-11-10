// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"
#include <QDebug>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHash>
#include <QIconEngine>
#include <QLabel>
#include <QPainter>
#include <QToolButton>

namespace nova::ui {
namespace {
Tokens active = tokens(true, "violet");
QString hex(const QColor &c) { return c.name(QColor::HexRgb); }
QString rgba(const QColor &c) {
    return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}
} // namespace

Tokens tokens(bool dark, const QString &accent) {
    Tokens t;
    t.dark = dark;
    const bool blue = accent == "blue";
    if (dark) {
        t.window = QColor("#1d1f28");
        t.surface = QColor("#21242e");
        t.raised = QColor("#272a36");
        t.control = QColor("#2c2f3d");
        t.base = QColor("#161820");
        t.border = QColor("#343744");
        t.borderStrong = QColor("#4a4e60");
        t.text = QColor("#dee1eb");
        t.muted = QColor("#a1a7b6");
        t.disabled = QColor("#7c8291");
        t.placeholder = QColor("#868c9b");
        t.accent = QColor(blue ? "#6ca8f7" : "#9992f5");
        t.selection = QColor(blue ? "#355b8c" : "#4a4578");
        t.selectionText = Qt::white;
        t.warning = QColor("#e3b45c");
        t.danger = QColor("#f07474");
        t.success = QColor("#6cc38f");
        t.checkerDark = QColor("#5a5e68");
        t.checkerLight = QColor("#6a6e78");
        t.pixelGrid = QColor(0, 0, 0, 65);
    } else {
        t.window = QColor("#f0f2f6");
        t.surface = QColor("#f6f7fa");
        t.raised = QColor("#e9ecf2");
        t.control = QColor("#e4e8f0");
        t.base = QColor("#ffffff");
        t.border = QColor("#cbd0dc");
        t.borderStrong = QColor("#a3aab9");
        t.text = QColor("#252a36");
        t.muted = QColor("#565d6d");
        t.disabled = QColor("#80879a");
        t.placeholder = QColor("#676e7f"); // 5.1:1 on white (AA)
        t.accent = QColor(blue ? "#2f5d97" : "#4f47a3");
        t.selection = QColor(blue ? "#b9d4f8" : "#d2cef8");
        t.selectionText = QColor("#202432");
        t.warning = QColor("#94600a");
        t.danger = QColor("#b42318");
        t.success = QColor("#1f7a45");
        t.checkerDark = QColor("#a9adb5");
        t.checkerLight = QColor("#bec1c8");
        t.pixelGrid = QColor(0, 0, 0, 65);
    }
    return t;
}

QPalette palette(const Tokens &t) {
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.base);
    p.setColor(QPalette::AlternateBase, t.dark ? QColor("#1f2230") : QColor("#eef1f6"));
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.control);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, t.danger);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.selectionText);
    p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::PlaceholderText, t.placeholder);
    p.setColor(QPalette::ToolTipBase, t.dark ? QColor("#2b2f3d") : QColor("#ffffff"));
    p.setColor(QPalette::ToolTipText, t.text);
    if (t.dark) {
        p.setColor(QPalette::Light, QColor("#3a3e4e"));
        p.setColor(QPalette::Midlight, QColor("#323544"));
        p.setColor(QPalette::Mid, QColor("#262935"));
        p.setColor(QPalette::Dark, QColor("#15171e"));
        p.setColor(QPalette::Shadow, QColor("#0c0d12"));
    } else {
        p.setColor(QPalette::Light, Qt::white);
        p.setColor(QPalette::Midlight, QColor("#eef0f4"));
        p.setColor(QPalette::Mid, QColor("#a0a7b5"));
        p.setColor(QPalette::Dark, QColor("#8992a4"));
        p.setColor(QPalette::Shadow, QColor("#6a7282"));
    }
    for (auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
        p.setColor(QPalette::Disabled, role, t.disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.dark ? QColor("#33364a") : QColor("#dfe3ea"));
    return p;
}

// One stylesheet, scoped by role properties rather than per-widget exceptions.
// Inputs keep Fusion's native frames (palette-driven); only spacing is tuned.
QString styleSheet(const Tokens &t) {
    QString s = QStringLiteral(
        "QToolBar{spacing:@tbSpacing;padding:@tbPadV @tbPadH;border:0;border-bottom:1px solid @border;background:@window}"
        "QToolBar#toolBox{border-bottom:0;border-right:1px solid @border;padding:@stripPadV @stripPadH}"
        "QToolBar::separator{background:@border;width:1px;height:1px;margin:@sepMinor @sepMajor}"
        "QToolBar#toolBox::separator{margin:@sepTool @sepMinor}"
        "QWidget#toolsDockHost QToolBar#toolBox{border:0;border-bottom:1px solid @border}"
        "QDockWidget::title{padding:@panelPadV @panelPad;background:@surface;border-bottom:1px solid @border;"
        "text-align:left;font-weight:600}"
        "QMainWindow::separator{background:@border;width:3px;height:3px}"
        "QMainWindow::separator:hover{background:@accent}"
        "QTreeView::item{height:24px} QTreeView,QListView,QListWidget,QTreeWidget{border:0}"
        "QLineEdit{padding:2px 6px;min-height:18px}"
        "QPushButton{padding:3px 10px;min-height:18px;border:1px solid @border;border-radius:4px;"
        "background:@control}"
        "QPushButton:hover{border-color:@borderStrong;background:@raised}"
        "QPushButton:pressed{background:@surface}"
        "QPushButton:checked{border-color:@accent;background:palette(highlight);color:palette(highlighted-text)}"
        "QPushButton:focus{border-color:@accent}"
        "QPushButton:disabled{color:@disabled;border-color:@border;background:@surface}"
        "QPushButton[role=\"primary\"]{border-color:@accent;font-weight:600}"
        "QPushButton[role=\"segment\"]{padding:2px 8px;border-radius:3px}"
        "QPushButton[role=\"icon\"]{padding:3px;min-width:18px;border-color:transparent;background:transparent}"
        "QPushButton[role=\"icon\"]:hover{background:@raised;border-color:@border}"
        "QPushButton[role=\"icon\"]:disabled{background:transparent;border-color:transparent}"
        "QToolButton{border:@btnBorder solid transparent;border-radius:4px;padding:@btnPad}"
        "QToolButton:hover{background:@raised;border-color:@border}"
        "QToolButton:pressed{background:@surface}"
        "QToolButton:checked{background:palette(highlight);border-color:@accent}"
        "QToolButton:focus{border-color:@accent}"
        // Split buttons reserve the arrow segment plus a gap, so the arrow never touches the icon.
        "QToolButton[popupMode=\"1\"]{padding-right:@arrowReserve}"
        "QToolButton::menu-button{border:0;width:@arrowWidth}"
        "QToolButton#toolPlacementGrip::menu-indicator{image:none;width:0}"
        "QGroupBox[role=\"section\"]{font-weight:600;border:1px solid @border;border-radius:4px;"
        "margin-top:12px;padding:@panelPad @panelPad @panelPadV @panelPad}"
        "QGroupBox[role=\"section\"]::title{subcontrol-origin:margin;left:@panelPad;padding:0 @btnPad;color:@text}"
        "QPlainTextEdit[role=\"path\"]{border:1px solid @border;border-radius:4px;background:@surface}"
        "QToolButton[role=\"disclosure\"]{border:0;padding:3px 2px;font-weight:600;color:@muted}"
        "QToolButton[role=\"disclosure\"]:hover{color:@text;background:transparent}"
        "QToolButton[role=\"disclosure\"]:checked{background:transparent;color:@text}"
        "QTabWidget::pane{border:0;border-top:1px solid @border}"
        "QTabBar::tab{padding:5px 12px;border:0;border-bottom:2px solid transparent;color:@muted}"
        "QTabBar::tab:selected{color:@text;border-bottom-color:@accent;background:@surface}"
        "QTabBar::tab:hover:!selected{color:@text;background:@raised}"
        "QTabBar#documentTabs::tab{padding:5px 6px 5px 12px;border-right:1px solid @border}"
        "QMenu{padding:4px 0;border:1px solid @borderStrong}"
        "QMenu::item{padding:4px 28px 4px 22px}"
        "QMenu::item:selected{background:palette(highlight);color:palette(highlighted-text)}"
        "QMenu::item:disabled{color:@disabled}"
        "QMenu::separator{height:1px;background:@border;margin:4px 8px}"
        "QMenuBar::item{padding:4px 9px;background:transparent}"
        "QMenuBar::item:selected{background:@raised;border-radius:3px}"
        "QToolTip{border:1px solid @borderStrong;padding:4px 6px}"
        "QStatusBar{border-top:1px solid @border;background:@window}"
        "QStatusBar::item{border:0}"
        "QSplitter::handle{background:@border}"
        "QGroupBox{font-weight:600;border:1px solid @border;border-radius:4px;margin-top:10px;padding-top:6px}"
        "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 3px;color:@muted}"
        "QLabel[role=\"section\"]{font-weight:600;color:@muted;padding-top:4px}"
        "QLabel[role=\"hint\"]{color:@muted}"
        "QLabel[role=\"value\"]{color:@text}"
        "QLabel[role=\"warning\"]{color:@warning}"
        "QLabel[role=\"error\"]{color:@danger}"
        "QFrame[role=\"rule\"]{background:@border;max-height:1px;min-height:1px;border:0}"
        "QFrame[role=\"vrule\"]{background:@border;border:0}"
        "QLineEdit#projectPath{border:1px solid @border;border-radius:4px;background:@surface}"
        "QLineEdit#projectPath:focus{border-color:@accent;background:@base}");
    const std::pair<const char *, QColor> values[] = {
        {"@borderStrong", t.borderStrong}, {"@border", t.border}, {"@window", t.window},
        {"@surface", t.surface},           {"@raised", t.raised}, {"@control", t.control},
        {"@accent", t.accent},             {"@muted", t.muted},   {"@text", t.text},
        {"@disabled", t.disabled},         {"@warning", t.warning}, {"@danger", t.danger}};
    for (const auto &[key, color] : values)
        s.replace(key, color.alpha() == 255 ? hex(color) : rgba(color));
    const auto px = [](int v) { return QString::number(v) + "px"; };
    const std::pair<const char *, QString> metrics[] = {
        {"@tbSpacing", px(ToolbarSpacing)},     {"@tbPadV", px(ToolbarPaddingV)},
        {"@tbPadH", px(ToolbarPaddingH)},       {"@sepTool", px(ToolStripSeparatorMargin)},
        {"@stripPadV", px(ToolStripPaddingV)},  {"@stripPadH", px(ToolStripPaddingH)},
        {"@sepMajor", px(SeparatorMarginMajor)}, {"@sepMinor", px(SeparatorMarginMinor)},
        {"@btnBorder", px(ButtonBorder)},       {"@btnPad", px(ButtonPadding)},
        {"@arrowReserve", px(MenuArrowWidth + MenuArrowGap + ButtonPadding)},
        {"@arrowWidth", px(MenuArrowWidth)},     {"@panelPadV", px(PanelPaddingV)},
        {"@panelPad", px(PanelPadding)}};
    for (const auto &[key, value] : metrics)
        s.replace(key, value);
    return s;
}
const Tokens &current() { return active; }
void setCurrent(const Tokens &t) { active = t; }

namespace {
QString materialFontFamily() {
    static const QString family = [] {
        const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/MaterialSymbolsOutlined.ttf"));
        if (id < 0) {
            qWarning() << "Unable to load the embedded Material Symbols Outlined font";
            return QString();
        }
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (families.isEmpty()) {
            qWarning() << "The embedded Material Symbols Outlined font has no family";
            return QString();
        }
        return families.first();
    }();
    return family;
}

QChar materialGlyph(const QString &name) {
    static const QHash<QString, QChar> glyphs = {
        {"save", QChar(0xe161)},
        {"undo", QChar(0xe166)},
        {"redo", QChar(0xe15a)},
        {"open", QChar(0xe2c8)},
        {"folder", QChar(0xe2c7)},
        {"search", QChar(0xef7a)},
        {"command", QChar(0xef7a)},
        {"close", QChar(0xe5cd)},
        {"add", QChar(0xe145)},
        {"duplicate", QChar(0xe14d)},
        {"delete", QChar(0xe92e)},
        {"rename", QChar(0xf097)},
        {"up", QChar(0xe5d8)},
        {"down", QChar(0xe5db)},
        {"parent", QChar(0xe5d9)},
        {"tree", QChar(0xe97a)},
        {"grid", QChar(0xe9b0)},
        {"sort", QChar(0xe164)},
        {"zoom-in", QChar(0xe8ff)},
        {"zoom-out", QChar(0xe900)},
        {"fit", QChar(0xea10)},
        {"actual", QChar(0xe693)},
        {"center", QChar(0xe3b4)},
        {"settings", QChar(0xe8b8)},
        {"activity", QChar(0xe7f5)},
        {"workspace", QChar(0xe871)},
        {"keyboard", QChar(0xe312)},
        {"transform", QChar(0xe428)},
        {"eye", QChar(0xe8f4)},
        {"pencil", QChar(0xf604)},
        {"brush", QChar(0xe3ae)},
        {"eraser", QChar(0xe6d0)},
        {"replace", QChar(0xf20a)},
        {"rect-select", QChar(0xf74d)},
        {"ellipse-select", QChar(0xef4a)},
        {"lasso", QChar(0xeb03)},
        {"polygon-lasso", QChar(0xebbb)},
        {"magic-wand", QChar(0xf31e)},
        {"color-range", QChar(0xe40a)},
        {"move", QChar(0xe89f)},
        {"line", QChar(0xf41e)},
        {"shape", QChar(0xe602)},
        {"fill", QChar(0xe23a)},
        {"gradient", QChar(0xe3e9)},
        {"eyedropper", QChar(0xe3b8)},
        {"drag", QChar(0xe945)} // drag_indicator: the tool strip's move grip
    };
    return glyphs.value(name);
}

// Material Symbols Outlined rendered at the exact logical size and device
// pixel ratio requested. Font outlines and palette ink replace bespoke paths;
// the existing on-demand rendered-size caching behavior is retained.
class MaterialIconEngine : public QIconEngine {
    QChar glyph;
    QColor ink, off;
    QHash<QString, QPixmap> pixmaps;

  public:
    MaterialIconEngine(QChar g, QColor i, QColor o) : glyph(g), ink(i), off(o) {}
    QIconEngine *clone() const override { return new MaterialIconEngine(glyph, ink, off); }
    bool isNull() override { return false; }
    QString key() const override { return QStringLiteral("NovaMaterialSymbol"); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        painter->save();
        painter->setRenderHint(QPainter::TextAntialiasing);
        painter->setPen(mode == QIcon::Disabled ? off : ink);
        QFont font(materialFontFamily());
        font.setWeight(QFont::Normal);
        font.setPixelSize(qMax(1, qMin(rect.width(), rect.height())));
        painter->setFont(font);
        const QString text(glyph);
        const QFontMetricsF metrics(font);
        const QRectF bounds = metrics.tightBoundingRect(text);
        painter->drawText(QRectF(rect).center() - bounds.center(), text);
        painter->restore();
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        const QSize device = (QSizeF(size) * scale).toSize();
        // Device dimensions alone are insufficient: 18 px at DPR 2 and 24 px at
        // DPR 1.5 share pixels but must return pixmaps with different DPR metadata.
        const QString key = QStringLiteral("%1x%2@%3/%4")
                                .arg(device.width())
                                .arg(device.height())
                                .arg(scale, 0, 'g', 17)
                                .arg(mode == QIcon::Disabled ? 1 : 0);
        if (auto it = pixmaps.constFind(key); it != pixmaps.constEnd())
            return *it;
        QPixmap pm(device);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            paint(&p, QRect(QPoint(), device), mode, state);
        }
        pm.setDevicePixelRatio(scale);
        if (pixmaps.size() > 32)
            pixmaps.clear();
        pixmaps.insert(key, pm);
        return pm;
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
};
} // namespace

QIcon lineIcon(const QString &name, const QColor &ink, const QColor &disabledInk) {
    const QChar glyph = materialGlyph(name);
    if (glyph.isNull()) {
        qWarning() << "Unknown Material Symbols icon name:" << name;
        return {};
    }
    if (materialFontFamily().isEmpty())
        return {};
    // Icons are shared (QIcon is implicitly shared): one engine per name and ink,
    // so dozens of buttons and repeated theme applications share rendered sizes.
    static QHash<QString, QIcon> cache;
    const QColor off = disabledInk.isValid() ? disabledInk : QColor(ink.red(), ink.green(), ink.blue(), 95);
    const auto key = name + ink.name(QColor::HexArgb) + off.name(QColor::HexArgb);
    if (auto it = cache.constFind(key); it != cache.constEnd())
        return *it;
    QIcon icon(new MaterialIconEngine(glyph, ink, off));
    if (cache.size() > 256) // theme/accent churn cannot grow it without bound
        cache.clear();
    cache.insert(key, icon);
    return icon;
}
QIcon lineIcon(const QString &name) { return lineIcon(name, current().text, current().disabled); }

QLabel *sectionTitle(const QString &text, QWidget *parent) {
    auto label = new QLabel(text.toUpper(), parent);
    label->setProperty("role", "section");
    label->setAccessibleName(text);
    return label;
}
QLabel *hint(const QString &text, QWidget *parent) {
    auto label = new QLabel(text, parent);
    label->setProperty("role", "hint");
    label->setWordWrap(true);
    return label;
}
namespace {
class EventRelay : public QObject {
    QList<QEvent::Type> types;
    std::function<void()> callback;
    bool pending = false;

  public:
    EventRelay(QObject *target, std::initializer_list<QEvent::Type> t, std::function<void()> f)
        : QObject(target), types(t), callback(std::move(f)) {
        target->installEventFilter(this);
    }
    bool eventFilter(QObject *, QEvent *event) override {
        if (!pending && types.contains(event->type())) {
            pending = true; // coalesce bursts (fast wheel scrolling) into one update
            QMetaObject::invokeMethod(this, [this] { pending = false; callback(); }, Qt::QueuedConnection);
        }
        return false;
    }
};
} // namespace
void afterEvents(QObject *target, std::initializer_list<QEvent::Type> types, std::function<void()> callback) {
    new EventRelay(target, types, std::move(callback));
}
QToolButton *iconButton(const QString &icon, const QString &tooltip, const QString &objectName, QWidget *parent) {
    auto button = new QToolButton(parent);
    button->setProperty("novaIcon", icon);
    button->setIcon(lineIcon(icon));
    button->setIconSize({IconSize, IconSize});
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip.section('(', 0, 0).trimmed());
    button->setAutoRaise(true);
    if (!objectName.isEmpty())
        button->setObjectName(objectName);
    return button;
}
} // namespace nova::ui
