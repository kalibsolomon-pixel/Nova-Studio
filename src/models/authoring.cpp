// SPDX-License-Identifier: GPL-3.0-or-later
#include "creation.h"
#include "edit.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
#include <numeric>
namespace nova::mc {
namespace {
bool fail(QString *e, const QString &s) {
    if (e)
        *e = s;
    return false;
}
const QStringList directions{"down", "up", "north", "south", "west", "east"};
QJsonArray vector(QVector3D v) { return {v.x(), v.y(), v.z()}; }
bool validFace(const QJsonObject &f) {
    const auto t = f["texture"].toString();
    if (!(t.startsWith('#') ? QRegularExpression("^#[a-zA-Z0-9_.-]+$").match(t).hasMatch()
                            : Location::parse(t).valid()))
        return false;
    if (f.contains("cullface") && !directions.contains(f["cullface"].toString()))
        return false;
    if (f.contains("tintindex") &&
        (!f["tintindex"].isDouble() || f["tintindex"].toDouble() != f["tintindex"].toInt() ||
         f["tintindex"].toInt() < -1))
        return false;
    if (f.contains("rotation") &&
        (!f["rotation"].isDouble() || f["rotation"].toDouble() != f["rotation"].toInt() ||
         f["rotation"].toInt() < 0 || f["rotation"].toInt() > 270 || f["rotation"].toInt() % 90))
        return false;
    if (f.contains("uv")) {
        const auto a = f["uv"].toArray();
        if (a.size() != 4)
            return false;
        for (const auto &v : a)
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 4096)
                return false;
    }
    return true;
}
QString faceReference(QJsonObject &source, const QString &resource, QString *error) {
    if (resource.startsWith('#'))
        return resource;
    if (!Location::parse(resource).valid()) {
        fail(error, "Invalid face texture resource");
        return {};
    }
    auto textures = source["textures"].toObject();
    for (auto it = textures.begin(); it != textures.end(); ++it)
        if ((it.value().isString() ? it.value().toString()
                                   : it.value().toObject()["sprite"].toString()) == resource)
            return '#' + it.key();
    if (textures.size() >= 4096) {
        fail(error, "Texture binding budget exceeded");
        return {};
    }
    QString alias = "texture";
    int n = 1;
    while (textures.contains(alias))
        alias = "texture" + QString::number(n++);
    textures[alias] = resource;
    source["textures"] = textures;
    return '#' + alias;
}
} // namespace
bool normalizeFaceReferences(QJsonObject *source, QString *error) {
    auto candidate = *source;
    auto elements = candidate["elements"].toArray();
    for (int i = 0; i < elements.size(); ++i) {
        auto e = elements[i].toObject(), faces = e["faces"].toObject();
        for (auto it = faces.begin(); it != faces.end(); ++it) {
            auto f = it.value().toObject();
            const auto texture = f["texture"].toString();
            if (texture.startsWith('#'))
                continue;
            const auto ref = faceReference(candidate, texture, error);
            if (ref.isEmpty())
                return false;
            f["texture"] = ref;
            it.value() = f;
        }
        e["faces"] = faces;
        elements[i] = e;
    }
    if (candidate.contains("elements"))
        candidate["elements"] = elements;
    *source = candidate;
    return true;
}
bool ModelEdit::create(const QJsonObject &value, QString *error) {
    if (QJsonDocument(value).toJson().size() > 2 * 1024 * 1024)
        return fail(error, "Model exceeds 2 MiB");
    provisionalRoot.clear();
    file.clear();
    asset.clear();
    revision = {};
    committed = staged = value;
    committedBytes = -1;
    saved = {};
    commands.clear();
    cursor = 0;
    preview = false;
    return true;
}
bool ModelEdit::bindProvisional(const QString &root, const QString &path, QString *error) {
    QString destination;
    if (!file.isEmpty() || Location::fromAsset(path, "models").isEmpty() ||
        !safeDestination(root, path, &destination, error) || QFileInfo::exists(destination) ||
        !QFileInfo(QFileInfo(destination).absolutePath()).isDir())
        return fail(error, "Choose a vacant model destination in an existing pack directory");
    provisionalRoot = root;
    asset = path;
    file = destination;
    ++serial;
    return true;
}
bool ModelEdit::saveAs(const QString &root, const QString &path, QString *error) {
    if (preview)
        return fail(error, "Apply or cancel the preview before Save As");
    if (Location::fromAsset(path, "models").isEmpty())
        return fail(error, "Choose a model asset path");
    QString destination;
    if (!safeDestination(root, path, &destination, error))
        return false;
    if (QFileInfo::exists(destination))
        return fail(error, "Save As requires a new destination; existing assets are preserved");
    const auto bytes = QJsonDocument(committed).toJson();
    if (bytes.size() > 2 * 1024 * 1024)
        return fail(error, "Model exceeds 2 MiB");
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()) ||
        !safeDestination(root, path, &destination, error) ||
        !atomicWrite(destination, bytes, Revision{}, error))
        return false;
    file = destination;
    asset = path;
    revision = Revision::read(file);
    saved = committed;
    provisionalRoot.clear();
    ++serial;
    return true;
}
bool ModelEdit::externalChange() const {
    return !file.isEmpty() && !revision.sameContent(Revision::read(file, 2 * 1024 * 1024));
}
bool ModelEdit::renameKeepingAlias(const QString &root, const QString &newAsset, QString *error) {
    QString originalPath;
    if (preview || provisional() || file.isEmpty() || !safeDestination(root, asset, &originalPath, error) ||
        pathIdentity(originalPath) != pathIdentity(file) ||
        Location::fromAsset(newAsset, "models").isEmpty())
        return fail(error, "Apply/cancel preview and choose a project-local model destination");
    if (externalChange())
        return fail(error, "External revision conflict before rename");
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly))
        return fail(error, "Cannot back up original model");
    const auto original = input.readAll();
    input.close();
    if (QCryptographicHash::hash(original, QCryptographicHash::Sha256) != revision.hash)
        return fail(error, "External revision conflict while reading original");
    CreationPlan plan;
    plan.root = root;
    plan.files.append({newAsset, QJsonDocument(committed).toJson()});
    const auto created = executeCreation(plan);
    if (!created.complete)
        return fail(error, created.error + " · journal: " + created.journal);
    const auto folder = root + '/' + created.journal;
    const auto manifest = folder + "/manifest.json";
    const auto manifestRevision = Revision::read(manifest);
    QFile journalFile(manifest);
    if (!journalFile.open(QIODevice::ReadOnly))
        return fail(error, "Copy retained; cannot read rename journal: " + created.journal);
    auto journal = QJsonDocument::fromJson(journalFile.readAll()).object();
    journalFile.close();
    const auto alias =
        QJsonDocument(QJsonObject{{"parent", Location::fromAsset(newAsset, "models")}}).toJson();
    journal["alias"] = QJsonObject{
        {"file", asset},
        {"original_sha256", QString::fromLatin1(revision.hash.toHex())},
        {"alias_sha256",
         QString::fromLatin1(QCryptographicHash::hash(alias, QCryptographicHash::Sha256).toHex())}};
    if (!atomicWrite(folder + "/original.json", original, Revision{}, error) ||
        !atomicWrite(manifest, QJsonDocument(journal).toJson(), manifestRevision, error) ||
        !atomicWrite(file, alias, revision, error)) {
        if (error)
            *error += " · new copy retained; recover journal " + created.journal;
        return false;
    }
    ++serial;
    asset = newAsset;
    file = root + '/' + newAsset;
    revision = Revision::read(file);
    saved = committed;
    return true;
}
bool ModelEdit::writeRecovery(const QString &path, QString *error) const {
    if (asset.isEmpty())
        return fail(error, "Save the new model once before recovery snapshots");
    QJsonObject snapshot{{"schema", 1},
                         {"asset", asset},
                         {"provisional", provisional()},
                         {"source", committed},
                         {"baseline_hash", QString::fromLatin1(revision.hash.toHex())},
                         {"baseline_size", double(revision.size)}};
    const auto payload = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    if (payload.size() > 3 * 1024 * 1024)
        return fail(error, "Recovery snapshot exceeds budget");
    QJsonObject envelope{
        {"schema", 1},
        {"payload", QString::fromUtf8(payload)},
        {"sha256", QString::fromLatin1(
                       QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex())}};
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(error, "Recovery destination unavailable");
    return atomicWrite(path, QJsonDocument(envelope).toJson(QJsonDocument::Compact),
                       Revision::read(path, 4 * 1024 * 1024), error);
}
bool ModelEdit::restoreRecovery(const QString &root, const QString &path, QString *error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 4 * 1024 * 1024)
        return fail(error, "Recovery snapshot unreadable or over budget");
    const auto envelope = QJsonDocument::fromJson(f.readAll()).object();
    const auto payload = envelope["payload"].toString().toUtf8();
    if (envelope["schema"].toInt() != 1 ||
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex() !=
            envelope["sha256"].toString().toLatin1())
        return fail(error, "Corrupt model recovery checksum");
    const auto snapshot = QJsonDocument::fromJson(payload).object();
    const bool provisional = snapshot["provisional"] == true;
    if (snapshot["schema"].toInt() != 1 || !snapshot["source"].isObject() ||
        snapshot["baseline_hash"].toString().size() != (provisional ? 0 : 64))
        return fail(error, "Invalid recovery payload");
    ModelEdit restored;
    if (provisional) {
        if (!restored.create(snapshot["source"].toObject(), error) ||
            !restored.bindProvisional(root, snapshot["asset"].toString(), error))
            return false;
        *this = std::move(restored);
        return true;
    }
    if (!restored.open(root, snapshot["asset"].toString(), error))
        return false;
    restored.staged = snapshot["source"].toObject();
    restored.preview = true;
    if (!restored.commitPreview("Restore model snapshot"))
        return fail(error, "Recovery exceeds history budget");
    restored.revision.hash = QByteArray::fromHex(snapshot["baseline_hash"].toString().toLatin1());
    restored.revision.size = qint64(snapshot["baseline_size"].toDouble());
    *this = std::move(restored);
    return true;
}
bool ModelEdit::previewElement(int index, const ElementTransform &t, QString *error) {
    auto elements = staged["elements"].toArray();
    if (index < 0 || index >= elements.size())
        return fail(error, "Select local geometry first");
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(t.from[i]) || !std::isfinite(t.to[i]) || t.from[i] < -16 ||
            t.to[i] > 32 || t.from[i] > t.to[i] || !std::isfinite(t.origin[i]) ||
            std::abs(t.origin[i]) > 10000 || !std::isfinite(t.angles[i]) ||
            std::abs(t.angles[i]) > 36000)
            return fail(
                error,
                "Coordinates must be ordered in [-16,32]; finite rotation and origin required");
    auto e = elements[index].toObject();
    e["from"] = vector(t.from);
    e["to"] = vector(t.to);
    if (t.rotation) {
        auto r = e["rotation"].toObject();
        r.remove("axis");
        r.remove("angle");
        r["origin"] = vector(t.origin);
        r["x"] = t.angles.x();
        r["y"] = t.angles.y();
        r["z"] = t.angles.z();
        r["rescale"] = t.rescale;
        e["rotation"] = r;
    } else
        e.remove("rotation");
    elements[index] = e;
    staged["elements"] = elements;
    preview = true;
    return true;
}
bool ModelEdit::addElement(const QString &texture, QString *error) {
    if (!validFace({{"texture", texture}}))
        return fail(error, "Invalid texture resource or alias");
    auto elements = staged["elements"].toArray();
    if (elements.size() >= 4096)
        return fail(error, "4096 element limit");
    auto candidate = staged;
    const auto ref = faceReference(candidate, texture, error);
    if (ref.isEmpty())
        return false;
    QJsonObject f;
    for (const auto &d : directions)
        f[d] = QJsonObject{{"texture", ref}};
    elements.append(
        QJsonObject{{"from", QJsonArray{0, 0, 0}}, {"to", QJsonArray{16, 16, 16}}, {"faces", f}});
    candidate["elements"] = elements;
    staged = candidate;
    preview = true;
    return true;
}
bool ModelEdit::duplicateElement(int i, QString *error) {
    auto a = staged["elements"].toArray();
    if (i < 0 || i >= a.size() || a.size() >= 4096)
        return fail(error, "No element selected or element budget reached");
    a.append(a[i]);
    staged["elements"] = a;
    preview = true;
    return true;
}
bool ModelEdit::deleteElement(int i, QString *error) {
    auto a = staged["elements"].toArray();
    if (i < 0 || i >= a.size())
        return fail(error, "No element selected");
    a.removeAt(i);
    staged["elements"] = a;
    preview = true;
    return true;
}
bool ModelEdit::labelElement(int i, const QString &name, QString *error) {
    auto a = staged["elements"].toArray();
    if (i < 0 || i >= a.size() || name.size() > 256)
        return fail(error, "Invalid element label");
    auto e = a[i].toObject();
    e["name"] = name;
    a[i] = e;
    staged["elements"] = a;
    preview = true;
    return true;
}
bool ModelEdit::previewFaces(int i, const FaceEdit &edit, QString *error) {
    auto a = staged["elements"].toArray();
    if (i < 0 || i >= a.size() || edit.directions.isEmpty())
        return fail(error, "Select an element and faces");
    auto e = a[i].toObject(), f = e["faces"].toObject();
    auto candidate = staged;
    QString ref;
    if (edit.enabled) {
        ref = faceReference(candidate, edit.texture, error);
        if (ref.isEmpty())
            return false;
    }
    for (const auto &d : edit.directions) {
        if (!directions.contains(d))
            return fail(error, "Invalid face direction");
        if (!edit.enabled) {
            f.remove(d);
            continue;
        }
        auto p = f[d].toObject();
        p["texture"] = ref;
        p["tintindex"] = edit.tint;
        if (edit.cullface.isEmpty())
            p.remove("cullface");
        else
            p["cullface"] = edit.cullface;
        if (!validFace(p))
            return fail(error, "Invalid face texture, tint or cull direction");
        f[d] = p;
    }
    e["faces"] = f;
    a[i] = e;
    candidate["elements"] = a;
    staged = candidate;
    preview = true;
    return true;
}
bool ModelEdit::pasteFaces(int i, const QStringList &ds, const QJsonObject &p, QString *error) {
    if (!validFace(p))
        return fail(error, "Invalid face properties");
    auto a = staged["elements"].toArray();
    if (i < 0 || i >= a.size() || ds.isEmpty())
        return fail(error, "Select an element and faces");
    auto e = a[i].toObject(), f = e["faces"].toObject();
    auto candidate = staged;
    const auto ref = faceReference(candidate, p["texture"].toString(), error);
    if (ref.isEmpty())
        return false;
    for (const auto &d : ds) {
        if (!directions.contains(d))
            return fail(error, "Invalid face");
        auto dest = f[d].toObject();
        for (auto it = p.begin(); it != p.end(); ++it)
            dest[it.key()] = it.value();
        dest["texture"] = ref;
        f[d] = dest;
    }
    e["faces"] = f;
    a[i] = e;
    candidate["elements"] = a;
    staged = candidate;
    preview = true;
    return true;
}
bool ModelEdit::textureBinding(const QString &alias, const QString &resource, QString *error) {
    if (!QRegularExpression("^[a-zA-Z0-9_.-]+$").match(alias).hasMatch() ||
        !validFace({{"texture", resource}}))
        return fail(error, "Invalid texture alias or resource");
    auto t = staged["textures"].toObject();
    auto old = t[alias];
    if (old.isObject()) {
        auto o = old.toObject();
        o["sprite"] = resource;
        t[alias] = o;
    } else
        t[alias] = resource;
    staged["textures"] = t;
    preview = true;
    return true;
}
bool ModelEdit::properties(const QString &parent, const QString &light, bool ao, QString *error) {
    if ((!parent.isEmpty() && !Location::parse(parent).valid()) ||
        !QStringList{"front", "side"}.contains(light))
        return fail(error, "Invalid parent or GUI light");
    if (parent.isEmpty())
        staged.remove("parent");
    else
        staged["parent"] = parent;
    staged["gui_light"] = light;
    staged["ambientocclusion"] = ao;
    preview = true;
    return true;
}
bool ModelEdit::transformUvs(const QVector<UvTarget> &targets, UvOperation operation,
                             QPointF amount, QSize texture, const QString &version,
                             QString *error) {
    if (targets.isEmpty() || targets.size() > 24576 || !texture.isValid() ||
        !Profile::forVersion(version).valid())
        return fail(error, "Select bounded faces and valid texture dimensions/profile");
    auto before = staged;
    bool wasPreview = preview;
    Model raw;
    raw.valid = true;
    raw.version = version;
    raw.source = raw.resolved = staged;
    const auto geometry = mesh(raw);
    for (const auto &target : targets) {
        bool found = false;
        for (const auto &face : geometry.faces)
            if (face.elementIndex == target.element && face.name == target.face) {
                found = true;
                int start = (4 - face.rotation / 90) % 4;
                auto a = face.uv[start] * 16, b = face.uv[(start + 2) % 4] * 16;
                int rotation = face.rotation;
                switch (operation) {
                case UvOperation::Move:
                    a += QVector2D(amount);
                    b += QVector2D(amount);
                    break;
                case UvOperation::MirrorU: {
                    auto x = a.x();
                    a.setX(b.x());
                    b.setX(x);
                    break;
                }
                case UvOperation::MirrorV: {
                    auto y = a.y();
                    a.setY(b.y());
                    b.setY(y);
                    break;
                }
                case UvOperation::Rotate:
                    rotation = (rotation + 90) % 360;
                    break;
                case UvOperation::Align: {
                    auto delta = QVector2D(amount) -
                                 QVector2D(std::min(a.x(), b.x()), std::min(a.y(), b.y()));
                    a += delta;
                    b += delta;
                    break;
                }
                case UvOperation::Snap: {
                    auto snap = [&](QVector2D v) {
                        return QVector2D(float(std::round(v.x() * texture.width() / 16.) * 16. /
                                               texture.width()),
                                         float(std::round(v.y() * texture.height() / 16.) * 16. /
                                               texture.height()));
                    };
                    a = snap(a);
                    b = snap(b);
                    break;
                }
                case UvOperation::Reset:
                    break;
                }
                bool ok = operation == UvOperation::Reset
                              ? resetUv(target.element, target.face, error)
                              : previewUv(target.element, target.face, {a.x(), a.y(), b.x(), b.y()},
                                          rotation, error);
                if (!ok) {
                    staged = before;
                    preview = wasPreview;
                    return false;
                }
                break;
            }
        if (!found) {
            staged = before;
            preview = wasPreview;
            return fail(error, "Selected face is unavailable; no UVs changed");
        }
    }
    return true;
}
bool ModelEdit::previewUvs(const QVector<UvTarget> &targets, QVector4D uv, int rotation,
                           QString *error) {
    if (targets.isEmpty() || targets.size() > 24576)
        return fail(error, "Select model faces first");
    const auto before = staged;
    const bool previous = preview;
    for (const auto &target : targets)
        if (!previewUv(target.element, target.face, uv, rotation, error)) {
            staged = before;
            preview = previous;
            return false;
        }
    return true;
}
QVector<UvState> uvStates(const QJsonObject &model, const QVector<UvTarget> &targets,
                          const QString &version) {
    QVector<UvState> out;
    if (targets.isEmpty() || targets.size() > 24576 || !Profile::forVersion(version).valid())
        return out;
    Model raw;
    raw.valid = true;
    raw.version = version;
    raw.source = raw.resolved = model;
    const auto geometry = mesh(raw);
    const auto elements = model["elements"].toArray();
    for (const auto &target : targets) {
        bool found = false;
        for (const auto &face : geometry.faces)
            if (face.elementIndex == target.element && face.name == target.face) {
                const int start = (4 - face.rotation / 90) % 4;
                const auto a = face.uv[start] * 16, b = face.uv[(start + 2) % 4] * 16;
                const auto json = elements[target.element].toObject()["faces"].toObject()[target.face]
                                      .toObject();
                out.append({{a.x(), a.y(), b.x(), b.y()}, face.rotation, !json.contains("uv")});
                found = true;
                break;
            }
        if (!found)
            return {};
    }
    return out;
}
bool ModelEdit::bulkUv(const QVector<UvTarget> &input, const UvBulk &op,
                       const std::function<QSize(const UvTarget &)> &textureSize,
                       const QString &version, QString *error) {
    QVector<UvTarget> targets;
    for (const auto &t : input)
        if (std::none_of(targets.begin(), targets.end(),
                         [&](const UvTarget &o) { return o.element == t.element && o.face == t.face; }))
            targets.append(t);
    if (targets.isEmpty() || targets.size() > 24576)
        return fail(error, "Select bounded model faces first");
    if (!std::isfinite(op.value.x()) || !std::isfinite(op.value.y()) || !std::isfinite(op.gap))
        return fail(error, "UV values must be finite");
    auto states = uvStates(staged, targets, version);
    if (states.size() != targets.size())
        return fail(error, "Selected face is unavailable; no UVs changed");
    const auto before = staged;
    const bool wasPreview = preview;
    auto rollback = [&](const QString &message = {}) {
        staged = before;
        preview = wasPreview;
        if (!message.isEmpty() && error)
            *error = message;
        return false;
    };
    auto topLeft = [](const QVector4D &u) {
        return QPointF(std::min(u.x(), u.z()), std::min(u.y(), u.w()));
    };
    QVector<int> order(targets.size());
    std::iota(order.begin(), order.end(), 0);
    if (op.kind == UvBulk::Distribute)
        std::stable_sort(order.begin(), order.end(), [&](int l, int r) {
            const auto a = topLeft(states[l].uv), b = topLeft(states[r].uv);
            return op.horizontal ? a.x() < b.x() : a.y() < b.y();
        });
    double cursor = states.isEmpty() ? 0
                    : op.horizontal  ? topLeft(states[order[0]].uv).x()
                                     : topLeft(states[order[0]].uv).y();
    QJsonArray pasted = op.clipboard["uv"].toArray();
    if (op.kind == UvBulk::Paste && pasted.size() != 4)
        return fail(error, "The UV clipboard is empty");
    if (op.kind == UvBulk::Size && (op.value.x() < 0 || op.value.y() < 0))
        return fail(error, "UV width and height must not be negative");
    for (int n : order) {
        const auto &target = targets[n];
        auto u = states[n].uv;
        int rotation = states[n].rotation;
        const auto tl = topLeft(u);
        switch (op.kind) {
        case UvBulk::Offset:
            u += QVector4D(op.value.x(), op.value.y(), op.value.x(), op.value.y());
            break;
        case UvBulk::Position: {
            const float dx = op.first ? float(op.value.x() - tl.x()) : 0,
                        dy = op.second ? float(op.value.y() - tl.y()) : 0;
            u += QVector4D(dx, dy, dx, dy);
            break;
        }
        case UvBulk::Size: {
            const float w = op.first ? float(op.value.x()) : std::abs(u.z() - u.x()),
                        h = op.second ? float(op.value.y()) : std::abs(u.w() - u.y());
            const bool normalU = u.x() <= u.z(), normalV = u.y() <= u.w();
            u = {normalU ? float(tl.x()) : float(tl.x()) + w, normalV ? float(tl.y()) : float(tl.y()) + h,
                 normalU ? float(tl.x()) + w : float(tl.x()), normalV ? float(tl.y()) + h : float(tl.y())};
            break;
        }
        case UvBulk::Distribute: {
            const double extent = op.horizontal ? std::abs(u.z() - u.x()) : std::abs(u.w() - u.y());
            const double d = cursor - (op.horizontal ? tl.x() : tl.y());
            if (op.horizontal)
                u += QVector4D(float(d), 0, float(d), 0);
            else
                u += QVector4D(0, float(d), 0, float(d));
            cursor += extent + op.gap;
            break;
        }
        case UvBulk::Rotate:
            rotation = (rotation + 90) % 360;
            break;
        case UvBulk::MirrorU:
            u = {u.z(), u.y(), u.x(), u.w()};
            break;
        case UvBulk::MirrorV:
            u = {u.x(), u.w(), u.z(), u.y()};
            break;
        case UvBulk::Snap: {
            auto size = textureSize ? textureSize(target) : QSize(16, 16);
            if (!size.isValid() || size.isEmpty())
                size = {16, 16};
            auto snap = [](float v, int pixels) {
                return float(std::round(double(v) * pixels / 16.) * 16. / pixels);
            };
            u = {snap(u.x(), size.width()), snap(u.y(), size.height()), snap(u.z(), size.width()),
                 snap(u.w(), size.height())};
            break;
        }
        case UvBulk::Paste:
            for (int i = 0; i < 4; ++i)
                if (!pasted[i].isDouble())
                    return rollback("The UV clipboard is invalid");
            u = {float(pasted[0].toDouble()), float(pasted[1].toDouble()),
                 float(pasted[2].toDouble()), float(pasted[3].toDouble())};
            rotation = op.clipboard["rotation"].toInt();
            break;
        case UvBulk::Reset:
        case UvBulk::Infer:
            break;
        }
        const bool ok = op.kind == UvBulk::Reset ? resetUv(target.element, target.face, error)
                                                 : previewUv(target.element, target.face, u, rotation, error);
        if (!ok)
            return rollback();
    }
    preview = true;
    return true;
}
} // namespace nova::mc
