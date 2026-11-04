// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include "metrics.h"
#include "models.h"
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <memory>
namespace nova {
class Cube : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
    std::unique_ptr<QOpenGLShaderProgram> program;
    void cleanup();
    QOpenGLBuffer buffer, lineBuffer, guideBuffer;
    GLuint texture = 0;
    QSize textureSize;
    bool fullUpload = true;
    QImage playbackPreview;
    QPointF last;
    float pitch = 25, yaw = 38, scale = 1;
    QPointF pressed, cameraPan;
    std::shared_ptr<const mc::Mesh> modelMesh;
    mc::Mesh genericMesh;
    QString activeTexture;
    QMap<QString, QImage> materialImages;
    QMap<QString, GLuint> materialTextures;
    QMap<QString, QSize> materialSizes;
    QSet<QString> dirtyMaterials;
    bool materialPreview = false;
    struct AlphaInfo {
        qint64 key = 0;
        bool partial = false;
    };
    QHash<QString, AlphaInfo> alphaCache;
    bool geometryDirty = true, materialsDirty = true;
    QMatrix4x4 contextTransform;
    QMatrix4x4 matrix() const;
    float captureAspect = 0;
    const mc::Mesh &scene() const { return modelMesh ? *modelMesh : genericMesh; }
    int vertexCount = 0;
    void uploadGeometry();

  protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

  public:
    std::shared_ptr<Document> document;
    Metrics *metrics;
    explicit Cube(Metrics *m, QWidget *parent = nullptr);
    ~Cube() override;
    bool perspective = false, wireframe = false, outlines = false, axes = false, groundGrid = false,
         neutral = false;
    float ambient = .55f;
    QVector3D lightDirection{-.4f, .8f, .6f};
    int selectedFace = -1;
    // Display authoring: fixed camera (no fit/centring) plus an optional reference
    // (0 none, 1 untransformed block bounds, 2 GUI slot square).
    bool contextFraming = false, displayManipulation = false;
    int referenceFrame = 0;
    void setContextFraming(bool enabled, bool frontView);
    void setModel(std::shared_ptr<const mc::Mesh> mesh, const QString &editableTexture = {},
                  const QMap<QString, QImage> &materials = {});
    void setContextTransform(const QMatrix4x4 &transform) {
        contextTransform = transform;
        update();
    }
    void resetView();
    void setMaterialImage(const QString &asset, const QImage &image);
    void setMaterialPreview(bool value) {
        materialPreview = value;
        update();
    }
    QJsonObject viewSettings() const;
    QJsonObject resourceCounts() const;
    void restoreView(const QJsonObject &);
    void fitModel();
    QImage captureModel(bool comparison = false);
    int faceAt(QPointF position) const;
    const mc::Mesh &meshData() const { return scene(); }
    void setDocument(std::shared_ptr<Document> doc);
    void setPlaybackPreview(const QImage &pixels) {
        if (playbackPreview.cacheKey() == pixels.cacheKey())
            return;
        playbackPreview = pixels;
        fullUpload = true;
        update();
    }
  signals:
    void faceSelected(int face);
    void displayDragged(QPointF delta, Qt::KeyboardModifiers modifiers);
    void visibilityChanged(bool visible);
};
} // namespace nova
