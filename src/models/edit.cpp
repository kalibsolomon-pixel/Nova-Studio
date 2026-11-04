// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit.h"
#include "creation.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
namespace nova::mc {
namespace {
bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}
} // namespace
bool ModelEdit::open(const QString &root, const QString &path, QString *error) {
    if (Location::fromAsset(path, "models").isEmpty())
        return fail(error, "Expected a model asset");
    QString safe;
    if (!safeDestination(root, path, &safe, error))
        return false;
    const auto base = QFileInfo(root).canonicalFilePath(),
               canonical = QFileInfo(root + '/' + path).canonicalFilePath();
    if (base.isEmpty() || !canonical.startsWith(base + '/', Qt::CaseInsensitive))
        return fail(error, "Only an editable pack model can be changed; import a fallback first");
    auto expected = Revision::read(canonical, 2 * 1024 * 1024);
    QFile f(canonical);
    if (!expected.readable || !f.open(QIODevice::ReadOnly) || f.size() > 2 * 1024 * 1024)
        return fail(error, "Model is unreadable or exceeds 2 MiB");
    const auto bytes = f.readAll();
    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != expected.hash)
        return fail(error, "External model conflict while opening");
    QJsonParseError parse;
    auto json = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !json.isObject())
        return fail(error, "Invalid model JSON");
    provisionalRoot.clear();
    file = canonical;
    asset = path;
    revision = expected;
    committed = staged = saved = json.object();
    committedBytes = -1;
    ++serial;
    commands.clear();
    cursor = 0;
    preview = false;
    return true;
}
bool ModelEdit::face(int element, const QString &name, QJsonArray &elements, QJsonObject &item,
                     QJsonObject &faces, QString *error) const {
    elements = staged["elements"].toArray();
    if (element < 0 || element >= elements.size())
        return fail(error, "Inherited geometry requires an explicit local geometry override");
    item = elements[element].toObject();
    faces = item["faces"].toObject();
    const auto from = item["from"].toArray(), to = item["to"].toArray();
    if (from.size() != 3 || to.size() != 3)
        return fail(error, "Invalid model element geometry");
    for (int i = 0; i < 3; ++i)
        if (!from[i].isDouble() || !to[i].isDouble() || !std::isfinite(from[i].toDouble()) ||
            !std::isfinite(to[i].toDouble()) || from[i].toDouble() > to[i].toDouble() ||
            from[i].toDouble() < -16 || to[i].toDouble() > 32)
            return fail(error, "Invalid model element geometry");
    if (!QStringList{"up", "down", "north", "south", "east", "west"}.contains(name) ||
        !faces[name].isObject())
        return fail(error, "Model face is unavailable");
    return true;
}
bool ModelEdit::localGeometry(const QJsonArray &elements, QString *error) {
    if (elements.isEmpty() || elements.size() > 4096)
        return fail(error, "No bounded inherited geometry to convert");
    staged = committed;
    staged["elements"] = elements;
    preview = true;
    return commitPreview("Create local geometry override");
}
bool ModelEdit::previewUv(int element, const QString &name, QVector4D uv, int rotation,
                          QString *error) {
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(uv[i]) || std::abs(uv[i]) > 4096)
            return fail(error, "UV values must be finite and within +/-4096 model units");
    if (rotation < 0 || rotation > 270 || rotation % 90)
        return fail(error, "UV rotation must be 0, 90, 180 or 270");
    QJsonArray elements;
    QJsonObject item, faces;
    if (!face(element, name, elements, item, faces, error))
        return false;
    auto value = faces[name].toObject();
    value["uv"] = QJsonArray{uv.x(), uv.y(), uv.z(), uv.w()};
    value["rotation"] = rotation;
    faces[name] = value;
    item["faces"] = faces;
    elements[element] = item;
    staged["elements"] = elements;
    preview = true;
    return true;
}
bool ModelEdit::resetUv(int element, const QString &name, QString *error) {
    QJsonArray elements;
    QJsonObject item, faces;
    if (!face(element, name, elements, item, faces, error))
        return false;
    auto value = faces[name].toObject();
    value.remove("uv");
    value.remove("rotation");
    faces[name] = value;
    item["faces"] = faces;
    elements[element] = item;
    staged["elements"] = elements;
    preview = true;
    return true;
}
qsizetype ModelEdit::historyBytes() const {
    qsizetype total = 0;
    for (const auto &c : commands)
        total += c.cost;
    return total;
}
bool ModelEdit::commitPreview(const QString &label, const QString &owner) {
    if (!preview)
        return false;
    if (committed == staged) {
        preview = false;
        return true;
    }
    // The committed size is known from the previous commit; serialize only the new state.
    if (committedBytes < 0)
        committedBytes = QJsonDocument(committed).toJson(QJsonDocument::Compact).size();
    const auto stagedBytes = QJsonDocument(staged).toJson(QJsonDocument::Compact).size();
    auto cost = committedBytes + stagedBytes;
    if (cost > 16 * 1024 * 1024 || QJsonDocument(staged).toJson().size() > 2 * 1024 * 1024)
        return false;
    preview = false;
    commands.resize(cursor);
    commands.append({committed, staged, label, cost, owner});
    ++serial;
    committed = staged;
    committedBytes = stagedBytes;
    ++cursor;
    while (commands.size() > 128 || historyBytes() > 16 * 1024 * 1024) {
        commands.removeFirst();
        --cursor;
    }
    return true;
}
std::function<bool()> ModelEdit::prepareBatch(const QJsonArray &operations, const QString &owner,
                             const QString &version, QString *error, const std::function<bool()> &continuePreparation) {
    if (preview || operations.isEmpty()) {
        fail(error, "Finish the active model preview first");
        return {};
    }
    const auto expected = serial;
    if (continuePreparation && !continuePreparation())
        return {};
    // Stage on a copy. Native widgets retain the same live ModelEdit instance.
    ModelEdit draft = *this;
    for (const auto &value : operations) {
        const auto op = value.toObject();
        const auto name = op["command"].toString();
        const auto element = op["element"].toInt(-1);
        const auto face = op["face"].toString();
        bool ok = false;
        if (name == "model_add" && draft.staged["elements"].isArray())
            ok = draft.addElement(op["texture"].toString(), error);
        else if (name == "model_remove")
            ok = draft.deleteElement(element, error);
        else if (name == "model_label")
            ok = draft.labelElement(element, op["name"].toString(), error);
        else if (name == "model_transform") {
            const auto vector = [](const QJsonValue &value) {
                const auto xyz = value.toArray();
                return QVector3D(float(xyz.at(0).toDouble()), float(xyz.at(1).toDouble()),
                                 float(xyz.at(2).toDouble()));
            };
            const auto rotation = op["rotation"].toObject();
            ElementTransform transform;
            transform.from = vector(op["from"]);
            transform.to = vector(op["to"]);
            transform.rotation = rotation["enabled"].toBool();
            if (transform.rotation) {
                if (version != "26.1" && version != "26.3") {
                    fail(error, "Euler element rotation requires the 26.1 or 26.3 model profile");
                    return {};
                }
                transform.origin = vector(rotation["origin"]);
                transform.angles = vector(rotation["angles"]);
                transform.rescale = rotation["rescale"].toBool();
            }
            ok = draft.previewElement(element, transform, error);
        } else if (name == "model_face")
            ok = draft.previewFaces(element, {{face}, op["texture"].toString(),
                op["cullface"].toString(), op["tint"].toInt(), true}, error);
        else if (name == "model_texture")
            ok = draft.textureBinding(op["alias"].toString(), op["texture"].toString(), error);
        else if (name == "uv_set") {
            const auto uv = op["uv"].toArray();
            if (uv.size() == 4)
                ok = draft.previewUv(element, face, {float(uv[0].toDouble()), float(uv[1].toDouble()),
                    float(uv[2].toDouble()), float(uv[3].toDouble())}, op["rotation"].toInt(), error);
        } else if (name == "uv_mirror_u" || name == "uv_mirror_v" || name == "uv_rotate") {
            const auto kind = name == "uv_mirror_u" ? UvOperation::MirrorU :
                              name == "uv_mirror_v" ? UvOperation::MirrorV : UvOperation::Rotate;
            ok = draft.transformUvs({{element, face}}, kind, {}, {16, 16}, version, error);
        }
        if (!ok || (continuePreparation && !continuePreparation()))
            return {};
    }
    if (!draft.commitPreview("Edit Model Transaction", owner)) {
        fail(error, "The edited model exceeds the native history or document budget");
        return {};
    }
    if (preview || serial != expected)
        return {};
    return [this, draft = std::move(draft), expected]() mutable {
        if (preview || serial != expected)
            return false;
        *this = std::move(draft);
        return true;
    };
}

bool ModelEdit::undo() {
    cancelPreview();
    if (cursor == 0)
        return false;
    ++serial;
    committed = staged = commands[--cursor].before;
    committedBytes = -1;
    return true;
}
bool ModelEdit::redo() {
    cancelPreview();
    if (cursor >= commands.size())
        return false;
    ++serial;
    committed = staged = commands[cursor++].after;
    committedBytes = -1;
    return true;
}
bool ModelEdit::save(QString *error) {
    if (preview)
        return fail(error, "Commit or cancel the UV preview before saving");
    if (file.isEmpty())
        return fail(error, "No editable model is open");
    auto bytes = QJsonDocument(committed).toJson(QJsonDocument::Indented);
    if (bytes.size() > 2 * 1024 * 1024)
        return fail(error, "Edited model exceeds 2 MiB");
    if (provisional()) {
        QString destination;
        if (!safeDestination(provisionalRoot, asset, &destination, error) ||
            pathIdentity(destination) != pathIdentity(file))
            return fail(error, "The model destination changed");
    }
    if (!atomicWrite(file, bytes, revision, error))
        return false;
    revision = Revision::read(file, 2 * 1024 * 1024);
    saved = committed;
    if (provisional()) {
        const auto key = QString::fromLatin1(
            QCryptographicHash::hash(asset.toUtf8(), QCryptographicHash::Sha256).toHex());
        QString snapshot;
        if (safeDestination(provisionalRoot, ".nova/model-recovery/" + key + ".json", &snapshot))
            QFile::remove(snapshot);
        provisionalRoot.clear();
    }
    ++serial;
    return true;
}
} // namespace nova::mc
