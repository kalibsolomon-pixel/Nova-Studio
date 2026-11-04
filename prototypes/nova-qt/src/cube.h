#pragma once
#include "core.h"
#include "metrics.h"
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <memory>
namespace nova {
class Cube : public QOpenGLWidget, protected QOpenGLFunctions {
    std::unique_ptr<QOpenGLShaderProgram> program;
    void cleanup();
    QOpenGLBuffer buffer;
    GLuint texture = 0;
    QSize textureSize;
    bool fullUpload = true;
    QPointF last;
    float pitch = -25, yaw = 38, scale = 1;

  protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

  public:
    std::shared_ptr<Document> document;
    Metrics *metrics;
    explicit Cube(Metrics *m, QWidget *parent = nullptr);
    ~Cube() override;
    void setDocument(std::shared_ptr<Document> doc);
};
} // namespace nova
