// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cube.h"
#include "edit.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSpinBox>
namespace nova {
class ElementHandles : public QWidget {
    mc::ElementTransform value, initial;
    QPointF pressed;
    bool dragging = false;
    QRectF box() const;
    QPointF project(QVector3D) const;
    double unit() const;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

  public:
    int plane = 0, mode = 0; // XY, XZ, YZ; move, resize, rotate
    double snap = 1;
    std::function<void(const mc::ElementTransform &)> changed;
    explicit ElementHandles(QWidget *parent = nullptr);
    void setTransform(const mc::ElementTransform &v) {
        value = v;
        update();
    }
};
class ModelAuthorDialog : public QDialog {
    std::shared_ptr<mc::ModelEdit> edit;
    std::shared_ptr<mc::Library> library;
    QString root, asset;
    QListWidget *elements, *directions;
    QLineEdit *label, *parentId, *bindingAlias, *bindingResource;
    QComboBox *texture, *cull, *light, *rotation;
    QDoubleSpinBox *numbers[12]{}, *uv[4]{};
    QSpinBox *tint;
    QCheckBox *rotate, *rescale, *ao, *isolate;
    QLabel *status, *textureInfo, *texturePreview;
    ElementHandles *handles;
    Cube *preview;
    Metrics metrics;
    QSet<int> hidden;
    QJsonObject clipboard;
    QMap<QString, QImage> images;
    qint64 imageBytes = 0;
    bool syncing = false;
    void refresh(int selected = -2);
    void selection();
    void showPreview();
    void transform();
    QStringList selectedFaces() const;
    bool result(bool ok, const QString &error);
    void saveAs();
    void checkpoint();
    void updateTextureInfo();
    // Bulk numeric UV (C8)
    static constexpr double MixedValue = -4097;
    QDoubleSpinBox *bulkValues[6]{};
    QCheckBox *bulkAspect = nullptr;
    QLabel *bulkSummary = nullptr;
    double bulkAspectRatio = 0;
    QJsonObject uvClipboard;
    QHash<QString, QSize> sizeCache;
    QVector<mc::UvTarget> bulkTargets() const;
    QSize faceTextureSize(const mc::UvTarget &);
    void updateBulkSummary();
    void applyBulk(const mc::UvBulk &);

  public:
    std::function<void()> changed; // live updates for the main workspace preview
    ModelAuthorDialog(std::shared_ptr<mc::ModelEdit>, std::shared_ptr<mc::Library>,
                      const QString &root, QWidget *parent = nullptr);
    ~ModelAuthorDialog() override;
};
} // namespace nova
