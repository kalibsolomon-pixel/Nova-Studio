// SPDX-License-Identifier: GPL-3.0-or-later
#include "models.h"
#include "display.h"
#include <QJsonArray>
#include <cmath>

namespace nova::mc {
namespace {
bool vector(const QJsonValue &value, QVector3D &out, float low = -1e4, float high = 1e4) {
    const auto a = value.toArray();
    if (a.size() != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (!a[i].isDouble() || !std::isfinite(a[i].toDouble()) || a[i].toDouble() < low ||
            a[i].toDouble() > high)
            return false;
        out[i] = float(a[i].toDouble());
    }
    return true;
}
const QStringList directions{"down", "up", "north", "south", "west", "east"};
QVector3D normal(const QString &name) {
    static const QVector3D n[] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                  {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
    const auto i = directions.indexOf(name);
    return i < 0 ? QVector3D(0, 1, 0) : n[i];
}
void bounds(Mesh &mesh) {
    bool first = true;
    for (const auto &face : mesh.faces)
        for (const auto &p : face.points) {
            if (first) {
                mesh.minimum = mesh.maximum = p;
                first = false;
            }
            for (int i = 0; i < 3; ++i) {
                mesh.minimum[i] = std::min(mesh.minimum[i], p[i]);
                mesh.maximum[i] = std::max(mesh.maximum[i], p[i]);
            }
        }
}
} // namespace
QRectF Face::uvRect() const {
    float x1 = uv[0].x(), x2 = x1, y1 = uv[0].y(), y2 = y1;
    for (const auto &p : uv) {
        x1 = std::min(x1, p.x());
        x2 = std::max(x2, p.x());
        y1 = std::min(y1, p.y());
        y2 = std::max(y2, p.y());
    }
    return {x1 * 16, y1 * 16, (x2 - x1) * 16, (y2 - y1) * 16};
}
Mesh mesh(const Model &model) {
    Mesh mesh;
    mesh.diagnostics = model.diagnostics;
    if (!model.valid)
        return mesh;
    if (model.generated && model.resolved["elements"].toArray().isEmpty()) {
        // Flat layered generated-item approximation. Alpha contour extrusion is explicit scope.
        mesh.diagnostics << "Generated item: flat layers; contour extrusion is not reproduced";
        for (int layer = 0; layer < 5; ++layer) {
            const auto texture = model.textures.value("layer" + QString::number(layer));
            if (texture.isEmpty())
                continue;
            Face f;
            f.texture = texture;
            f.name = "south";
            f.element = "generated layer " + QString::number(layer);
            f.normal = {0, 0, 1};
            f.shade = false;
            float z = float(layer) * .001f;
            f.points = {{{-1, 1, z}, {-1, -1, z}, {1, -1, z}, {1, 1, z}}};
            f.uv = {{{0, 0}, {0, 1}, {1, 1}, {1, 0}}};
            mesh.faces << f;
            std::swap(f.points[0], f.points[3]);
            std::swap(f.points[1], f.points[2]);
            std::swap(f.uv[0], f.uv[3]);
            std::swap(f.uv[1], f.uv[2]);
            f.normal = {0, 0, -1};
            f.name = "north";
            mesh.faces << f;
        }
        bounds(mesh);
        return mesh;
    }
    const auto profile = Profile::forVersion(model.version);
    int index = 0;
    for (const auto &value : model.resolved["elements"].toArray()) {
        const auto e = value.toObject();
        if(e.size()>64 || e["faces"].toObject().size()>6){mesh.diagnostics<<"Element field/face allocation limit";continue;}
        QVector3D from, to;
        ++index;
        auto fail = [&](const QString &s) {
            mesh.diagnostics << QString("Element %1: %2").arg(index).arg(s);
        };
        if (!value.isObject() || !vector(e["from"], from, -16, 32) ||
            !vector(e["to"], to, -16, 32) || from.x() > to.x() || from.y() > to.y() ||
            from.z() > to.z() || !e["faces"].isObject()) {
            fail("invalid cuboid coordinates/faces");
            continue;
        }
        bool shade = true;
        if (e.contains("shade")) {
            if (profile.shadeOverride) {
                fail("shade was removed in 26.3; use shade_direction_override");
                continue;
            }
            if (!e["shade"].isBool()) {
                fail("shade must be boolean");
                continue;
            }
            shade = e["shade"].toBool();
        }
        const auto override = e["shade_direction_override"].toString();
        if (e.contains("shade_direction_override") &&
            (!profile.shadeOverride || !directions.contains(override))) {
            fail("unsupported shade_direction_override");
            continue;
        }
        for (auto it = e.begin(); it != e.end(); ++it)
            if (!QStringList{"from", "to", "faces", "rotation", "shade", "shade_direction_override",
                             "name", "light_emission"}
                     .contains(it.key()))
                fail("unsupported field retained: " + it.key());
        if (e["light_emission"].toInt() != 0)
            fail("light_emission retained; neutral preview omits emissive lighting");
        QMatrix4x4 transform;
        if (e.contains("rotation")) {
            const auto r = e["rotation"].toObject();
            QVector3D origin, angles;
            if (!e["rotation"].isObject() || !vector(r["origin"], origin)) {
                fail("invalid rotation origin");
                continue;
            }
            if (r.contains("rescale") && !r["rescale"].isBool()) {
                fail("rescale must be boolean");
                continue;
            }
            for (auto it = r.begin(); it != r.end(); ++it)
                if (!QStringList{"origin", "x", "y", "z", "axis", "angle", "rescale"}.contains(
                        it.key()))
                    fail("unsupported rotation field retained: " + it.key());
            if (r.contains("axis") || r.contains("angle")) {
                const auto axis = r["axis"].toString();
                const int a = QStringList{"x", "y", "z"}.indexOf(axis);
                if (a < 0 || !r["angle"].isDouble() || !std::isfinite(r["angle"].toDouble()) ||
                    std::abs(r["angle"].toDouble()) > 36000) {
                    fail("invalid legacy rotation");
                    continue;
                }
                angles[a] = float(r["angle"].toDouble());
            } else {
                bool ok = true;
                int a = 0;
                for (const auto &key : {"x", "y", "z"}) {
                    if (r.contains(key) &&
                        (!r[key].isDouble() || !std::isfinite(r[key].toDouble()) ||
                         std::abs(r[key].toDouble()) > 36000))
                        ok = false;
                    angles[a++] = float(r[key].toDouble());
                }
                if (!ok) {
                    fail("invalid multi-axis rotation");
                    continue;
                }
            }
            transform.translate(origin);
            // QMatrix multiplies on the right: Z*Y*X applies X first.
            transform.rotate(angles.z(), 0, 0, 1);
            transform.rotate(angles.y(), 0, 1, 0);
            transform.rotate(angles.x(), 1, 0, 0);
            if (r["rescale"].toBool()) {
                // Verified modern codec: scale each rotated basis vector by the
                // reciprocal of its largest absolute component (also at >45°).
                QVector3D scale;
                for (int axis = 0; axis < 3; ++axis) {
                    QVector3D unit;
                    unit[axis] = 1;
                    auto basis = transform.mapVector(unit);
                    scale[axis] = 1 / std::max({std::abs(basis.x()), std::abs(basis.y()),
                                                std::abs(basis.z())});
                }
                transform.scale(scale);
            }
            transform.translate(-origin);
        }
        const float x1 = from.x(), y1 = from.y(), z1 = from.z(), x2 = to.x(), y2 = to.y(),
                    z2 = to.z();
        // Minecraft FaceInfo order: top-left, bottom-left, bottom-right, top-right.
        const std::array<QVector3D, 4> points[] = {
            {{{x1, y1, z2}, {x1, y1, z1}, {x2, y1, z1}, {x2, y1, z2}}},
            {{{x1, y2, z1}, {x1, y2, z2}, {x2, y2, z2}, {x2, y2, z1}}},
            {{{x2, y2, z1}, {x2, y1, z1}, {x1, y1, z1}, {x1, y2, z1}}},
            {{{x1, y2, z2}, {x1, y1, z2}, {x2, y1, z2}, {x2, y2, z2}}},
            {{{x1, y2, z1}, {x1, y1, z1}, {x1, y1, z2}, {x1, y2, z2}}},
            {{{x2, y2, z2}, {x2, y1, z2}, {x2, y1, z1}, {x2, y2, z1}}}};
        const std::array<float, 4> defaults[] = {
            {{x1, 16 - z2, x2, 16 - z1}},           {{x1, z1, x2, z2}},
            {{16 - x2, 16 - y2, 16 - x1, 16 - y1}}, {{x1, 16 - y2, x2, 16 - y1}},
            {{z1, 16 - y2, z2, 16 - y1}},           {{16 - z2, 16 - y2, 16 - z1, 16 - y1}}};
        const auto faces = e["faces"].toObject();
        for (auto it = faces.begin(); it != faces.end(); ++it) {
            int direction = int(directions.indexOf(it.key()));
            if (direction < 0 || !it.value().isObject()) {
                fail("invalid face declaration: " + it.key());
                continue;
            }
            const auto o = it.value().toObject();
            Face face;
            face.modelId = model.id;
            face.elementIndex = index - 1;
            face.name = it.key();
            face.element = e["name"].toString(QString("Element %1").arg(index));
            for (auto field = o.begin(); field != o.end(); ++field)
                if (!QStringList{"texture", "uv", "rotation", "tintindex", "cullface"}.contains(
                        field.key()))
                    fail("unsupported face field retained: " + field.key());
            if (o.contains("cullface") && !directions.contains(o["cullface"].toString())) {
                fail("invalid cullface");
                continue;
            }
            face.shade = shade;
            face.shadeDirection = override;
            face.cullface = o["cullface"].toString();
            face.tint = o["tintindex"].toInt(-1);
            const auto ref = o["texture"].toString();
            face.texture = ref.startsWith('#') ? model.textures.value(ref.mid(1))
                                               : Location::parse(ref).asset("textures");
            if (face.texture.isEmpty())
                fail("unresolved face texture: " + ref);
            auto texValue = model.resolved["textures"].toObject().value(ref.mid(1));
            QSet<QString> seen;
            while (texValue.isString() && texValue.toString().startsWith('#') &&
                   !seen.contains(texValue.toString())) {
                seen.insert(texValue.toString());
                texValue = model.resolved["textures"].toObject().value(texValue.toString().mid(1));
            }
            face.forceTranslucent = texValue.toObject()["force_translucent"].toBool();
            auto uv = defaults[direction];
            bool valid = true;
            if (o.contains("uv")) {
                auto values = o["uv"].toArray();
                valid = values.size() == 4;
                if (valid)
                    for (int a = 0; a < 4; ++a) {
                        valid &= values[a].isDouble() && std::isfinite(values[a].toDouble()) &&
                                 std::abs(values[a].toDouble()) <= 4096;
                        uv[a] = float(values[a].toDouble());
                    }
            }
            const auto rotation = o["rotation"];
            face.rotation = rotation.toInt();
            if (!valid || (!rotation.isUndefined() &&
                           (!rotation.isDouble() || rotation.toDouble() != face.rotation ||
                            face.rotation < 0 || face.rotation > 270 || face.rotation % 90))) {
                fail("invalid UV coordinates/rotation");
                continue;
            }
            const QVector2D coords[] = {{uv[0] / 16, uv[1] / 16},
                                        {uv[0] / 16, uv[3] / 16},
                                        {uv[2] / 16, uv[3] / 16},
                                        {uv[2] / 16, uv[1] / 16}};
            for (int a = 0; a < 4; ++a) {
                face.points[a] = (transform.map(points[direction][a]) - QVector3D(8, 8, 8)) / 8;
                face.uv[a] = coords[(a + face.rotation / 90) % 4];
            }
            face.normal = QVector3D::crossProduct(face.points[1] - face.points[0],
                                                  face.points[2] - face.points[0])
                              .normalized();
            mesh.faces << face;
        }
    }
    bounds(mesh);
    mesh.diagnostics.removeDuplicates();
    return mesh;
}
Mesh transformed(Mesh mesh, const Instance &instance) {
    QMatrix4x4 m;
    m.rotate(float(-instance.z), 0, 0, 1);
    m.rotate(float(-instance.y), 0, 1, 0);
    m.rotate(float(-instance.x), 1, 0, 0);
    const QVector3D u[] = {{1, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    const QVector3D v[] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0}};
    for (auto &f : mesh.faces) {
        if (instance.uvlock) {
            int original = int(directions.indexOf(f.name));
            if (original >= 0) {
                const auto n = m.mapVector(normal(f.name));
                int target = 0;
                float best = -2;
                for (int a = 0; a < 6; ++a) {
                    float dot = QVector3D::dotProduct(n, normal(directions[a]));
                    if (dot > best) {
                        target = a;
                        best = dot;
                    }
                }
                const float coords[4][2] = {{-.5f, -.5f}, {-.5f, .5f}, {.5f, .5f}, {.5f, -.5f}};
                auto before = f.uv;
                for (int a = 0; a < 4; ++a) {
                    auto p = m.mapVector(u[original] * coords[a][0] + v[original] * coords[a][1]);
                    bool right = QVector3D::dotProduct(p, u[target]) > 0,
                         bottom = QVector3D::dotProduct(p, v[target]) > 0;
                    int corner = right ? (bottom ? 2 : 3) : (bottom ? 1 : 0);
                    f.uv[a] = before[corner];
                }
            }
        }
        for (auto &p : f.points)
            p = m.map(p);
        f.normal = m.mapVector(f.normal).normalized();
    }
    bounds(mesh);
    return mesh;
}
QMatrix4x4 displayTransform(const Model &model, const QString &context) {
    // Per-context chain resolution (including the left-hand fallback) happens while
    // resolving the model; see display.h.
    const auto r = resolveDisplay(model, context);
    return displayMatrix(r.valid ? r.value : DisplayTransform{}, leftHandContext(context));
}
} // namespace nova::mc
