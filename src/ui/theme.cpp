// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QScrollArea>
#include <QAbstractButton>
#include <QAbstractSlider>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QAbstractSpinBox>
#include <QSpinBox>
#include <QSlider>
#include <QHBoxLayout>
#include <QApplication>
#include <QDebug>
#include <QDockWidget>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
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
#include <QMenu>
#include <QWidgetAction>
#include <QVBoxLayout>

namespace nova::ui {
QWidget *sliderField(QSpinBox *value, const QString &label) {
    auto field = new QWidget;
    auto row = new QHBoxLayout(field);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(SpaceL);
    auto slider = new QSlider(Qt::Horizontal);
    slider->setObjectName(value->objectName() + "Slider");
    slider->setAccessibleName(label);
    slider->setToolTip(value->toolTip().isEmpty() ? label : value->toolTip());
    slider->setRange(value->minimum(), value->maximum());
    slider->setSingleStep(value->singleStep());
    slider->setValue(value->value());
    slider->setMinimumWidth(96);
    value->setAccessibleName(label);
    value->setFixedWidth(SettingsValueWidth);
    // The slider is the pointer control; the value box takes typing, arrows and the wheel.
    value->setButtonSymbols(QAbstractSpinBox::NoButtons);
    row->addWidget(slider, 1);
    row->addWidget(value);
    field->setFocusProxy(value);
    QObject::connect(slider, &QSlider::valueChanged, value, &QSpinBox::setValue);
    QObject::connect(value, &QSpinBox::valueChanged, slider, &QSlider::setValue);
    return field;
}
void settingsMenu(QToolButton *button, QWidget *content) {
    if (auto form = qobject_cast<QFormLayout *>(content->layout())) {
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        for (int row = 0; row < form->rowCount(); ++row)
            if (auto item = form->itemAt(row, QFormLayout::LabelRole); item && item->widget())
                item->widget()->setMinimumWidth(SettingsLabelWidth);
    }
    for (auto field : content->findChildren<QAbstractSpinBox *>()) {
        field->setAlignment(Qt::AlignRight);
        field->setMinimumWidth(SettingsValueWidth);
    }
    auto menu = new QMenu(button);
    auto action = new QWidgetAction(menu);
    auto host = new QWidget;
    auto layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto title = new QLabel(button->text());
    title->setProperty("role", "settingsTitle");
    title->setFont(font(Type::Strong));
    title->setContentsMargins(PanelPadding, PanelPadding, PanelPadding, 0);
    layout->addWidget(title);
    layout->addWidget(content);
    action->setDefaultWidget(host);
    menu->addAction(action);
    button->setMenu(menu);
    button->setPopupMode(QToolButton::InstantPopup);
}
namespace {
class PathField final : public QLineEdit {
  public:
    using QLineEdit::QLineEdit;
  protected:
    void paintEvent(QPaintEvent *event) override {
        QStyleOptionFrame option;
        initStyleOption(&option);
        const auto area = style()->subElementRect(QStyle::SE_LineEditContents, &option, this)
                              .adjusted(SpaceXS, 0, -SpaceXS, 0);
        if (hasFocus() || text().isEmpty() || fontMetrics().horizontalAdvance(text()) <= area.width()) {
            QLineEdit::paintEvent(event);
            return;
        }
        auto display = text();
        display.replace('\\', '/');
        const auto parts = display.split('/', Qt::SkipEmptyParts);
        if (parts.size() > 2) {
            const auto prefix = display.startsWith("//") ? "//" + parts.first() : parts.first();
            display = prefix + "/…/" + parts.last();
        }
        QPainter p(this);
        style()->drawPrimitive(QStyle::PE_PanelLineEdit, &option, &p, this);
        p.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::Text));
        p.drawText(area, Qt::AlignVCenter | Qt::AlignLeft,
                   fontMetrics().elidedText(display, Qt::ElideMiddle, area.width()));
    }
};
}
QLineEdit *pathField(const QString &text, QWidget *parent) {
    auto field = new PathField(text, parent);
    field->setToolTip(text);
    QObject::connect(field, &QLineEdit::textChanged, field, [field](const QString &value) { field->setToolTip(value); });
    return field;
}
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
        t.list = t.base;
        t.window = QColor("#191a1f");
        t.surface = QColor("#1e1f25");
        t.raised = QColor("#272830");
        t.control = QColor("#24252c");
        t.border = QColor("#2a2b33");
        t.borderStrong = QColor("#3a3c46");
        t.text = QColor("#e3e4e8");
        t.muted = QColor("#a4a7b2");
        t.disabled = QColor("#7a7d89"); // 4.1:1 on panels: readable yet clearly inactive
        t.placeholder = QColor("#7e818d");
        t.icon = QColor("#c4c6ce");
        t.accent = QColor(blue ? "#4a8af4" : "#7b6cf0");
        t.accentHover = QColor(blue ? "#619bf7" : "#8d80f4");
        t.accentMuted = QColor(blue ? "#1b2a42" : "#29263f");
        t.accentFill = QColor(blue ? "#2d6ad0" : "#6a5ae0"); // 5.1:1 under white labels
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
        t.backdrop = QColor("#c9cbd2");
        t.base = QColor("#ffffff");
        t.canvas = QColor("#d5d7dc");
        t.window = QColor("#f4f4f6");
        t.list = t.window;
        t.surface = QColor("#ececef");
        t.raised = QColor("#e5e6ea");
        t.control = QColor("#ffffff");
        t.border = QColor("#d6d8de");
        t.borderStrong = QColor("#bfc2ca");
        t.text = QColor("#1b1d22");
        t.muted = QColor("#5b5f6b");
        t.disabled = QColor("#878a93"); // 3.1:1 on panels: legible yet clearly inactive
        t.placeholder = QColor("#6e7280"); // 4.8:1 on white (AA)
        t.icon = QColor("#3d414b");
        t.accent = QColor(blue ? "#2563d9" : "#6550e0");
        t.accentHover = QColor(blue ? "#1f56c2" : "#5641d2");
        t.accentMuted = QColor(blue ? "#e3ecfb" : "#ece9fd");
        t.accentFill = t.accent; // already 5.4:1 under white labels
        t.onAccent = Qt::white;
        t.selection = QColor(blue ? "#b6cdf3" : "#c6bcf4");
        t.selectionText = QColor("#1b1d22");
        t.warning = QColor("#94600a");
        t.danger = QColor("#b42318");
        t.success = QColor("#1e7b48");
        t.info = QColor("#2563d9");
        t.warningSurface = QColor("#fdf4e3");
        t.dangerSurface = QColor("#fdecea");
        t.infoSurface = QColor("#e8f0fd");
        t.checkerDark = QColor("#8f9197");
        t.checkerLight = QColor("#9d9fa4"); // a step below the canvas well, keeping the edge visible
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
        "QToolBar#toolOptions QMenu QLabel{padding-left:0}"
        // The options bar is chrome, not a well: its scroll rows show the toolbar fill.
        "QToolBar#toolOptions QScrollArea,QToolBar#toolOptions QScrollArea>QWidget,"
        "QToolBar#toolOptions QScrollArea>QWidget>QWidget{background:transparent}"
        "QToolBar#toolOptions QLabel#paintToolName,QToolBar#toolOptions QLabel#toolOptionsName{color:@text;font-weight:600;padding-left:2px;padding-right:6px}"
        "QToolBar#toolBox{border-bottom:0;border-right:1px solid @border;padding:@stripPadV @stripPadH}"
        "QToolBar#toolBox QToolButton{padding:@toolPad}"
        "QToolBar::separator{background:@border;width:1px;height:1px;margin:@sepMinor @sepMajor}"
        "QToolBar#toolBox::separator{margin:@sepTool @sepMinor}"
        "QStatusBar{background:@window;border-top:1px solid @border;color:@muted}"
        "QStatusBar::item{border:0}"
        "QStatusBar QLabel{color:@muted}"
        // Panels.
        // Establish the panel palette before transparent child containers inherit
        // the main window's gutter colour, especially visible in the light theme.
        "QDockWidget{background:@window}"
        "QDockWidget>QWidget{background:@window}"
        // Dock titles are painted by NovaStyle (semibold, token colours) so no font rule
        // pins a size onto docks and their contents.
        "QDockWidget::close-button,QDockWidget::float-button{border:0;border-radius:@rS;background:transparent;"
        "padding:2px}"
        "QDockWidget::close-button:hover,QDockWidget::float-button:hover{background:@raised}"
        "QSplitter::handle{background:@border}"
        "QWidget#animationTimeline{background:@window}"
        "QLabel[role=\"settingsTitle\"],QToolBar#toolOptions QLabel[role=\"settingsTitle\"]{font-weight:600;color:@text}"
        // Layer rows match tree rows so the eye toggle and name have room.
        "QListWidget#layerList::item{min-height:@rowHeight}"
        // Search floats over the workspace: a hairline frame defines its edge in captures too.
        "QDialog#commandPalette{border:1px solid @borderStrong}"
        // Wide gutters between cards stay empty space; they show only while dragged over.
        "QSplitter[role=\"gutter\"]::handle{background:transparent}"
        "QSplitter[role=\"gutter\"]::handle:hover{background:@accentMuted}"
        "QFrame[role=\"rule\"]{background:@border;max-height:1px;min-height:1px;border:0}"
        "QFrame[role=\"vrule\"]{background:@borderStrong;border:0}"
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
        "QMainWindow>QTabBar{background:@window;border-bottom:1px solid @border}"
        "QMainWindow>QTabBar::tab{padding:7px 12px 6px 12px;margin:0;background:@window;font-weight:600}"
        "QMainWindow>QTabBar::tab:!selected{font-weight:400}"
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
        "QPushButton[role=\"primary\"]{border-color:@accentFill;background:@accentFill;color:@onAccent;font-weight:600}"
        "QPushButton[role=\"primary\"]:hover{border-color:@accent;background:@accent}"
        "QPushButton[role=\"primary\"]:focus{border-color:@focusOnAccent}"
        "QPushButton[role=\"primary\"]:disabled{border-color:@primaryDisabledFill;background:@primaryDisabledFill;"
        "color:@primaryDisabledText}"
        "QPushButton[role=\"segment\"]{padding:3px 10px;border-radius:@rS}"
        "QPushButton[role=\"icon\"]{padding:4px;min-width:18px;border-color:transparent;background:transparent}"
        "QPushButton[role=\"icon\"]:hover{background:@raised;border-color:transparent}"
        "QPushButton[role=\"icon\"]:disabled{background:transparent;border-color:transparent}"
        // Link: a quiet text command beside a heading (no frame, no menu arrow). accentHover is
        // the higher-contrast accent in both themes.
        "QPushButton[role=\"link\"]{padding:2px 6px;border-color:transparent;background:transparent;color:@accentHover}"
        "QPushButton[role=\"link\"]:hover{background:@accentMuted;border-color:transparent}"
        "QPushButton[role=\"link\"]:focus{border-color:@accent}"
        "QPushButton[role=\"link\"]:disabled{color:@disabled;background:transparent;border-color:transparent}"
        "QPushButton[role=\"link\"]::menu-indicator{image:none;width:0}"
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
        "QToolButton[popupMode=\"2\"]{padding-right:18px}"
        "QToolButton[popupMode=\"2\"]::menu-indicator{subcontrol-origin:padding;subcontrol-position:center right;"
        "right:4px;width:10px;height:10px}"
        // Rail buttons mark a menu with a corner triangle (ToolGroupButton), not an arrow.
        "QToolBar#toolBox QToolButton[popupMode=\"2\"]{padding:@toolPad}"
        "QToolBar#toolBox QToolButton::menu-indicator{image:none;width:0;height:0}"
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
        "QCheckBox[role=\"toggle\"]{spacing:0;padding:4px;border:1px solid transparent;border-radius:@rS}"
        "QCheckBox[role=\"toggle\"]::indicator{width:0;height:0}"
        "QCheckBox[role=\"toggle\"]:hover{background:@raised}"
        "QCheckBox[role=\"toggle\"]:checked{background:@accentMuted;border-color:@accent}"
        "QSlider::groove:horizontal{height:4px;border-radius:2px;background:@sliderTrack}"
        "QSlider::sub-page:horizontal{height:4px;border-radius:2px;background:@accent}"
        "QSlider::add-page:horizontal{height:4px;border-radius:2px;background:@sliderTrack}"
        "QSlider::handle:horizontal{width:14px;height:14px;margin:-5px 0;border-radius:7px;"
        "background:@sliderHandle;border:1px solid @borderStrong}"
        "QSlider::handle:horizontal:hover{border-color:@accent}"
        "QSlider::sub-page:horizontal:disabled{background:@borderStrong}"
        "QSlider::groove:horizontal:disabled,QSlider::add-page:horizontal:disabled{background:@borderStrong}"
        "QSlider::handle:horizontal:disabled{background:@disabled;border-color:@borderStrong}"
        "QProgressBar{min-height:6px;max-height:6px;border:0;border-radius:3px;background:@raised;"
        "text-align:center;color:transparent}"
        "QProgressBar::chunk{border-radius:3px;background:@accent}"
        // Lists, trees and tables.
        "QTreeView,QListView,QTableView,QListWidget,QTreeWidget,QTableWidget{border:0;background:@list;"
        "alternate-background-color:@alternate;outline:0;selection-background-color:@selection;"
        "selection-color:@selectionText}"
        "QTreeView::item{height:@rowHeight}"
        "QTreeView::item:hover,QListView::item:hover,QTableView::item:hover{background:@hoverRow}"
        "QTreeView::item:selected,QListView::item:selected,QTableView::item:selected{background:@selection;"
        "color:@selectionText}"
        // The asset tree and layer list mark the selected row with an accent edge too,
        // so selection reads at a glance in both themes.
        "QTreeView#assetTree::item,QListWidget#layerList::item{border-left:2px solid transparent}"
        "QTreeView#assetTree::item:selected,QListWidget#layerList::item:selected{border-left-color:@accent}"
        // Short lists embedded in a panel sit in a bordered inset well.
        "QListWidget[role=\"inset\"]{background:@base;border:1px solid @border;border-radius:@rS;padding:2px}"
        "QListWidget[role=\"inset\"]::item{height:@insetRow;padding:0 6px;border-radius:@rS}"
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
        "QGroupBox::title{subcontrol-origin:margin;subcontrol-position:top left;left:16px;padding:0 0 4px 0;"
        "color:@text}"
        "QGroupBox QAbstractItemView{background:@window;border:1px solid @border;border-radius:@rS}"
        "QGroupBox[role=\"section\"]{font-weight:600;border:1px solid @border;border-radius:@rM;"
        "margin-top:20px;padding:@panelPad @panelPad @panelPadV @panelPad;background:@surface}"
        "QGroupBox[role=\"section\"]::title{subcontrol-origin:margin;left:2px;padding:0 0 4px 0;color:@text}"
        // Text roles.
        // Section titles take the text colour: their field labels are muted, so the title has to
        // outrank them by more than case and weight.
        "QLabel[role=\"section\"]{font-size:@captionPt;font-weight:600;color:@text;padding-top:6px}"
        "QLabel[role=\"title\"]{font-size:@titlePt;font-weight:600;color:@text}"
        "QLabel[role=\"display\"]{font-size:@displayPt;font-weight:600;color:@text}"
        "QLabel[role=\"hint\"]{color:@muted}"
        "QLabel:disabled{color:@disabled}"
        "QLabel[role=\"strong\"]{font-weight:600;color:@text}"
        "QLabel[role=\"keycap\"]{color:@muted;background:@surface;border:1px solid @borderStrong;"
        "border-radius:@rS;padding:1px 6px}"
        "QLabel[role=\"empty\"]{color:@muted;padding:16px}"
        "QLabel[role=\"value\"]{color:@text}"
        "QLabel[role=\"warning\"]{color:@warning}"
        "QLabel[role=\"info\"]{color:@info}"
        "QLabel[role=\"error\"]{color:@danger}"
        "QLabel[role=\"notice\"]{background:@warningSurface;color:@text;border:1px solid @noticeBorder;"
        "border-radius:@rM;padding:0}");
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
        {"@alternate", mix(t.list, t.raised, 0.35)},
        {"@hoverRow", mix(t.list, t.raised, 0.7)},
        {"@list", t.list},
        {"@accentFill", t.accentFill},
        {"@accentHover", t.accentHover},
        {"@accentMuted", t.accentMuted},
        {"@accent", t.accent},
        {"@onAccent", t.onAccent},
        {"@focusOnAccent", mix(t.accent, t.onAccent, 0.55)},
        {"@primaryDisabledText", mix(t.accent, t.onAccent, 0.45)},
        {"@primaryDisabledFill", mix(t.window, t.accent, 0.42)},
        {"@selectionText", t.selectionText},
        {"@selection", t.selection},
        {"@muted", t.muted},
        {"@text", t.text},
        {"@disabled", t.disabled},
        {"@warningSurface", t.warningSurface},
        {"@noticeBorder", mix(t.warningSurface, t.warning, 0.45)},
        {"@warning", t.warning},
        {"@danger", t.danger},
        {"@info", t.info},
        {"@sliderHandle", sliderHandle},
        {"@sliderTrack", mix(t.borderStrong, t.muted, 0.55)},
        {"@scrollThumb", t.dark ? mix(t.borderStrong, t.muted, 0.25) : mix(t.borderStrong, t.window, 0.15)},
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
        {"@scroll", px(ScrollBarWidth)},
        {"@rowHeight", px(RowHeight)},
        {"@insetRow", px(InsetRowHeight)},
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
        {"layers", QChar(0xe53b)},
        {"info", QChar(0xe88e)},
        {"cube", QChar(0xefc9)}, // view_in_ar
        {"results", QChar(0xf02f)}, // manage_search
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
        {"eye-off", QChar(0xe8f5)},   // visibility_off
        {"palette", QChar(0xe40a)},
        {"opacity", QChar(0xe91c)},
        {"menu", QChar(0xe5d2)},
        {"go", QChar(0xe5c8)}, // arrow_forward
        {"swap-colors", QChar(0xe8d4)},  // swap_horiz
        {"reset-colors", QChar(0xe23b)}, // format_color_reset
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
        const int pixels = qMax(1, qMin(rect.width(), rect.height()));
        font.setPixelSize(pixels);
        // Regular outlines stay legible in compact panel actions and at 100% scale.
        font.setVariableAxis(QFont::Tag("wght"), 400);
        font.setVariableAxis(QFont::Tag("opsz"), std::clamp(pixels, 20, 48));
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
    using QProxyStyle::polish;
    void polish(QWidget *widget) override {
        QProxyStyle::polish(widget);
        if (qobject_cast<QDockWidget *>(widget->parentWidget()))
            widget->setAttribute(Qt::WA_StyledBackground);
    }
    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override {
        switch (metric) {
        case PM_DockWidgetSeparatorExtent:
            return GutterWidth;
        case PM_DockWidgetTitleMargin:
            return PanelPaddingV;
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
        case SH_UnderlineShortcut: // mnemonics appear while Alt is held, as on Windows
            return 0;
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
        case PE_IndicatorItemViewItemCheck:
            if (widget && widget->objectName() == QLatin1String("layerList")) {
                const bool visible = option->state & State_On;
                QRect r(QPoint(), QSize(16, 16));
                r.moveCenter(option->rect.center());
                const QColor ink = !(option->state & State_Enabled) ? t.disabled
                                   : visible                         ? t.icon
                                                                     : t.disabled;
                lineIcon(visible ? QStringLiteral("eye") : QStringLiteral("eye-off"), ink, t.disabled)
                    .paint(painter, r);
                return;
            }
            drawCheck(option, painter, t, false);
            return;
        case PE_IndicatorCheckBox:
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
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget) const override {
        if (element == CE_DockWidgetTitle) {
            if (const auto dock = qstyleoption_cast<const QStyleOptionDockWidget *>(option)) {
                const Tokens &t = current();
                painter->save();
                painter->fillRect(option->rect, t.window);
                painter->setPen(t.border);
                painter->drawLine(option->rect.bottomLeft(), option->rect.bottomRight());
                // The body font, not the dock's platform title font, so titles match panel text.
                QFont strong = QApplication::font();
                strong.setWeight(QFont::DemiBold);
                painter->setFont(strong);
                painter->setPen(option->state & State_Enabled ? t.text : t.disabled);
                QRect text = subElementRect(SE_DockWidgetTitleBarText, option, widget);
                text.setLeft(option->rect.left() + PanelPadding);
                painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                                  QFontMetrics(strong).elidedText(dock->title, Qt::ElideRight, text.width()));
                painter->restore();
                return;
            }
        }
        QProxyStyle::drawControl(element, option, painter, widget);
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
    static constexpr int FieldPadding = 7, SpinButtonWidth = Space3XL, ComboArrowWidth = 20;
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
                                                       : t.borderStrong;
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
        // A disabled choice keeps the accent language, faded towards the panel, so "on but
        // unavailable" still reads as on rather than as a different kind of control.
        QColor fill = on ? (enabled ? t.accent : mix(t.accent, t.window, 0.6)) : (enabled ? t.base : t.window);
        QColor edge = on ? fill : hover ? t.accent : (enabled ? mix(t.borderStrong, t.muted, 0.55) : t.border);
        p->setPen(QPen(edge, 1));
        p->setBrush(fill);
        if (radio)
            p->drawEllipse(box);
        else
            p->drawRoundedRect(box, 3.5, 3.5);
        if (on) {
            const QColor mark = enabled ? t.onAccent : mix(t.onAccent, fill, 0.4);
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

namespace {
// The platform assigns its own (smaller) fonts to menus, item views and docks, and
// re-applies them when the style or palette changes; pin them to the body font each
// time so every surface shares one size. Existing widgets receive the change.
void pinClassFonts() {
    QFont body = QApplication::font();
    body.setPointSizeF(TypeBody);
    for (const char *cls : {"QMenuBar", "QMenu", "QAbstractItemView", "QHeaderView", "QTabBar", "QDockWidget",
                            "QGroupBox", "QMessageBox", "QComboBoxListView", "QStatusBar"})
        if (QApplication::font(cls) != body)
            QApplication::setFont(body, cls);
}
} // namespace
void prepareApplication() {
    // Created once at startup by main(); tests and hosts that construct the
    // workspace under plain Fusion get it here, before the palette is set.
    if (!installedStyle)
        qApp->setStyle(createStyle());
    // Setting the default font once also clears the platform's per-class fonts (dock
    // titles, menus, message boxes), so every surface shares the body size.
    static bool fontApplied = false;
    QFont font = QApplication::font();
    if (!fontApplied || font.pointSizeF() != TypeBody) {
#ifdef Q_OS_WIN
        // Resolve the Windows UI family explicitly before changing weight in QSS;
        // an unresolved platform alias can otherwise fall back on bold labels.
        font.setFamily(QStringLiteral("Segoe UI"));
#endif
        font.setPointSizeF(TypeBody);
        QApplication::setFont(font);
        fontApplied = true;
    }
    pinClassFonts();
}

void apply(const Tokens &t) {
    setCurrent(t);
    prepareApplication();
    qApp->setPalette(palette(t));
    qApp->setStyleSheet(styleSheet(t));
    pinClassFonts();
    // Widgets that resolved a platform class font before the pin keep it cached; make
    // every widget re-resolve so all panels show the body size.
    for (auto widget : QApplication::allWidgets()) {
        QEvent change(QEvent::ApplicationFontChange);
        QApplication::sendEvent(widget, &change);
    }
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
namespace {
class EmptyState : public QLabel {
    QString glyph;

  public:
    EmptyState(const QString &text, QWidget *parent, QString icon) : QLabel(text, parent), glyph(std::move(icon)) {
        // A top margin reserves the glyph's room, so layouts and size hints account for it.
        if (!glyph.isEmpty())
            setContentsMargins(0, 32 + SpaceL, 0, 0);
    }

  protected:
    void paintEvent(QPaintEvent *event) override {
        QLabel::paintEvent(event);
        if (glyph.isEmpty())
            return;
        // The glyph sits a gap above the centred text block, in the disabled ink.
        const int size = 32;
        const QRect content = contentsRect();
        const QRect textBlock =
            fontMetrics().boundingRect(content, int(alignment()) | Qt::TextWordWrap, text());
        const int top = std::max(rect().top(), textBlock.top() - size - SpaceL);
        QPainter p(this);
        lineIcon(glyph, current().disabled).paint(&p, QRect(content.center().x() - size / 2, top, size, size));
    }
};
} // namespace
QLabel *emptyState(const QString &text, QWidget *parent, const QString &icon) {
    auto label = new EmptyState(text, parent, icon);
    label->setProperty("role", "empty");
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    return label;
}
namespace {
class ScrollFade : public QWidget {
    QAbstractScrollArea *area;

  public:
    explicit ScrollFade(QAbstractScrollArea *a) : QWidget(a), area(a) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        const auto update = [this] { place(); };
        connect(area->verticalScrollBar(), &QScrollBar::valueChanged, this, update);
        connect(area->verticalScrollBar(), &QScrollBar::rangeChanged, this, update);
        afterEvents(area, {QEvent::Resize, QEvent::Show, QEvent::LayoutRequest}, update);
    }
    void place() {
        const auto bar = area->verticalScrollBar();
        const auto viewport = area->viewport()->geometry();
        setGeometry(viewport.left(), viewport.bottom() - 11, viewport.width(), 12);
        const bool more = bar->maximum() > 0 && bar->value() < bar->maximum();
        setVisible(more);
        raise();
        hideCutRows(more);
    }
    // A row that the bottom edge would slice in half is hidden whole, so the panel ends on
    // a clean line above the shadow instead of on half a heading.
    void hideCutRows(bool more) {
        auto scroll = qobject_cast<QScrollArea *>(area);
        auto content = scroll ? scroll->widget() : nullptr;
        if (!content)
            return;
        const int edge = area->verticalScrollBar()->value() + area->viewport()->height();
        QRegion visible(content->rect());
        bool cut = false;
        // An embedded list can also cross its parent's viewport edge. Keep the last
        // visible item whole instead of exposing the top half of its text.
        if (more)
            for (auto list : content->findChildren<QListWidget *>()) {
                if (!list->isVisibleTo(content))
                    continue;
                const auto viewport = list->viewport();
                const auto point = viewport->mapFrom(content, QPoint(list->mapTo(content, QPoint()).x() + SpaceL, edge - 1));
                const auto index = list->indexAt(point);
                if (!index.isValid())
                    continue;
                const auto item = list->visualRect(index);
                const QRect r(viewport->mapTo(content, item.topLeft()), item.size());
                if (r.top() < edge && r.bottom() >= edge) {
                    visible -= QRect(0, r.top(), content->width(), r.height());
                    cut = true;
                }
            }
        if (more)
            for (auto w : content->findChildren<QWidget *>()) {
                if (!w->isVisibleTo(content) || !(qobject_cast<QLabel *>(w) || qobject_cast<QAbstractButton *>(w) ||
                                                  qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QComboBox *>(w) ||
                                                  qobject_cast<QAbstractSlider *>(w) || qobject_cast<QLineEdit *>(w)))
                    continue;
                const QRect r(w->mapTo(content, QPoint()), w->size());
                if (r.top() < edge && r.bottom() >= edge) {
                    visible -= QRect(0, r.top() - SpaceXS, content->width(), r.height() + 2 * SpaceXS);
                    cut = true;
                }
            }
        if (cut)
            content->setMask(visible);
        else if (!content->mask().isEmpty())
            content->clearMask();
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        QLinearGradient fade(0, 0, 0, height());
        fade.setColorAt(0, QColor(0, 0, 0, 0));
        fade.setColorAt(1, QColor(0, 0, 0, current().dark ? 110 : 30));
        p.fillRect(rect(), fade);
        p.fillRect(QRect(0, height() - 1, width(), 1), current().border);
    }
};
} // namespace
void addScrollFade(QAbstractScrollArea *area) { new ScrollFade(area); }
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
