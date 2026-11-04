// SPDX-License-Identifier: GPL-3.0-or-later
#include "models.h"
#include <QJsonArray>
#include <QQuaternion>
#include <cmath>
namespace nova::mc {
bool itemTransform(const QJsonValue &value, QMatrix4x4 &out, QString *error) {
    auto fail = [&] {
        if (error)
            *error =
                "Invalid item transformation (finite affine matrix or decomposed TRSR required)";
        return false;
    };
    out.setToIdentity();
    if (value.isUndefined())
        return true;
    auto numeric = [](const QJsonValue &v) {
        return v.isDouble() && std::isfinite(v.toDouble()) && std::abs(v.toDouble()) <= 10000;
    };
    if (value.isArray()) {
        auto a = value.toArray();
        if (a.size() != 16)
            return fail();
        for (int i = 0; i < 16; ++i) {
            if (!numeric(a[i]))
                return fail();
            out(i / 4, i % 4) = float(a[i].toDouble());
        }
        if (!out.isAffine())
            return fail();
        return true;
    }
    if (!value.isObject())
        return fail();
    const auto o = value.toObject();
    auto vec = [&](const char *key, QVector3D &v, bool scalar = false) {
        if (!o.contains(key))
            return true;
        auto a = o[key].toArray();
        if (scalar && numeric(o[key])) {
            v = QVector3D(1, 1, 1) * float(o[key].toDouble());
            return true;
        }
        if (a.size() != 3)
            return false;
        for (int i = 0; i < 3; ++i) {
            if (!numeric(a[i]))
                return false;
            v[i] = float(a[i].toDouble());
        }
        return true;
    };
    auto rotation = [&](const char *key, QQuaternion &q) {
        if (!o.contains(key))
            return true;
        auto a = o[key].toArray();
        if (a.size() == 4) {
            for (auto n : a)
                if (!numeric(n))
                    return false;
            q = QQuaternion(float(a[3].toDouble()), float(a[0].toDouble()), float(a[1].toDouble()),
                            float(a[2].toDouble()));
            if (q.lengthSquared() < 1e-12)
                return false;
            q.normalize();
            return true;
        }
        auto r = o[key].toObject();
        auto axis = r["axis"].toArray();
        if (axis.size() != 3 || !numeric(r["angle"]))
            return false;
        QVector3D v;
        for (int i = 0; i < 3; ++i) {
            if (!numeric(axis[i]))
                return false;
            v[i] = float(axis[i].toDouble());
        }
        if (v.lengthSquared() < 1e-12)
            return false;
        q = QQuaternion::fromAxisAndAngle(v.normalized(),
                                          float(r["angle"].toDouble() * 180 / 3.141592653589793));
        return true;
    };
    QVector3D t, s(1, 1, 1);
    QQuaternion left, right;
    if (!vec("translation", t) || !vec("scale", s, true) || !rotation("left_rotation", left) ||
        !rotation("right_rotation", right))
        return fail();
    out.translate(t);
    out.rotate(left);
    out.scale(s);
    out.rotate(right);
    return true;
}
Mesh applyTransform(Mesh mesh, const QMatrix4x4 &matrix) {
    for (auto &f : mesh.faces) {
        for (auto &p : f.points)
            p = matrix.map(p);
        if (matrix.determinant() < 0) {
            std::swap(f.points[0], f.points[3]);
            std::swap(f.points[1], f.points[2]);
            std::swap(f.uv[0], f.uv[3]);
            std::swap(f.uv[1], f.uv[2]);
        }
        f.normal = QVector3D::crossProduct(f.points[1] - f.points[0], f.points[2] - f.points[0])
                       .normalized();
    }
    bool first = true;
    for (const auto &f : mesh.faces)
        for (const auto &p : f.points) {
            if (first) {
                mesh.minimum = mesh.maximum = p;
                first = false;
            }
            for (int i = 0; i < 3; ++i) {
                mesh.minimum[i] = std::min(mesh.minimum[i], p[i]);
                mesh.maximum[i] = std::max(mesh.maximum[i], p[i]);
            }
        }
    return mesh;
}
} // namespace nova::mc
