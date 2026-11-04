// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include "metrics.h"
#include <QTimer>
#include <QCache>
#include <QWidget>
#include <functional>
#include <memory>
namespace nova {
class DocumentSession;
class ToolState;
enum class Tool;
// Pixel canvas and tool controller: converts pointer/keyboard input into
// DocumentSession commands. Previews (strokes in progress excepted) are
// separate composites; authoritative pixels change only on commit.
class Canvas : public QWidget {
  public:
    struct UvRegion { int face = -1; QRectF bounds; QString label; };
    QVector<UvRegion> uvRegions;
    bool uvVisible = false;
    int uvFace = -1;
    std::function<void(int)> uvSelected;
    bool uvEditing = false, uvSnap = true, uvAspect = false;
    std::function<void(QRectF)> uvChanged;
    std::function<void()> uvCommit, uvCancel;
    std::function<void(bool)> uvUndo;
    struct Floating {
        struct GroupLayer {
            QUuid id;
            QImage original, cached;
        };
        QVector<GroupLayer> layers;
        QMap<QUuid, QImage> replacements;
        bool includeHidden = false, valid = true;
        quint64 sourceSerial = 0;
        bool active = false;
        QImage pixels;   // untransformed content (cropped)
        QPointF origin;  // top-left of the untransformed content in document pixels
        QImage lift;     // pixels removed from the source layer on commit (null for paste)
        QString label;
        int scaleX = 100, scaleY = 100;
        double angle = 0;
        QString resampling = "Nearest neighbour";
        QImage cached;   // transformed pixels
        QPoint placed;   // top-left of cached pixels
        QByteArray cacheKey;
    };

  private:
    QPointF pan, lastMouse, pressMouse;
    bool uvDragging = false;
    int uvHandle = -1;
    QPointF uvAnchor;
    QRectF uvOriginal;
    std::optional<QPoint> lastPixel, strokeEnd;
    QPointF smoothed;
    enum class Drag { None, Paint, Pan, Rect, Lasso, Move, Scale, Rotate, Line, Shape, Gradient };
    Drag drag = Drag::None;
    Qt::KeyboardModifiers pressModifiers;
    QPointF anchor, current; // document pixel coordinates (float)
    QVector<QPointF> path;   // lasso / polygon points
    bool polygonOpen = false, spaceHeld = false, moved = false;
    QPointF moveStartOrigin;
    int startScaleX = 100, startScaleY = 100;
    double startAngle = 0;
    Floating floating;
    QImage preview;          // composite shown instead of the document when set
    QImage externalPreview;  // operation-dialog preview
    QImage playbackPreview;
    mutable QCache<QString, QImage> onionCache{4 * 1024 * 1024};
    void paintOnions(QPainter &painter, const QRectF &target);
    QTimer ants;
    int antsPhase = 0;
    qint64 outlineKey = -1;
    QVector<QLineF> outlineCache;
    QByteArray cursorKey;
    QVector<QLineF> cursorOutline;
    std::optional<QPoint> hover;
    std::unique_ptr<ToolState> ownedTools;
    QPointF origin() const;
    QPoint clampPixel(QPointF p) const;
    QRect dragRect(bool square) const;
    ops::Combine combineFor(Qt::KeyboardModifiers m) const;
    QUuid layer() const;
    bool run(const std::function<bool(QString *)> &command);
    void paintAt(QPointF position, bool first);
    void pickColor(QPoint pixel, bool background);
    void finishSelection();
    void startLift();
    void refreshFloating();
    void refreshLayerFloating();
    bool commitLayerFloating();
    QRectF floatingBounds() const;
    int handleAt(QPointF widget) const;
    void updateShapePreview();
    ops::ShapeSpec currentShape() const;
    void notify();

  protected:
    bool event(QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void leaveEvent(QEvent *) override;

  public:
    struct OnionSettings { bool enabled = false; int previous = 1, next = 1, opacity = 25; bool tint = true; } onion;
    void setPlaybackPreview(const QImage &pixels) { playbackPreview = pixels; update(); }
    bool playbackVisible() const { return !playbackPreview.isNull(); }
    bool selectionTimerActive() const { return ants.isActive(); }
    std::shared_ptr<Document> document;
    double zoom = 16;
    bool grid = true;
    Metrics *metrics;
    ToolState *tools;
    DocumentSession *session = nullptr;
    std::function<void()> changed;
    std::function<void(const QString &)> error;
    std::function<void(QPoint globalPosition)> contextMenu;
    std::function<QVector<QUuid>()> transformLayers;
    explicit Canvas(std::shared_ptr<Document> doc, Metrics *metrics, QWidget *parent = nullptr,
                    ToolState *tools = nullptr);
    ~Canvas() override;
    void center() {
        pan = {};
        update();
    }
    QPointF pixelPosition(QPointF widget) const;
    QPointF widgetPosition(QPointF pixel) const;
    // Floating selections (paste / move / transform placement).
    bool hasFloating() const { return floating.active; }
    const Floating &floatingState() const { return floating; }
    void beginFloating(const QImage &pixels, QPointF origin, const QImage &lift, const QString &label);
    bool beginLayerTransform(const QVector<QUuid> &layers, bool includeHidden = false);
    void moveFloating(QPointF delta);
    bool commitFloating(bool asNewLayer = false);
    void cancelFloating();
    void cancelInteraction();
    bool interactionActive() const;
    void setExternalPreview(const QImage &composite);
    void clearExternalPreview();
    QImage displayed() const;
};
} // namespace nova
