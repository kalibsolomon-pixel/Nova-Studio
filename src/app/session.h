// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include <QObject>
#include <functional>

namespace nova {
enum class Command { Undo, Redo, Save, SaveAs, ExportPng, SaveNative };
class DocumentSession : public QObject {
    Q_OBJECT
    std::vector<std::shared_ptr<Document>> documents;
    QString projectRoot;

  public:
    using QObject::QObject;
    void setProject(const QString &root) { projectRoot = root; }
    std::shared_ptr<Document> open(const QString &path, QString *error = nullptr);
    static std::shared_ptr<Document> newRaster(const QString &path, const QImage &pixels,
                                               const QString &layerName, int frames, int ticks,
                                               bool interpolate, QString *error);
    bool adopt(const std::shared_ptr<Document> &doc);
    bool canRestorePixels(const std::shared_ptr<Document> &doc, const QImage &pixels) const;
    bool canRestoreDocument(const std::shared_ptr<Document> &doc,
                            const Document &replacement) const;
    const std::vector<std::shared_ptr<Document>> &all() const { return documents; }
    bool dispatch(const std::shared_ptr<Document> &doc, Command command, const QString &target = {},
                  bool overwrite = false, QString *error = nullptr);
    bool edit(const std::shared_ptr<Document> &doc, const LayerEdit &edit,
              QString *error = nullptr);
    bool paint(const std::shared_ptr<Document> &doc, QUuid layer, QPoint a, QPoint b, QRgb color,
               bool finishStroke = false, QString *error = nullptr);
    void close(const std::shared_ptr<Document> &doc);
    bool animationEdit(const std::shared_ptr<Document> &doc, const AnimationState &state,
                       const QString &label, QString *error = nullptr);
    bool frameEdit(const std::shared_ptr<Document> &doc, FrameCommand command, QUuid frame = {},
                   int position = -1, const QString &name = {}, QString *error = nullptr);
    bool frameSelect(const std::shared_ptr<Document> &doc, QUuid frame, QString *error = nullptr);
    // ---- Phase 2-C3 editing commands: one entry point for UI, batch and macros ----
    bool owns(const std::shared_ptr<Document> &doc, QString *error = nullptr) const;
    bool replaceLayers(const std::shared_ptr<Document> &doc, const QMap<QUuid, QImage> &pixels,
                       const QString &label, bool includeHidden = false, QString *error = nullptr,
                       const QImage *selection = nullptr);
    bool stroke(const std::shared_ptr<Document> &doc, QUuid layer, QPoint a, QPoint b,
                const StrokeStyle &style, bool finishStroke = false, QString *error = nullptr);
    void finishStroke(const std::shared_ptr<Document> &doc);
    void cancelStroke(const std::shared_ptr<Document> &doc);
    bool select(const std::shared_ptr<Document> &doc, const QImage &shape, ops::Combine mode,
                const QString &label = "Select", QString *error = nullptr);
    enum class SelectionCommand { All, None, Invert };
    bool selection(const std::shared_ptr<Document> &doc, SelectionCommand command);
    bool apply(const std::shared_ptr<Document> &doc, QUuid layer, const ops::Spec &spec,
               QString *error = nullptr, const ops::Context *context = nullptr);
    bool applyLayers(const std::shared_ptr<Document> &doc, const QVector<QUuid> &layers,
                     const ops::Spec &spec, QString *error = nullptr, const ops::Context *context = nullptr);
    bool fill(const std::shared_ptr<Document> &doc, QUuid layer, QPoint seed, QRgb color,
              int tolerance, ops::Distance metric, bool contiguous, bool sampleComposite,
              int opacity = 255, QString *error = nullptr);
    bool universalFill(const std::shared_ptr<Document> &doc, QUuid layer, QPoint seed, QRgb color,
                       int tolerance, bool sampleComposite, int opacity = 255, QString *error = nullptr);
    bool shape(const std::shared_ptr<Document> &doc, QUuid layer, const ops::ShapeSpec &shape,
               QRgb color, int opacity = 255, bool erase = false, QString *error = nullptr);
    bool commitFloating(const std::shared_ptr<Document> &doc, QUuid layer, const QImage &floating,
                        const QImage *liftMask, const QString &label, bool asNewLayer,
                        QString *error = nullptr, const QImage *newSelection = nullptr);
    // Commits pixels already produced by the shared operation engine (e.g. a
    // background preview result) as one history step.
    bool replacePixels(const std::shared_ptr<Document> &doc, QUuid layer, const QImage &pixels,
                       const QString &label, QString *error = nullptr);
    bool addLayer(const std::shared_ptr<Document> &doc, const QString &name, const QImage &pixels,
                  const QJsonObject &extensions = {}, QString *error = nullptr);
    bool mergeDown(const std::shared_ptr<Document> &doc, QUuid layer, QString *error = nullptr);
    bool flatten(const std::shared_ptr<Document> &doc, QString *error = nullptr);
    bool clear(const std::shared_ptr<Document> &doc, QUuid layer, QString *error = nullptr);
    bool jump(const std::shared_ptr<Document> &doc, int historyIndex);
    // JSON command for scripts/macros/agents, e.g. {"command":"operation",
    // "operation":"hue_saturation","params":{...}} or {"command":"select_rect",...}.
    bool execute(const std::shared_ptr<Document> &doc, const QJsonObject &command,
                 QString *error = nullptr);
    // Prepare native commands on an isolated document and publish one history step.
    std::function<bool(QString *)> prepareBatch(const std::shared_ptr<Document> &doc, const QJsonArray &commands,
                      QString *error = nullptr, const QString &owner = {},
                      qint64 maxPixelBytes = Document::PixelLimit,
                      const std::function<bool()> &continuePreparation = {});
    bool withinPixelBudget(const std::shared_ptr<Document> &doc, qint64 extraBytes) const;
    qsizetype count() const { return qsizetype(documents.size()); }
    void notify(const std::shared_ptr<Document> &doc) { emit changed(doc->id()); }
  signals:
    void changed(QUuid document);
    void closed(QUuid document);
};
} // namespace nova
