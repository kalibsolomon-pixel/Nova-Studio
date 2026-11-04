// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "models.h"
#include "revision.h"
#include <QVector4D>
#include <QUuid>
#include <functional>
namespace nova::mc {
bool normalizeFaceReferences(QJsonObject *source, QString *error = nullptr);
struct ElementTransform {
    QVector3D from{0, 0, 0}, to{16, 16, 16}, origin{8, 8, 8}, angles;
    bool rotation = false, rescale = false;
};
struct FaceEdit {
    QStringList directions;
    QString texture, cullface;
    int tint = -1;
    bool enabled = true;
};
struct UvTarget { int element; QString face; };
enum class UvOperation { Move, MirrorU, MirrorV, Rotate, Align, Snap, Reset };
// Multi-face UV edit. Coordinates are model UV units (0-16 spans the texture),
// independent of each face's texture resolution; only Snap uses texture pixels,
// and it snaps every face to the texel grid of its own texture.
struct UvBulk {
    enum Kind {
        Offset,     // relative move by value
        Position,   // absolute: move each rectangle's top-left to value
        Size,       // absolute width/height (value), keeping top-left and mirroring
        Distribute, // place rectangles side by side starting at the first one, with gap
        Rotate,     // face rotation +90°
        MirrorU,
        MirrorV,
        Snap,
        Reset,      // remove explicit uv/rotation (inferred defaults again)
        Infer,      // write the inferred default UV explicitly (identical rendering)
        Paste       // uv/rotation from clipboard
    } kind = Offset;
    QPointF value;
    bool first = true, second = true; // Position/Size: apply the U (first) / V (second) axis
    bool horizontal = true;
    double gap = 0;
    QJsonObject clipboard;
};
struct UvState {
    QVector4D uv; // u1, v1, u2, v2 (model UV units)
    int rotation = 0;
    bool inferred = false;
};
// Current (staged) UV rectangle per target, explicit or inferred. Empty on failure.
QVector<UvState> uvStates(const QJsonObject &model, const QVector<UvTarget> &,
                          const QString &version = "26.1");
struct DisplayTransform;
// Model JSON owns a separate bounded command history. Raster history is untouched.
class ModelEdit {
    QString file, asset, provisionalRoot;
    Revision revision;
    QJsonObject committed, staged, saved;
    struct Command {
        QJsonObject before, after;
        QString label;
        qsizetype cost;
        QString automationOwner;
    };
    QVector<Command> commands;
    int cursor = 0;
    qsizetype committedBytes = -1; // compact size of `committed`; -1 = unknown
    bool preview = false;
    QUuid instance = QUuid::createUuid();
    quint64 serial = 0;
    bool face(int element, const QString &name, QJsonArray &elements, QJsonObject &item,
              QJsonObject &faces, QString *error) const;

  public:
    bool open(const QString &editableRoot, const QString &modelAsset, QString *error = nullptr);
    bool create(const QJsonObject &source, QString *error = nullptr);
    bool bindProvisional(const QString &root, const QString &asset, QString *error = nullptr);
    bool provisional() const { return !provisionalRoot.isEmpty(); }
    bool saveAs(const QString &root, const QString &asset, QString *error = nullptr);
    bool renameKeepingAlias(const QString &root, const QString &newAsset, QString *error = nullptr);
    bool externalChange() const;
    bool writeRecovery(const QString &path, QString *error = nullptr) const;
    bool restoreRecovery(const QString &root, const QString &path, QString *error = nullptr);
    bool previewElement(int index, const ElementTransform &, QString *error = nullptr);
    bool addElement(const QString &texture = "#all", QString *error = nullptr);
    bool duplicateElement(int index, QString *error = nullptr);
    bool deleteElement(int index, QString *error = nullptr);
    bool labelElement(int index, const QString &name, QString *error = nullptr);
    bool previewFaces(int index, const FaceEdit &, QString *error = nullptr);
    bool pasteFaces(int index, const QStringList &directions, const QJsonObject &properties,
                    QString *error = nullptr);
    bool textureBinding(const QString &alias, const QString &resource, QString *error = nullptr);
    bool properties(const QString &parent, const QString &guiLight, bool ambientOcclusion,
                    QString *error = nullptr);
    bool localGeometry(const QJsonArray &resolvedElements, QString *error = nullptr);
    bool previewUv(int element, const QString &name, QVector4D uv, int rotation,
                   QString *error = nullptr);
    bool previewUvs(const QVector<UvTarget> &, QVector4D uv, int rotation, QString *error = nullptr);
    bool resetUv(int element, const QString &name, QString *error = nullptr);
    bool transformUvs(const QVector<UvTarget> &, UvOperation, QPointF amount = {},
                      QSize texture = {16,16}, const QString &version = "26.1",
                      QString *error = nullptr);
    // Item display transforms (display.cpp). Staged like every other preview.
    bool previewDisplay(const QString &context, const DisplayTransform &, QString *error = nullptr);
    bool resetDisplay(const QString &context, QString *error = nullptr); // inherit again
    bool pasteDisplay(const QString &context, const QJsonObject &, QString *error = nullptr);
    bool linkHands(const QString &context, bool linked, const DisplayTransform &right,
                   QString *error = nullptr);
    bool applyDisplayPreset(const QJsonObject &display, QString *error = nullptr);
    QJsonObject localDisplay(const QString &context) const;
    // Bulk face UV operations in model UV units (authoring.cpp).
    bool bulkUv(const QVector<UvTarget> &, const UvBulk &,
                const std::function<QSize(const UvTarget &)> &textureSize,
                const QString &version = "26.1", QString *error = nullptr);
    bool commitPreview(const QString &label = "Edit UV", const QString &owner = {});
    std::function<bool()> prepareBatch(const QJsonArray &operations, const QString &owner, const QString &version,
                      QString *error, const std::function<bool()> &continuePreparation = {});
    QUuid instanceId() const { return instance; }
    quint64 changeSerial() const { return serial; }
    bool ownsHistoryHead(const QString &owner, bool redo) const {
        const int index = redo ? cursor : cursor - 1;
        return !owner.isEmpty() && index >= 0 && index < commands.size() &&
               commands[index].automationOwner == owner;
    }
    void cancelPreview() {
        staged = committed;
        preview = false;
    }
    bool undo();
    bool redo();
    bool save(QString *error = nullptr);
    bool modified() const { return provisional() || committed != saved; }
    bool previewing() const { return preview; }
    bool hasLocalGeometry() const { return committed["elements"].isArray(); }
    QJsonObject source() const { return staged; }
    QString path() const { return file; }
    QString assetPath() const { return asset; }
    QJsonObject authoritative() const { return committed; }
    qsizetype historyBytes() const;
};
} // namespace nova::mc
