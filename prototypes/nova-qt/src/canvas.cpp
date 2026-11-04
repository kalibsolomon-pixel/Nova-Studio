#include "canvas.h"
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
namespace nova {
Canvas::Canvas(std::shared_ptr<Document> doc, Metrics *m, QWidget *parent)
    : QWidget(parent), document(std::move(doc)), metrics(m) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setMinimumSize(200, 150);
    zoom = std::clamp(384.0 / document->image.width(), 0.25, 24.0);
}
QPointF Canvas::origin() const {
    return QPointF(width() / 2.0, height() / 2.0) -
           QPointF(document->image.width() * zoom / 2, document->image.height() * zoom / 2) + pan;
}
void Canvas::paintEvent(QPaintEvent *) {
    QElapsedTimer timer;
    timer.start();
    QPainter p(this);
    p.fillRect(rect(), QColor(21, 23, 30));
    QRectF target(origin(), QSizeF(document->image.size()) * zoom);
    auto clip = target.intersected(rect());
    p.save();
    p.setClipRect(clip);
    for (int y = int(clip.top()); y < clip.bottom(); y += 12)
        for (int x = int(clip.left()); x < clip.right(); x += 12)
            p.fillRect(QRect(x, y, 12, 12),
                       ((x - int(clip.left())) / 12 + (y - int(clip.top())) / 12) % 2
                           ? QColor(53, 53, 53)
                           : QColor(43, 43, 43));
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(target, document->image);
    if (grid && zoom >= 6) {
        p.setPen(QPen(QColor(0, 0, 0, 65), 0.5));
        for (int x = 0; x <= document->image.width(); ++x)
            p.drawLine(QPointF(target.left() + x * zoom, target.top()),
                       QPointF(target.left() + x * zoom, target.bottom()));
        for (int y = 0; y <= document->image.height(); ++y)
            p.drawLine(QPointF(target.left(), target.top() + y * zoom),
                       QPointF(target.right(), target.top() + y * zoom));
    }
    p.restore();
    p.setPen(QColor(180, 184, 198));
    p.drawText(12, 20,
               QString("%1 × %2 RGBA  ·  %3%  ·  Wheel: zoom / Middle or right drag: pan")
                   .arg(document->image.width())
                   .arg(document->image.height())
                   .arg(int(zoom * 100)));
    metrics->record("canvas_paint_cpu", timer);
}
void Canvas::brush(QPointF position) {
    auto point = coordinate(position, origin(), zoom, document->image.size());
    if (!point) {
        lastPixel.reset();
        return;
    }
    QElapsedTimer t;
    t.start();
    document->draw(lastPixel.value_or(*point), *point, eraser ? qRgba(0, 0, 0, 0) : color);
    lastPixel = point;
    metrics->record("brush_event_cpu", t);
    if (changed)
        changed();
    update();
}
void Canvas::mousePressEvent(QMouseEvent *e) {
    setFocus();
    lastMouse = e->position();
    if (e->button() == Qt::LeftButton)
        brush(e->position());
}
void Canvas::mouseMoveEvent(QMouseEvent *e) {
    if (e->buttons() & Qt::LeftButton)
        brush(e->position());
    else if (e->buttons() & (Qt::MiddleButton | Qt::RightButton)) {
        pan += e->position() - lastMouse;
        update();
    }
    lastMouse = e->position();
}
void Canvas::mouseReleaseEvent(QMouseEvent *) {
    document->finish();
    lastPixel.reset();
    if (changed)
        changed();
}
void Canvas::wheelEvent(QWheelEvent *e) {
    double old = zoom;
    zoom = zoomBy(zoom, e->angleDelta().y() / 3.0);
    pan += (e->position() - QPointF(width() / 2.0, height() / 2.0) - pan) * (1.0 - zoom / old);
    update();
    e->accept();
}
} // namespace nova
