// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "imageops.h"
#include "presets.h"
#include "session.h"
#include <QDialog>
#include <atomic>
#include <memory>

class QComboBox;
class QFormLayout;
class QLabel;
class QProgressBar;
class QCheckBox;
class QTimer;
class QPushButton;
namespace nova {
class Canvas;
// Parameter editor generated from an OperationInfo (shared by the operation
// dialog and the batch pipeline editor).
class ParamEditor : public QWidget {
    Q_OBJECT
    QString id;
    QFormLayout *form;
    QMap<QString, QWidget *> editors;
    bool updating = false;

  public:
    explicit ParamEditor(QWidget *parent = nullptr);
    void setOperation(const QString &operation, const QJsonObject &params = {});
    QString operation() const { return id; }
    QJsonObject params() const;
    void setParams(const QJsonObject &params);
    std::function<QString()> paletteText; // supplies the active palette for Palette params
  signals:
    void edited();
};
// One dialog for every image operation: preview, numeric controls, presets,
// reset, apply/cancel, scope, background computation with cancellation.
class OperationDialog : public QDialog {
    Q_OBJECT
    DocumentSession *session;
    std::shared_ptr<Document> document;
    Canvas *canvas;
    PresetStore *presets;
    QComboBox *operations, *presetBox, *heightLayer, *output;
    QFormLayout *extraForm = nullptr;
    QCheckBox *aspect = nullptr;
    bool syncingAspect = false;
    ParamEditor *editor;
    QLabel *before, *after, *info, *scope;
    QProgressBar *busy;
    QCheckBox *previewToggle;
    QPushButton *applyButton;
    QTimer *debounce;
    std::shared_ptr<std::atomic_bool> cancel;
    quint64 generation = 0, readyGeneration = 0;
    QImage result;
    QJsonObject resultParams;
    std::vector<QRgb> palette;
    QUuid layer;
    QComboBox *layerScope;
    QVector<QUuid> selectedLayers;
    void schedule();
    void compute();
    void loadPresets();
    ops::Context context() const;
    void showThumbs(const QImage &out);

  public:
    OperationDialog(DocumentSession *session, std::shared_ptr<Document> doc, Canvas *canvas,
                    PresetStore *presets, std::vector<QRgb> palette, const QStringList &ids,
                    std::function<QString()> paletteText, QWidget *parent = nullptr);
    ~OperationDialog() override;
    bool applyOperation(QString *error);
    void setSelectedLayers(const QVector<QUuid> &ids, bool selectScope = false);
    ParamEditor *parameters() const { return editor; }
    QString lastError;
};
// Tiling / offset / seam diagnostics for the current composite.
void showTilePreview(QWidget *parent, const QImage &image);
} // namespace nova
