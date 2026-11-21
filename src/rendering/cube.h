// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include "metrics.h"
#include "models.h"
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <map>
#include <memory>
class QLabel;
namespace nova {
class Cube : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
    std::unique_ptr<QOpenGLShaderProgram> program;
    void cleanup();
    QOpenGLBuffer buffer, lineBuffer, guideBuffer;
    // Attribute and uniform locations, looked up once after the program links so painting
    // never goes through string lookups.
    struct Locations {
        int position = -1, texcoord = -1, normal = -1, shade = -1, worldShade = -1;
        int mvp = -1, tex = -1, solid = -1, direction = -1, ambient = -1, neutral = -1,
            light0 = -1, light1 = -1, lightingMode = -1, twoSided = -1, alphaCutoff = -1,
            uvOrigin = -1, textureAlpha = -1, tintColor = -1, solidColor = -1;
    } loc;
    // How each face of the current mesh is drawn; depends only on the mesh and the preview
    // block, so it is classified once instead of on every frame.
    struct FacePass {
        bool blockSprite = false, blended = false;
        float cutoff = 0;
    };
    QVector<FacePass> passes;
    std::shared_ptr<const mc::Mesh> passOwner; // keeps passScene's address from being reused
    const mc::Mesh *passScene = nullptr;
    QString passBlock;
    bool passPlaced = false;
    void classifyFaces();
    // Capture targets by edge length, reused between captures (Explorer takes hundreds of
    // identical 64 px icons).
    std::map<int, std::unique_ptr<QOpenGLFramebufferObject>> captureTargets;
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
    QMap<QString, qint64> materialCacheKeys;
    QSet<QString> dirtyMaterials;
    bool materialPreview = false;
    bool geometryDirty = true, materialsDirty = true;
    QMatrix4x4 contextTransform;
    QString previewContext, previewBlock;
    bool placedBlock = false, oversizedInGui = false, inventoryCapture = false;
    QRect guiSlot(const QSize &viewport) const;
    QMatrix4x4 matrix(bool model = true) const;
    QImage captureFrame(int size, bool comparison, bool inventory);
    int captureSize = 0;
    QString noticeText;
    QLabel *noticeLabel = nullptr; // child widget: shown on screen and in grabs
    void placeNotice();
    QLabel *emptyLabel = nullptr; // "nothing to preview" state, also a child widget
    void updateEmptyState();
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
    void resizeEvent(QResizeEvent *) override;
    void changeEvent(QEvent *) override;

  public:
    std::shared_ptr<Document> document;
    Metrics *metrics;
    explicit Cube(Metrics *m, QWidget *parent = nullptr);
    ~Cube() override;
    bool perspective = false, wireframe = false, outlines = false, axes = false, groundGrid = false,
         neutral = false, studioLighting = false;
    float ambient = .55f;
    QVector3D lightDirection{-.4f, .8f, .6f};
    int selectedFace = -1;
    // Display authoring: fixed camera (no fit/centring) plus an optional reference
    // (0 none, 1 untransformed block bounds, 2 GUI slot square).
    bool contextFraming = false, displayManipulation = false;
    int referenceFrame = 0;
    void setContextFraming(bool enabled, bool frontView);
    void setPreviewContext(const QString &context, bool placed, const QString &blockId = {},
                           bool oversized = false) {
        previewContext = context;
        previewBlock = blockId;
        placedBlock = placed;
        oversizedInGui = oversized;
        update();
    }
    // Banner over the view saying what is shown when it is not the texture's resolved
    // Minecraft model (texture-only fallback, explicit generic cube, unresolved model).
    // Empty clears it. The banner takes at most a quarter of the view's height (never
    // less than one line), so the fallback stays visible at any size or scale; text that
    // does not fit is elided. The full text is the banner's accessible name, the view's
    // accessible description and tooltip, and the Models / UV diagnostics.
    void setNotice(const QString &text);
    QString notice() const { return noticeText; }
    // Lines of the banner for a text width and line budget: wrapped at words (anywhere in
    // over-long words), the last line elided with an ellipsis when text remains.
    static QStringList noticeLines(const QString &text, const QFont &font, int width, int maxLines);
    static constexpr double NoticeMaxShare = 0.25;
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
    QImage captureInventoryIcon(int scale = 1);
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
