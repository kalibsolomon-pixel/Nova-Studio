// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDebug>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHash>
#include <QIconEngine>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QToolButton>

namespace nova::ui {
namespace {
Tokens active = tokens(true, "violet");
QString hex(const QColor &c) { return c.name(QColor::HexRgb); }
QString rgba(const QColor &c) {
    return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}
// Linear blend from a to b; used for derived states so they track the tokens.
QColor mix(const QColor &a, const QColor &b, double amount) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount, a.greenF() + (b.greenF() - a.greenF()) * amount,
                            a.blueF() + (b.blueF() - a.blueF()) * amount);
}
} // namespace

Tokens tokens(bool dark, const QString &accent) {
    Tokens t;
    t.dark = dark;
    const bool blue = accent == "blue";
    if (dark) {
        // Neutral graphite with a faint cool cast: low chroma keeps long sessions calm
        // and leaves colour judgement to the artwork.
        t.backdrop = QColor("#0e0f12");
        t.base = QColor("#131418");
        t.canvas = t.base;
        t.window = QColor("#191a1f");
        t.surface = QColor("#1e1f25");
        t.raised = QColor("#272830");
        t.control = QColor("#24252c");
        t.border = QColor("#2a2b33");
        t.borderStrong = QColor("#3a3c46");
        t.text = QColor("#e3e4e8");
        t.muted = QColor("#9b9ea9");
        t.disabled = QColor("#62656f");
        t.placeholder = QColor("#767985");
        t.icon = QColor("#c4c6ce");
        t.accent = QColor(blue ? "#4a8af4" : "#7b6cf0");
        t.accentHover = QColor(blue ? "#619bf7" : "#8d80f4");
        t.accentMuted = QColor(blue ? "#1b2a42" : "#29263f");
        t.onAccent = Qt::white;
        t.selection = QColor(blue ? "#21406e" : "#383461");
        t.selectionText = Qt::white;
        t.warning = QColor("#e8b45f");
        t.danger = QColor("#f06a6a");
        t.success = QColor("#5cc489");
        t.info = QColor("#6aa8f5");
        t.warningSurface = QColor("#2b2418");
        t.dangerSurface = QColor("#2e1c1e");
        t.infoSurface = QColor("#18233a");
        t.checkerDark = QColor("#5a5e68");
        t.checkerLight = QColor("#6a6e78");
        t.pixelGrid = QColor(0, 0, 0, 65);
    } else {
        t.backdrop = QColor("#dcdfe5");
        t.base = QColor("#ffffff");
        t.canvas = QColor("#e4e6eb");
        t.window = QColor("#f5f6f8");
        t.surface = QColor("#eef0f3");
        t.raised = QColor("#e6e8ec");
        t.control = QColor("#ffffff");
        t.border = QColor("#dfe1e6");
        t.borderStrong = QColor("#c4c8d0");
        t.text = QColor("#1b1d22");
        t.muted = QColor("#5b5f6b");
        t.disabled = QColor("#9da1ab");
        t.placeholder = QColor("#6e7280"); // 4.8:1 on white (AA)
        t.icon = QColor("#3d414b");
        t.accent = QColor(blue ? "#2563d9" : "#6550e0");
        t.accentHover = QColor(blue ? "#1f56c2" : "#5641d2");
        t.accentMuted = QColor(blue ? "#e3ecfb" : "#ece9fd");
        t.onAccent = Qt::white;
        t.selection = QColor(blue ? "#d3e2fb" : "#e1dcfc");
        t.selectionText = QColor("#1b1d22");
        t.warning = QColor("#94600a");
        t.danger = QColor("#b42318");
        t.success = QColor("#1e7b48");
        t.info = QColor("#2563d9");
        t.warningSurface = QColor("#fdf4e3");
        t.dangerSurface = QColor("#fdecea");
        t.infoSurface = QColor("#e8f0fd");
        t.checkerDark = QColor("#9ca0a7");
        t.checkerLight = QColor("#aaaeb5");
        t.pixelGrid = QColor(0, 0, 0, 65);
    }
    return t;
}

QPalette palette(const Tokens &t) {
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.base);
    p.setColor(QPalette::AlternateBase, mix(t.base, t.window, 0.5));
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.control);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, t.danger);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.selectionText);
    p.setColor(QPalette::Accent, t.accent);
    p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::LinkVisited, t.accent);
    p.setColor(QPalette::PlaceholderText, t.placeholder);
    p.setColor(QPalette::ToolTipBase, t.raised);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::Light, t.dark ? t.borderStrong : QColor(Qt::white));
    p.setColor(QPalette::Midlight, t.dark ? t.raised : t.surface);
    p.setColor(QPalette::Mid, t.dark ? t.border : t.borderStrong);
    p.setColor(QPalette::Dark, t.dark ? t.base : mix(t.borderStrong, t.text, 0.3));
    p.setColor(QPalette::Shadow, t.dark ? t.backdrop : mix(t.borderStrong, t.text, 0.5));
    for (auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
        p.setColor(QPalette::Disabled, role, t.disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.raised);
    p.setColor(QPalette::Disabled, QPalette::Base, t.window);
    p.setColor(QPalette::Inactive, QPalette::Highlight, t.accentMuted);
    p.setColor(QPalette::Inactive, QPalette::HighlightedText, t.text);
    return p;
}

// One stylesheet, scoped by role properties rather than per-widget exceptions.
// Rules are grouped by component; @tokens and @metrics are substituted below.
QString styleSheet(const Tokens &t) {
    QString s = QStringLiteral(
        // Shell: panels float on the backdrop, separated by gutters.
        "QMainWindow{background:@backdrop}"
        "QMainWindow::separator{background:@backdrop;width:@gutter;height:@gutter}"
        "QMainWindow::separator:hover{background:@accent}"
        "QMenuBar{background:@window;border:0;padding:2px @sepMinor}"
        "QMenuBar::item{padding:4px 10px;background:transparent;border-radius:@rS;color:@text}"
        "QMenuBar::item:selected,QMenuBar::item:pressed{background:@raised}"
        "QToolBar{spacing:@tbSpacing;padding:@tbPadV @tbPadH;border:0;background:@window}"
        "QToolBar#commands{border-bottom:1px solid @border}"
        "QToolBar#toolOptions{border-bottom:1px solid @border;background:@window}"
        "QToolBar#toolOptions QLabel{color:@muted;padding-left:2px}"
        "QToolBar#toolOptions QLabel#paintToolName{color:@text;font-weight:600;padding-left:2px;padding-right:6px}"
        "QToolBar#toolBox{border-bottom:0;border-right:1px solid @border;padding:@stripPadV @stripPadH}"
        "QToolBar#toolBox QToolButton{padding:@toolPad}"
        "QToolBar::separator{background:@border;width:1px;height:1px;margin:@sepMinor @sepMajor}"
        "QToolBar#toolBox::separator{margin:@sepTool @sepMinor}"
        "QWidget#toolsDockHost QToolBar#toolBox{border:0;border-bottom:1px solid @border}"
        "QStatusBar{background:@window;border-top:1px solid @border;color:@muted}"
        "QStatusBar::item{border:0}"
        "QStatusBar QLabel{color:@muted}"
        // Panels.
        "QDockWidget{font-weight:600;color:@text}"
        "QDockWidget::title{padding:@panelPadV @panelPad;background:@window;border-bottom:1px solid @border;"
        "text-align:left}"
        "QDockWidget::close-button,QDockWidget::float-button{border:0;border-radius:@rS;background:transparent;"
        "padding:2px}"
        "QDockWidget::close-button:hover,QDockWidget::float-button:hover{background:@raised}"
        "QSplitter::handle{background:@border}"
        "QFrame[role=\"rule\"]{background:@border;max-height:1px;min-height:1px;border:0}"
        "QFrame[role=\"vrule\"]{background:@border;border:0}"
        "QFrame[role=\"card\"]{background:@surface;border:1px solid @border;border-radius:@rL}"
        "QFrame[role=\"notice\"]{background:@warningSurface;border:1px solid @warning;border-radius:@rM}"
        "QAbstractScrollArea{border:0}"
        // Tabs: underlined panel tabs; document tabs merge into the canvas.
        "QTabWidget::pane{border:0;border-top:1px solid @border}"
        "QTabBar{qproperty-drawBase:0}"
        "QTabBar::tab{padding:6px 12px;border:0;border-bottom:2px solid transparent;color:@muted;"
        "background:transparent}"
        "QTabBar::tab:selected{color:@text;border-bottom-color:@accent}"
        "QTabBar::tab:hover:!selected{color:@text}"
        "QMainWindow>QTabBar{background:@window}"
        "QMainWindow>QTabBar::tab{padding:6px 12px;margin:0;background:@window}"
        "QTabBar#documentTabs{background:@window}"
        "QTabBar#documentTabs::tab{padding:7px 6px 7px 14px;margin:0;border:0;border-right:1px solid @border;"
        "border-top:2px solid transparent;background:@window;color:@muted}"
        "QTabBar#documentTabs::tab:selected{background:@base;color:@text;border-top-color:@accent}"
        "QTabBar#documentTabs::tab:hover:!selected{background:@surface;color:@text}"
        // Buttons.
        "QPushButton{padding:4px 12px;min-height:16px;border:1px solid @borderStrong;border-radius:@rS;"
        "background:@control;color:@text}"
        "QPushButton:hover{border-color:@borderHover;background:@raised}"
        "QPushButton:pressed{background:@surface}"
        "QPushButton:checked{border-color:@accent;background:@accentMuted;color:@text}"
        "QPushButton:focus{border-color:@accent}"
        "QPushButton:disabled{color:@disabled;border-color:@border;background:@window}"
        "QPushButton[role=\"primary\"]{border-color:@accent;background:@accent;color:@onAccent;font-weight:600}"
        "QPushButton[role=\"primary\"]:hover{border-color:@accentHover;background:@accentHover}"
        "QPushButton[role=\"primary\"]:focus{border-color:@focusOnAccent}"
        "QPushButton[role=\"primary\"]:disabled{border-color:@border;background:@surface;color:@disabled}"
        "QPushButton[role=\"segment\"]{padding:3px 10px;border-radius:@rS}"
        "QPushButton[role=\"icon\"]{padding:4px;min-width:18px;border-color:transparent;background:transparent}"
        "QPushButton[role=\"icon\"]:hover{background:@raised;border-color:transparent}"
        "QPushButton[role=\"icon\"]:disabled{background:transparent;border-color:transparent}"
        "QPushButton::menu-indicator{subcontrol-origin:padding;subcontrol-position:center right;width:10px;"
        "right:-4px}"
        "QToolButton{border:@btnBorder solid transparent;border-radius:@rS;padding:@btnPad;background:transparent;"
        "color:@text}"
        "QToolButton:hover{background:@raised}"
        "QToolButton:pressed{background:@surface}"
        "QToolButton:checked{background:@accentMuted;border-color:@accent}"
        "QToolButton:disabled{color:@disabled}"
        // Split buttons reserve the arrow segment plus a gap, so the arrow never touches the icon.
        "QToolButton[popupMode=\"1\"]{padding-right:@arrowReserve}"
        "QToolButton::menu-button{border:0;width:@arrowWidth}"
        "QToolButton#toolPlacementGrip::menu-indicator{image:none;width:0}"
        "QToolButton[popupMode=\"2\"]{padding-right:18px}"
        "QToolButton[popupMode=\"2\"]::menu-indicator{subcontrol-origin:padding;subcontrol-position:center right;"
        "right:4px;width:10px;height:10px}"
        "QToolBar#toolBox QToolButton[popupMode=\"2\"]{padding:@toolPad}"
        "QToolBar#toolBox QToolButton[popupMode=\"2\"]::menu-indicator{subcontrol-position:bottom right;right:0;"
        "width:8px;height:8px}"
        "QToolButton[role=\"disclosure\"]{border:0;padding:4px 2px;font-weight:600;color:@muted}"
        "QToolButton[role=\"disclosure\"]:hover{color:@text;background:transparent}"
        "QToolButton[role=\"disclosure\"]:checked{background:transparent;color:@text}"
        // Line edits, spin boxes and combo boxes are drawn by NovaStyle (one field shape
        // with token borders); only their popup list is styled here.
        "QComboBox QAbstractItemView{background:@raised;border:1px solid @borderStrong;padding:4px;outline:0;"
        "selection-background-color:@selection;selection-color:@selectionText}"
        "QComboBox QAbstractItemView::item{min-height:24px;padding:0 8px;border-radius:@rS}"
        "QPlainTextEdit,QTextEdit{background:@base;border:1px solid @border;border-radius:@rS;"
        "selection-background-color:@selection;selection-color:@selectionText}"
        "QPlainTextEdit[role=\"path\"]{background:@surface}"
        "QCheckBox,QRadioButton{spacing:8px}"
        "QSlider::groove:horizontal{height:4px;border-radius:2px;background:@raised}"
        "QSlider::sub-page:horizontal{height:4px;border-radius:2px;background:@accent}"
        "QSlider::add-page:horizontal{height:4px;border-radius:2px;background:@raised}"
        "QSlider::handle:horizontal{width:14px;height:14px;margin:-5px 0;border-radius:7px;"
        "background:@sliderHandle;border:1px solid @borderStrong}"
        "QSlider::handle:horizontal:hover{border-color:@accent}"
        "QSlider::sub-page:horizontal:disabled{background:@borderStrong}"
        "QProgressBar{min-height:6px;max-height:6px;border:0;border-radius:3px;background:@raised;"
        "text-align:center;color:transparent}"
        "QProgressBar::chunk{border-radius:3px;background:@accent}"
        // Lists, trees and tables.
        "QTreeView,QListView,QTableView,QListWidget,QTreeWidget,QTableWidget{border:0;background:@base;"
        "alternate-background-color:@alternate;outline:0;selection-background-color:@selection;"
        "selection-color:@selectionText}"
        "QTreeView::item{height:@rowHeight}"
        "QTreeView::item:hover,QListView::item:hover,QTableView::item:hover{background:@hoverRow}"
        "QTreeView::item:selected,QListView::item:selected,QTableView::item:selected{background:@selection;"
        "color:@selectionText}"
        "QTreeView::item:selected:!active,QListView::item:selected:!active,"
        "QTableView::item:selected:!active{background:@accentMuted;color:@text}"
        "QHeaderView{background:@window;border:0}"
        "QHeaderView::section{background:@window;color:@muted;padding:5px 8px;border:0;"
        "border-bottom:1px solid @border;border-right:1px solid @border;font-weight:600}"
        "QTableCornerButton::section{background:@window;border:0}"
        // Scroll bars: slim, arrowless, rounded thumbs on a transparent track.
        "QScrollBar:vertical{width:@scroll;background:transparent;margin:0;border:0}"
        "QScrollBar:horizontal{height:@scroll;background:transparent;margin:0;border:0}"
        "QScrollBar::handle:vertical{min-height:28px;margin:2px;border-radius:3px;background:@scrollThumb}"
        "QScrollBar::handle:horizontal{min-width:28px;margin:2px;border-radius:3px;background:@scrollThumb}"
        "QScrollBar::handle:hover{background:@scrollHover}"
        "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;border:0;background:transparent}"
        "QScrollBar::add-page,QScrollBar::sub-page{background:transparent}"
        // Popups.
        "QMenu{padding:6px;border:1px solid @borderStrong;background:@raised}"
        "QMenu::item{padding:5px 28px 5px 24px;border-radius:@rS;background:transparent;color:@text}"
        "QMenu::item:selected{background:@selection;color:@selectionText}"
        "QMenu::item:disabled{color:@disabled}"
        "QMenu::separator{height:1px;background:@border;margin:5px 4px}"
        "QToolTip{border:1px solid @borderStrong;padding:5px 8px;background:@raised;color:@text}"
        // Grouped sections: titled cards.
        "QGroupBox{font-weight:600;border:1px solid @border;border-radius:@rM;margin-top:20px;"
        "padding:10px 8px 8px 8px;background:@surface}"
        "QGroupBox::title{subcontrol-origin:margin;subcontrol-position:top left;left:2px;padding:0 0 4px 0;"
        "color:@text}"
        "QGroupBox[role=\"section\"]{font-weight:600;border:1px solid @border;border-radius:@rM;"
        "margin-top:20px;padding:@panelPad @panelPad @panelPadV @panelPad;background:@surface}"
        "QGroupBox[role=\"section\"]::title{subcontrol-origin:margin;left:2px;padding:0 0 4px 0;color:@text}"
        // Text roles.
        "QLabel[role=\"section\"]{font-size:@captionPt;font-weight:600;color:@muted;padding-top:6px}"
        "QLabel[role=\"title\"]{font-size:@titlePt;font-weight:600;color:@text}"
        "QLabel[role=\"display\"]{font-size:@displayPt;font-weight:600;color:@text}"
        "QLabel[role=\"hint\"]{color:@muted}"
        "QLabel[role=\"empty\"]{color:@muted;padding:16px}"
        "QLabel[role=\"value\"]{color:@text}"
        "QLabel[role=\"warning\"]{color:@warning}"
        "QLabel[role=\"error\"]{color:@danger}"
        "QLabel[role=\"notice\"]{background:@warningSurface;color:@text;border:1px solid @noticeBorder;"
        "border-radius:@rM;padding:8px 12px}");
    const QColor sliderHandle = t.dark ? t.text : QColor(Qt::white);
    const std::pair<const char *, QColor> values[] = {
        {"@borderStrong", t.borderStrong},
        {"@borderHover", mix(t.borderStrong, t.muted, 0.35)},
        {"@border", t.border},
        {"@backdrop", t.backdrop},
        {"@window", t.window},
        {"@surface", t.surface},
        {"@raised", t.raised},
        {"@control", t.control},
        {"@base", t.base},
        {"@alternate", mix(t.base, t.window, 0.5)},
        {"@hoverRow", mix(t.base, t.raised, 0.7)},
        {"@accentHover", t.accentHover},
        {"@accentMuted", t.accentMuted},
        {"@accent", t.accent},
        {"@onAccent", t.onAccent},
        {"@focusOnAccent", mix(t.accent, t.onAccent, 0.55)},
        {"@selectionText", t.selectionText},
        {"@selection", t.selection},
        {"@muted", t.muted},
        {"@text", t.text},
        {"@disabled", t.disabled},
        {"@warningSurface", t.warningSurface},
        {"@noticeBorder", mix(t.warningSurface, t.warning, 0.45)},
        {"@warning", t.warning},
        {"@danger", t.danger},
        {"@sliderHandle", sliderHandle},
        {"@scrollThumb", t.dark ? t.borderStrong : mix(t.borderStrong, t.window, 0.15)},
        {"@scrollHover", mix(t.borderStrong, t.muted, 0.5)}};
    for (const auto &[key, color] : values)
        s.replace(key, color.alpha() == 255 ? hex(color) : rgba(color));
    const auto px = [](int v) { return QString::number(v) + "px"; };
    const auto pt = [](double v) { return QString::number(v) + "pt"; };
    const std::pair<const char *, QString> metrics[] = {
        {"@tbSpacing", px(ToolbarSpacing)},      {"@tbPadV", px(ToolbarPaddingV)},
        {"@tbPadH", px(ToolbarPaddingH)},        {"@sepTool", px(ToolStripSeparatorMargin)},
        {"@stripPadV", px(ToolStripPaddingV)},   {"@stripPadH", px(ToolStripPaddingH)},
        {"@sepMajor", px(SeparatorMarginMajor)}, {"@sepMinor", px(SeparatorMarginMinor)},
        {"@btnBorder", px(ButtonBorder)},        {"@btnPad", px(ButtonPadding)},
        {"@toolPad", px(ToolButtonPadding)},     {"@arrowReserve", px(MenuArrowWidth + MenuArrowGap + ButtonPadding)},
        {"@arrowWidth", px(MenuArrowWidth)},     {"@panelPadV", px(PanelPaddingV)},
        {"@panelPad", px(PanelPadding)},         {"@gutter", px(GutterWidth)},
        {"@scroll", px(ScrollBarWidth)},         {"@rowHeight", px(RowHeight)},
        {"@rS", px(RadiusS)},                    {"@rM", px(RadiusM)},
        {"@rL", px(RadiusL)},                    {"@captionPt", pt(TypeCaption)},
        {"@titlePt", pt(TypeTitle)},             {"@displayPt", pt(TypeDisplay)}};
    for (const auto &[key, value] : metrics)
        s.replace(key, value);
    return s;
}
const Tokens &current() { return active; }
void setCurrent(const Tokens &t) { active = t; }

QFont font(Type type) {
    QFont f = QApplication::font();
    switch (type) {
    case Type::Caption:
        f.setPointSizeF(TypeCaption);
        f.setWeight(QFont::DemiBold);
        f.setLetterSpacing(QFont::PercentageSpacing, 106);
        break;
    case Type::Body:
        f.setPointSizeF(TypeBody);
        break;
    case Type::Strong:
        f.setPointSizeF(TypeBody);
        f.setWeight(QFont::DemiBold);
        break;
    case Type::Title:
        f.setPointSizeF(TypeTitle);
        f.setWeight(QFont::DemiBold);
        break;
    case Type::Heading:
        f.setPointSizeF(TypeHeading);
        f.setWeight(QFont::DemiBold);
        break;
    case Type::Display:
        f.setPointSizeF(TypeDisplay);
        f.setWeight(QFont::DemiBold);
        break;
    }
    return f;
}

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
        {"file", QChar(0xe66d)},   // draft
        {"undock", QChar(0xe89e)}, // open_in_new: dock float buttons
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
        {"effects", QChar(0xe429)},  // tune
        {"select-replace", QChar(0xe3c6)},   // crop_square
        {"select-add", QChar(0xe145)},       // add
        {"select-subtract", QChar(0xe15b)},  // remove
        {"select-intersect", QChar(0xeaf4)}, // join_inner
        {"first", QChar(0xe045)},    // skip_previous
        {"previous", QChar(0xe5cb)}, // navigate_before
        {"play", QChar(0xe037)},
        {"pause", QChar(0xe034)},
        {"stop", QChar(0xe047)},
        {"next", QChar(0xe5cc)},     // navigate_next
        {"last", QChar(0xe044)},     // skip_next
        {"drag", QChar(0xe945)} // drag_indicator: the tool strip's move grip
    };
    return glyphs.value(name);
}

// Ink for icons that follow the active theme instead of a fixed colour.
enum class Ink { Fixed, Icon, Muted };

// Material Symbols Outlined rendered at the exact logical size and device
// pixel ratio requested. Font outlines and palette ink replace bespoke paths;
// the existing on-demand rendered-size caching behavior is retained.
class MaterialIconEngine : public QIconEngine {
    QChar glyph;
    QColor ink, off;
    Ink source;
    QHash<QString, QPixmap> pixmaps;

    QColor normalInk() const {
        return source == Ink::Fixed ? ink : source == Ink::Muted ? current().muted : current().icon;
    }
    QColor disabledInk() const { return source == Ink::Fixed ? off : current().disabled; }

  public:
    MaterialIconEngine(QChar g, QColor i, QColor o, Ink s = Ink::Fixed) : glyph(g), ink(i), off(o), source(s) {}
    QIconEngine *clone() const override { return new MaterialIconEngine(glyph, ink, off, source); }
    bool isNull() override { return false; }
    QString key() const override { return QStringLiteral("NovaMaterialSymbol"); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        painter->save();
        painter->setRenderHint(QPainter::TextAntialiasing);
        painter->setPen(mode == QIcon::Disabled ? disabledInk() : normalInk());
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
        // Theme-following icons also key on the ink they resolve to now.
        const QColor resolved = mode == QIcon::Disabled ? disabledInk() : normalInk();
        const QString key = QStringLiteral("%1x%2@%3/%4/%5")
                                .arg(device.width())
                                .arg(device.height())
                                .arg(scale, 0, 'g', 17)
                                .arg(mode == QIcon::Disabled ? 1 : 0)
                                .arg(resolved.rgba());
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

QIcon themedIcon(const QString &name, Ink ink) {
    const QChar glyph = materialGlyph(name);
    if (glyph.isNull() || materialFontFamily().isEmpty())
        return {};
    return QIcon(new MaterialIconEngine(glyph, {}, {}, ink));
}

// A crisp chevron centred in rect, pointing along direction (degrees: 0 right, 90 down).
void chevron(QPainter *p, const QRectF &rect, int direction, const QColor &ink) {
    const qreal extent = qMin(rect.width(), rect.height());
    const qreal size = extent < 12 ? std::clamp(extent * 0.45, 3.5, 5.0) : std::clamp(extent * 0.5, 5.0, 9.0);
    const qreal half = size / 2;
    QPainterPath path;
    path.moveTo(-half / 2, -half);
    path.lineTo(half / 2, 0);
    path.lineTo(-half / 2, half);
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->translate(rect.center());
    p->rotate(direction);
    QPen pen(ink, extent < 12 ? 1.25 : 1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    p->drawPath(path);
    p->restore();
}

// Fusion, with Nova's indicators, icons and metrics. The stylesheet covers frames
// and fills; this covers the pieces a stylesheet can only replace with image files.
class NovaStyle : public QProxyStyle {
  public:
    NovaStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {
        setObjectName(QStringLiteral("nova"));
    }
    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override {
        switch (metric) {
        case PM_DockWidgetSeparatorExtent:
            return GutterWidth;
        case PM_ScrollBarExtent:
            return ScrollBarWidth;
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
        case PM_ExclusiveIndicatorWidth:
        case PM_ExclusiveIndicatorHeight:
            return 16;
        case PM_SmallIconSize:
            return 16;
        case PM_MenuPanelWidth:
            return 0;
        default:
            return QProxyStyle::pixelMetric(metric, option, widget);
        }
    }
    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                  QStyleHintReturn *data) const override {
        switch (hint) {
        case SH_DockWidget_ButtonsHaveFrame:
            return 0;
        case SH_ComboBox_Popup: // a list below the field, not a menu over it
            return 0;
        case SH_ItemView_ShowDecorationSelected:
            return 1;
        case SH_ToolTipLabel_Opacity:
            return 255;
        default:
            return QProxyStyle::styleHint(hint, option, widget, data);
        }
    }
    QIcon standardIcon(StandardPixmap pixmap, const QStyleOption *option, const QWidget *widget) const override {
        switch (pixmap) {
        case SP_TitleBarCloseButton:
        case SP_DockWidgetCloseButton:
            return themedIcon(QStringLiteral("close"), Ink::Icon);
        case SP_TitleBarNormalButton:
            return themedIcon(QStringLiteral("undock"), Ink::Icon);
        case SP_DirIcon:
        case SP_DirClosedIcon:
        case SP_DirOpenIcon:
            return themedIcon(QStringLiteral("folder"), Ink::Muted);
        case SP_FileIcon:
            return themedIcon(QStringLiteral("file"), Ink::Muted);
        default:
            return QProxyStyle::standardIcon(pixmap, option, widget);
        }
    }
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override {
        const Tokens &t = current();
        const bool enabled = option->state & State_Enabled;
        switch (element) {
        case PE_IndicatorArrowDown:
        case PE_IndicatorSpinDown:
        case PE_IndicatorSpinMinus:
            chevron(painter, option->rect, 90, enabled ? t.icon : t.disabled);
            return;
        case PE_IndicatorArrowUp:
        case PE_IndicatorSpinUp:
        case PE_IndicatorSpinPlus:
            chevron(painter, option->rect, -90, enabled ? t.icon : t.disabled);
            return;
        case PE_IndicatorArrowRight:
            chevron(painter, option->rect, 0, enabled ? t.icon : t.disabled);
            return;
        case PE_IndicatorArrowLeft:
            chevron(painter, option->rect, 180, enabled ? t.icon : t.disabled);
            return;
        case PE_IndicatorBranch:
            if (option->state & State_Children) {
                const QRectF box(QPointF(), QSizeF(12, 12));
                QRectF r = box;
                r.moveCenter(QRectF(option->rect).center());
                chevron(painter, r, option->state & State_Open ? 90 : 0,
                        option->state & State_Selected ? t.selectionText : t.muted);
            }
            return;
        case PE_IndicatorCheckBox:
        case PE_IndicatorItemViewItemCheck:
            drawCheck(option, painter, t, false);
            return;
        case PE_IndicatorRadioButton:
            drawCheck(option, painter, t, true);
            return;
        case PE_PanelLineEdit:
            if (const auto frame = qstyleoption_cast<const QStyleOptionFrame *>(option)) {
                if (frame->lineWidth > 0)
                    drawField(painter, frame->rect, frame->state, false);
                return; // frameless editors (inside spin and combo boxes) show their field
            }
            break;
        case PE_FrameLineEdit:
            return; // drawn with the panel
        case PE_FrameFocusRect:
            if (qobject_cast<const QAbstractItemView *>(widget))
                return; // rows already show selection and hover
            break;
        default:
            break;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter,
                            const QWidget *widget) const override {
        const Tokens &t = current();
        const bool enabled = option->state & State_Enabled;
        if (control == CC_SpinBox) {
            if (const auto spin = qstyleoption_cast<const QStyleOptionSpinBox *>(option)) {
                if (spin->frame)
                    drawField(painter, spin->rect, spin->state, false);
                if (spin->buttonSymbols == QAbstractSpinBox::NoButtons)
                    return;
                for (const auto sub : {SC_SpinBoxUp, SC_SpinBoxDown}) {
                    const bool up = sub == SC_SpinBoxUp;
                    const bool can = enabled && (spin->stepEnabled & (up ? QAbstractSpinBox::StepUpEnabled
                                                                         : QAbstractSpinBox::StepDownEnabled));
                    const QRect r = subControlRect(control, spin, sub, widget);
                    if (can && (spin->activeSubControls & sub) && (spin->state & State_MouseOver)) {
                        painter->save();
                        painter->setRenderHint(QPainter::Antialiasing);
                        painter->setPen(Qt::NoPen);
                        painter->setBrush(t.raised);
                        painter->drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 2.5, 2.5);
                        painter->restore();
                    }
                    chevron(painter, QRectF(r).adjusted(2, 0, -2, 0), up ? -90 : 90, can ? t.icon : t.disabled);
                }
                return;
            }
        }
        if (control == CC_ComboBox) {
            if (const auto combo = qstyleoption_cast<const QStyleOptionComboBox *>(option)) {
                if (combo->frame)
                    drawField(painter, combo->rect, combo->state, !combo->editable);
                chevron(painter, subControlRect(control, combo, SC_ComboBoxArrow, widget), 90,
                        enabled ? t.icon : t.disabled);
                return;
            }
        }
        QProxyStyle::drawComplexControl(control, option, painter, widget);
    }
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl sub,
                         const QWidget *widget) const override {
        const QRect r = option->rect;
        if (control == CC_SpinBox) {
            const auto spin = qstyleoption_cast<const QStyleOptionSpinBox *>(option);
            const bool buttons = spin && spin->buttonSymbols != QAbstractSpinBox::NoButtons;
            const int half = (r.height() - 4) / 2;
            switch (sub) {
            case SC_SpinBoxFrame:
                return r;
            case SC_SpinBoxUp:
                return buttons ? visualRect(option->direction, r,
                                            QRect(r.right() - SpinButtonWidth - 1, r.top() + 2, SpinButtonWidth, half))
                               : QRect();
            case SC_SpinBoxDown:
                return buttons ? visualRect(option->direction, r,
                                            QRect(r.right() - SpinButtonWidth - 1, r.bottom() - 1 - half,
                                                  SpinButtonWidth, half))
                               : QRect();
            case SC_SpinBoxEditField:
                return visualRect(option->direction, r,
                                  r.adjusted(FieldPadding, 1, -(buttons ? SpinButtonWidth + 2 : FieldPadding), -1));
            default:
                break;
            }
        }
        if (control == CC_ComboBox) {
            switch (sub) {
            case SC_ComboBoxFrame:
                return r;
            case SC_ComboBoxArrow:
                return visualRect(option->direction, r,
                                  QRect(r.right() - ComboArrowWidth, r.top(), ComboArrowWidth, r.height()));
            case SC_ComboBoxEditField:
                return visualRect(option->direction, r, r.adjusted(FieldPadding, 1, -ComboArrowWidth - 2, -1));
            default:
                break;
            }
        }
        return QProxyStyle::subControlRect(control, option, sub, widget);
    }
    QRect subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget) const override {
        if (element == SE_LineEditContents) {
            const auto frame = qstyleoption_cast<const QStyleOptionFrame *>(option);
            if (frame && frame->lineWidth > 0) // framed line edits only; spin and combo editors sit inset
                return visualRect(option->direction, option->rect,
                                  option->rect.adjusted(FieldPadding - 1, 1, -(FieldPadding - 1), -1));
        }
        return QProxyStyle::subElementRect(element, option, widget);
    }
    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &contents,
                           const QWidget *widget) const override {
        switch (type) {
        case CT_SpinBox:
            if (const auto spin = qstyleoption_cast<const QStyleOptionSpinBox *>(option)) {
                const int right = spin->buttonSymbols == QAbstractSpinBox::NoButtons ? FieldPadding
                                                                                     : SpinButtonWidth + 2;
                return {contents.width() + FieldPadding + right + 2, qMax(ControlHeight, contents.height() + 4)};
            }
            break;
        case CT_ComboBox:
            if (qstyleoption_cast<const QStyleOptionComboBox *>(option))
                return {contents.width() + FieldPadding + ComboArrowWidth + 4,
                        qMax(ControlHeight, contents.height() + 6)};
            break;
        case CT_LineEdit:
            if (const auto frame = qstyleoption_cast<const QStyleOptionFrame *>(option); frame && frame->lineWidth > 0)
                return {contents.width() + 2 * FieldPadding, qMax(ControlHeight, contents.height() + 4)};
            break;
        default:
            break;
        }
        return QProxyStyle::sizeFromContents(type, option, contents, widget);
    }

  private:
    static constexpr int FieldPadding = 7, SpinButtonWidth = 15, ComboArrowWidth = 20;
    // The one field shape: a recessed well whose border brightens on hover and takes
    // the accent on focus. Pickers (non-editable combo boxes) lift to the control fill.
    static void drawField(QPainter *p, const QRect &rect, State state, bool picker) {
        const Tokens &t = current();
        const bool enabled = state & State_Enabled;
        const bool focus = enabled && (state & (State_HasFocus | State_On));
        const bool hover = enabled && (state & State_MouseOver);
        const bool readOnly = state & State_ReadOnly;
        QColor fill = !enabled || readOnly ? t.window : picker ? (hover ? t.raised : t.control) : t.base;
        QColor edge = !enabled ? t.border : focus ? t.accent : hover ? mix(t.borderStrong, t.muted, 0.35)
                                                       : picker ? t.borderStrong : t.border;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(edge, 1));
        p->setBrush(fill);
        p->drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), RadiusS, RadiusS);
        p->restore();
    }
    static void drawCheck(const QStyleOption *option, QPainter *p, const Tokens &t, bool radio) {
        const bool enabled = option->state & State_Enabled;
        const bool on = option->state & (State_On | State_NoChange);
        const bool hover = enabled && (option->state & State_MouseOver);
        QRectF box(QPointF(), QSizeF(15, 15));
        box.moveCenter(QRectF(option->rect).center());
        box = box.adjusted(0.5, 0.5, -0.5, -0.5);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        QColor fill = on ? (enabled ? t.accent : t.borderStrong) : (enabled ? t.base : t.window);
        QColor edge = on ? fill : hover ? t.accent : (enabled ? t.borderStrong : t.border);
        p->setPen(QPen(edge, 1));
        p->setBrush(fill);
        if (radio)
            p->drawEllipse(box);
        else
            p->drawRoundedRect(box, 3.5, 3.5);
        if (on) {
            const QColor mark = enabled ? t.onAccent : t.window;
            if (radio) {
                p->setPen(Qt::NoPen);
                p->setBrush(mark);
                p->drawEllipse(box.center(), 3.0, 3.0);
            } else {
                QPen pen(mark, 1.8);
                pen.setCapStyle(Qt::RoundCap);
                pen.setJoinStyle(Qt::RoundJoin);
                p->setPen(pen);
                const QPointF c = box.center();
                if (option->state & State_NoChange) {
                    p->drawLine(c + QPointF(-3.5, 0), c + QPointF(3.5, 0));
                } else {
                    QPainterPath tick;
                    tick.moveTo(c + QPointF(-3.6, 0.2));
                    tick.lineTo(c + QPointF(-1.0, 2.8));
                    tick.lineTo(c + QPointF(3.8, -2.6));
                    p->setBrush(Qt::NoBrush);
                    p->drawPath(tick);
                }
            }
        }
        p->restore();
    }
};

QPointer<QStyle> installedStyle;
} // namespace

QStyle *createStyle() {
    auto style = new NovaStyle;
    installedStyle = style;
    return style;
}

void apply(const Tokens &t) {
    setCurrent(t);
    // Created once at startup by main(); tests and hosts that construct the
    // workspace under plain Fusion get it here, before the palette is set.
    if (!installedStyle)
        qApp->setStyle(createStyle());
    QFont font = QApplication::font();
    if (font.pointSizeF() != TypeBody) {
        font.setPointSizeF(TypeBody);
        QApplication::setFont(font);
    }
    qApp->setPalette(palette(t));
    qApp->setStyleSheet(styleSheet(t));
}

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
QIcon lineIcon(const QString &name) { return lineIcon(name, current().icon, current().disabled); }

QLabel *sectionTitle(const QString &text, QWidget *parent) {
    auto label = new QLabel(text.toUpper(), parent);
    label->setProperty("role", "section");
    label->setAccessibleName(text);
    QFont f = label->font();
    f.setLetterSpacing(QFont::PercentageSpacing, 108);
    label->setFont(f);
    return label;
}
QLabel *hint(const QString &text, QWidget *parent) {
    auto label = new QLabel(text, parent);
    label->setProperty("role", "hint");
    label->setWordWrap(true);
    return label;
}
QLabel *emptyState(const QString &text, QWidget *parent) {
    auto label = new QLabel(text, parent);
    label->setProperty("role", "empty");
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    return label;
}
void setPrimary(QWidget *button) {
    button->setProperty("role", "primary");
    button->style()->unpolish(button);
    button->style()->polish(button);
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
