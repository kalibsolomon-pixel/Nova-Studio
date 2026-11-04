// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"
#include <QHash>
#include <QIconEngine>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <cmath>
#include <numbers>

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
        "QToolBar{spacing:4px;padding:3px 6px;border:0;border-bottom:1px solid @border;background:@window}"
        "QToolBar#toolBox{border-bottom:0;border-right:1px solid @border;padding:4px 3px}"
        "QToolBar::separator{background:@border;width:1px;height:1px;margin:4px 6px}"
        "QToolBar#toolBox::separator{margin:5px 4px}"
        "QDockWidget::title{padding:5px 8px;background:@surface;border-bottom:1px solid @border;"
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
        "QToolButton{border:1px solid transparent;border-radius:4px;padding:2px}"
        "QToolButton:hover{background:@raised;border-color:@border}"
        "QToolButton:pressed{background:@surface}"
        "QToolButton:checked{background:palette(highlight);border-color:@accent}"
        "QToolButton:focus{border-color:@accent}"
        "QToolButton::menu-button{border:0;width:10px}"
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
    return s;
}
const Tokens &current() { return active; }
void setCurrent(const Tokens &t) { active = t; }

namespace {
void drawIcon(QPainter &p, const QString &name, const QColor &ink) {
    QPen pen(ink, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    auto arrowHead = [&](QPointF tip, double angle) {
        const double a = angle * std::numbers::pi / 180.0;
        for (double side : {-0.6, 0.6})
            p.drawLine(tip, tip - QPointF(std::cos(a + side), std::sin(a + side)) * 4.5);
    };
    if (name == "save") {
        p.drawRoundedRect(QRectF(4.5, 4.5, 15, 15), 2, 2);
        p.drawRect(QRectF(8, 4.5, 8, 5));
        p.drawRect(QRectF(7.5, 13, 9, 6.5));
    } else if (name == "undo" || name == "redo") {
        if (name == "redo") {
            p.translate(24, 0);
            p.scale(-1, 1);
        }
        QPainterPath path(QPointF(6, 10));
        path.lineTo(14, 10);
        path.cubicTo(20, 10, 20, 19, 14, 19);
        path.lineTo(10, 19);
        p.drawPath(path);
        arrowHead(QPointF(5.5, 10), 0);
    } else if (name == "open" || name == "folder") {
        QPainterPath path(QPointF(3.5, 7));
        path.lineTo(9, 7);
        path.lineTo(11, 9);
        path.lineTo(20.5, 9);
        path.lineTo(20.5, 18.5);
        path.lineTo(3.5, 18.5);
        path.closeSubpath();
        p.drawPath(path);
        if (name == "open")
            p.drawLine(QPointF(3.5, 12), QPointF(20.5, 12));
    } else if (name == "search" || name == "command") {
        p.drawEllipse(QPointF(10.5, 10.5), 5.5, 5.5);
        p.drawLine(QPointF(14.5, 14.5), QPointF(19.5, 19.5));
        if (name == "command") {
            p.drawLine(QPointF(8, 10.5), QPointF(13, 10.5));
        }
    } else if (name == "close") {
        p.drawLine(QPointF(7, 7), QPointF(17, 17));
        p.drawLine(QPointF(17, 7), QPointF(7, 17));
    } else if (name == "add") {
        p.drawLine(QPointF(12, 5), QPointF(12, 19));
        p.drawLine(QPointF(5, 12), QPointF(19, 12));
    } else if (name == "duplicate") {
        p.drawRoundedRect(QRectF(8.5, 8.5, 11, 11), 1.5, 1.5);
        p.drawPolyline(QPolygonF({QPointF(15.5, 5.5), QPointF(4.5, 5.5), QPointF(4.5, 15.5)}));
    } else if (name == "delete") {
        p.drawLine(QPointF(5, 7), QPointF(19, 7));
        p.drawLine(QPointF(10, 4.5), QPointF(14, 4.5));
        p.drawPolyline(QPolygonF({QPointF(7, 7), QPointF(8, 19.5), QPointF(16, 19.5), QPointF(17, 7)}));
    } else if (name == "rename") {
        p.drawLine(QPointF(5, 19), QPointF(19, 19));
        p.drawLine(QPointF(8, 15.5), QPointF(16.5, 7));
        p.drawLine(QPointF(15, 5.5), QPointF(18, 8.5));
    } else if (name == "up" || name == "down") {
        if (name == "down") {
            p.translate(0, 24);
            p.scale(1, -1);
        }
        p.drawLine(QPointF(12, 19), QPointF(12, 5.5));
        arrowHead(QPointF(12, 5), -90);
    } else if (name == "parent") {
        p.drawPolyline(QPolygonF({QPointF(18, 19), QPointF(18, 11), QPointF(7, 11)}));
        arrowHead(QPointF(6.5, 11), 180);
    } else if (name == "tree") {
        for (double y : {6.0, 12.0, 18.0})
            p.drawLine(QPointF(y == 6 ? 5 : 10, y), QPointF(19, y));
        p.drawPolyline(QPolygonF({QPointF(6, 8), QPointF(6, 18), QPointF(9, 18)}));
        p.drawLine(QPointF(6, 12), QPointF(9, 12));
    } else if (name == "grid") {
        for (double x : {4.5, 13.5})
            for (double y : {4.5, 13.5})
                p.drawRoundedRect(QRectF(x, y, 6, 6), 1, 1);
    } else if (name == "sort") {
        p.drawLine(QPointF(8, 5), QPointF(8, 19));
        arrowHead(QPointF(8, 19.5), 90);
        for (int i = 0; i < 3; ++i)
            p.drawLine(QPointF(12, 7 + i * 5), QPointF(20 - i * 3, 7 + i * 5));
    } else if (name == "zoom-in" || name == "zoom-out") {
        p.drawEllipse(QPointF(10.5, 10.5), 5.5, 5.5);
        p.drawLine(QPointF(14.5, 14.5), QPointF(19.5, 19.5));
        p.drawLine(QPointF(8, 10.5), QPointF(13, 10.5));
        if (name == "zoom-in")
            p.drawLine(QPointF(10.5, 8), QPointF(10.5, 13));
    } else if (name == "fit") {
        for (auto [x, y, dx, dy] : {std::tuple{4.5, 4.5, 1, 1}, {19.5, 4.5, -1, 1}, {4.5, 19.5, 1, -1}, {19.5, 19.5, -1, -1}}) {
            p.drawLine(QPointF(x, y), QPointF(x + dx * 5, y));
            p.drawLine(QPointF(x, y), QPointF(x, y + dy * 5));
        }
        p.drawRect(QRectF(9, 9, 6, 6));
    } else if (name == "actual") {
        p.drawRect(QRectF(5.5, 5.5, 13, 13));
        p.drawLine(QPointF(10.5, 9), QPointF(10.5, 15));
        p.drawLine(QPointF(14, 9), QPointF(14, 15));
    } else if (name == "center") {
        p.drawEllipse(QPointF(12, 12), 6.5, 6.5);
        p.drawLine(QPointF(12, 3), QPointF(12, 7));
        p.drawLine(QPointF(12, 17), QPointF(12, 21));
        p.drawLine(QPointF(3, 12), QPointF(7, 12));
        p.drawLine(QPointF(17, 12), QPointF(21, 12));
    } else if (name == "settings") {
        p.drawEllipse(QPointF(12, 12), 3, 3);
        for (int i = 0; i < 8; ++i) {
            const double a = i * std::numbers::pi / 4;
            p.drawLine(QPointF(12 + std::cos(a) * 5.5, 12 + std::sin(a) * 5.5),
                       QPointF(12 + std::cos(a) * 8, 12 + std::sin(a) * 8));
        }
        p.drawEllipse(QPointF(12, 12), 5.5, 5.5);
    } else if (name == "activity") {
        QPainterPath bell(QPointF(6, 17));
        bell.lineTo(6, 11);
        bell.cubicTo(6, 4.5, 18, 4.5, 18, 11);
        bell.lineTo(18, 17);
        p.drawPath(bell);
        p.drawLine(QPointF(4.5, 17), QPointF(19.5, 17));
        p.drawLine(QPointF(10.5, 20), QPointF(13.5, 20));
    } else if (name == "workspace") {
        p.drawRoundedRect(QRectF(3.5, 5, 17, 14), 2, 2);
        p.drawLine(QPointF(9, 5), QPointF(9, 19));
        p.drawLine(QPointF(15.5, 5), QPointF(15.5, 19));
    } else if (name == "keyboard") {
        p.drawRoundedRect(QRectF(3, 7, 18, 11), 2, 2);
        for (int i = 0; i < 4; ++i)
            p.drawPoint(QPointF(7 + i * 3.4, 10.5));
        p.drawLine(QPointF(8, 14.5), QPointF(16, 14.5));
    } else if (name == "transform") {
        p.drawRect(QRectF(6.5, 6.5, 11, 11));
        for (auto c : {QPointF(6.5, 6.5), QPointF(17.5, 6.5), QPointF(6.5, 17.5), QPointF(17.5, 17.5)})
            p.drawRect(QRectF(c - QPointF(1.8, 1.8), QSizeF(3.6, 3.6)));
    } else if (name == "eye") {
        QPainterPath eye(QPointF(3.5, 12));
        eye.quadTo(12, 3.5, 20.5, 12);
        eye.quadTo(12, 20.5, 3.5, 12);
        p.drawPath(eye);
        p.drawEllipse(QPointF(12, 12), 2.5, 2.5);
    }
}
// Vector icon rendered on demand at the exact logical size and device pixel
// ratio requested (crisp at 100–200 % and on mixed-DPI moves); each rendered
// size is cached, so nothing is painted until an icon is actually shown.
class LineIconEngine : public QIconEngine {
    QString name;
    QColor ink, off;
    QHash<quint64, QPixmap> pixmaps;

  public:
    LineIconEngine(QString n, QColor i, QColor o) : name(std::move(n)), ink(i), off(o) {}
    QIconEngine *clone() const override { return new LineIconEngine(name, ink, off); }
    bool isNull() override { return false; }
    QString key() const override { return QStringLiteral("NovaLineIcon"); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(rect.topLeft());
        painter->scale(rect.width() / 24.0, rect.height() / 24.0);
        drawIcon(*painter, name, mode == QIcon::Disabled ? off : ink);
        painter->restore();
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        const QSize device = (QSizeF(size) * scale).toSize();
        const quint64 key = (quint64(device.width()) << 32) | (quint64(device.height()) << 8) |
                            (mode == QIcon::Disabled ? 1 : 0);
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
    // Icons are shared (QIcon is implicitly shared): one engine per name and ink,
    // so dozens of buttons and repeated theme applications share rendered sizes.
    static QHash<QString, QIcon> cache;
    const QColor off = disabledInk.isValid() ? disabledInk : QColor(ink.red(), ink.green(), ink.blue(), 95);
    const auto key = name + ink.name(QColor::HexArgb) + off.name(QColor::HexArgb);
    if (auto it = cache.constFind(key); it != cache.constEnd())
        return *it;
    QIcon icon(new LineIconEngine(name, ink, off));
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
