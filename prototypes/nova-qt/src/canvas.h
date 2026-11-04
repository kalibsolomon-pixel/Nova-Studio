#pragma once
#include "core.h"
#include "metrics.h"
#include <QWidget>
#include <functional>
#include <memory>
namespace nova {
class Canvas : public QWidget {
    QPointF pan, lastMouse;
    std::optional<QPoint> lastPixel;
    QPointF origin() const;
    void brush(QPointF position);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

  public:
    std::shared_ptr<Document> document;
    double zoom = 16;
    QRgb color = qRgba(130, 141, 255, 255);
    bool eraser = false, grid = true;
    Metrics *metrics;
    std::function<void()> changed;
    explicit Canvas(std::shared_ptr<Document> doc, Metrics *metrics, QWidget *parent = nullptr);
    void center() {
        pan = {};
        update();
    }
};
} // namespace nova
