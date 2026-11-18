// SPDX-License-Identifier: GPL-3.0-or-later
#include "special_models.h"
#include <cmath>
#include <numbers>

namespace nova::mc {
namespace {
QString material(const QJsonObject &renderer, const QString &prefix, const QString &fallback) {
    auto id = Location::parse(renderer["texture"].toString(fallback));
    if (id.valid())
        id.path.prepend("entity/" + prefix);
    return id.asset("textures");
}

struct EntityMesh {
    Mesh mesh;
    QString modelId;
    QString texture;
    float columns = 64, rows = 64;

    void box(const QString &name, QVector3D from, QVector3D size, float u, float v,
             const QMatrix4x4 &pose = {}, float inflate = 0) {
        const auto to = from + size + QVector3D(inflate, inflate, inflate);
        from -= QVector3D(inflate, inflate, inflate);
        const float x0 = from.x(), y0 = from.y(), z0 = from.z();
        const float x1 = to.x(), y1 = to.y(), z1 = to.z();
        const QVector3D points[] = {{x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0},
                                    {x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1}, {x1, y1, z1}};
        const float u1 = u + size.z(), u2 = u1 + size.x(), u3 = u2 + size.z(), u4 = u3 + size.x();
        const float v1 = v + size.z(), v2 = v1 + size.y();
        // The horizontal strip is west, north, east, south. End caps sit above it.
        const struct {
            const char *name;
            std::array<int, 4> corners;
            float left, top, right, bottom;
        } panels[] = {
            {"west",  {0, 4, 6, 2}, u, v1, u1, v2},
            {"north", {1, 0, 2, 3}, u1, v1, u2, v2},
            {"east",  {5, 1, 3, 7}, u2, v1, u3, v2},
            {"south", {4, 5, 7, 6}, u3, v1, u4, v2},
            {"down",  {5, 4, 0, 1}, u1, v, u2, v1},
            {"up",    {3, 2, 6, 7}, u2, v1, u2 + size.x(), v}};
        QMatrix4x4 transform;
        transform.scale(2);
        transform.translate(-.5f, -.5f, -.5f);
        // Poses are expressed in entity pixels, then converted to centred mesh units.
        transform.scale(1.f / 16);
        transform *= pose;
        const bool reflected = transform.determinant() < 0;
        for (const auto &panel : panels) {
            if (panel.left == panel.right || panel.top == panel.bottom)
                continue;
            Face face;
            face.name = panel.name;
            face.element = name;
            face.modelId = modelId;
            face.texture = texture;
            face.entityCutout = true;
            const float l = panel.left / columns, r = panel.right / columns,
                        t = panel.top / rows, b = panel.bottom / rows;
            face.uv = {{{r, t}, {l, t}, {l, b}, {r, b}}};
            for (int i = 0; i < 4; ++i)
                face.points[i] = transform.map(points[panel.corners[i]]);
            if (reflected) {
                std::reverse(face.points.begin(), face.points.end());
                std::reverse(face.uv.begin(), face.uv.end());
            }
            face.normal = face.lightNormal =
                QVector3D::normal(face.points[1] - face.points[0], face.points[2] - face.points[0]);
            if (mesh.faces.isEmpty())
                mesh.minimum = mesh.maximum = face.points[0];
            for (const auto &point : face.points) {
                for (int j = 0; j < 3; ++j) {
                    mesh.minimum[j] = std::min(mesh.minimum[j], point[j]);
                    mesh.maximum[j] = std::max(mesh.maximum[j], point[j]);
                }
            }
            mesh.faces << face;
        }
    }
};

void chest(EntityMesh &out, const QJsonObject &renderer) {
    out.texture = material(renderer, "chest/", "normal");
    out.box("Body", {1, 0, 1}, {14, 10, 14}, 0, 19);
    QMatrix4x4 lid;
    lid.translate(0, 9, 1);
    lid.rotate(-90.f * std::clamp(float(renderer["openness"].toDouble()), 0.f, 1.f), 1, 0, 0);
    out.box("Lid", {1, 0, 0}, {14, 5, 14}, 0, 0, lid);
    out.box("Latch", {7, -2, 14}, {2, 4, 1}, 0, 0, lid);
}

void shulker(EntityMesh &out, const QJsonObject &renderer) {
    out.texture = material(renderer, "shulker/", "shulker");
    QMatrix4x4 pose;
    pose.translate(8, 8, 8);
    pose.scale(.9995f);
    const auto facing = renderer["orientation"].toString("up");
    if (facing == "down")
        pose.rotate(180, 1, 0, 0);
    else if (facing != "up") {
        pose.rotate(90, 1, 0, 0);
        pose.rotate(facing == "north" ? 180 : facing == "east" ? -90 : facing == "west" ? 90 : 0,
                    0, 0, 1);
    }
    pose.scale(1, -1, -1);
    pose.translate(0, -16, 0);
    auto base = pose;
    base.translate(0, 24, 0);
    out.box("Base", {-8, -8, -8}, {16, 8, 16}, 0, 28, base);

    const float open = std::clamp(float(renderer["openness"].toDouble()), 0.f, 1.f);
    const float height = std::cos(open * std::numbers::pi_v<float>);
    pose.translate(0, 16 + 8 * height, 0);
    if (open > .3f) {
        const float turn = height - 1;
        pose.rotate(22.5f * turn * turn * turn * turn, 0, 1, 0);
    }
    out.box("Lid", {-8, -16, -8}, {16, 12, 16}, 0, 0, pose);
}

void bed(EntityMesh &out, const QJsonObject &renderer) {
    out.texture = material(renderer, "bed/", "red");
    const float legAngles[2][2] = {{90, 180}, {0, 270}};
    for (int half = 0; half < 2; ++half) {
        QMatrix4x4 pose;
        pose.translate(0, 9, -16.f * half);
        pose.rotate(90, 1, 0, 0);
        pose.translate(8, 8, 8);
        pose.rotate(180, 0, 0, 1);
        pose.translate(-8, -8, -8);
        out.box(half ? "Foot" : "Head", {0, 0, 0}, {16, 16, 6}, 0, 22.f * half, pose);
        for (int leg = 0; leg < 2; ++leg) {
            auto legPose = pose;
            legPose.rotate(legAngles[half][leg], 0, 0, 1);
            legPose.rotate(90, 1, 0, 0);
            out.box("Leg", {-16.f * leg, 6, -16.f * half}, {3, 3, 3}, 50,
                    (half ? 0.f : 6.f) + 12 * leg, legPose);
        }
    }
}

bool head(EntityMesh &out, const QJsonObject &renderer, bool playerHead) {
    const auto kind = playerHead ? QString("player") : renderer["kind"].toString();
    static const QHash<QString, QString> skins{
        {"player", "player/wide/steve"}, {"zombie", "zombie/zombie"},
        {"skeleton", "skeleton/skeleton"}, {"wither_skeleton", "skeleton/wither_skeleton"},
        {"creeper", "creeper/creeper"}};
    if (!skins.contains(kind))
        return false;
    out.texture = material(renderer, {}, skins.value(kind));
    const bool outerLayer = kind == "player" || kind == "zombie";
    out.rows = outerLayer ? 64.f : 32.f;
    QMatrix4x4 pose;
    pose.translate(8, 0, 8);
    pose.scale(-1, -1, 1);
    pose.rotate(180, 0, 1, 0);
    out.box("Head", {-4, -8, -4}, {8, 8, 8}, 0, 0, pose);
    if (outerLayer)
        out.box("Outer layer", {-4, -8, -4}, {8, 8, 8}, 32, 0, pose, .25f);
    return true;
}
} // namespace

Mesh specialMesh(const Model &base, const QJsonObject &renderer) {
    EntityMesh out;
    out.modelId = base.id;
    out.mesh.diagnostics = base.diagnostics;
    out.mesh.guiLight = base.guiLight;
    if (!base.valid)
        return out.mesh;
    const auto type = Location::parse(renderer["type"].toString()).id();
    if (type == "minecraft:chest")
        chest(out, renderer);
    else if (type == "minecraft:shulker_box")
        shulker(out, renderer);
    else if (type == "minecraft:bed")
        bed(out, renderer);
    else if ((type == "minecraft:head" || type == "minecraft:player_head") &&
             head(out, renderer, type == "minecraft:player_head")) {
        // Baked above.
    } else if (type == "minecraft:conduit") {
        out.texture = material(renderer, "conduit/", "base");
        out.columns = 32;
        out.rows = 16;
        out.box("Shell", {5, 5, 5}, {6, 6, 6}, 0, 0);
    } else if (type == "minecraft:shield") {
        out.texture = material(renderer, {}, "shield_base_nopattern");
        QMatrix4x4 pose;
        pose.scale(1, -1, -1);
        out.box("Plate", {-6, -11, -2}, {12, 22, 1}, 0, 0, pose);
        out.box("Handle", {-1, -3, -1}, {2, 6, 6}, 26, 0, pose);
    } else {
        out.mesh.diagnostics << "Unsupported special model renderer: " + type +
                                    (type == "minecraft:head" ? " / " + renderer["kind"].toString() : QString());
    }
    return out.mesh;
}
} // namespace nova::mc
