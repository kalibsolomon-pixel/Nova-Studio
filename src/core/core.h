// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "imageops.h"
#include "revision.h"
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QPointF>
#include <QStringList>
#include <QUuid>
#include <memory>
#include <optional>
#include <vector>

namespace nova {
std::optional<QPoint> coordinate(QPointF point, QPointF origin, double zoom, QSize size);
double zoomBy(double current, double delta);
QVector<QPoint> line(QPoint a, QPoint b);
QImage loadPng(const QString &path, QString *error = nullptr);
bool savePng(const QString &path, const QImage &image, QString *error = nullptr);
QString nativeDocumentPath(const QString &project, const QString &png, QString *error = nullptr);
struct Change {
    int index;
    QRgb before, after;
};
struct RasterLayer {
    QUuid id;
    QString name;
    bool visible = true;
    int opacity = 255;
    QImage pixels;
    quint64 revision = 0;
    QJsonObject extensions;
};
struct SourceFrame {
    QUuid id = QUuid::createUuid();
    QString name = "Frame";
    std::vector<RasterLayer> layers;
    QUuid activeLayer;
};
struct PlaybackEntry {
    QUuid frame;
    int ticks = 0; // 0 inherits the positive default; never written as time:0
};
struct AnimationState {
    bool enabled = false;
    std::vector<SourceFrame> frames;
    std::vector<PlaybackEntry> entries;
    QUuid active;
    int defaultTicks = 1;
    bool interpolate = false;
    QString target = "26.1";
    QJsonObject metadata; // complete .mcmeta, including unknown sections
};
struct AnimationLimits {
    int sourceFrames = 1024, playbackEntries = 16384;
    qint64 decodedBytes = 128LL * 1024 * 1024;
    qint64 cacheBytes = 16LL * 1024 * 1024;
    qint64 serializedBytes = 120LL * 1024 * 1024;
};
enum class FrameCommand { Add, Duplicate, Delete, Move, Rename };
enum class LayerCommand { Add, Delete, Duplicate, Rename, Move, Visibility, Opacity, Select };
struct LayerEdit {
    LayerCommand command;
    QUuid layer;
    QString name;
    int value = 0;
    QImage pixels;           // Add: optional initial pixels (document size)
    QJsonObject extensions;  // Add: optional layer extension metadata
};
// Unified stroke settings shared by pencil, brush, eraser and colour replacement.
struct StrokeStyle {
    ops::Brush brush;
    QRgb color = qRgba(0, 0, 0, 255);
    bool erase = false;
    ops::Symmetry symmetry = ops::Symmetry::None;
    int radial = 6;
    std::optional<QPointF> centre; // symmetry centre in pixel-edge units; default canvas centre
    std::vector<QRgb> palette;     // non-empty: constrain the colour to the nearest entry
    bool replaceOnly = false;      // colour replacement: only pixels matching replaceColor
    QRgb replaceColor = 0;
    int replaceTolerance = 32;
    bool lockAlpha = false;        // keep each pixel's existing alpha
};
class Document {
    friend class DocumentSession;
  public:
    enum class State { Opening, Ready, Modified, Saving, Error, Closing };

  private:
    QUuid identity = QUuid::createUuid();
    QUuid openIdentity = QUuid::createUuid();
    State lifecycle = State::Opening;
    QString lastError;
    struct Patch {
        QUuid layer;
        QRect rect;
        QImage before, after;
    };
    struct Operation {
        enum Kind { Pixels, Insert, Remove, Name, Visible, Alpha, Order, Raster, Selection, Compound, Animation } kind = Pixels;
        QUuid frame;
        QString automationOwner;
        std::shared_ptr<AnimationState> animationBefore, animationAfter;
        QUuid layer, activeBefore, activeAfter;
        std::vector<Change> pixels;
        RasterLayer snapshot;
        QString beforeName, afterName;
        int before = 0, after = 0;
        QString label;
        std::vector<Patch> patches;            // Raster: cropped before/after pixels
        QSize sizeBefore, sizeAfter;           // Raster: document resize when different
        bool selectionChanged = false;
        QImage selectionBefore, selectionAfter;
        std::vector<Operation> children;       // Compound: applied in order
    };
    std::vector<Operation> history;
    std::vector<RasterLayer> stack;
    QUuid active;
    QUuid strokeLayer;
    bool native = false;
    QString nativePath;
    Revision nativeRevision;
    QByteArray exportedPixels;
    bool exportMetadataChanged = false;
    quint64 serial = 1;
    QJsonObject extensions;
    void commit(Operation operation);
    void compose(QRect region);
    void applyOperation(Operation &op, bool forward);
    static void composeInto(const std::vector<RasterLayer> &layers, QImage &out, QRect region);
    QImage selectionMask;
    int compoundDepth = 0;
    std::vector<Operation> compoundChildren;
    QString compoundLabel;
    void beginCompound(const QString &label);
    bool endCompound(bool keep, const QString &owner = {});
    QHash<int, uchar> strokeCover;
    qint64 strokeTravel = 0;
    std::optional<QPoint> strokeLast;
    QByteArray strokeKey;
    bool setSelectionInternal(const QImage &mask, const QString &label);
    bool rasterCommit(std::vector<Patch> patches, const QString &label, QSize newSize = {},
                      const QImage *newSelection = nullptr);
    static qint64 operationBytes(const Operation &op);
    RasterLayer *findLayer(QUuid id);
    QByteArray pixelDigest() const;
    bool prepareFrameEdit(FrameCommand command, QUuid id, int position, const QString &name,
                          AnimationState &state, QString &label, QString *error) const;
    QHash<int, Change> pendingStroke;
    int cursor = 0;
    std::optional<int> saved = 0;
    void apply(bool forward);
    qint64 historyBudget = 16LL * 1024 * 1024;
    int operationLimit = 2048;
    void pruneHistory();
    Revision savedRevision;
    QUuid frameId = QUuid::createUuid();
    AnimationState animation; // active layers reside in stack; snapshots materialize them
    AnimationLimits animationLimits;
    Revision metadataRevision;
    struct FrameCache { QUuid id; QByteArray key; QImage image; };
    mutable std::vector<FrameCache> frameCache;
    void installAnimation(const AnimationState &state);
    QByteArray encodeAnimation(const QString &asset, QString *error) const;
    bool decodeAnimation(const QByteArray &bytes, const QString &asset, QString *error);

  public:
    static constexpr qint64 PixelLimit = 128LL * 1024 * 1024;
    static constexpr int LayerLimit = 64;
    QString path;
    QImage image;
    QRect dirty;
    Document(QString path, QImage image);
    bool modified() const;
    static constexpr int MaxDimension = 4096;
    bool nativeDocument() const { return native; }
    bool exportModified() const;
    // Whether the Minecraft files on disk lag behind this document. Native and
    // animated documents track their last exported/imported pixels, so merely
    // opening an animated .png.mcmeta (an undoable import step) is not pending.
    bool minecraftOutputPending() const {
        return native || animated() ? exportModified() : modified();
    }
    QUuid instanceId() const { return openIdentity; }
    bool ownsHistoryHead(const QString &owner, bool redo) const {
        const int index = redo ? cursor : cursor - 1;
        return !owner.isEmpty() && index >= 0 && index < int(history.size()) &&
               history[size_t(index)].automationOwner == owner;
    }
    quint64 changeSerial() const { return serial; }
    qint64 pixelBytes() const;
    bool animated() const { return animation.enabled; }
    QUuid activeFrame() const { return frameId; }
    AnimationState animationState() const;
    const AnimationLimits &frameLimits() const { return animationLimits; }
    bool setAnimationLimits(const AnimationLimits &limits, QString *error = nullptr);
    bool validateAnimation(const AnimationState &state, QString *error = nullptr) const;
    bool replaceAnimation(const AnimationState &state, const QString &label, QString *error = nullptr);
    bool selectFrame(QUuid id, QString *error = nullptr);
    bool editFrame(FrameCommand command, QUuid id = {}, int position = -1,
                   const QString &name = {}, QString *error = nullptr);
    bool convertToAnimation(QString *error = nullptr);
    QImage frameComposite(QUuid id) const;
    qint64 frameCacheBytes() const;
    // Reuses existing layer commands/ops on a detached snapshot, then commits once.
    bool applyFrames(const std::vector<QUuid> &frames, const ops::Spec &spec,
                     QString *error = nullptr, const ops::Context *context = nullptr);
    bool importAnimation(const QImage &sheet, const QJsonObject &metadata,
                         const QString &target, QString *error = nullptr);
    Revision mcmetaRevision() const { return metadataRevision; }
    void acceptAnimationExport(const Revision &png, const Revision &mcmeta, bool synchronized = true);
    // Marks the state produced by opening Minecraft files as the saved baseline:
    // the current history cursor is what disk represents and no editable sidecar
    // lags behind it. Opening must never leave a document modified; genuine later
    // edits still diverge from this cursor and dirty the document.
    void markOpenedBaseline() {
        saved = cursor;
        exportMetadataChanged = false;
    }
    bool acceptNativeSave(const Document &snapshot, quint64 expectedSerial);
    const std::vector<RasterLayer> &layers() const { return stack; }
    QUuid activeLayer() const { return active; }
    bool editLayer(const LayerEdit &edit, QString *error = nullptr);
    bool paint(QUuid layer, QPoint a, QPoint b, QRgb color, QString *error = nullptr);
    // ---- Phase 2-C3 editing commands (all respect the active selection) ----
    const QImage &selection() const { return selectionMask; }
    bool hasSelection() const { return !selectionMask.isNull(); }
    // Selection changes are undoable history entries that never mark content dirty.
    bool select(const QImage &shape, ops::Combine mode, const QString &label = "Select",
                QString *error = nullptr);
    bool setSelection(const QImage &mask, const QString &label = "Select", QString *error = nullptr);
    bool selectAll();
    bool deselect();
    bool invertSelection();
    // Continuous strokes accumulate into one transaction until finish();
    // cancelStroke() restores every pixel the open stroke touched.
    bool stroke(QUuid layer, QPoint a, QPoint b, const StrokeStyle &style, QString *error = nullptr);
    void cancelStroke();
    bool strokeOpen() const { return !pendingStroke.isEmpty(); }
    bool replaceLayerPixels(QUuid layer, const QImage &pixels, const QString &label,
                            QString *error = nullptr, const QImage *newSelection = nullptr);
    bool replaceLayersPixels(const QMap<QUuid, QImage> &pixels, const QString &label,
                             bool includeHidden = false, QString *error = nullptr,
                             const QImage *newSelection = nullptr);
    bool applyOperation(QUuid layer, const ops::Spec &spec, QString *error = nullptr,
                        const ops::Context *context = nullptr, QUuid *createdLayer = nullptr);
    bool applyLayers(const QVector<QUuid> &layers, const ops::Spec &spec,
                     QString *error = nullptr, const ops::Context *context = nullptr);
    bool applyDocumentOperation(ops::Spec spec, QString *error = nullptr, const ops::Context *context = nullptr);
    bool addLayerWithPixels(const QString &name, const QImage &pixels,
                            const QJsonObject &extensions = {}, QString *error = nullptr,
                            QUuid *created = nullptr);
    // Commits a floating selection: optionally clears the lifted source pixels,
    // then composites the positioned floating pixels (document-sized) as one step.
    bool commitFloating(QUuid layer, const QImage &floating, const QImage *liftMask,
                        const QString &label, bool asNewLayer, QString *error = nullptr,
                        const QImage *newSelection = nullptr);
    bool fill(QUuid layer, QPoint seed, QRgb color, int tolerance, ops::Distance metric,
              bool contiguous, bool sampleComposite, int opacity = 255, QString *error = nullptr);
    // Universal Fill: paints every pixel matching the seed under ops::universalFillMatches()
    // across the whole image, gated by the selection like fill(). Matching reads the active
    // layer, or the visible composite with sampleComposite; only the active layer changes.
    bool universalFill(QUuid layer, QPoint seed, QRgb color, int tolerance, bool sampleComposite,
                       int opacity = 255, QString *error = nullptr);
    bool drawShape(QUuid layer, const ops::ShapeSpec &shape, QRgb color, int opacity = 255,
                   bool erase = false, QString *error = nullptr);
    bool mergeDown(QUuid layer, QString *error = nullptr);
    bool flatten(QString *error = nullptr);
    bool clearSelected(QUuid layer, QString *error = nullptr);
    // Previews: composites computed without touching authoritative pixels.
    QImage compositeWith(QUuid layer, const QImage &replacement) const;
    QImage compositeWith(const QMap<QUuid, QImage> &replacements) const;
    QImage compositeWithInserted(QUuid above, const QImage &pixels) const;
    QImage selectedPixels(QUuid layer) const; // layer pixels × selection alpha
    const RasterLayer *layer(QUuid id) const;
    QStringList historyLabels() const;
    int historyCursor() const { return cursor; }
    void jumpHistory(int target);
    bool saveNative(const QString &project, QString *error = nullptr);
    bool loadNative(const QString &project, QString *error = nullptr);
    QString editablePath() const { return nativePath; }
    QByteArray encodeNative(const QString &asset, QString *error = nullptr) const;
    bool decodeNative(const QByteArray &bytes, const QString &asset, QString *error = nullptr);
    void markRecovered() {
        saved.reset();
        ++serial;
    }
    std::shared_ptr<Document> recoverySnapshot() const;
    Revision editableRevision() const { return nativeRevision; }
    void restoreEditableBinding(const QString &file, const Revision &revision) {
        nativePath = file;
        nativeRevision = revision;
        markRecovered();
    }
    void draw(QPoint a, QPoint b, QRgb color);
    void finish();
    void undo();
    void redo();
    bool save(QString *error = nullptr);
    bool saveAs(const QString &target, bool overwrite = false, QString *error = nullptr);
    bool saveOver(const Revision &confirmed, QString *error = nullptr);
    bool saveAsRevision(const QString &target, const Revision &expected, QString *error = nullptr);
    bool reload(QString *error = nullptr);
    Revision revision() const { return savedRevision; }
    bool externalChanged() const {
        return !savedRevision.sameContent(Revision::read(path)) ||
               (animated() && !metadataRevision.sameContent(Revision::read(path + ".mcmeta")));
    }
    qint64 historyBytes() const;
    int historyOperations() const { return int(history.size()); }
    void setHistoryLimits(qint64 bytes, int operations);
    void restorePixels(const QImage &pixels, const Revision &originalRevision);
    static std::shared_ptr<Document> open(const QString &path, QString *error = nullptr);
    // A document bound to a project path that does not exist yet, built from verified baseline
    // bytes. Opening writes nothing and leaves it unmodified; both revisions are "missing", so its
    // first save (or, for an animated texture, PNG + .mcmeta pair export) is create-only.
    static std::shared_ptr<Document> fromBaseline(const QString &targetPath, const QByteArray &png,
                                                  const QByteArray &mcmeta,
                                                  const QString &minecraftVersion,
                                                  QString *error = nullptr);
    QUuid id() const { return identity; }
    void setRecoveryIdentity(QUuid id) { identity = id; }
    State state() const;
    QString error() const { return lastError; }
    bool canUndo() const { return cursor > 0 || !pendingStroke.isEmpty(); }
    bool canRedo() const { return cursor < int(history.size()); }
    void close() {
        finish();
        lifecycle = State::Closing;
    }
};
} // namespace nova
