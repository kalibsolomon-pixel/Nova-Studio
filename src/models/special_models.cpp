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
    enum class BoxFaces { All, Mirrored, North };
    Mesh mesh;
    QString modelId;
    QString texture;
    float columns = 64, rows = 64;
    EntityLayer layer = EntityLayer::Cutout;

    void box(const QString &name, QVector3D from, QVector3D size, float u, float v,
             const QMatrix4x4 &pose = {}, float inflate = 0, BoxFaces faces = BoxFaces::All) {
        const bool mirror = faces == BoxFaces::Mirrored;
        const auto to = from + size + QVector3D(inflate, inflate, inflate);
        from -= QVector3D(inflate, inflate, inflate);
        const float x0 = mirror ? to.x() : from.x(), y0 = from.y(), z0 = from.z();
        const float x1 = mirror ? from.x() : to.x(), y1 = to.y(), z1 = to.z();
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
        const bool reflected = (transform.determinant() < 0) != mirror;
        for (const auto &panel : panels) {
            if (faces == BoxFaces::North && panel.name != QLatin1String("north"))
                continue;
            if (panel.left == panel.right || panel.top == panel.bottom)
                continue;
            Face face;
            face.name = panel.name;
            face.element = name;
            face.modelId = modelId;
            face.texture = texture;
            face.entityLayer = layer;
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
    out.layer = EntityLayer::Solid;
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

void banner(EntityMesh &out, const QJsonObject &renderer) {
    // DyeColor texture diffuse RGBs. The pole and bar use the undyed banner sheet.
    static const QHash<QString, QRgb> dyes{
        {"white", 16383998}, {"orange", 16351261}, {"magenta", 13061821},
        {"light_blue", 3847130}, {"yellow", 16701501}, {"lime", 8439583},
        {"pink", 15961002}, {"gray", 4673362}, {"light_gray", 10329495},
        {"cyan", 1481884}, {"purple", 8991416}, {"blue", 3949738},
        {"brown", 8606770}, {"green", 6192150}, {"red", 11546150}, {"black", 1908001}};
    const auto color = dyes.constFind(renderer["color"].toString("white"));
    if (color == dyes.cend()) {
        out.mesh.diagnostics << "Invalid banner dye color";
        return;
    }
    out.layer = EntityLayer::Solid;
    out.texture = "assets/minecraft/textures/entity/banner_base.png";
    QMatrix4x4 stand;
    constexpr float scale = 2.f / 3.f;
    stand.translate(8, 0, 8);
    stand.scale(scale, -scale, -scale);
    out.box("Pole", {-1, -42, -1}, {2, 42, 2}, 44, 0, stand);
    out.box("Bar", {-10, -44, -1}, {20, 2, 2}, 0, 42, stand);
    QMatrix4x4 cloth = stand;
    cloth.translate(0, -44, 0);
    cloth.rotate(-.45f, 1, 0, 0); // Standing flag at phase zero.
    out.box("Cloth", {-10, 0, -2}, {20, 40, 1}, 0, 0, cloth);

    const auto overlay = out.mesh.faces.size();
    out.layer = EntityLayer::Overlay;
    out.texture = "assets/minecraft/textures/entity/banner/base.png";
    out.box("Cloth dye", {-10, 0, -2}, {20, 40, 1}, 0, 0, cloth);
    for (auto i = overlay; i < out.mesh.faces.size(); ++i)
        out.mesh.faces[i].color = 0xff000000u | *color;
}

void decoratedPot(EntityMesh &out) {
    out.layer = EntityLayer::Solid;
    out.columns = out.rows = 32;
    out.texture = "assets/minecraft/textures/entity/decorated_pot/decorated_pot_base.png";
    QMatrix4x4 neck;
    neck.translate(0, 37, 16);
    neck.rotate(180, 1, 0, 0);
    out.box("Collar", {4, 17, 4}, {8, 3, 8}, 0, 0, neck, -.1f);
    out.box("Lip", {5, 20, 5}, {6, 1, 6}, 0, 5, neck, .2f);
    out.box("Top", {1, 16, 1}, {14, 0, 14}, -14, 13);
    out.box("Bottom", {1, 0, 1}, {14, 0, 14}, -14, 13);

    out.columns = out.rows = 16;
    out.texture = "assets/minecraft/textures/entity/decorated_pot/decorated_pot_side.png";
    // All four sides use the same upright, north-facing panel before rotation.
    constexpr struct {
        const char *name;
        float x, z, yAngle, xAngle, zAngle;
    } sides[] = {
        {"North", 15, 1, 0, 0, 180}, {"West", 1, 1, -90, 0, 180},
        {"East", 15, 15, 90, 0, 180}, {"South", 1, 15, 0, 180, 0}};
    for (const auto &side : sides) {
        QMatrix4x4 pose;
        pose.translate(side.x, 16, side.z);
        pose.rotate(side.zAngle, 0, 0, 1);
        pose.rotate(side.yAngle, 0, 1, 0);
        pose.rotate(side.xAngle, 1, 0, 0);
        out.box(side.name, {}, {14, 16, 0}, 1, 0, pose, 0, EntityMesh::BoxFaces::North);
    }
}

void dragonHead(EntityMesh &out, QMatrix4x4 pose, float animation) {
    out.columns = out.rows = 256;
    pose.scale(.75f);
    pose.translate(0, -7.986666f, 0);
    out.box("Snout", {-6, -1, -24}, {12, 5, 16}, 176, 44, pose);
    out.box("Head", {-8, -8, -10}, {16, 16, 16}, 112, 30, pose);
    for (const bool mirror : {true, false}) {
        const float x = mirror ? -5.f : 3.f;
        const auto faces = mirror ? EntityMesh::BoxFaces::Mirrored : EntityMesh::BoxFaces::All;
        out.box("Horn", {x, -12, -4}, {2, 4, 6}, 0, 0, pose, 0, faces);
        out.box("Nostril", {x, -3, -22}, {2, 2, 4}, 112, 0, pose, 0, faces);
    }
    pose.translate(0, 4, -8);
    const float phase = animation * std::numbers::pi_v<float> * .2f;
    pose.rotate((1.f + std::sin(phase)) * .2f * 180.f / std::numbers::pi_v<float>, 1, 0, 0);
    out.box("Jaw", {-6, 0, -16}, {12, 4, 16}, 176, 65, pose);
}

void piglinHead(EntityMesh &out, const QMatrix4x4 &pose, float animation) {
    out.columns = out.rows = 64;
    out.box("Head", {-5, -8, -4}, {10, 8, 8}, 0, 0, pose);
    out.box("Snout", {-2, -4, -5}, {4, 4, 1}, 31, 1, pose);
    out.box("Left tusk", {2, -2, -5}, {1, 2, 1}, 2, 4, pose);
    out.box("Right tusk", {-3, -2, -5}, {1, 2, 1}, 2, 0, pose);
    for (const bool left : {true, false}) {
        const float phase = animation * std::numbers::pi_v<float> * .2f * (left ? 1.2f : 1.f);
        const float angle = .2f * (2.5f + std::cos(phase)) * (left ? -1.f : 1.f);
        auto ear = pose;
        ear.translate(left ? 4.5f : -4.5f, -6, 0);
        ear.rotate(angle * 180.f / std::numbers::pi_v<float>, 0, 0, 1);
        out.box(left ? "Left ear" : "Right ear", {left ? 0.f : -1.f, 0, -2},
                {1, 5, 4}, left ? 51.f : 39.f, 6, ear);
    }
}

void copperGolemStatue(EntityMesh &out, const QJsonObject &renderer) {
    const auto kind = renderer["pose"].toString("standing");
    if (kind != "standing") {
        out.mesh.diagnostics << "Unsupported copper golem statue pose: " + kind;
        return;
    }
    auto skin = Location::parse(renderer["texture"].toString(
        "minecraft:textures/entity/copper_golem/copper_golem.png"));
    if (!skin.valid() || !skin.path.startsWith("textures/") || !skin.path.endsWith(".png")) {
        out.mesh.diagnostics << "Invalid copper golem statue texture";
        return;
    }
    out.texture = "assets/" + skin.nameSpace + '/' + skin.path;
    out.layer = EntityLayer::CutoutNoCull;
    QMatrix4x4 root;
    // The south-facing statue resets the mob's root Y and turns it 180° about Y and Z.
    root.translate(8, 24, 8);
    root.scale(-1, 1, -1);
    auto body = root;
    body.translate(0, -5, 0);
    out.box("Body", {-4, -6, -3}, {8, 6, 6}, 0, 15, body);
    auto head = body;
    head.translate(0, -6, 0);
    out.box("Head", {-4, -5, -5}, {8, 5, 10}, 0, 0, head, .015f);
    out.box("Nose", {-1, -2, -6}, {2, 3, 2}, 56, 0, head);
    out.box("Antenna", {-1, -9, -1}, {2, 4, 2}, 37, 8, head, -.015f);
    out.box("Antenna tip", {-2, -13, -2}, {4, 4, 4}, 37, 0, head, -.015f);
    for (const bool left : {false, true}) {
        auto arm = head;
        arm.translate(left ? 4.f : -4.f, 0, 0);
        out.box(left ? "Left arm" : "Right arm", {left ? 0.f : -3.f, -1, -2},
                {3, 10, 4}, left ? 50.f : 36.f, 16, arm);
        out.box(left ? "Left leg" : "Right leg", {left ? 0.f : -4.f, 0, -2},
                {4, 5, 4}, left ? 16.f : 0.f, 27, body);
    }
}

bool head(EntityMesh &out, const QJsonObject &renderer, bool playerHead) {
    const auto kind = playerHead ? QString("player") : renderer["kind"].toString();
    static const QHash<QString, QString> skins{
        {"player", "player/wide/steve"}, {"zombie", "zombie/zombie"},
        {"skeleton", "skeleton/skeleton"}, {"wither_skeleton", "skeleton/wither_skeleton"},
        {"creeper", "creeper/creeper"}, {"dragon", "enderdragon/dragon"},
        {"piglin", "piglin/piglin"}};
    if (!skins.contains(kind))
        return false;
    out.texture = material(renderer, {}, skins.value(kind));
    const bool outerLayer = kind == "player" || kind == "zombie";
    out.rows = outerLayer ? 64.f : 32.f;
    QMatrix4x4 pose;
    pose.translate(8, 0, 8);
    pose.scale(-1, -1, 1);
    pose.rotate(180, 0, 1, 0);
    if (kind == "dragon" || kind == "piglin") {
        out.layer = EntityLayer::CutoutNoCull;
        const auto animation = float(renderer["animation"].toDouble());
        if (kind == "dragon")
            dragonHead(out, pose, animation);
        else
            piglinHead(out, pose, animation);
        return true;
    }
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
    else if (type == "minecraft:banner")
        banner(out, renderer);
    else if (type == "minecraft:decorated_pot")
        decoratedPot(out);
    else if (type == "minecraft:copper_golem_statue")
        copperGolemStatue(out, renderer);
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
