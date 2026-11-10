// SPDX-License-Identifier: GPL-3.0-or-later
#include "canvas.h"
#include "session.h"
#include "theme.h"
#include "tools.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QWheelEvent>
#include <cmath>
namespace nova {
namespace {
constexpr double Pi = 3.14159265358979323846;
QVector<QLineF> footprintOutline(const std::vector<ops::Dab> &dabs) {
    QSet<quint64> on;
    auto key = [](int x, int y) { return (quint64(quint32(x)) << 32) | quint32(y); };
    for (const auto &d : dabs)
        on.insert(key(d.offset.x(), d.offset.y()));
    QVector<QLineF> lines;
    for (const auto &d : dabs) {
        const int x = d.offset.x(), y = d.offset.y();
        if (!on.contains(key(x, y - 1)))
            lines.append(QLineF(x, y, x + 1, y));
        if (!on.contains(key(x, y + 1)))
            lines.append(QLineF(x, y + 1, x + 1, y + 1));
        if (!on.contains(key(x - 1, y)))
            lines.append(QLineF(x, y, x, y + 1));
        if (!on.contains(key(x + 1, y)))
            lines.append(QLineF(x + 1, y, x + 1, y + 1));
    }
    return lines;
}
QImage alphaMask(const QImage &rgba) {
    QImage m(rgba.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < rgba.height(); ++y) {
        auto s = rgba.constScanLine(y);
        auto o = m.scanLine(y);
        for (int x = 0; x < rgba.width(); ++x)
            o[x] = s[x * 4 + 3] ? 255 : 0;
    }
    return m;
}
} // namespace

Canvas::Canvas(std::shared_ptr<Document> doc, Metrics *m, QWidget *parent, ToolState *t)
    : QWidget(parent), document(std::move(doc)), metrics(m), tools(t) {
    if (!tools) {
        ownedTools = std::make_unique<ToolState>();
        tools = ownedTools.get();
    }
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setMinimumSize(200, 150);
    setAttribute(Qt::WA_InputMethodEnabled, false);
    zoom = std::clamp(384.0 / std::max(1, document->image.width()), 0.25, 24.0);
    ants.setInterval(220);
    ants.setObjectName("selectionAnts");
    QObject::connect(&ants, &QTimer::timeout, this, [this] {
        antsPhase = (antsPhase + 1) % 8;
        update();
    });
    QObject::connect(tools, &ToolState::commitRequested, this, [this] {
        if (floating.active && isVisible())
            commitFloating();
    });
    QObject::connect(tools, &ToolState::cancelRequested, this, [this] { cancelInteraction(); });
    QObject::connect(tools, &ToolState::transformChanged, this, [this] {
        if (!floating.active || tools->tool != Tool::Transform)
            return;
        floating.scaleX = tools->scaleX;
        floating.scaleY = tools->scaleY;
        floating.angle = tools->angle;
        floating.resampling = tools->resampling;
        refreshFloating();
    });
    QObject::connect(tools, &ToolState::resetTransformRequested, this, [this] {
        tools->scaleX = tools->scaleY = 100;
        tools->angle = 0;
        tools->transformFlipH = tools->transformFlipV = false;
        tools->touch();
        emit tools->transformChanged();
    });
    QObject::connect(tools, &ToolState::changed, this, [this] { update(); });
}
Canvas::~Canvas() = default;
bool Canvas::event(QEvent *event) {
    // Hidden tabs receive no paint event, so stopping only in paintEvent leaves
    // their selection timer alive indefinitely.
    if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate)
        ants.stop();
    return QWidget::event(event);
}
QPointF Canvas::origin() const {
    return QPointF(width() / 2.0, height() / 2.0) -
           QPointF(document->image.width() * zoom / 2, document->image.height() * zoom / 2) + pan;
}
QPointF Canvas::pixelPosition(QPointF w) const { return (w - origin()) / zoom; }
QPointF Canvas::widgetPosition(QPointF p) const { return origin() + p * zoom; }
QPoint Canvas::clampPixel(QPointF p) const {
    return {std::clamp(int(std::floor(p.x())), 0, document->image.width() - 1),
            std::clamp(int(std::floor(p.y())), 0, document->image.height() - 1)};
}
QUuid Canvas::layer() const { return document->activeLayer(); }
bool Canvas::run(const std::function<bool(QString *)> &command) {
    QString message;
    const bool ok = command(&message);
    if (!ok && error && !message.isEmpty())
        error(message);
    notify();
    return ok;
}
void Canvas::notify() {
    // A session emits changed() itself; standalone canvases use the callback.
    if (!session && changed)
        changed();
    update();
}
QImage Canvas::displayed() const {
    if (!playbackPreview.isNull()) return playbackPreview;
    if (!externalPreview.isNull() && externalPreview.size() == document->image.size())
        return externalPreview;
    if (!preview.isNull() && preview.size() == document->image.size())
        return preview;
    return document->image;
}
void Canvas::setExternalPreview(const QImage &composite) {
    externalPreview = composite;
    update();
}
void Canvas::clearExternalPreview() {
    externalPreview = {};
    update();
}
ops::Combine Canvas::combineFor(Qt::KeyboardModifiers m) const {
    const bool shift = m & Qt::ShiftModifier, alt = m & Qt::AltModifier;
    if (shift && alt)
        return ops::Combine::Intersect;
    if (shift)
        return ops::Combine::Add;
    if (alt)
        return ops::Combine::Subtract;
    return tools->selectMode;
}
QRect Canvas::dragRect(bool square) const {
    QPoint a = clampPixel(anchor), b = clampPixel(current);
    if (tools->snap > 1) {
        // Snap to grid cells: the rectangle covers whole cells containing both ends.
        const int n = tools->snap;
        auto lo = [n](int v) { return v / n * n; };
        const QPoint mn(std::min(a.x(), b.x()), std::min(a.y(), b.y())), mx(std::max(a.x(), b.x()), std::max(a.y(), b.y()));
        a = QPoint(lo(mn.x()), lo(mn.y()));
        b = QPoint(std::min(document->image.width() - 1, lo(mx.x()) + n - 1),
                   std::min(document->image.height() - 1, lo(mx.y()) + n - 1));
    }
    if (square) {
        const int n = std::max(std::abs(b.x() - a.x()), std::abs(b.y() - a.y()));
        b = QPoint(a.x() + (b.x() >= a.x() ? n : -n), a.y() + (b.y() >= a.y() ? n : -n));
    }
    return QRect(a, b).normalized(); // inclusive of both end pixels
}
bool Canvas::interactionActive() const { return drag != Drag::None || polygonOpen || floating.active; }

// ---------------------------------------------------------------- painting
void Canvas::paintEvent(QPaintEvent *) {
    QElapsedTimer timer;
    timer.start();
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base));
    const QImage &image = displayed();
    QRectF target(origin(), QSizeF(document->image.size()) * zoom);
    auto clip = target.intersected(rect());
    p.save();
    p.setClipRect(clip);
    // Anchor transparency to the texture, so a magnified editable pixel never
    // contains misleading checker subdivisions. Group whole pixels when tiny.
    // Colours come from the active theme (ui::Tokens::checkerDark/checkerLight).
    const auto &theme = ui::current();
    QImage checker(2, 2, QImage::Format_RGB32);
    checker.setPixel(0, 0, theme.checkerDark.rgb());
    checker.setPixel(1, 1, theme.checkerDark.rgb());
    checker.setPixel(1, 0, theme.checkerLight.rgb());
    checker.setPixel(0, 1, theme.checkerLight.rgb());
    QBrush transparency(checker);
    QTransform transform;
    const double cell = zoom * std::max(1.0, std::ceil(6.0 / zoom));
    transform.translate(target.left(), target.top());
    transform.scale(cell, cell);
    transparency.setTransform(transform);
    p.fillRect(target, transparency);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(target, image);
    if (playbackPreview.isNull()) paintOnions(p, target);
    if (grid && zoom >= 6) {
        p.setPen(QPen(theme.pixelGrid, 0.5));
        for (int x = std::max(0, int((clip.left() - target.left()) / zoom));
             x <= std::min(document->image.width(), int((clip.right() - target.left()) / zoom) + 1); ++x)
            p.drawLine(QPointF(target.left() + x * zoom, target.top()),
                       QPointF(target.left() + x * zoom, target.bottom()));
        for (int y = std::max(0, int((clip.top() - target.top()) / zoom));
             y <= std::min(document->image.height(), int((clip.bottom() - target.top()) / zoom) + 1); ++y)
            p.drawLine(QPointF(target.left(), target.top() + y * zoom),
                       QPointF(target.right(), target.top() + y * zoom));
    }
    p.restore();
    if (uvVisible) {
        p.save(); p.setClipRect(target.intersected(rect()));
        for (const auto &region : uvRegions) {
            const QRectF r(widgetPosition(region.bounds.topLeft()), widgetPosition(region.bounds.bottomRight()));
            const bool selected = uvFace == region.face;
            p.setPen(QPen(selected ? QColor(255,210,80) : QColor(80,220,240,180), selected ? 2 : 1));
            p.setBrush(selected ? QColor(255,210,80,25) : Qt::transparent); p.drawRect(r);
            if (selected && uvEditing) {
                p.setBrush(Qt::white);
                p.setPen(QPen(Qt::black, 1));
                for (auto point :
                     {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight(),
                      QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()),
                      QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y())})
                    p.drawRect(QRectF(point - QPointF(4, 4), QSizeF(8, 8)));
            }
            if (selected || (zoom >= 12 && r.width() > 70)) p.drawText(r.adjusted(3,2,-2,-2), Qt::AlignTop|Qt::AlignLeft,region.label);
        }
        p.restore();
    }
    const QColor accent = palette().color(QPalette::Highlight).lighter(170);
    auto toWidget = [&](const QLineF &l) {
        return QLineF(widgetPosition(l.p1()), widgetPosition(l.p2()));
    };
    auto dashedLines = [&](const QVector<QLineF> &lines) {
        // Two-tone marching ants: always visible over light and dark pixels.
        QVector<QLineF> mapped;
        mapped.reserve(lines.size());
        for (const auto &l : lines)
            mapped.append(toWidget(l));
        QPen white(Qt::white, 1);
        p.setPen(white);
        p.drawLines(mapped);
        QPen black(Qt::black, 1, Qt::CustomDashLine);
        black.setDashPattern({4, 4});
        black.setDashOffset(antsPhase);
        p.setPen(black);
        p.drawLines(mapped);
    };
    // Selection boundary on exact pixel edges.
    const bool showAnts = document->hasSelection() && !floating.active && playbackPreview.isNull();
    if (showAnts) {
        if (outlineKey != document->selection().cacheKey()) {
            outlineKey = document->selection().cacheKey();
            outlineCache = ops::maskOutline(document->selection());
        }
        dashedLines(outlineCache);
    }
    const bool animate = (showAnts || floating.active) && isVisible() && window()->isActiveWindow();
    if (animate && !ants.isActive())
        ants.start();
    else if (!animate && ants.isActive())
        ants.stop();
    // Floating selection bounds and transform handles.
    if (floating.active) {
        const QRectF b = floatingBounds();
        dashedLines({QLineF(b.topLeft(), b.topRight()), QLineF(b.topRight(), b.bottomRight()),
                     QLineF(b.bottomRight(), b.bottomLeft()), QLineF(b.bottomLeft(), b.topLeft())});
        if (tools->tool == Tool::Transform) {
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(Qt::white);
            for (auto c : {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight(),
                           QPointF(b.center().x(), b.top()), QPointF(b.center().x(), b.bottom()),
                           QPointF(b.left(), b.center().y()), QPointF(b.right(), b.center().y())})
                p.drawRect(QRectF(widgetPosition(c) - QPointF(4, 4), QSizeF(8, 8)));
            p.setBrush(Qt::NoBrush);
        }
    }
    // In-progress selection shapes.
    if (drag == Drag::Rect) {
        const QRect r = dragRect(pressModifiers & Qt::ControlModifier);
        const QRectF w(widgetPosition(r.topLeft()), widgetPosition(r.bottomRight() + QPoint(1, 1)));
        p.setPen(QPen(accent, 1, Qt::DashLine));
        if (tools->tool == Tool::EllipseSelect)
            p.drawEllipse(w);
        else
            p.drawRect(w);
    }
    if ((drag == Drag::Lasso || polygonOpen) && !path.isEmpty()) {
        QPolygonF poly;
        for (auto q : path)
            poly.append(widgetPosition(q));
        if (polygonOpen && hover)
            poly.append(widgetPosition(current));
        p.setPen(QPen(accent, 1, Qt::DashLine));
        p.drawPolyline(poly);
        for (auto q : std::as_const(poly))
            p.fillRect(QRectF(q - QPointF(2, 2), QSizeF(4, 4)), accent);
    }
    if (drag == Drag::Gradient) {
        p.setPen(QPen(accent, 1.5));
        p.drawLine(widgetPosition(anchor), widgetPosition(current));
    }
    // Symmetry guides for painting tools.
    if ((isPaintTool(tools->tool) || tools->tool == Tool::Line) && tools->symmetry != ops::Symmetry::None) {
        const QPointF c(document->image.width() / 2.0, document->image.height() / 2.0);
        QPen guide(QColor(255, 90, 190, 200), 1, Qt::DashLine);
        p.setPen(guide);
        const auto s = tools->symmetry;
        if (s == ops::Symmetry::Horizontal || s == ops::Symmetry::Both)
            p.drawLine(widgetPosition({c.x(), 0}), widgetPosition({c.x(), double(document->image.height())}));
        if (s == ops::Symmetry::Vertical || s == ops::Symmetry::Both)
            p.drawLine(widgetPosition({0, c.y()}), widgetPosition({double(document->image.width()), c.y()}));
        if (s == ops::Symmetry::Radial) {
            const double r = std::hypot(document->image.width(), document->image.height()) / 2;
            for (int k = 0; k < std::clamp(tools->radial, 2, 32); ++k) {
                const double t = -Pi / 2 + 2 * Pi * k / std::clamp(tools->radial, 2, 32);
                p.drawLine(widgetPosition(c), widgetPosition(c + QPointF(std::cos(t), std::sin(t)) * r));
            }
        }
    }
    // Brush footprint cursor, aligned to whole texture pixels at every zoom.
    if (hover && (isPaintTool(tools->tool) || tools->tool == Tool::Line) && drag != Drag::Pan) {
        const auto brush = tools->brush(tools->tool);
        QByteArray key;
        key.append(char(brush.shape)).append(QByteArray::number(brush.size)).append(':')
            .append(QByteArray::number(brush.height)).append(':').append(QByteArray::number(brush.hardness))
            .append(':').append(QByteArray::number(brush.stamp.cacheKey()));
        if (key != cursorKey) {
            cursorKey = key;
            cursorOutline = footprintOutline(ops::footprint(brush));
        }
        QVector<QLineF> lines;
        for (const auto &l : std::as_const(cursorOutline))
            lines.append(l.translated(*hover));
        QVector<QLineF> mapped;
        for (const auto &l : lines)
            mapped.append(toWidget(l));
        p.setPen(QPen(Qt::black, 2));
        p.drawLines(mapped);
        p.setPen(QPen(Qt::white, 1));
        p.drawLines(mapped);
    }
    p.setPen(palette().color(QPalette::Text));
    QString status = QString("%1 × %2 RGBA  ·  %3%")
                         .arg(document->image.width())
                         .arg(document->image.height())
                         .arg(int(zoom * 100));
    if (floating.active)
        status += " · " + floating.label;
    if (hover && document->image.rect().contains(*hover))
        status += QString("  ·  %1, %2  %3")
                      .arg(hover->x())
                      .arg(hover->y())
                      .arg(ops::colorName(document->image.pixel(*hover)));
    if (document->hasSelection() && !floating.active) {
        const auto b = ops::maskBounds(document->selection());
        status += QString("  ·  selection %1×%2").arg(b.width()).arg(b.height());
    }
    if (document->animated()) status += playbackPreview.isNull() ? " · Editing frame" : " · Playback preview (read only)";
    p.drawText(12, 20, status);
    metrics->record("canvas_paint_cpu", timer);
}

// ---------------------------------------------------------------- pointer
void Canvas::paintAt(QPointF position, bool first) {
    QPointF target = position;
    if (tools->smoothing > 0 && !first) {
        // Exponential smoothing in pixel space; deterministic for a given input.
        const double k = 1.0 - tools->smoothing / 100.0 * 0.85;
        smoothed += (position - smoothed) * k;
        target = smoothed;
    } else {
        smoothed = position;
    }
    const QSize s = document->image.size();
    const QPoint pixel(std::clamp(int(std::floor(target.x())), -255, s.width() + 254),
                       std::clamp(int(std::floor(target.y())), -255, s.height() + 254));
    QElapsedTimer t;
    t.start();
    const auto style = tools->strokeStyle(tools->tool);
    const QPoint from = first && (pressModifiers & Qt::ShiftModifier) && strokeEnd ? *strokeEnd
                                                                                   : lastPixel.value_or(pixel);
    run([&](QString *e) {
        return session ? session->stroke(document, layer(), from, pixel, style, false, e)
                       : document->stroke(layer(), from, pixel, style, e);
    });
    lastPixel = pixel;
    metrics->record("brush_event_cpu", t);
}
void Canvas::pickColor(QPoint pixel, bool background) {
    if (!document->image.rect().contains(pixel))
        return;
    const auto l = document->layer(layer());
    const QRgb c = tools->sampleComposite || !l ? document->image.pixel(pixel) : l->pixels.pixel(pixel);
    if (background)
        tools->setBackground(c);
    else
        tools->setForeground(c);
}
void Canvas::mousePressEvent(QMouseEvent *e) {
    if (uvVisible && e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier) && (e->modifiers() & Qt::ControlModifier)) {
        const auto pixel = pixelPosition(e->position());
        QVector<int> hits;
        for (const auto &region : uvRegions) if (region.bounds.contains(pixel)) hits << region.face;
        if (!hits.isEmpty() && uvSelected) { const int next = (int(hits.indexOf(uvFace)) + 1) % int(hits.size()); uvSelected(hits[next]); }
        e->accept(); return;
    }
    if (uvEditing && e->button() == Qt::LeftButton && !spaceHeld) {
        setFocus();
        uvHandle = -1;
        uvDragging = false;
        for (const auto &region : uvRegions)
            if (region.face == uvFace) {
                uvOriginal = region.bounds;
                const auto r = QRectF(widgetPosition(region.bounds.topLeft()),
                                      widgetPosition(region.bounds.bottomRight()));
                const QPointF points[] = {r.topLeft(),
                                          r.topRight(),
                                          r.bottomLeft(),
                                          r.bottomRight(),
                                          {r.center().x(), r.top()},
                                          {r.center().x(), r.bottom()},
                                          {r.left(), r.center().y()},
                                          {r.right(), r.center().y()}};
                for (int i = 0; i < 8; ++i)
                    if (QLineF(points[i], e->position()).length() <= 7) {
                        uvHandle = i;
                        break;
                    }
                uvDragging = uvHandle >= 0 || r.contains(e->position());
                break;
            }
        uvAnchor = pixelPosition(e->position());
        e->accept();
        return;
    }
    if (!playbackPreview.isNull() && e->button() == Qt::LeftButton) return;
    setFocus();
    lastMouse = pressMouse = e->position();
    pressModifiers = e->modifiers();
    moved = false;
    anchor = current = pixelPosition(e->position());
    const QPoint pixel = QPoint(int(std::floor(anchor.x())), int(std::floor(anchor.y())));
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && spaceHeld)) {
        drag = Drag::Pan;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() == Qt::RightButton) {
        if (tools->tool == Tool::Eyedropper) {
            pickColor(pixel, true);
            return;
        }
        drag = Drag::Pan; // right-drag pans; a click without movement opens the context menu
        return;
    }
    if (e->button() != Qt::LeftButton || drag != Drag::None)
        return;
    const Tool t = tools->tool;
    if ((isPaintTool(t) || t == Tool::Line || t == Tool::Shape) && (e->modifiers() & Qt::AltModifier) &&
        !(e->modifiers() & Qt::ShiftModifier) && t != Tool::Shape) {
        pickColor(pixel, false); // Alt-click: temporary eyedropper
        return;
    }
    if (floating.active && t != Tool::Move && t != Tool::Transform)
        commitFloating();
    switch (t) {
    case Tool::Pencil:
    case Tool::Brush:
    case Tool::Eraser:
    case Tool::ColorReplace:
        drag = Drag::Paint;
        lastPixel.reset();
        paintAt(anchor, true);
        break;
    case Tool::RectSelect:
    case Tool::EllipseSelect:
        drag = Drag::Rect;
        break;
    case Tool::Lasso:
        drag = Drag::Lasso;
        path = {anchor};
        break;
    case Tool::PolygonLasso:
        if (!polygonOpen) {
            polygonOpen = true;
            path.clear();
        }
        path.append(QPointF(std::round(anchor.x()), std::round(anchor.y())));
        break;
    case Tool::MagicWand:
    case Tool::ColorRange: {
        if (!document->image.rect().contains(pixel))
            break;
        const auto l = document->layer(layer());
        const QImage &source = tools->sampleComposite || !l ? document->image : l->pixels;
        const auto mask = ops::colorMask(source, pixel, tools->tolerance, tools->metric, t == Tool::MagicWand,
                                         tools->includeAlpha);
        const auto mode = combineFor(e->modifiers());
        run([&](QString *err) {
            return session ? session->select(document, mask, mode, toolInfo(t).name, err)
                           : document->select(mask, mode, toolInfo(t).name, err);
        });
        break;
    }
    case Tool::Move:
    case Tool::Transform: {
        if (floating.active) {
            const int h = t == Tool::Transform ? handleAt(e->position()) : -1;
            if (h >= 0) {
                drag = h == 8 ? Drag::Rotate : Drag::Scale;
                startScaleX = floating.scaleX;
                startScaleY = floating.scaleY;
                startAngle = floating.angle;
                setProperty("handle", h);
                break;
            }
            if (!floatingBounds().contains(anchor) && t == Tool::Move) {
                commitFloating();
                break;
            }
        } else {
            startLift();
            if (!floating.active)
                break;
        }
        drag = Drag::Move;
        moveStartOrigin = floating.origin;
        break;
    }
    case Tool::Line:
        drag = Drag::Line;
        updateShapePreview();
        break;
    case Tool::Shape:
        if (tools->shape == ops::ShapeKind::Polygon) {
            if (!polygonOpen) {
                polygonOpen = true;
                path.clear();
            }
            path.append(QPointF(std::floor(anchor.x()) + .5, std::floor(anchor.y()) + .5));
            updateShapePreview();
        } else {
            drag = Drag::Shape;
            updateShapePreview();
        }
        break;
    case Tool::Fill:
        if (document->image.rect().contains(pixel) && !tools->contiguous)
            run([&](QString *err) {
                return session ? session->universalFill(document, layer(), pixel, tools->foreground, tools->tolerance,
                                                        tools->sampleComposite, tools->fillOpacity, err)
                               : document->universalFill(layer(), pixel, tools->foreground, tools->tolerance,
                                                         tools->sampleComposite, tools->fillOpacity, err);
            });
        else if (document->image.rect().contains(pixel))
            run([&](QString *err) {
                return session ? session->fill(document, layer(), pixel, tools->foreground, tools->tolerance,
                                               tools->metric, tools->contiguous, tools->sampleComposite,
                                               tools->fillOpacity, err)
                               : document->fill(layer(), pixel, tools->foreground, tools->tolerance, tools->metric,
                                                tools->contiguous, tools->sampleComposite, tools->fillOpacity, err);
            });
        break;
    case Tool::Gradient:
        drag = Drag::Gradient;
        updateShapePreview();
        break;
    case Tool::Eyedropper:
        pickColor(pixel, e->modifiers() & Qt::ShiftModifier);
        break;
    }
    update();
}
void Canvas::mouseMoveEvent(QMouseEvent *e) {
    const QPointF pos = pixelPosition(e->position());
    if (uvDragging) {
        auto delta = pos - uvAnchor;
        if (uvSnap)
            delta = QPointF(std::round(delta.x()), std::round(delta.y()));
        QRectF r = uvOriginal;
        if (uvHandle < 0)
            r.translate(delta);
        else {
            if (uvHandle == 0 || uvHandle == 2 || uvHandle == 6)
                r.setLeft(r.left() + delta.x());
            if (uvHandle == 1 || uvHandle == 3 || uvHandle == 7)
                r.setRight(r.right() + delta.x());
            if (uvHandle == 0 || uvHandle == 1 || uvHandle == 4)
                r.setTop(r.top() + delta.y());
            if (uvHandle == 2 || uvHandle == 3 || uvHandle == 5)
                r.setBottom(r.bottom() + delta.y());
            if ((uvAspect || e->modifiers() & Qt::ShiftModifier) && uvOriginal.height() > 0 &&
                uvOriginal.width() > 0) {
                const double ratio = uvOriginal.width() / uvOriginal.height();
                if (uvHandle == 4 || uvHandle == 5)
                    r.setWidth(r.height() * ratio);
                else
                    r.setHeight(r.width() / ratio);
            }
        }
        if (uvChanged)
            uvChanged(r.normalized());
        e->accept();
        return;
    }
    hover = QPoint(int(std::floor(pos.x())), int(std::floor(pos.y())));
    if ((e->position() - pressMouse).manhattanLength() > 3)
        moved = true;
    switch (drag) {
    case Drag::Pan:
        pan += e->position() - lastMouse;
        break;
    case Drag::Paint:
        if (e->buttons() & Qt::LeftButton)
            paintAt(pos, false);
        break;
    case Drag::Rect:
        current = pos;
        break;
    case Drag::Lasso:
        if (path.isEmpty() || QLineF(path.last(), pos).length() >= 0.35)
            path.append(pos);
        break;
    case Drag::Move: {
        current = pos;
        const QPointF d = current - anchor;
        floating.origin = moveStartOrigin + QPointF(std::round(d.x()), std::round(d.y()));
        refreshFloating();
        break;
    }
    case Drag::Scale: {
        current = pos;
        const QRectF b = floatingBounds();
        const QPointF c = b.center();
        const QPointF from = anchor - c, to = current - c;
        const double angle = -floating.angle * Pi / 180;
        auto local = [&](QPointF v) {
            return QPointF(v.x() * std::cos(angle) - v.y() * std::sin(angle),
                           v.x() * std::sin(angle) + v.y() * std::cos(angle));
        };
        const QPointF lf = local(from), lt = local(to);
        const int h = property("handle").toInt();
        const bool horizontal = h != 4 && h != 5, vertical = h != 6 && h != 7;
        double kx = horizontal && std::abs(lf.x()) > .5 ? lt.x() / lf.x() : 1;
        double ky = vertical && std::abs(lf.y()) > .5 ? lt.y() / lf.y() : 1;
        if (e->modifiers() & Qt::ShiftModifier)
            kx = ky = h >= 4 ? (horizontal ? kx : ky) : std::max(std::abs(kx), std::abs(ky));
        tools->scaleX = std::clamp(int(std::lround(startScaleX * std::abs(kx))), 1, 1600);
        tools->scaleY = std::clamp(int(std::lround(startScaleY * std::abs(ky))), 1, 1600);
        tools->touch();
        emit tools->transformChanged();
        break;
    }
    case Drag::Rotate: {
        current = pos;
        const QPointF c = floatingBounds().center();
        const double a0 = std::atan2(anchor.y() - c.y(), anchor.x() - c.x()),
                     a1 = std::atan2(current.y() - c.y(), current.x() - c.x());
        double angle = startAngle + (a1 - a0) * 180 / Pi;
        if (e->modifiers() & Qt::ShiftModifier)
            angle = std::round(angle / 15) * 15;
        tools->angle = std::remainder(angle, 360.0);
        tools->touch();
        emit tools->transformChanged();
        break;
    }
    case Drag::Line:
    case Drag::Shape:
    case Drag::Gradient:
        current = pos;
        updateShapePreview();
        break;
    case Drag::None:
        current = pos;
        if (polygonOpen)
            update();
        break;
    }
    lastMouse = e->position();
    update();
}
void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (uvDragging) {
        uvDragging = false;
        e->accept();
        return;
    }
    const Drag finished = drag;
    drag = Drag::None;
    setCursor(Qt::CrossCursor);
    if (finished == Drag::Pan && e->button() == Qt::RightButton && !moved && contextMenu) {
        contextMenu(e->globalPosition().toPoint());
        return;
    }
    switch (finished) {
    case Drag::Paint:
        strokeEnd = lastPixel;
        lastPixel.reset();
        if (session)
            session->finishStroke(document);
        else
            document->finish();
        notify();
        break;
    case Drag::Rect:
    case Drag::Lasso:
        finishSelection();
        break;
    case Drag::Line: {
        auto style = tools->strokeStyle(Tool::Line);
        const QPoint a = clampPixel(anchor), b = clampPixel(current);
        QPoint end = b;
        if (pressModifiers & Qt::ShiftModifier || e->modifiers() & Qt::ShiftModifier) {
            // Snap to 0/45/90°.
            const QPoint d = b - a;
            const double t = std::round(std::atan2(d.y(), d.x()) / (Pi / 4)) * (Pi / 4);
            const double len = std::max(std::abs(d.x()), std::abs(d.y()));
            end = a + QPoint(int(std::lround(std::cos(t) * len)), int(std::lround(std::sin(t) * len)));
        }
        preview = {};
        run([&](QString *err) {
            return session ? session->stroke(document, layer(), a, end, style, true, err)
                           : (document->stroke(layer(), a, end, style, err) ? (document->finish(), true) : false);
        });
        break;
    }
    case Drag::Shape: {
        const auto spec = currentShape();
        preview = {};
        if (!spec.bounds.isEmpty())
            run([&](QString *err) {
                return session ? session->shape(document, layer(), spec, tools->foreground, tools->shapeOpacity, false, err)
                               : document->drawShape(layer(), spec, tools->foreground, tools->shapeOpacity, false, err);
            });
        break;
    }
    case Drag::Gradient: {
        preview = {};
        if (QLineF(anchor, current).length() < .5)
            break;
        const ops::Spec spec{"gradient", {{"x0", anchor.x()}, {"y0", anchor.y()}, {"x1", current.x()},
                                          {"y1", current.y()}, {"color_a", ops::colorName(tools->foreground)},
                                          {"color_b", ops::colorName(tools->background)},
                                          {"type", tools->gradientType}, {"steps", tools->gradientSteps},
                                          {"dither", tools->gradientDither ? "Ordered (Bayer 4×4)" : "None"},
                                          {"opacity", tools->fillOpacity}}};
        run([&](QString *err) {
            return session ? session->apply(document, layer(), spec, err) : document->applyOperation(layer(), spec, err);
        });
        break;
    }
    default:
        break;
    }
    update();
}
void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
    if (polygonOpen) {
        // The second click of the double-click already added a duplicate point.
        if (path.size() > 1 && QLineF(path.last(), path[path.size() - 2]).length() < 1)
            path.removeLast();
        if (tools->tool == Tool::PolygonLasso)
            finishSelection();
        else if (tools->tool == Tool::Shape) {
            polygonOpen = false;
            const auto spec = currentShape();
            path.clear();
            preview = {};
            if (spec.points.size() >= 3)
                run([&](QString *err) {
                    return session ? session->shape(document, layer(), spec, tools->foreground, tools->shapeOpacity, false, err)
                                   : document->drawShape(layer(), spec, tools->foreground, tools->shapeOpacity, false, err);
                });
        }
        update();
        return;
    }
    if (floating.active && e->button() == Qt::LeftButton)
        commitFloating();
}
void Canvas::wheelEvent(QWheelEvent *e) {
    double old = zoom;
    zoom = zoomBy(zoom, e->angleDelta().y() / 3.0);
    pan += (e->position() - QPointF(width() / 2.0, height() / 2.0) - pan) * (1.0 - zoom / old);
    update();
    e->accept();
}
void Canvas::leaveEvent(QEvent *) {
    hover.reset();
    update();
}
void Canvas::keyPressEvent(QKeyEvent *e) {
    if (uvEditing) {
        if (e->key() == Qt::Key_Escape) {
            uvDragging = false;
            if (uvCancel)
                uvCancel();
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            if (uvCommit)
                uvCommit();
            return;
        }
        QPointF delta;
        const double step = e->modifiers() & Qt::ShiftModifier ? 10 : 1;
        if (e->key() == Qt::Key_Left)
            delta = {-step, 0};
        if (e->key() == Qt::Key_Right)
            delta = {step, 0};
        if (e->key() == Qt::Key_Up)
            delta = {0, -step};
        if (e->key() == Qt::Key_Down)
            delta = {0, step};
        if (!delta.isNull()) {
            for (const auto &r : uvRegions)
                if (r.face == uvFace) {
                    if (uvChanged)
                        uvChanged(r.bounds.translated(delta));
                    break;
                }
            return;
        }
    }
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        spaceHeld = true;
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        cancelInteraction();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (polygonOpen) {
            if (tools->tool == Tool::PolygonLasso)
                finishSelection();
            else
                mouseDoubleClickEvent(nullptr);
        } else if (floating.active)
            commitFloating();
        return;
    }
    if (e->key() == Qt::Key_Backspace && polygonOpen && !path.isEmpty()) {
        path.removeLast();
        if (path.isEmpty())
            polygonOpen = false;
        updateShapePreview();
        update();
        return;
    }
    const int step = e->modifiers() & Qt::ShiftModifier ? 10 : 1;
    QPointF d;
    if (e->key() == Qt::Key_Left)
        d = {-double(step), 0};
    else if (e->key() == Qt::Key_Right)
        d = {double(step), 0};
    else if (e->key() == Qt::Key_Up)
        d = {0, -double(step)};
    else if (e->key() == Qt::Key_Down)
        d = {0, double(step)};
    if (!d.isNull() && (tools->tool == Tool::Move || tools->tool == Tool::Transform || floating.active)) {
        if (!floating.active)
            startLift();
        moveFloating(d);
        return;
    }
    QWidget::keyPressEvent(e);
}
void Canvas::keyReleaseEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        spaceHeld = false;
        setCursor(Qt::CrossCursor);
        return;
    }
    QWidget::keyReleaseEvent(e);
}
void Canvas::cancelInteraction() {
    if (drag == Drag::Paint) {
        if (session)
            session->cancelStroke(document);
        else
            document->cancelStroke();
        lastPixel.reset();
    }
    drag = Drag::None;
    polygonOpen = false;
    path.clear();
    preview = {};
    if (floating.active)
        cancelFloating();
    notify();
}

// ---------------------------------------------------------------- selection
void Canvas::finishSelection() {
    const auto size = document->image.size();
    const auto mode = combineFor(pressModifiers);
    QImage mask;
    QString label;
    if (tools->tool == Tool::Lasso || tools->tool == Tool::PolygonLasso) {
        polygonOpen = false;
        if (path.size() >= 3)
            mask = ops::polygonMask(size, path);
        label = toolInfo(tools->tool).name;
        path.clear();
        if (mask.isNull())
            return;
    } else {
        if (!moved && mode == ops::Combine::Replace) {
            run([&](QString *) {
                return session ? session->selection(document, DocumentSession::SelectionCommand::None)
                               : document->deselect();
            });
            return;
        }
        const QRect r = dragRect(pressModifiers & Qt::ControlModifier);
        mask = tools->tool == Tool::EllipseSelect ? ops::ellipseMask(size, r) : ops::rectMask(size, r);
        label = toolInfo(tools->tool).name;
    }
    run([&](QString *err) {
        return session ? session->select(document, mask, mode, label, err) : document->select(mask, mode, label, err);
    });
}

// ---------------------------------------------------------------- floating
void Canvas::startLift() {
    if (transformLayers) {
        const auto ids = transformLayers();
        if (ids.size() > 1 || (tools->transformHidden && !ids.isEmpty())) {
            beginLayerTransform(ids, tools->transformHidden);
            return;
        }
    }
    const auto l = document->layer(layer());
    if (!l) {
        if (error)
            error("Select a layer first");
        return;
    }
    if (!l->visible) {
        if (error)
            error("Show the selected layer before moving its pixels");
        return;
    }
    const QImage lift = document->hasSelection() ? document->selection() : ops::fullMask(document->image.size());
    QRect bounds = ops::maskBounds(lift);
    if (bounds.isEmpty()) {
        if (error)
            error("The selection contains no pixels to move; deselect to edit the layer");
        return;
    }
    QImage content = document->selectedPixels(layer());
    if (!document->hasSelection()) {
        bounds = ops::alphaBounds(content, 0);
        if (bounds.isEmpty()) {
            if (error)
                error("The layer has no visible pixels to move");
            return;
        }
    }
    tools->scaleX = tools->scaleY = 100;
    tools->angle = 0;
    tools->touch();
    beginFloating(content.copy(bounds), bounds.topLeft(), lift,
                  tools->tool == Tool::Transform ? "Transform" : "Move");
}
void Canvas::beginFloating(const QImage &pixels, QPointF at, const QImage &lift, const QString &label) {
    floating = {};
    floating.active = true;
    floating.pixels = pixels.convertToFormat(QImage::Format_RGBA8888);
    floating.origin = QPointF(std::round(at.x()), std::round(at.y()));
    floating.lift = lift;
    floating.label = label;
    floating.scaleX = tools->scaleX;
    floating.scaleY = tools->scaleY;
    floating.angle = tools->angle;
    floating.resampling = tools->resampling;
    refreshFloating();
}
void Canvas::moveFloating(QPointF delta) {
    if (!floating.active)
        return;
    floating.origin += delta;
    refreshFloating();
}
QRectF Canvas::floatingBounds() const {
    if (!floating.active)
        return {};
    return QRectF(floating.placed, QSizeF(floating.cached.size()));
}
int Canvas::handleAt(QPointF widget) const {
    const QRectF b = floatingBounds();
    const QPointF pts[8] = {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight(),
                            QPointF(b.center().x(), b.top()), QPointF(b.center().x(), b.bottom()),
                            QPointF(b.left(), b.center().y()), QPointF(b.right(), b.center().y())};
    for (int i = 0; i < 8; ++i)
        if (QLineF(widgetPosition(pts[i]), widget).length() <= 7)
            return i;
    // Outside the box but near it: rotation.
    const QRectF w(widgetPosition(b.topLeft()), widgetPosition(b.bottomRight()));
    if (!w.contains(widget) && w.adjusted(-28, -28, 28, 28).contains(widget))
        return 8;
    return -1;
}
void Canvas::refreshFloating() {
    if (!floating.active)
        return;
    if (!floating.layers.isEmpty()) {
        refreshLayerFloating();
        return;
    }
    QByteArray key = QByteArray::number(floating.scaleX) + ',' +
                     QByteArray::number(floating.scaleY) + ',' +
                     QByteArray::number(floating.angle) + floating.resampling.toUtf8() +
                     char(tools->transformFlipH) + char(tools->transformFlipV);
    if (key != floating.cacheKey || floating.cached.isNull()) {
        floating.cacheKey = key;
        QImage img = floating.pixels;
        if (tools->transformFlipH || tools->transformFlipV)
            img = img.flipped((tools->transformFlipH ? Qt::Horizontal : Qt::Orientations{}) |
                              (tools->transformFlipV ? Qt::Vertical : Qt::Orientations{}));
        const int w = std::max(1, int(std::lround(img.width() * floating.scaleX / 100.0)));
        const int h = std::max(1, int(std::lround(img.height() * floating.scaleY / 100.0)));
        if (QSize(w, h) != img.size())
            img = ops::run({"resize_image", {{"width", std::min(w, 4096)}, {"height", std::min(h, 4096)},
                                             {"resampling", floating.resampling}}},
                           img)
                      .image;
        if (std::abs(std::remainder(floating.angle, 360.0)) > 1e-9 && !img.isNull()) {
            const int d = std::min(4096, int(std::ceil(std::hypot(img.width(), img.height()))) + 2);
            img = ops::run({"resize_canvas", {{"width", d}, {"height", d}}}, img).image;
            img = ops::run({"rotate", {{"angle", floating.angle}, {"resampling", floating.resampling}}}, img).image;
            const auto tight = ops::alphaBounds(img, 0);
            if (!tight.isEmpty()) {
                const QPointF shift = QPointF(tight.center()) - QPointF(img.width() / 2.0 - .5, img.height() / 2.0 - .5);
                img = img.copy(tight);
                setProperty("rotationShift", shift);
            }
        } else {
            setProperty("rotationShift", QPointF());
        }
        floating.cached = img;
    }
    // Keep the transformed content centred on the untransformed content centre.
    const QPointF centre = floating.origin + QPointF(floating.pixels.width(), floating.pixels.height()) / 2.0 +
                           property("rotationShift").toPointF();
    floating.placed = QPoint(int(std::lround(centre.x() - floating.cached.width() / 2.0)),
                             int(std::lround(centre.y() - floating.cached.height() / 2.0)));
    // Preview = active layer with the lifted hole plus the placed pixels.
    const auto l = document->layer(layer());
    if (!l)
        return;
    QImage base = l->pixels.copy();
    if (!floating.lift.isNull())
        for (int y = 0; y < base.height(); ++y) {
            auto row = base.scanLine(y);
            auto m = floating.lift.constScanLine(y);
            for (int x = 0; x < base.width(); ++x)
                row[x * 4 + 3] = uchar(row[x * 4 + 3] * (255 - m[x]) / 255);
        }
    for (int y = 0; y < floating.cached.height(); ++y)
        for (int x = 0; x < floating.cached.width(); ++x) {
            const QPoint q = floating.placed + QPoint(x, y);
            const QRgb s = floating.cached.pixel(x, y);
            if (qAlpha(s) && base.rect().contains(q))
                base.setPixel(q, ops::blend(base.pixel(q), s, 255));
        }
    preview = document->compositeWith(layer(), base);
    update();
}
bool Canvas::commitFloating(bool asNewLayer) {
    if (!floating.active)
        return false;
    if (!floating.layers.isEmpty()) {
        if (asNewLayer) {
            if (error)
                error("Commit selected layers in place to preserve their identities");
            return false;
        }
        return commitLayerFloating();
    }
    QImage placed(document->image.size(), QImage::Format_RGBA8888);
    placed.fill(Qt::transparent);
    for (int y = 0; y < floating.cached.height(); ++y)
        for (int x = 0; x < floating.cached.width(); ++x) {
            const QPoint q = floating.placed + QPoint(x, y);
            if (placed.rect().contains(q))
                placed.setPixel(q, floating.cached.pixel(x, y));
        }
    const QImage selection = alphaMask(placed);
    const QImage lift = floating.lift;
    const QString label = floating.label;
    auto pending = std::move(floating);
    const auto pendingPreview = preview;
    floating = {};
    preview = {};
    const bool ok = run([&](QString *err) {
        const QImage *liftPtr = lift.isNull() ? nullptr : &lift;
        return session ? session->commitFloating(document, layer(), placed, liftPtr, label, asNewLayer, err, &selection)
                       : document->commitFloating(layer(), placed, liftPtr, label, asNewLayer, err, &selection);
    });
    if (!ok) {
        floating = std::move(pending);
        preview = pendingPreview;
        update();
        return false;
    }
    tools->scaleX = tools->scaleY = 100;
    tools->angle = 0;
    tools->transformFlipH = tools->transformFlipV = false;
    tools->touch();
    return ok;
}
void Canvas::cancelFloating() {
    floating = {};
    preview = {};
    tools->scaleX = tools->scaleY = 100;
    tools->angle = 0;
    tools->transformFlipH = tools->transformFlipV = false;
    tools->touch();
    update();
}

// ---------------------------------------------------------------- previews
ops::ShapeSpec Canvas::currentShape() const {
    ops::ShapeSpec spec;
    spec.kind = tools->shape;
    spec.filled = tools->filled;
    spec.smooth = tools->smoothShape;
    spec.thickness = tools->thickness;
    spec.radius = tools->radius;
    spec.sides = tools->sides;
    spec.innerRatio = tools->innerRatio;
    spec.rotation = tools->rotation;
    spec.flipH = tools->flipH;
    spec.flipV = tools->flipV;
    if (spec.kind == ops::ShapeKind::Polygon) {
        spec.points = path;
        return spec;
    }
    QPointF a(std::floor(anchor.x()), std::floor(anchor.y())), b(std::floor(current.x()), std::floor(current.y()));
    if (tools->snap > 1) {
        const double n = tools->snap;
        a = QPointF(std::round(anchor.x() / n) * n, std::round(anchor.y() / n) * n);
        b = QPointF(std::round(current.x() / n) * n - 1, std::round(current.y() / n) * n - 1);
    }
    const bool square = pressModifiers & Qt::ShiftModifier;
    const bool centred = tools->fromCentre || (pressModifiers & Qt::AltModifier);
    QPointF d = b - a;
    if (square) {
        const double n = std::max(std::abs(d.x()), std::abs(d.y()));
        d = QPointF(d.x() < 0 ? -n : n, d.y() < 0 ? -n : n);
    }
    QRectF r = centred ? QRectF(a - d, a + d + QPointF(1, 1)) : QRectF(a, a + d);
    r = r.normalized();
    if (!centred)
        r.adjust(0, 0, 1, 1); // inclusive of the end pixel
    spec.bounds = r;
    return spec;
}
void Canvas::updateShapePreview() {
    const auto l = document->layer(layer());
    if (!l)
        return;
    const QImage *selection = document->hasSelection() ? &document->selection() : nullptr;
    QImage pixels;
    if (tools->tool == Tool::Line && drag == Drag::Line) {
        // Preview the exact stroke engine result on a scratch document.
        Document scratch(QString(), l->pixels);
        if (selection)
            scratch.setSelection(*selection);
        scratch.stroke(scratch.activeLayer(), clampPixel(anchor), clampPixel(current), tools->strokeStyle(Tool::Line));
        scratch.finish();
        pixels = scratch.layers()[0].pixels;
    } else if (tools->tool == Tool::Shape) {
        const auto spec = currentShape();
        if ((spec.kind == ops::ShapeKind::Polygon && spec.points.size() < 2) ||
            (spec.kind != ops::ShapeKind::Polygon && spec.bounds.isEmpty())) {
            preview = {};
            return;
        }
        auto shown = spec;
        if (shown.kind == ops::ShapeKind::Polygon && polygonOpen && hover)
            shown.points.append(QPointF(hover->x() + .5, hover->y() + .5));
        pixels = ops::paintCoverage(l->pixels, ops::rasterizeShape(document->image.size(), shown),
                                    tools->foreground, tools->shapeOpacity, false, selection);
    } else if (tools->tool == Tool::Gradient && drag == Drag::Gradient) {
        ops::Context ctx;
        ctx.mask = selection;
        pixels = ops::run({"gradient", {{"x0", anchor.x()}, {"y0", anchor.y()}, {"x1", current.x()}, {"y1", current.y()},
                                        {"color_a", ops::colorName(tools->foreground)},
                                        {"color_b", ops::colorName(tools->background)}, {"type", tools->gradientType},
                                        {"steps", tools->gradientSteps},
                                        {"dither", tools->gradientDither ? "Ordered (Bayer 4×4)" : "None"},
                                        {"opacity", tools->fillOpacity}}},
                          l->pixels, ctx)
                     .image;
    }
    preview = pixels.isNull() ? QImage() : document->compositeWith(layer(), pixels);
}
} // namespace nova
