// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_preview.h"
#include <QColor>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <cmath>

namespace nova::mc {
namespace {

struct Builder {
    enum class Mirror { None, X };
    Mesh mesh;
    QString texture;
    float cols = 64.f;
    float rows = 64.f;
    EntityLayer layer = EntityLayer::Cutout;
    QRgb color = qRgb(255, 255, 255);

    void box(const QString &name, QVector3D from, QVector3D size, float u, float v,
             const QMatrix4x4 &pose = {}, float inflate = 0.f, Mirror mirrorMode = Mirror::None,
             bool onlyNorth = false) {
        const bool mirror = mirrorMode == Mirror::X;
        const QVector3D to = from + size + QVector3D(inflate, inflate, inflate);
        from -= QVector3D(inflate, inflate, inflate);
        const float x0 = mirror ? to.x() : from.x(), y0 = from.y(), z0 = from.z();
        const float x1 = mirror ? from.x() : to.x(), y1 = to.y(), z1 = to.z();
        const QVector3D pts[] = {
            {x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0},
            {x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1}, {x1, y1, z1},
        };
        const float u1 = u + size.z();
        const float u2 = u1 + size.x();
        const float u3 = u2 + size.z();
        const float u4 = u3 + size.x();
        const float v1 = v + size.z();
        const float v2 = v1 + size.y();
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
            {"up",    {3, 2, 6, 7}, u2, v1, u2 + size.x(), v},
        };
        QMatrix4x4 xf;
        xf.scale(2.f);
        xf.translate(-.5f, -.5f, -.5f);
        xf.scale(1.f / 16.f);
        xf *= pose;
        const bool reflected = (xf.determinant() < 0.f) != mirror;
        for (const auto &panel : panels) {
            if (onlyNorth && panel.name != QLatin1String("north"))
                continue;
            if (panel.left == panel.right || panel.top == panel.bottom)
                continue;
            Face face;
            face.name = QString::fromLatin1(panel.name);
            face.element = name;
            face.texture = texture;
            face.entityLayer = layer;
            face.color = color;
            const float l = panel.left / cols, r = panel.right / cols;
            const float t = panel.top / rows, b = panel.bottom / rows;
            face.uv = {{{r, t}, {l, t}, {l, b}, {r, b}}};
            for (int i = 0; i < 4; ++i)
                face.points[i] = xf.map(pts[panel.corners[size_t(i)]]);
            if (reflected) {
                std::reverse(face.points.begin(), face.points.end());
                std::reverse(face.uv.begin(), face.uv.end());
            }
            const auto cross = QVector3D::crossProduct(face.points[1] - face.points[0],
                                                       face.points[2] - face.points[0]);
            if (cross.lengthSquared() < 1e-12f)
                continue;
            face.normal = face.lightNormal = cross.normalized();
            if (mesh.faces.isEmpty())
                mesh.minimum = mesh.maximum = face.points[0];
            for (const auto &p : face.points) {
                for (int j = 0; j < 3; ++j) {
                    mesh.minimum[j] = std::min(mesh.minimum[j], p[j]);
                    mesh.maximum[j] = std::max(mesh.maximum[j], p[j]);
                }
            }
            mesh.faces << face;
        }
    }

    // Adds an explicit quad in Nova mesh coordinates with normalized UVs [0..1].
    void quad(const QString &element, const QString &faceName,
              const std::array<QVector3D, 4> &points, const std::array<QVector2D, 4> &uvs) {
        const auto cross = QVector3D::crossProduct(points[1] - points[0], points[2] - points[0]);
        if (cross.lengthSquared() < 1e-12f)
            return;
        Face face;
        face.element = element;
        face.name = faceName;
        face.texture = texture;
        face.entityLayer = layer;
        face.color = color;
        face.points = points;
        face.uv = uvs;
        face.normal = face.lightNormal = cross.normalized();
        if (mesh.faces.isEmpty())
            mesh.minimum = mesh.maximum = face.points[0];
        for (const auto &p : face.points) {
            for (int j = 0; j < 3; ++j) {
                mesh.minimum[j] = std::min(mesh.minimum[j], p[j]);
                mesh.maximum[j] = std::max(mesh.maximum[j], p[j]);
            }
        }
        mesh.faces << face;
    }
};

// Standard Minecraft Java Edition entity coordinate transform:
// Entity model pixels have +Y down, feet at y = groundY (default 24), face looking toward -Z.
// Maps feet to block y = 0 (mesh y = -1) and faces the entity toward +Z (the default camera).
QMatrix4x4 entityRoot(float groundY = 24.f, float scale = 1.f) {
    QMatrix4x4 m;
    m.translate(8.f, groundY * scale, 8.f);
    m.scale(-scale, -scale, scale);
    m.rotate(180.f, 0.f, 1.f, 0.f);
    return m;
}

QMatrix4x4 partPose(const QMatrix4x4 &parent, float px, float py, float pz,
                    float rxDeg = 0.f, float ryDeg = 0.f, float rzDeg = 0.f) {
    QMatrix4x4 m = parent;
    m.translate(px, py, pz);
    if (rzDeg != 0.f)
        m.rotate(rzDeg, 0.f, 0.f, 1.f);
    if (ryDeg != 0.f)
        m.rotate(ryDeg, 0.f, 1.f, 0.f);
    if (rxDeg != 0.f)
        m.rotate(rxDeg, 1.f, 0.f, 0.f);
    return m;
}

// ---- Clean synthetic mannequin / base textures when the library has no vanilla base ----

QImage fillRect(QImage img, int x, int y, int w, int h, QColor c) {
    for (int j = y; j < y + h && j < img.height(); ++j)
        for (int i = x; i < x + w && i < img.width(); ++i)
            if (i >= 0 && j >= 0)
                img.setPixelColor(i, j, c);
    return img;
}

QImage makeMannequinSkin() {
    // Crisp, neutral artist mannequin (64x64 player UV) with subtle anatomical tone
    // cues so worn armor, trims, and elytra read immediately even without a vanilla pack.
    QImage img(64, 64, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    const QColor headTone(168, 164, 158, 255);
    const QColor headShade(148, 144, 138, 255);
    const QColor torsoTone(132, 136, 144, 255);
    const QColor limbTone(146, 148, 154, 255);
    const QColor legTone(116, 120, 128, 255);
    const QColor eyeTone(88, 92, 100, 255);
    // Head (0..32, 0..16)
    img = fillRect(std::move(img), 0, 0, 32, 16, headTone);
    img = fillRect(std::move(img), 8, 0, 8, 8, headShade); // crown
    img = fillRect(std::move(img), 9, 11, 2, 1, eyeTone);  // subtle eyes on front face
    img = fillRect(std::move(img), 13, 11, 2, 1, eyeTone);
    // Torso (16..40, 16..32)
    img = fillRect(std::move(img), 16, 16, 24, 16, torsoTone);
    // Right arm (40..56, 16..32) and Left arm (32..48, 48..64)
    img = fillRect(std::move(img), 40, 16, 16, 16, limbTone);
    img = fillRect(std::move(img), 32, 48, 16, 16, limbTone);
    // Right leg (0..16, 16..32) and Left leg (16..32, 48..64)
    img = fillRect(std::move(img), 0, 16, 16, 16, legTone);
    img = fillRect(std::move(img), 16, 48, 16, 16, legTone);
    return img;
}

QImage makeSolidEntityTone(int w, int h, QColor primary, QColor accent = {}) {
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(primary);
    if (accent.isValid()) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (((x / 4) + (y / 4)) % 2 == 0)
                    img.setPixelColor(x, y, accent);
    }
    return img;
}

QImage makeArmorBaseTexture(bool leggings) {
    // Neutral brushed iron-slate base armor (64x32) used underneath armor trim previews.
    QImage img(64, 32, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    const QColor steel(142, 148, 158, 255);
    const QColor trimEdge(116, 122, 134, 255);
    if (!leggings) {
        // Helmet (0..32, 0..16), Body (16..40, 16..32), Arms (40..56, 16..32), Boots (0..16, 24..32)
        img = fillRect(std::move(img), 0, 0, 32, 16, steel);
        img = fillRect(std::move(img), 9, 10, 6, 4, Qt::transparent); // face visor opening
        img = fillRect(std::move(img), 16, 16, 24, 16, steel);
        img = fillRect(std::move(img), 40, 16, 16, 14, steel);
        img = fillRect(std::move(img), 0, 26, 16, 6, trimEdge);
        img = fillRect(std::move(img), 8, 16, 4, 4, trimEdge);
    } else {
        img = fillRect(std::move(img), 16, 20, 24, 12, steel);
        img = fillRect(std::move(img), 0, 16, 16, 14, steel);
    }
    return img;
}

QImage makeWoodBackTexture() {
    QImage img(16, 16, QImage::Format_RGBA8888);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const bool seam = (y % 4 == 0) || ((x + (y / 4) * 5) % 8 == 0);
            const int shade = seam ? -18 : ((x * 7 + y * 13) % 9 - 4);
            img.setPixelColor(x, y,
                              QColor(std::clamp(138 + shade, 0, 255),
                                     std::clamp(104 + shade, 0, 255),
                                     std::clamp(68 + shade, 0, 255), 255));
        }
    return img;
}

QString resolveCompanion(const QString &preferredAsset, const QString &syntheticKey,
                         const std::function<QImage()> &makeSynthetic, const Library *library,
                         TexturePreviewResult &out) {
    if (library && !preferredAsset.isEmpty() && library->hasAsset(preferredAsset)) {
        if (!out.referencedAssets.contains(preferredAsset))
            out.referencedAssets << preferredAsset;
        return preferredAsset;
    }
    if (!out.syntheticTextures.contains(syntheticKey))
        out.syntheticTextures.insert(syntheticKey, makeSynthetic());
    return syntheticKey;
}

// Companion lookup for armor layer 1 <-> layer 2.
QString companionArmorAsset(const QString &textureAsset, bool wantLeggings) {
    const auto loc = Location::parse(Location::fromAsset(textureAsset, "textures"));
    if (!loc.valid())
        return {};
    QString path = loc.path;
    if (wantLeggings) {
        if (path.startsWith("entity/equipment/humanoid/"))
            return Location{loc.nameSpace, "entity/equipment/humanoid_leggings/" + path.mid(26)}
                .asset("textures");
        if (path.startsWith("models/armor/") && path.endsWith("_layer_1"))
            return Location{loc.nameSpace, path.left(path.size() - 8) + "_layer_2"}
                .asset("textures");
        if (path.startsWith("models/armor/") && path.endsWith("_layer_1_overlay"))
            return Location{loc.nameSpace, path.left(path.size() - 16) + "_layer_2_overlay"}
                .asset("textures");
    } else {
        if (path.startsWith("entity/equipment/humanoid_leggings/"))
            return Location{loc.nameSpace, "entity/equipment/humanoid/" + path.mid(35)}
                .asset("textures");
        if (path.startsWith("models/armor/") && path.endsWith("_layer_2"))
            return Location{loc.nameSpace, path.left(path.size() - 8) + "_layer_1"}
                .asset("textures");
        if (path.startsWith("models/armor/") && path.endsWith("_layer_2_overlay"))
            return Location{loc.nameSpace, path.left(path.size() - 16) + "_layer_1_overlay"}
                .asset("textures");
    }
    return {};
}

// ---- Reusable Entity Geometry Builders (authentic Java Edition proportions & UVs) ----

void addPlayerBody(Builder &b, const QMatrix4x4 &root, bool slim, bool legacy64x32,
                   bool outerLayers, const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = legacy64x32 ? 32.f : 64.f;
    const float armW = slim ? 3.f : 4.f;
    const float rArmX = slim ? -2.f : -3.f;
    const float lArmX = -1.f;
    const auto head = partPose(root, 0, 0, 0);
    const auto body = partPose(root, 0, 0, 0);
    const auto rArm = partPose(root, -5.f, slim ? 2.5f : 2.f, 0);
    const auto lArm = partPose(root, 5.f, slim ? 2.5f : 2.f, 0);
    const auto rLeg = partPose(root, -1.9f, 12.f, 0);
    const auto lLeg = partPose(root, 1.9f, 12.f, 0);

    b.box(prefix + "Head", {-4, -8, -4}, {8, 8, 8}, 0, 0, head);
    b.box(prefix + "Body", {-4, 0, -2}, {8, 12, 4}, 16, 16, body);
    b.box(prefix + "Right arm", {rArmX, -2, -2}, {armW, 12, 4}, 40, 16, rArm);
    if (legacy64x32) {
        b.box(prefix + "Left arm", {lArmX, -2, -2}, {armW, 12, 4}, 40, 16, lArm, 0.f,
              Builder::Mirror::X);
        b.box(prefix + "Right leg", {-2, 0, -2}, {4, 12, 4}, 0, 16, rLeg);
        b.box(prefix + "Left leg", {-2, 0, -2}, {4, 12, 4}, 0, 16, lLeg, 0.f, Builder::Mirror::X);
    } else {
        b.box(prefix + "Left arm", {lArmX, -2, -2}, {armW, 12, 4}, 32, 48, lArm);
        b.box(prefix + "Right leg", {-2, 0, -2}, {4, 12, 4}, 0, 16, rLeg);
        b.box(prefix + "Left leg", {-2, 0, -2}, {4, 12, 4}, 16, 48, lLeg);
    }
    if (outerLayers) {
        b.box(prefix + "Hat", {-4, -8, -4}, {8, 8, 8}, 32, 0, head, .5f);
        if (!legacy64x32) {
            b.box(prefix + "Jacket", {-4, 0, -2}, {8, 12, 4}, 16, 32, body, .25f);
            b.box(prefix + "Right sleeve", {rArmX, -2, -2}, {armW, 12, 4}, 40, 32, rArm, .25f);
            b.box(prefix + "Left sleeve", {lArmX, -2, -2}, {armW, 12, 4}, 48, 48, lArm, .25f);
            b.box(prefix + "Right pants", {-2, 0, -2}, {4, 12, 4}, 0, 32, rLeg, .25f);
            b.box(prefix + "Left pants", {-2, 0, -2}, {4, 12, 4}, 0, 48, lLeg, .25f);
        }
    }
}

void addArmorOuter(Builder &b, const QMatrix4x4 &root, float extraInflate = 0.f,
                   const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = 32.f;
    const float inf = 1.0f + extraInflate;
    const auto head = partPose(root, 0, 0, 0);
    const auto body = partPose(root, 0, 0, 0);
    const auto rArm = partPose(root, -5.f, 2.f, 0);
    const auto lArm = partPose(root, 5.f, 2.f, 0);
    const auto rLeg = partPose(root, -1.9f, 12.f, 0);
    const auto lLeg = partPose(root, 1.9f, 12.f, 0);
    b.box(prefix + "Helmet", {-4, -8, -4}, {8, 8, 8}, 0, 0, head, inf);
    b.box(prefix + "Helmet overlay", {-4, -8, -4}, {8, 8, 8}, 32, 0, head, inf + .5f);
    b.box(prefix + "Chestplate", {-4, 0, -2}, {8, 12, 4}, 16, 16, body, inf + .01f);
    b.box(prefix + "Right shoulder", {-3, -2, -2}, {4, 12, 4}, 40, 16, rArm, inf);
    b.box(prefix + "Left shoulder", {-1, -2, -2}, {4, 12, 4}, 40, 16, lArm, inf,
          Builder::Mirror::X);
    b.box(prefix + "Right boot", {-2, 0, -2}, {4, 12, 4}, 0, 16, rLeg, inf);
    b.box(prefix + "Left boot", {-2, 0, -2}, {4, 12, 4}, 0, 16, lLeg, inf, Builder::Mirror::X);
}

void addArmorLeggings(Builder &b, const QMatrix4x4 &root, float extraInflate = 0.f,
                      const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = 32.f;
    const float inf = 0.5f + extraInflate;
    const auto body = partPose(root, 0, 0, 0);
    const auto rLeg = partPose(root, -1.9f, 12.f, 0);
    const auto lLeg = partPose(root, 1.9f, 12.f, 0);
    b.box(prefix + "Waist", {-4, 0, -2}, {8, 12, 4}, 16, 16, body, inf + .01f);
    b.box(prefix + "Right legging", {-2, 0, -2}, {4, 12, 4}, 0, 16, rLeg, inf);
    b.box(prefix + "Left legging", {-2, 0, -2}, {4, 12, 4}, 0, 16, lLeg, inf, Builder::Mirror::X);
}

void addElytraWings(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 32.f;
    const auto leftWing = partPose(root, 5.f, 0.f, 2.f, 15.f, 5.f, -15.f);
    const auto rightWing = partPose(root, -5.f, 0.f, 2.f, 15.f, -5.f, 15.f);
    b.box("Left wing", {-10, 0, 0}, {10, 20, 2}, 22, 0, leftWing, 1.f);
    b.box("Right wing", {0, 0, 0}, {10, 20, 2}, 22, 0, rightWing, 1.f, Builder::Mirror::X);
}

void addHorse(Builder &b, const QMatrix4x4 &root, float inflate = 0.f, const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto body = partPose(root, 0, 11, 5);
    b.box(prefix + "Body", {-5, -8, -17}, {10, 10, 22}, 0, 32, body, inflate);
    const auto tail = partPose(body, 0, -5, 5, 30.f, 0, 0);
    b.box(prefix + "Tail", {-1.5f, 0, 0}, {3, 14, 4}, 42, 36, tail, inflate);
    const auto neck = partPose(root, 0, 4, -12, 30.f, 0, 0);
    b.box(prefix + "Neck", {-2.05f, -6, -2}, {4, 12, 7}, 0, 35, neck, inflate);
    b.box(prefix + "Mane", {-1, -11, 5.01f}, {2, 16, 2}, 56, 36, neck, inflate);
    b.box(prefix + "Head", {-3, -11, -2}, {6, 5, 7}, 0, 13, neck, inflate);
    b.box(prefix + "Muzzle", {-2, -11, -7}, {4, 5, 5}, 0, 25, neck, inflate);
    b.box(prefix + "Left ear", {0.55f, -13, 3.99f}, {2, 3, 1}, 19, 16, neck, inflate);
    b.box(prefix + "Right ear", {-2.55f, -13, 3.99f}, {2, 3, 1}, 19, 16, neck, inflate);
    const struct {
        const char *name;
        float x, z;
    } legs[] = {{"Left hind leg", 4, 7}, {"Right hind leg", -4, 7},
                {"Left front leg", 4, -10}, {"Right front leg", -4, -10}};
    for (const auto &leg : legs) {
        const auto p = partPose(root, leg.x, 14, leg.z);
        b.box(prefix + leg.name, {-2, 0, -2}, {4, 10, 4}, 48, 21, p, inflate);
    }
}

void addWolf(Builder &b, const QMatrix4x4 &root, float inflate = 0.f, const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = 32.f;
    const auto head = partPose(root, -1, 13.5f, -7);
    b.box(prefix + "Head", {-2, -3, -2}, {6, 6, 4}, 0, 0, head, inflate);
    b.box(prefix + "Right ear", {-2, -5, 0}, {2, 2, 1}, 16, 14, head, inflate);
    b.box(prefix + "Left ear", {2, -5, 0}, {2, 2, 1}, 16, 14, head, inflate);
    b.box(prefix + "Nose", {-0.5f, -0.01f, -5}, {3, 3, 4}, 0, 10, head, inflate);
    const auto body = partPose(root, 0, 14, 2, 90.f, 0, 0);
    b.box(prefix + "Body", {-3, -2, -3}, {6, 9, 6}, 18, 14, body, inflate);
    const auto mane = partPose(root, -1, 14, -3, 90.f, 0, 0);
    b.box(prefix + "Mane", {-3, -3, -3}, {8, 6, 7}, 21, 0, mane, inflate);
    const struct {
        const char *name;
        float x, z;
    } legs[] = {{"Right hind leg", -2.5f, 7}, {"Left hind leg", 0.5f, 7},
                {"Right front leg", -2.5f, -4}, {"Left front leg", 0.5f, -4}};
    for (const auto &leg : legs)
        b.box(prefix + leg.name, {0, 0, -1}, {2, 8, 2}, 0, 18, partPose(root, leg.x, 16, leg.z),
              inflate);
    const auto tail = partPose(root, -1, 12, 8, 35.f, 0, 0);
    b.box(prefix + "Tail", {0, 0, -1}, {2, 8, 2}, 9, 18, tail, inflate);
}

void addPig(Builder &b, const QMatrix4x4 &root, float inflate = 0.f, const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = 32.f;
    const auto head = partPose(root, 0, 12, -6);
    b.box(prefix + "Head", {-4, -4, -8}, {8, 8, 8}, 0, 0, head, inflate);
    b.box(prefix + "Snout", {-2, 0, -9}, {4, 3, 1}, 16, 16, head, inflate);
    const auto body = partPose(root, 0, 11, 2, 90.f, 0, 0);
    b.box(prefix + "Body", {-5, -10, -7}, {10, 16, 8}, 28, 8, body, inflate);
    const struct {
        const char *name;
        float x, z;
    } legs[] = {{"Right hind leg", -3, 7}, {"Left hind leg", 3, 7},
                {"Right front leg", -3, -5}, {"Left front leg", 3, -5}};
    for (const auto &leg : legs)
        b.box(prefix + leg.name, {-2, 0, -2}, {4, 6, 4}, 0, 16, partPose(root, leg.x, 18, leg.z),
              inflate);
}

void addLlama(Builder &b, const QMatrix4x4 &root, float inflate = 0.f, const QString &prefix = {}) {
    b.cols = 128.f;
    b.rows = 64.f;
    const auto head = partPose(root, 0, 7, -6);
    b.box(prefix + "Snout", {-2, -14, -10}, {4, 4, 9}, 0, 0, head, inflate);
    b.box(prefix + "Neck", {-4, -16, -6}, {8, 18, 6}, 0, 14, head, inflate);
    b.box(prefix + "Right ear", {-4, -19, -4}, {3, 3, 2}, 17, 0, head, inflate);
    b.box(prefix + "Left ear", {1, -19, -4}, {3, 3, 2}, 17, 0, head, inflate);
    const auto body = partPose(root, 0, 5, 2, 90.f, 0, 0);
    b.box(prefix + "Body", {-6, -10, -7}, {12, 18, 10}, 29, 0, body, inflate);
    const struct {
        const char *name;
        float x, z;
    } legs[] = {{"Right hind leg", -3.5f, 6}, {"Left hind leg", 3.5f, 6},
                {"Right front leg", -3.5f, -5}, {"Left front leg", 3.5f, -5}};
    for (const auto &leg : legs)
        b.box(prefix + leg.name, {-2, 0, -2}, {4, 14, 4}, 29, 29, partPose(root, leg.x, 10, leg.z),
              inflate);
}

void addStrider(Builder &b, const QMatrix4x4 &root, float inflate = 0.f,
                const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = 128.f;
    b.box(prefix + "Body", {-8, -6, -8}, {16, 14, 16}, 0, 0, partPose(root, 0, 1, 0), inflate);
    b.box(prefix + "Right leg", {-2, 0, -2}, {4, 17, 4}, 0, 32, partPose(root, -4, 8, 0), inflate);
    b.box(prefix + "Left leg", {-2, 0, -2}, {4, 17, 4}, 0, 55, partPose(root, 4, 8, 0), inflate);
}

void addCamel(Builder &b, const QMatrix4x4 &root, float inflate = 0.f, const QString &prefix = {}) {
    b.cols = 128.f;
    b.rows = 128.f;
    const auto body = partPose(root, 0, 4, 9.5f);
    b.box(prefix + "Body", {-7.5f, -12, -23.5f}, {15, 12, 27}, 0, 25, body, inflate);
    b.box(prefix + "Hump", {-4.5f, -5, -5.5f}, {9, 5, 11}, 74, 0, partPose(body, 0, -12, -10),
          inflate);
    const auto head = partPose(body, 0, -3, -19.5f);
    b.box(prefix + "Neck lower", {-3.5f, -14, -5}, {7, 14, 7}, 60, 24, head, inflate);
    b.box(prefix + "Head", {-3.5f, -21, -15}, {7, 8, 19}, 21, 0, head, inflate);
    b.box(prefix + "Muzzle", {-2.5f, -21, -21}, {5, 5, 6}, 50, 0, head, inflate);
    b.box(prefix + "Left ear", {2.5f, -21, -1.5f}, {3, 1, 2}, 45, 0, head, inflate);
    b.box(prefix + "Right ear", {-5.5f, -21, -1.5f}, {3, 1, 2}, 67, 0, head, inflate);
    const struct {
        const char *name;
        float x, y, z;
        float u, v;
    } legs[] = {{"Left front leg", 4.9f, 1, -10.5f, 0, 0},
                {"Right front leg", -4.9f, 1, -10.5f, 0, 26},
                {"Left hind leg", 4.9f, 1, 9.5f, 58, 16},
                {"Right hind leg", -4.9f, 1, 9.5f, 94, 16}};
    for (const auto &leg : legs)
        b.box(prefix + leg.name, {-2.5f, 0, -2.5f}, {5, 21, 5}, leg.u, leg.v,
              partPose(root, leg.x, leg.y, leg.z), inflate);
}

void addGhast(Builder &b, const QMatrix4x4 &root, float inflate = 0.f, const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = 32.f;
    b.box(prefix + "Body", {-8, -8, -8}, {16, 16, 16}, 0, 0, partPose(root, 0, 8, 0), inflate);
    const struct {
        float x, z, h;
    } tentacles[] = {{-3.8f, -5, 9}, {1.3f, -5, 11}, {6.3f, -5, 8},
                     {-6.3f, 0, 10}, {-1.3f, 0, 13}, {3.8f, 0, 9},
                     {-3.8f, 5, 11}, {1.3f, 5, 8},   {6.3f, 5, 10}};
    int idx = 1;
    for (const auto &t : tentacles)
        b.box(prefix + QString("Tentacle %1").arg(idx++), {-1, 0, -1}, {2, t.h, 2}, 0, 0,
              partPose(root, t.x, 15, t.z), inflate);
}

void addVillager(Builder &b, const QMatrix4x4 &root, float inflate = 0.f,
                 const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto head = partPose(root, 0, 0, 0);
    b.box(prefix + "Head", {-4, -10, -4}, {8, 10, 8}, 0, 0, head, inflate);
    b.box(prefix + "Hat", {-4, -10, -4}, {8, 10, 8}, 32, 0, head, inflate + .5f);
    b.box(prefix + "Nose", {-1, -3, -6}, {2, 4, 2}, 24, 0, head, inflate);
    b.box(prefix + "Body", {-4, 0, -3}, {8, 12, 6}, 16, 20, root, inflate);
    b.box(prefix + "Robe", {-4, 0, -3}, {8, 20, 6}, 0, 38, root, inflate + .5f);
    const auto arms = partPose(root, 0, 3, -1, -43.f, 0, 0);
    b.box(prefix + "Right arm", {-8, -2, -2}, {4, 8, 4}, 44, 22, arms, inflate);
    b.box(prefix + "Left arm", {4, -2, -2}, {4, 8, 4}, 44, 22, arms, inflate, Builder::Mirror::X);
    b.box(prefix + "Crossed hands", {-4, 2, -2}, {8, 4, 4}, 40, 38, arms, inflate);
    b.box(prefix + "Right leg", {-2, 0, -2}, {4, 12, 4}, 0, 22, partPose(root, -2, 12, 0), inflate);
    b.box(prefix + "Left leg", {-2, 0, -2}, {4, 12, 4}, 0, 22, partPose(root, 2, 12, 0), inflate,
          Builder::Mirror::X);
}

void addCreeper(Builder &b, const QMatrix4x4 &root, float inflate = 0.f,
                const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = 32.f;
    b.box(prefix + "Head", {-4, -8, -4}, {8, 8, 8}, 0, 0, partPose(root, 0, 6, 0), inflate);
    b.box(prefix + "Body", {-4, 0, -2}, {8, 12, 4}, 16, 16, partPose(root, 0, 6, 0), inflate);
    const struct {
        const char *name;
        float x, z;
    } legs[] = {{"Right hind leg", -2, 4}, {"Left hind leg", 2, 4},
                {"Right front leg", -2, -4}, {"Left front leg", 2, -4}};
    for (const auto &leg : legs)
        b.box(prefix + leg.name, {-2, 0, -2}, {4, 6, 4}, 0, 16, partPose(root, leg.x, 18, leg.z),
              inflate);
}

void addSheep(Builder &b, const QMatrix4x4 &root, bool woolLayer, float inflate = 0.f,
              const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = 32.f;
    if (woolLayer) {
        b.box(prefix + "Head wool", {-3, -4, -4}, {6, 6, 6}, 0, 0, partPose(root, 0, 6, -8),
              .6f + inflate);
        b.box(prefix + "Body wool", {-4, -10, -7}, {8, 16, 6}, 28, 8,
              partPose(root, 0, 5, 2, 90.f, 0, 0), 1.75f + inflate);
        const struct {
            const char *name;
            float x, z;
        } legs[] = {{"Right hind wool", -3, 7}, {"Left hind wool", 3, 7},
                    {"Right front wool", -3, -5}, {"Left front wool", 3, -5}};
        for (const auto &leg : legs)
            b.box(prefix + leg.name, {-2, 0, -2}, {4, 6, 4}, 0, 16,
                  partPose(root, leg.x, 12, leg.z), .5f + inflate);
    } else {
        b.box(prefix + "Head", {-3, -4, -6}, {6, 6, 8}, 0, 0, partPose(root, 0, 6, -8), inflate);
        b.box(prefix + "Body", {-4, -10, -7}, {8, 16, 6}, 28, 8,
              partPose(root, 0, 5, 2, 90.f, 0, 0), inflate);
        const struct {
            const char *name;
            float x, z;
        } legs[] = {{"Right hind leg", -3, 7}, {"Left hind leg", 3, 7},
                    {"Right front leg", -3, -5}, {"Left front leg", 3, -5}};
        for (const auto &leg : legs)
            b.box(prefix + leg.name, {-2, 0, -2}, {4, 12, 4}, 0, 16,
                  partPose(root, leg.x, 12, leg.z), inflate);
    }
}

void addSkeleton(Builder &b, const QMatrix4x4 &root, float inflate = 0.f,
                 const QString &prefix = {}) {
    b.cols = 64.f;
    b.rows = 32.f;
    b.box(prefix + "Head", {-4, -8, -4}, {8, 8, 8}, 0, 0, root, inflate);
    b.box(prefix + "Body", {-4, 0, -2}, {8, 12, 4}, 16, 16, root, inflate);
    b.box(prefix + "Right arm", {-1, -2, -1}, {2, 12, 2}, 40, 16, partPose(root, -5, 2, 0),
          inflate);
    b.box(prefix + "Left arm", {-1, -2, -1}, {2, 12, 2}, 40, 16, partPose(root, 5, 2, 0), inflate,
          Builder::Mirror::X);
    b.box(prefix + "Right leg", {-1, 0, -1}, {2, 12, 2}, 0, 16, partPose(root, -2, 12, 0), inflate);
    b.box(prefix + "Left leg", {-1, 0, -1}, {2, 12, 2}, 0, 16, partPose(root, 2, 12, 0), inflate,
          Builder::Mirror::X);
}

void addPiglin(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto head = partPose(root, 0, 0, 0);
    b.box("Head", {-5, -8, -4}, {10, 8, 8}, 0, 0, head);
    b.box("Snout", {-2, -4, -5}, {4, 4, 1}, 31, 1, head);
    b.box("Left tusk", {2, -2, -5}, {1, 2, 1}, 2, 4, head);
    b.box("Right tusk", {-3, -2, -5}, {1, 2, 1}, 2, 0, head);
    b.box("Left ear", {0, 0, -2}, {1, 5, 4}, 51, 6, partPose(head, 4.5f, -6, 0, 0, 0, -30.f));
    b.box("Right ear", {-1, 0, -2}, {1, 5, 4}, 39, 6, partPose(head, -4.5f, -6, 0, 0, 0, 30.f));
    b.box("Body", {-4, 0, -2}, {8, 12, 4}, 16, 16, root);
    b.box("Jacket", {-4, 0, -2}, {8, 12, 4}, 16, 32, root, .25f);
    const auto rArm = partPose(root, -5, 2, 0), lArm = partPose(root, 5, 2, 0);
    b.box("Right arm", {-3, -2, -2}, {4, 12, 4}, 40, 16, rArm);
    b.box("Right sleeve", {-3, -2, -2}, {4, 12, 4}, 40, 32, rArm, .25f);
    b.box("Left arm", {-1, -2, -2}, {4, 12, 4}, 32, 48, lArm);
    b.box("Left sleeve", {-1, -2, -2}, {4, 12, 4}, 48, 48, lArm, .25f);
    const auto rLeg = partPose(root, -1.9f, 12, 0), lLeg = partPose(root, 1.9f, 12, 0);
    b.box("Right leg", {-2, 0, -2}, {4, 12, 4}, 0, 16, rLeg);
    b.box("Right pants", {-2, 0, -2}, {4, 12, 4}, 0, 32, rLeg, .25f);
    b.box("Left leg", {-2, 0, -2}, {4, 12, 4}, 16, 48, lLeg);
    b.box("Left pants", {-2, 0, -2}, {4, 12, 4}, 0, 48, lLeg, .25f);
}

void addEnderman(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 32.f;
    const auto head = partPose(root, 0, -14, 0);
    b.box("Head", {-4, -8, -4}, {8, 8, 8}, 0, 0, head);
    b.box("Jaw", {-4, -8, -4}, {8, 8, 8}, 0, 16, head, -.5f);
    b.box("Body", {-4, 0, -2}, {8, 12, 4}, 32, 16, partPose(root, 0, -14, 0));
    b.box("Right arm", {-1, -2, -1}, {2, 30, 2}, 56, 0, partPose(root, -5, -12, 0));
    b.box("Left arm", {-1, -2, -1}, {2, 30, 2}, 56, 0, partPose(root, 5, -12, 0), 0.f,
          Builder::Mirror::X);
    b.box("Right leg", {-1, 0, -1}, {2, 30, 2}, 56, 0, partPose(root, -2, -2, 0));
    b.box("Left leg", {-1, 0, -1}, {2, 30, 2}, 56, 0, partPose(root, 2, -2, 0), 0.f,
          Builder::Mirror::X);
}

void addIronGolem(Builder &b, const QMatrix4x4 &root) {
    b.cols = 128.f;
    b.rows = 128.f;
    const auto head = partPose(root, 0, -7, -2);
    b.box("Head", {-4, -12, -5.5f}, {8, 10, 8}, 0, 0, head);
    b.box("Nose", {-1, -5, -7.5f}, {2, 4, 2}, 24, 0, head);
    b.box("Torso", {-9, -2, -6}, {18, 12, 11}, 0, 40, partPose(root, 0, -7, 0));
    b.box("Waist", {-4.5f, 10, -3}, {9, 5, 6}, 0, 70, partPose(root, 0, -7, 0), .5f);
    b.box("Right arm", {-13, -2.5f, -3}, {4, 30, 6}, 60, 21, partPose(root, 0, -7, 0));
    b.box("Left arm", {9, -2.5f, -3}, {4, 30, 6}, 60, 58, partPose(root, 0, -7, 0));
    b.box("Right leg", {-3.5f, -3, -3}, {6, 16, 5}, 37, 0, partPose(root, -4, 11, 0));
    b.box("Left leg", {-3.5f, -3, -3}, {6, 16, 5}, 60, 0, partPose(root, 5, 11, 0), 0.f,
          Builder::Mirror::X);
}

void addSpider(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 32.f;
    b.box("Head", {-4, -4, -8}, {8, 8, 8}, 32, 4, partPose(root, 0, 15, -3));
    b.box("Thorax", {-3, -3, -3}, {6, 6, 6}, 0, 0, partPose(root, 0, 15, 0));
    b.box("Abdomen", {-5, -4, -6}, {10, 8, 12}, 0, 12, partPose(root, 0, 15, 9));
    const float yAngles[4] = {-45.f, -22.5f, 22.5f, 45.f};
    const float zAngles[4] = {-45.f, -33.f, -33.f, -45.f};
    for (int i = 0; i < 4; ++i) {
        const float zOff = 2.f - float(i);
        b.box(QString("Right leg %1").arg(i + 1), {-15, -1, -1}, {16, 2, 2}, 18, 0,
              partPose(root, -4, 15, zOff, 0, yAngles[i], zAngles[i]));
        b.box(QString("Left leg %1").arg(i + 1), {-1, -1, -1}, {16, 2, 2}, 18, 0,
              partPose(root, 4, 15, zOff, 0, -yAngles[i], -zAngles[i]), 0.f, Builder::Mirror::X);
    }
}

void addCow(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 32.f;
    const auto head = partPose(root, 0, 4, -8);
    b.box("Head", {-4, -4, -6}, {8, 8, 6}, 0, 0, head);
    b.box("Right horn", {-5, -5, -4}, {1, 3, 1}, 22, 0, head);
    b.box("Left horn", {4, -5, -4}, {1, 3, 1}, 22, 0, head);
    const auto body = partPose(root, 0, 5, 2, 90.f, 0, 0);
    b.box("Body", {-6, -10, -7}, {12, 18, 10}, 18, 4, body);
    b.box("Udder", {-2, 2, -8}, {4, 6, 1}, 52, 0, body);
    const struct {
        const char *name;
        float x, z;
    } legs[] = {{"Right hind leg", -4, 7}, {"Left hind leg", 4, 7},
                {"Right front leg", -4, -6}, {"Left front leg", 4, -6}};
    for (const auto &leg : legs)
        b.box(leg.name, {-2, 0, -2}, {4, 12, 4}, 0, 16, partPose(root, leg.x, 12, leg.z));
}

void addChicken(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 32.f;
    const auto head = partPose(root, 0, 15, -4);
    b.box("Head", {-2, -6, -2}, {4, 6, 3}, 0, 0, head);
    b.box("Beak", {-2, -4, -4}, {4, 2, 2}, 14, 0, head);
    b.box("Wattle", {-1, -2, -3}, {2, 2, 2}, 14, 4, head);
    b.box("Body", {-3, -4, -3}, {6, 8, 6}, 0, 9, partPose(root, 0, 16, 0, 90.f, 0, 0));
    b.box("Right wing", {0, 0, -3}, {1, 4, 6}, 24, 13, partPose(root, -4, 13, 0));
    b.box("Left wing", {-1, 0, -3}, {1, 4, 6}, 24, 13, partPose(root, 4, 13, 0));
    b.box("Right leg", {-1, 0, -3}, {3, 5, 3}, 26, 0, partPose(root, -2, 19, 1));
    b.box("Left leg", {-1, 0, -3}, {3, 5, 3}, 26, 0, partPose(root, 1, 19, 1));
}

void addCat(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 32.f;
    const auto head = partPose(root, 0, 15, -9);
    b.box("Head", {-2.5f, -2, -3}, {5, 4, 5}, 0, 0, head);
    b.box("Nose", {-1.5f, -0.01f, -4}, {3, 2, 2}, 0, 24, head);
    b.box("Right ear", {-2, -3, 0}, {1, 1, 2}, 0, 10, head);
    b.box("Left ear", {1, -3, 0}, {1, 1, 2}, 6, 10, head);
    b.box("Body", {-2, 3, -8}, {4, 16, 6}, 20, 0, partPose(root, 0, 12, -10, 90.f, 0, 0));
    b.box("Tail 1", {-0.5f, 0, 0}, {1, 8, 1}, 0, 15, partPose(root, 0, 15, 8, 50.f, 0, 0));
    b.box("Tail 2", {-0.5f, 0, 0}, {1, 8, 1}, 4, 15, partPose(root, 0, 20, 14, 98.f, 0, 0));
    b.box("Left hind leg", {-1, 0, 1}, {2, 6, 2}, 8, 13, partPose(root, 1.1f, 18, 5));
    b.box("Right hind leg", {-1, 0, 1}, {2, 6, 2}, 8, 13, partPose(root, -1.1f, 18, 5));
    b.box("Left front leg", {-1, 0, 0}, {2, 10, 2}, 40, 0, partPose(root, 1.2f, 14.1f, -5));
    b.box("Right front leg", {-1, 0, 0}, {2, 10, 2}, 40, 0, partPose(root, -1.2f, 14.1f, -5));
}

void addFox(Builder &b, const QMatrix4x4 &root) {
    b.cols = 48.f;
    b.rows = 32.f;
    const auto head = partPose(root, -1, 16.5f, -3);
    b.box("Head", {-3, -2, -5}, {8, 6, 6}, 1, 5, head);
    b.box("Right ear", {-3, -4, -4}, {2, 2, 1}, 8, 1, head);
    b.box("Left ear", {3, -4, -4}, {2, 2, 1}, 15, 1, head);
    b.box("Nose", {-1, 2.01f, -8}, {4, 2, 3}, 6, 18, head);
    const auto body = partPose(root, 0, 16, -6, 90.f, 0, 0);
    b.box("Body", {-3, 3, -3}, {6, 11, 6}, 24, 15, body);
    b.box("Tail", {-2, 0, -2}, {4, 9, 5}, 30, 0, partPose(body, -1, 14, -1, -5.f, 0, 0));
    b.box("Right hind leg", {0, 0.5f, 0}, {2, 6, 2}, 13, 24, partPose(root, -3, 17.5f, 7));
    b.box("Left hind leg", {0, 0.5f, 0}, {2, 6, 2}, 4, 24, partPose(root, 1, 17.5f, 7));
    b.box("Right front leg", {0, 0.5f, 0}, {2, 6, 2}, 13, 24, partPose(root, -3, 17.5f, 0));
    b.box("Left front leg", {0, 0.5f, 0}, {2, 6, 2}, 4, 24, partPose(root, 1, 17.5f, 0));
}

void addBee(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto body = partPose(root, 0, 19, 0);
    b.box("Body", {-3.5f, -4, -5}, {7, 7, 10}, 0, 0, body);
    b.box("Stinger", {0, -1, 5}, {0, 1, 2}, 26, 7, body);
    b.box("Left antenna", {1.5f, -4, -8}, {1, 2, 3}, 2, 0, body);
    b.box("Right antenna", {-2.5f, -4, -8}, {1, 2, 3}, 2, 3, body);
    b.box("Right wing", {-9, 0, 0}, {9, 0, 6}, 0, 18, partPose(body, -1.5f, -4, -3, 0, -15.f, 15.f));
    b.box("Left wing", {0, 0, 0}, {9, 0, 6}, 0, 18, partPose(body, 1.5f, -4, -3, 0, 15.f, -15.f),
          0.f, Builder::Mirror::X);
    b.box("Front legs", {-2.5f, 3, -2}, {5, 2, 0}, 26, 1, body);
    b.box("Middle legs", {-3.5f, 3, 0}, {7, 2, 0}, 26, 3, body);
    b.box("Back legs", {-3.5f, 3, 2}, {7, 2, 0}, 26, 5, body);
}

void addSlime(Builder &b, const QMatrix4x4 &root, bool magma) {
    b.cols = 64.f;
    b.rows = 32.f;
    if (magma) {
        for (int i = 0; i < 8; ++i) {
            const float u = i == 2 ? 24.f : i == 3 ? 24.f : 0.f;
            const float v = i == 2 ? 10.f : i == 3 ? 19.f : float(i);
            b.box(QString("Slice %1").arg(i + 1), {-4, float(16 + i), -4}, {8, 1, 8}, u, v, root);
        }
        b.box("Core", {-2, 18, -2}, {4, 4, 4}, 0, 16, root);
    } else {
        b.layer = EntityLayer::Cutout;
        b.box("Core", {-3, 17, -3}, {6, 6, 6}, 0, 16, root);
        b.box("Right eye", {-3.25f, 18, -3.5f}, {2, 2, 2}, 32, 0, root);
        b.box("Left eye", {1.25f, 18, -3.5f}, {2, 2, 2}, 32, 4, root);
        b.box("Mouth", {0, 21, -3.5f}, {1, 1, 1}, 32, 8, root);
        b.layer = EntityLayer::Overlay;
        b.box("Outer gel", {-4, 16, -4}, {8, 8, 8}, 0, 0, root);
        b.layer = EntityLayer::Cutout;
    }
}

void addBlaze(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 32.f;
    b.box("Head", {-4, -4, -4}, {8, 8, 8}, 0, 0, partPose(root, 0, 4, 0));
    constexpr float pi = 3.14159265f;
    for (int i = 0; i < 12; ++i) {
        const float ring = i < 4 ? 9.f : i < 8 ? 7.f : 5.f;
        const float y = i < 4 ? 4.f : i < 8 ? 10.f : 15.f;
        const float ang = float(i % 4) * (pi * .5f) + float(i / 4) * .4f;
        b.box(QString("Rod %1").arg(i + 1), {-1, 0, -1}, {2, 8, 2}, 0, 16,
              partPose(root, std::cos(ang) * ring, y, std::sin(ang) * ring));
    }
}

void addBreeze(Builder &b, const QMatrix4x4 &root) {
    b.cols = 32.f;
    b.rows = 32.f;
    b.box("Head", {-4, -8, -4}, {8, 8, 8}, 0, 0, partPose(root, 0, 4, 0));
    b.box("Brows", {-4, -8, -4}, {8, 8, 8}, 4, 24, partPose(root, 0, 4, 0), .25f);
    for (int i = 0; i < 3; ++i) {
        const float ang = float(i) * 2.0943951f;
        b.box(QString("Rod %1").arg(i + 1), {-1, 0, -1}, {2, 8, 2}, 0, 17,
              partPose(root, std::cos(ang) * 4.5f, 8, std::sin(ang) * 4.5f, 10.f, 0, 0));
    }
}

void addWarden(Builder &b, const QMatrix4x4 &root) {
    b.cols = 128.f;
    b.rows = 128.f;
    const auto torso = partPose(root, 0, -10, 0);
    b.box("Body", {-9, -13, -4}, {18, 21, 11}, 0, 0, torso);
    const auto head = partPose(torso, 0, -13, 0);
    b.box("Head", {-8, -16, -5}, {16, 16, 10}, 0, 32, head);
    b.box("Right tendril", {-16, -13, 0}, {16, 16, 0}, 52, 32, partPose(head, -8, -12, 0));
    b.box("Left tendril", {0, -13, 0}, {16, 16, 0}, 58, 0, partPose(head, 8, -12, 0));
    b.box("Right arm", {-4, 0, -4}, {8, 28, 8}, 44, 50, partPose(torso, -13, -13, 1));
    b.box("Left arm", {-4, 0, -4}, {8, 28, 8}, 0, 58, partPose(torso, 13, -13, 1));
    b.box("Right leg", {-3.1f, 0, -3}, {6, 13, 6}, 76, 48, partPose(root, -5.9f, 11, 0));
    b.box("Left leg", {-2.9f, 0, -3}, {6, 13, 6}, 76, 76, partPose(root, 5.9f, 11, 0));
}

void addGuardian(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto body = partPose(root, 0, 16, 0);
    b.box("Head", {-6, -6, -8}, {12, 12, 16}, 0, 0, body);
    b.box("Left side", {-8, -6, -6}, {2, 12, 12}, 0, 28, body);
    b.box("Right side", {6, -6, -6}, {2, 12, 12}, 0, 28, body, 0.f, Builder::Mirror::X);
    b.box("Top", {-6, -8, -6}, {12, 2, 12}, 16, 40, body);
    b.box("Bottom", {-6, 6, -6}, {12, 2, 12}, 16, 40, body);
    b.box("Eye", {-1, -1, -8.25f}, {2, 2, 1}, 8, 0, body);
    b.box("Tail 1", {-2, -2, 8}, {4, 4, 8}, 40, 0, body);
    b.box("Tail 2", {-1.5f, -1.5f, 16}, {3, 3, 7}, 0, 54, body);
    b.box("Tail 3", {-1, -1, 23}, {2, 2, 6}, 41, 32, body);
    b.box("Tail fin", {0, -4.5f, 26}, {1, 9, 9}, 25, 19, body);
}

void addSquid(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 32.f;
    b.box("Mantle", {-6, -8, -6}, {12, 16, 12}, 0, 0, partPose(root, 0, 8, 0));
    constexpr float pi = 3.14159265f;
    for (int i = 0; i < 8; ++i) {
        const float ang = float(i) * (pi * .25f);
        b.box(QString("Tentacle %1").arg(i + 1), {-1, 0, -1}, {2, 18, 2}, 48, 0,
              partPose(root, std::cos(ang) * 5.f, 15, std::sin(ang) * 5.f, 0,
                       -float(i) * 45.f + 90.f, 0));
    }
}

void addFish(Builder &b, const QMatrix4x4 &root, bool salmon) {
    b.cols = 32.f;
    b.rows = 32.f;
    if (salmon) {
        b.box("Body front", {-1.5f, -2.5f, 0}, {3, 5, 8}, 0, 0, partPose(root, 0, 18, -4));
        b.box("Body back", {-1.5f, -2.5f, 0}, {3, 5, 8}, 0, 13, partPose(root, 0, 18, 4));
        b.box("Head", {-1, -2, -3}, {2, 4, 3}, 22, 0, partPose(root, 0, 18, -4));
        b.box("Tail fin", {0, -2.5f, 0}, {0, 5, 6}, 20, 10, partPose(root, 0, 18, 12));
    } else {
        b.box("Body", {-1, -2, 0}, {2, 4, 7}, 0, 0, partPose(root, 0, 20, -3));
        b.box("Head", {-1, -2, -3}, {2, 4, 3}, 11, 0, partPose(root, 0, 20, -3));
        b.box("Nose", {-1, -2, -4}, {2, 3, 1}, 0, 0, partPose(root, 0, 20, -3));
        b.box("Tail fin", {0, -2, 0}, {0, 4, 4}, 22, 3, partPose(root, 0, 20, 4));
        b.box("Top fin", {0, -1, -1}, {0, 1, 6}, 20, -6, partPose(root, 0, 18, -3));
    }
}

void addPhantom(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto body = partPose(root, 0, 16, 0);
    b.box("Body", {-3, -2, -8}, {5, 3, 9}, 0, 8, body);
    b.box("Head", {-4, -2, -5}, {7, 3, 5}, 0, 0, partPose(body, 0, 1, -7));
    b.box("Tail 1", {-2, 0, 0}, {3, 2, 6}, 3, 20, partPose(body, 0, -2, 1));
    b.box("Tail 2", {-1, 0, 0}, {1, 1, 6}, 4, 29, partPose(body, 0, -1.5f, 7));
    b.box("Right wing base", {-6, 0, 0}, {6, 2, 9}, 23, 12, partPose(body, -3, -2, -8, 0, 0, 5.f));
    b.box("Right wing tip", {-13, 0, 0}, {13, 1, 9}, 16, 24,
          partPose(body, -9, -1.5f, -8, 0, 0, 12.f));
    b.box("Left wing base", {0, 0, 0}, {6, 2, 9}, 23, 12, partPose(body, 2, -2, -8, 0, 0, -5.f),
          0.f, Builder::Mirror::X);
    b.box("Left wing tip", {0, 0, 0}, {13, 1, 9}, 16, 24, partPose(body, 8, -1.5f, -8, 0, 0, -12.f),
          0.f, Builder::Mirror::X);
}

void addBat(Builder &b, const QMatrix4x4 &root) {
    b.cols = 32.f;
    b.rows = 32.f;
    const auto head = partPose(root, 0, 15, 0);
    b.box("Head", {-2, -3, -1}, {4, 3, 2}, 0, 7, head);
    b.box("Right ear", {-3, -6, 0}, {3, 5, 0}, 1, 15, head);
    b.box("Left ear", {0, -6, 0}, {3, 5, 0}, 8, 15, head);
    b.box("Body", {-1.5f, 0, -1}, {3, 5, 2}, 0, 0, partPose(root, 0, 15, 0));
    b.box("Right wing", {-2, -2, 0}, {2, 7, 0}, 12, 7, partPose(root, -1.5f, 15, 0, 0, 20.f, 0));
    b.box("Right wing tip", {-6, -2, 0}, {6, 8, 0}, 16, 8, partPose(root, -3.5f, 15, 0, 0, 35.f, 0));
    b.box("Left wing", {0, -2, 0}, {2, 7, 0}, 12, 0, partPose(root, 1.5f, 15, 0, 0, -20.f, 0));
    b.box("Left wing tip", {0, -2, 0}, {6, 8, 0}, 16, 0, partPose(root, 3.5f, 15, 0, 0, -35.f, 0));
}

void addEnderDragon(Builder &b, const QMatrix4x4 &root) {
    b.cols = 256.f;
    b.rows = 256.f;
    const auto body = partPose(root, 0, 10, 0);
    b.box("Body", {-12, 0, -16}, {24, 24, 64}, 0, 0, body);
    const auto head = partPose(body, 0, 12, -24);
    b.box("Head", {-8, -8, -10}, {16, 16, 16}, 112, 30, head);
    b.box("Snout", {-6, -1, -24}, {12, 5, 16}, 176, 44, head);
    b.box("Jaw", {-6, 4, -24}, {12, 4, 16}, 176, 65, head);
    b.box("Right horn", {-5, -12, -4}, {2, 4, 6}, 0, 0, head, 0.f, Builder::Mirror::X);
    b.box("Left horn", {3, -12, -4}, {2, 4, 6}, 0, 0, head);
    b.box("Right wing", {-56, -4, -4}, {56, 8, 8}, 112, 88, partPose(body, -12, 5, 2, 0, 0, 10.f));
    b.box("Right wing skin", {-56, 0, 2}, {56, 0, 56}, -56, 88,
          partPose(body, -12, 5, 2, 0, 0, 10.f));
    b.box("Left wing", {0, -4, -4}, {56, 8, 8}, 112, 88, partPose(body, 12, 5, 2, 0, 0, -10.f), 0.f,
          Builder::Mirror::X);
    b.box("Left wing skin", {0, 0, 2}, {56, 0, 56}, -56, 88, partPose(body, 12, 5, 2, 0, 0, -10.f),
          0.f, Builder::Mirror::X);
}

void addWither(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    b.box("Shoulders", {-10, 3.9f, -0.5f}, {20, 3, 3}, 0, 16, root);
    b.box("Spine", {-2, 6.9f, -0.5f}, {3, 10, 3}, 0, 22, root);
    b.box("Rib 1", {-6, 8.4f, 0}, {11, 2, 2}, 24, 22, root);
    b.box("Rib 2", {-6, 10.9f, 0}, {11, 2, 2}, 24, 22, root);
    b.box("Rib 3", {-6, 13.4f, 0}, {11, 2, 2}, 24, 22, root);
    b.box("Tail", {-1, 16.9f, 0}, {3, 6, 3}, 12, 22, partPose(root, 0, 0, 0, 12.f, 0, 0));
    b.box("Center head", {-4, -4, -4}, {8, 8, 8}, 0, 0, partPose(root, 0, 0, 0));
    b.box("Right head", {-4, -4, -4}, {6, 6, 6}, 32, 0, partPose(root, -8, 4, 0));
    b.box("Left head", {-4, -4, -4}, {6, 6, 6}, 32, 0, partPose(root, 10, 4, 0));
}

void addGoat(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto head = partPose(root, 1, 14, 0);
    b.box("Head", {-2.5f, -7, -14}, {5, 7, 10}, 34, 46, head);
    b.box("Left horn", {0.5f, -13, -10}, {2, 7, 2}, 12, 55, head);
    b.box("Right horn", {-2.5f, -13, -10}, {2, 7, 2}, 12, 55, head);
    b.box("Beard", {-0.5f, 0, -14}, {1, 7, 5}, 23, 52, head);
    b.box("Body", {-4, -17, -7}, {9, 11, 16}, 1, 1, partPose(root, 0, 24, 0));
    b.box("Left hind leg", {0, 0, 0}, {3, 6, 3}, 36, 29, partPose(root, 1, 18, 4));
    b.box("Right hind leg", {0, 0, 0}, {3, 6, 3}, 49, 29, partPose(root, -3, 18, 4));
    b.box("Left front leg", {0, 0, 0}, {3, 10, 3}, 49, 2, partPose(root, 1, 14, -6));
    b.box("Right front leg", {0, 0, 0}, {3, 10, 3}, 35, 2, partPose(root, -3, 14, -6));
}

void addPanda(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto head = partPose(root, 0, 11.5f, -17);
    b.box("Head", {-6.5f, -5, -4}, {13, 10, 9}, 0, 6, head);
    b.box("Snout", {-3.5f, 0, -6}, {7, 5, 2}, 45, 16, head);
    b.box("Left ear", {3.5f, -8, -1}, {5, 4, 1}, 52, 25, head);
    b.box("Right ear", {-8.5f, -8, -1}, {5, 4, 1}, 52, 25, head);
    b.box("Body", {-9.5f, -13, -6.5f}, {19, 26, 13}, 0, 25,
          partPose(root, 0, 10, 0, 90.f, 0, 0));
    const struct {
        const char *name;
        float x, z;
    } legs[] = {{"Right hind leg", -5.5f, 9}, {"Left hind leg", 5.5f, 9},
                {"Right front leg", -5.5f, -9}, {"Left front leg", 5.5f, -9}};
    for (const auto &leg : legs)
        b.box(leg.name, {-3, 0, -3}, {6, 9, 6}, 40, 0, partPose(root, leg.x, 15, leg.z));
}

void addPolarBear(Builder &b, const QMatrix4x4 &root) {
    b.cols = 128.f;
    b.rows = 64.f;
    const auto head = partPose(root, 0, 10, -16);
    b.box("Head", {-3.5f, -3, -3}, {7, 7, 7}, 0, 0, head);
    b.box("Snout", {-2.5f, 1, -6}, {5, 3, 3}, 0, 44, head);
    b.box("Right ear", {-4.5f, -4, -1}, {2, 2, 1}, 26, 0, head);
    b.box("Left ear", {2.5f, -4, -1}, {2, 2, 1}, 26, 0, head, 0.f, Builder::Mirror::X);
    const auto body = partPose(root, -2, 9, 12, 90.f, 0, 0);
    b.box("Body rear", {-5, -13, -7}, {14, 14, 11}, 0, 19, body);
    b.box("Body front", {-4, -25, -7}, {12, 12, 10}, 39, 0, body);
    b.box("Right hind leg", {-2, 0, -2}, {4, 10, 8}, 50, 22, partPose(root, -4.5f, 14, 6));
    b.box("Left hind leg", {-2, 0, -2}, {4, 10, 8}, 50, 22, partPose(root, 4.5f, 14, 6));
    b.box("Right front leg", {-2, 0, -2}, {4, 10, 6}, 50, 40, partPose(root, -3.5f, 14, -8));
    b.box("Left front leg", {-2, 0, -2}, {4, 10, 6}, 50, 40, partPose(root, 3.5f, 14, -8));
}

void addRabbit(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 32.f;
    const auto head = partPose(root, 0, 16, -1);
    b.box("Head", {-2.5f, -4, -5}, {5, 4, 5}, 32, 0, head);
    b.box("Nose", {-0.5f, -2.5f, -5.5f}, {1, 1, 1}, 32, 9, head);
    b.box("Right ear", {-2.5f, -9, -1}, {2, 5, 1}, 52, 0, head);
    b.box("Left ear", {0.5f, -9, -1}, {2, 5, 1}, 58, 0, head);
    b.box("Body", {-3, -2, -10}, {6, 5, 10}, 0, 0, partPose(root, 0, 19, 8, -20.f, 0, 0));
    b.box("Tail", {-1.5f, -1.5f, 0}, {3, 3, 2}, 52, 6, partPose(root, 0, 20, 7));
    b.box("Left haunch", {-1, 0, 0}, {2, 4, 5}, 30, 15, partPose(root, 3, 17.5f, 3.7f));
    b.box("Right haunch", {-1, 0, 0}, {2, 4, 5}, 16, 15, partPose(root, -3, 17.5f, 3.7f));
    b.box("Left front leg", {-1, 0, -1}, {2, 7, 2}, 8, 15, partPose(root, 3, 17, -1));
    b.box("Right front leg", {-1, 0, -1}, {2, 7, 2}, 0, 15, partPose(root, -3, 17, -1));
}

void addTurtle(Builder &b, const QMatrix4x4 &root) {
    b.cols = 128.f;
    b.rows = 64.f;
    b.box("Head", {-3, -1, -3}, {6, 5, 6}, 3, 0, partPose(root, 0, 19, -10));
    const auto body = partPose(root, 0, 11, -10, 90.f, 0, 0);
    b.box("Shell", {-9.5f, 3, -13}, {19, 20, 6}, 7, 37, body);
    b.box("Belly", {-5.5f, 3, -13}, {11, 18, 3}, 31, 1, body);
    b.box("Right hind leg", {-2, 0, 0}, {4, 1, 10}, 1, 23, partPose(root, -3.5f, 22, 11));
    b.box("Left hind leg", {-2, 0, 0}, {4, 1, 10}, 1, 12, partPose(root, 3.5f, 22, 11));
    b.box("Right flipper", {-13, 0, -2}, {13, 1, 5}, 27, 30, partPose(root, -5, 21, -4));
    b.box("Left flipper", {0, 0, -2}, {13, 1, 5}, 27, 24, partPose(root, 5, 21, -4));
}

void addFrog(Builder &b, const QMatrix4x4 &root) {
    b.cols = 48.f;
    b.rows = 48.f;
    const auto body = partPose(root, 0, 22, 4);
    b.box("Body", {-3.5f, -2, -8}, {7, 3, 9}, 3, 1, body);
    b.box("Head", {-3.5f, -4, -8}, {7, 3, 9}, 23, 13, body);
    b.box("Right eye", {-3.5f, -6, -6}, {3, 2, 3}, 0, 0, body);
    b.box("Left eye", {0.5f, -6, -6}, {3, 2, 3}, 0, 5, body);
    b.box("Right arm", {-1, 0, -1}, {2, 3, 3}, 0, 38, partPose(root, -4, 21, -2.5f));
    b.box("Left arm", {-1, 0, -1}, {2, 3, 3}, 0, 32, partPose(root, 4, 21, -2.5f));
    b.box("Right leg", {-2, 0, -2}, {3, 3, 4}, 0, 25, partPose(root, -3.5f, 21, 4));
    b.box("Left leg", {-1, 0, -2}, {3, 3, 4}, 14, 25, partPose(root, 3.5f, 21, 4));
}

void addAxolotl(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto body = partPose(root, 0, 20, 1);
    b.box("Body", {-4, -2, -9}, {8, 4, 10}, 0, 11, body);
    b.box("Head", {-4, -3, -5}, {8, 5, 5}, 0, 1, partPose(body, 0, 0, -9));
    b.box("Top gills", {-4, -3, 0}, {8, 3, 0}, 3, 37, partPose(body, 0, -3, -10));
    b.box("Left gills", {0, -5, 0}, {3, 7, 0}, 11, 40, partPose(body, 4, 0, -10));
    b.box("Right gills", {-3, -5, 0}, {3, 7, 0}, 0, 40, partPose(body, -4, 0, -10));
    b.box("Tail", {0, -2, 0}, {0, 5, 12}, 2, 19, partPose(body, 0, 0, 1));
    b.box("Right hind leg", {-2, 0, 0}, {3, 5, 0}, 2, 13, partPose(body, -3.5f, 1, -1));
    b.box("Left hind leg", {-1, 0, 0}, {3, 5, 0}, 2, 13, partPose(body, 3.5f, 1, -1));
    b.box("Right front leg", {-2, 0, 0}, {3, 5, 0}, 2, 13, partPose(body, -3.5f, 1, -8));
    b.box("Left front leg", {-1, 0, 0}, {3, 5, 0}, 2, 13, partPose(body, 3.5f, 1, -8));
}

void addAllay(Builder &b, const QMatrix4x4 &root) {
    b.cols = 32.f;
    b.rows = 32.f;
    b.box("Head", {-2.5f, -5, -2.5f}, {5, 5, 5}, 0, 0, partPose(root, 0, 15, 0));
    b.box("Body", {-1.5f, 0, -1}, {3, 4, 2}, 0, 10, partPose(root, 0, 15, 0));
    b.box("Skirt", {-1.5f, 0, -1}, {3, 5, 2}, 0, 16, partPose(root, 0, 15, 0), -.2f);
    b.box("Right arm", {-0.75f, -0.5f, -1}, {1, 4, 2}, 23, 0, partPose(root, -1.75f, 15.5f, 0));
    b.box("Left arm", {-0.25f, -0.5f, -1}, {1, 4, 2}, 23, 6, partPose(root, 1.75f, 15.5f, 0));
    b.box("Right wing", {0, 0, 0}, {0, 5, 8}, 16, 14, partPose(root, -0.5f, 15, 1, 0, -25.f, 0));
    b.box("Left wing", {0, 0, 0}, {0, 5, 8}, 16, 14, partPose(root, 0.5f, 15, 1, 0, 25.f, 0));
}

void addDolphin(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto body = partPose(root, 0, 20, -3);
    b.box("Body", {-4, -7, 0}, {8, 7, 13}, 22, 0, body);
    b.box("Back fin", {-0.5f, -4, 0}, {1, 4, 5}, 51, 0, partPose(body, 0, -7, 4, 60.f, 0, 0));
    b.box("Head", {-4, -3, -3}, {8, 7, 6}, 0, 0, partPose(body, 0, -4, -3));
    b.box("Nose", {-1, -1, -7}, {2, 2, 4}, 0, 13, partPose(body, 0, -2, -3));
    b.box("Tail", {-2, -2.5f, -1}, {4, 5, 11}, 0, 19, partPose(body, 0, -2.5f, 11));
    b.box("Tail fin", {-5, -0.5f, -1}, {10, 1, 6}, 19, 20, partPose(body, 0, -2.5f, 20));
    b.box("Left fin", {0, -4, -1.5f}, {1, 4, 7}, 48, 20, partPose(body, 3, -2, 5, 60.f, 0, 105.f));
    b.box("Right fin", {-1, -4, -1.5f}, {1, 4, 7}, 48, 20, partPose(body, -3, -2, 5, 60.f, 0, -105.f),
          0.f, Builder::Mirror::X);
}

void addParrot(Builder &b, const QMatrix4x4 &root) {
    b.cols = 32.f;
    b.rows = 32.f;
    b.box("Body", {-1.5f, 0, -1.5f}, {3, 6, 3}, 2, 8, partPose(root, 0, 16.5f, -3, 28.f, 0, 0));
    const auto head = partPose(root, 0, 15.7f, -2.8f);
    b.box("Head", {-1, -1.5f, -1}, {2, 3, 3}, 2, 2, head);
    b.box("Forehead", {-1, -2.5f, -1}, {2, 1, 4}, 10, 0, head);
    b.box("Beak", {-0.5f, -1.5f, -2}, {1, 2, 1}, 11, 7, head);
    b.box("Feather", {0, -4.5f, -2}, {0, 5, 4}, 2, 18, head);
    b.box("Left wing", {-0.5f, 0, -1.5f}, {1, 5, 3}, 19, 8, partPose(root, 1.5f, 16.9f, -2.8f, 28.f, 0, 0));
    b.box("Right wing", {-0.5f, 0, -1.5f}, {1, 5, 3}, 19, 8, partPose(root, -1.5f, 16.9f, -2.8f, 28.f, 0, 0));
    b.box("Tail", {-1.5f, -1, -1}, {3, 4, 1}, 22, 1, partPose(root, 0, 21.1f, 1.2f, 58.f, 0, 0));
    b.box("Left leg", {-0.5f, 0, -0.5f}, {1, 2, 1}, 14, 18, partPose(root, 1, 22, -1));
    b.box("Right leg", {-0.5f, 0, -0.5f}, {1, 2, 1}, 14, 18, partPose(root, -1, 22, -1));
}

void addHoglin(Builder &b, const QMatrix4x4 &root) {
    b.cols = 128.f;
    b.rows = 64.f;
    b.box("Body", {-8, -7, -13}, {16, 14, 26}, 1, 1, partPose(root, 0, 7, 0));
    b.box("Mane", {0, -14, -13}, {0, 10, 19}, 90, 33, partPose(root, 0, 7, 0));
    const auto head = partPose(root, 0, 2, -12, 50.f, 0, 0);
    b.box("Head", {-7, -3, -19}, {14, 6, 19}, 61, 1, head);
    b.box("Right tusk", {-8, -7, -13}, {2, 11, 2}, 10, 13, head);
    b.box("Left tusk", {6, -7, -13}, {2, 11, 2}, 1, 13, head);
    b.box("Right front leg", {-3, 0, -2}, {6, 14, 6}, 66, 42, partPose(root, -4, 10, -8.5f));
    b.box("Left front leg", {-3, 0, -2}, {6, 14, 6}, 41, 42, partPose(root, 4, 10, -8.5f));
    b.box("Right hind leg", {-3, 0, -1}, {5, 11, 5}, 21, 45, partPose(root, -5, 13, 10));
    b.box("Left hind leg", {-3, 0, -1}, {5, 11, 5}, 0, 45, partPose(root, 5, 13, 10));
}

void addRavager(Builder &b, const QMatrix4x4 &root) {
    b.cols = 128.f;
    b.rows = 128.f;
    const auto body = partPose(root, 0, 1, 2, 90.f, 0, 0);
    b.box("Chest", {-7, -10, -7}, {14, 16, 20}, 0, 55, body);
    b.box("Abdomen", {-6, 6, -7}, {12, 13, 18}, 0, 91, body);
    const auto head = partPose(root, 0, -6, -15);
    b.box("Head", {-8, -10, -14}, {16, 20, 16}, 0, 0, head);
    b.box("Nose", {-2, 4, -18}, {4, 8, 4}, 0, 0, head);
    b.box("Right horn", {-10, -14, -8}, {2, 14, 4}, 74, 55, partPose(head, 0, 0, 0, 62.f, 0, 0));
    b.box("Left horn", {8, -14, -8}, {2, 14, 4}, 74, 55, partPose(head, 0, 0, 0, 62.f, 0, 0), 0.f,
          Builder::Mirror::X);
    b.box("Right hind leg", {-4, 0, -4}, {8, 37, 8}, 96, 0, partPose(root, -8, -13, 18));
    b.box("Left hind leg", {-4, 0, -4}, {8, 37, 8}, 96, 0, partPose(root, 8, -13, 18), 0.f,
          Builder::Mirror::X);
    b.box("Right front leg", {-4, 0, -4}, {8, 37, 8}, 64, 0, partPose(root, -8, -13, -5));
    b.box("Left front leg", {-4, 0, -4}, {8, 37, 8}, 64, 0, partPose(root, 8, -13, -5), 0.f,
          Builder::Mirror::X);
}

void addSilverfish(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 32.f;
    const struct {
        float w, h, d, z, u, v;
    } segs[] = {{3, 2, 2, -6.5f, 0, 0}, {4, 3, 2, -4.5f, 0, 4}, {6, 4, 3, -2.f, 0, 9},
                {3, 3, 3, 1.f, 0, 16},  {2, 2, 3, 4.f, 0, 22},   {2, 1, 2, 6.5f, 11, 0},
                {1, 1, 2, 8.5f, 13, 4}};
    int i = 1;
    for (const auto &s : segs)
        b.box(QString("Segment %1").arg(i++), {-s.w * .5f, 24.f - s.h, s.z}, {s.w, s.h, s.d},
              s.u, s.v, root);
}

void addArmadillo(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto body = partPose(root, 0, 21, 4);
    b.box("Shell", {-4, -8, -10}, {8, 8, 12}, 0, 20, body);
    const auto head = partPose(body, 0, -3, -10);
    b.box("Head", {-1.5f, -1, -5}, {3, 5, 5}, 43, 15, head);
    b.box("Right ear", {-2, -3, 0}, {2, 5, 0}, 43, 10, partPose(head, -1, -1, -2, 0, 0, -25.f));
    b.box("Left ear", {0, -3, 0}, {2, 5, 0}, 47, 10, partPose(head, 1, -1, -2, 0, 0, 25.f));
    b.box("Tail", {-0.5f, 0, 0}, {1, 6, 1}, 44, 53, partPose(body, 0, -3, 2, 30.f, 0, 0));
    b.box("Right hind leg", {-1, 0, -1}, {2, 3, 2}, 51, 31, partPose(root, -2, 21, 4));
    b.box("Left hind leg", {-1, 0, -1}, {2, 3, 2}, 42, 31, partPose(root, 2, 21, 4));
    b.box("Right front leg", {-1, 0, -1}, {2, 3, 2}, 51, 43, partPose(root, -2, 21, -4));
    b.box("Left front leg", {-1, 0, -1}, {2, 3, 2}, 42, 43, partPose(root, 2, 21, -4));
}

void addSniffer(Builder &b, const QMatrix4x4 &root) {
    b.cols = 192.f;
    b.rows = 192.f;
    b.box("Body", {-12.5f, -14, -20}, {25, 24, 40}, 62, 68, partPose(root, 0, 5, 0));
    const auto head = partPose(root, 0, 6.5f, -19.5f);
    b.box("Head", {-6.5f, -7.5f, -11.5f}, {13, 14, 11}, 8, 15, head);
    b.box("Nose", {-6.5f, -7.5f, -20.5f}, {13, 10, 9}, 10, 45, head);
    b.box("Right ear", {-1, 0, -3}, {1, 19, 7}, 48, 0, partPose(head, -6.5f, -4.5f, -3.5f));
    b.box("Left ear", {0, 0, -3}, {1, 19, 7}, 2, 0, partPose(head, 6.5f, -4.5f, -3.5f));
    const struct {
        const char *name;
        float x, z, u, v;
    } legs[] = {{"Right front leg", -7.5f, -15, 32, 87}, {"Left front leg", 7.5f, -15, 0, 87},
                {"Right mid leg", -7.5f, 0, 32, 105},    {"Left mid leg", 7.5f, 0, 0, 105},
                {"Right hind leg", -7.5f, 15, 32, 123},  {"Left hind leg", 7.5f, 15, 0, 123}};
    for (const auto &leg : legs)
        b.box(leg.name, {-3.5f, -1, -4}, {7, 10, 8}, leg.u, leg.v,
              partPose(root, leg.x, 15, leg.z));
}

void addCreaking(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    b.box("Head", {-3, -10, -3}, {6, 10, 6}, 0, 0, partPose(root, -3, -10, 0));
    b.box("Body", {-5, -10, -3}, {10, 15, 6}, 0, 16, partPose(root, 0, 0, 0));
    b.box("Right arm", {-3, -1, -1.5f}, {3, 21, 3}, 22, 0, partPose(root, -5, -8, 0));
    b.box("Left arm", {0, -1, -1.5f}, {3, 21, 3}, 30, 40, partPose(root, 5, -8, 0));
    b.box("Right leg", {-1.5f, 0, -1.5f}, {3, 17, 3}, 0, 34, partPose(root, -1.5f, 7, 0));
    b.box("Left leg", {-1.5f, 0, -1.5f}, {3, 17, 3}, 42, 40, partPose(root, 1.5f, 7, 0));
}

void addCopperGolem(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    const auto body = partPose(root, 0, 19, 0);
    b.box("Body", {-4, -6, -3}, {8, 6, 6}, 0, 15, body);
    const auto head = partPose(body, 0, -6, 0);
    b.box("Head", {-4, -5, -5}, {8, 5, 10}, 0, 0, head, .015f);
    b.box("Nose", {-1, -2, -6}, {2, 3, 2}, 56, 0, head);
    b.box("Antenna", {-1, -9, -1}, {2, 4, 2}, 37, 8, head, -.015f);
    b.box("Antenna tip", {-2, -13, -2}, {4, 4, 4}, 37, 0, head, -.015f);
    b.box("Right arm", {-3, -1, -2}, {3, 10, 4}, 36, 16, partPose(head, -4, 0, 0));
    b.box("Left arm", {0, -1, -2}, {3, 10, 4}, 50, 16, partPose(head, 4, 0, 0));
    b.box("Right leg", {-4, 0, -2}, {4, 5, 4}, 0, 27, body);
    b.box("Left leg", {0, 0, -2}, {4, 5, 4}, 16, 27, body);
}

void addArmorStand(Builder &b, const QMatrix4x4 &root) {
    b.cols = 64.f;
    b.rows = 64.f;
    b.box("Base plate", {-6, -1, -6}, {12, 1, 12}, 0, 32, partPose(root, 0, 24, 0));
    b.box("Head post", {-1, -7, -1}, {2, 7, 2}, 0, 0, root);
    b.box("Shoulder bar", {-6, 0, -1.5f}, {12, 3, 3}, 0, 26, root);
    b.box("Right body strut", {-3, 3, -1}, {2, 7, 2}, 16, 0, root);
    b.box("Left body strut", {1, 3, -1}, {2, 7, 2}, 48, 16, root);
    b.box("Waist bar", {-4, 10, -1}, {8, 2, 2}, 0, 48, root);
    b.box("Right arm", {-2, -2, -1}, {2, 12, 2}, 24, 0, partPose(root, -5, 2, 0));
    b.box("Left arm", {0, -2, -1}, {2, 12, 2}, 32, 16, partPose(root, 5, 2, 0), 0.f,
          Builder::Mirror::X);
    b.box("Right leg", {-1, 0, -1}, {2, 11, 2}, 8, 0, partPose(root, -1.9f, 12, 0));
    b.box("Left leg", {-1, 0, -1}, {2, 11, 2}, 40, 16, partPose(root, 1.9f, 12, 0), 0.f,
          Builder::Mirror::X);
}

// ---- Block Entities, Vehicles & World Objects ----

void addChest(Builder &b, bool doubleHalf) {
    b.cols = 64.f;
    b.rows = 64.f;
    const float w = doubleHalf ? 15.f : 14.f;
    const float x = doubleHalf ? 0.5f : 1.f;
    b.box("Body", {x, 0, 1}, {w, 10, 14}, 0, 19);
    QMatrix4x4 lid;
    lid.translate(0, 9, 1);
    b.box("Lid", {x, 0, 0}, {w, 5, 14}, 0, 0, lid);
    b.box("Latch", {doubleHalf ? 0.5f : 7.f, -2, 14}, {doubleHalf ? 1.f : 2.f, 4, 1}, 0, 0, lid);
}

void addBed(Builder &b) {
    b.cols = 64.f;
    b.rows = 64.f;
    b.layer = EntityLayer::Solid;
    const float legAngles[2][2] = {{90, 180}, {0, 270}};
    for (int half = 0; half < 2; ++half) {
        QMatrix4x4 pose;
        pose.translate(0, 9, -16.f * float(half) + 8.f);
        pose.rotate(90, 1, 0, 0);
        pose.translate(8, 8, 8);
        pose.rotate(180, 0, 0, 1);
        pose.translate(-8, -8, -8);
        b.box(half ? "Foot" : "Head", {0, 0, 0}, {16, 16, 6}, 0, 22.f * float(half), pose);
        for (int leg = 0; leg < 2; ++leg) {
            auto lp = pose;
            lp.rotate(legAngles[half][leg], 0, 0, 1);
            lp.rotate(90, 1, 0, 0);
            b.box("Leg", {-16.f * float(leg), 6, -16.f * float(half)}, {3, 3, 3}, 50,
                  (half ? 0.f : 6.f) + 12.f * float(leg), lp);
        }
    }
}

void addShulker(Builder &b) {
    b.cols = 64.f;
    b.rows = 64.f;
    QMatrix4x4 pose;
    pose.translate(8, 8, 8);
    pose.scale(1, -1, -1);
    pose.translate(0, -16, 0);
    auto base = pose;
    base.translate(0, 24, 0);
    b.box("Base", {-8, -8, -8}, {16, 8, 16}, 0, 28, base);
    pose.translate(0, 24, 0);
    b.box("Lid", {-8, -16, -8}, {16, 12, 16}, 0, 0, pose);
}

void addBanner(Builder &b, const QString &textureAsset, bool isPattern, const Library *library,
               TexturePreviewResult &out) {
    b.cols = 64.f;
    b.rows = 64.f;
    QMatrix4x4 stand;
    constexpr float s = 2.f / 3.f;
    stand.translate(8, 0, 8);
    stand.scale(s, -s, -s);
    QMatrix4x4 cloth = stand;
    cloth.translate(0, -44, 0);
    cloth.rotate(-.45f, 1, 0, 0);
    if (isPattern) {
        b.layer = EntityLayer::Solid;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/banner_base.png", "nova:synthetic/banner_base",
            [] { return makeSolidEntityTone(64, 64, QColor(140, 112, 74), QColor(126, 98, 64)); },
            library, out);
        b.box("Pole", {-1, -42, -1}, {2, 42, 2}, 44, 0, stand);
        b.box("Bar", {-10, -44, -1}, {20, 2, 2}, 0, 42, stand);
        b.box("Cloth", {-10, 0, -2}, {20, 40, 1}, 0, 0, cloth);
        b.layer = EntityLayer::Overlay;
        b.texture = textureAsset;
        b.box("Banner pattern", {-10, 0, -2}, {20, 40, 1}, 0, 0, cloth);
    } else {
        b.layer = EntityLayer::Solid;
        b.texture = textureAsset;
        b.box("Pole", {-1, -42, -1}, {2, 42, 2}, 44, 0, stand);
        b.box("Bar", {-10, -44, -1}, {20, 2, 2}, 0, 42, stand);
        b.box("Cloth", {-10, 0, -2}, {20, 40, 1}, 0, 0, cloth);
    }
}

void addShield(Builder &b, const QString &textureAsset, bool isPattern, const Library *library,
               TexturePreviewResult &out) {
    b.cols = 64.f;
    b.rows = 64.f;
    QMatrix4x4 pose;
    pose.translate(8, 8, 8);
    pose.scale(1, -1, -1);
    if (isPattern) {
        b.layer = EntityLayer::Solid;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/shield_base.png", "nova:synthetic/shield_base",
            [] { return makeSolidEntityTone(64, 64, QColor(156, 128, 92), QColor(138, 112, 78)); },
            library, out);
        b.box("Plate", {-6, -11, -2}, {12, 22, 1}, 0, 0, pose);
        b.box("Handle", {-1, -3, -1}, {2, 6, 6}, 26, 0, pose);
        b.layer = EntityLayer::Overlay;
        b.texture = textureAsset;
        b.box("Shield pattern", {-6, -11, -2}, {12, 22, 1}, 0, 0, pose, .02f);
    } else {
        b.layer = EntityLayer::Cutout;
        b.texture = textureAsset;
        b.box("Plate", {-6, -11, -2}, {12, 22, 1}, 0, 0, pose);
        b.box("Handle", {-1, -3, -1}, {2, 6, 6}, 26, 0, pose);
    }
}

void addSign(Builder &b, bool hanging) {
    const auto root = entityRoot(20.f);
    if (hanging) {
        b.cols = 64.f;
        b.rows = 32.f;
        b.box("Board", {-7, 6, -1}, {14, 10, 2}, 0, 12, root);
        b.box("Plank", {-8, 0, -2}, {16, 2, 4}, 0, 0, root);
        b.box("Left chain", {4.5f, 2, 0}, {3, 4, 0}, 0, 6, root);
        b.box("Right chain", {-7.5f, 2, 0}, {3, 4, 0}, 6, 6, root);
    } else {
        b.cols = 64.f;
        b.rows = 32.f;
        b.box("Board", {-12, -14, -1}, {24, 12, 2}, 0, 0, partPose(root, 0, 14, 0));
        b.box("Post", {-1, -2, -1}, {2, 14, 2}, 0, 14, partPose(root, 0, 14, 0));
    }
}

void addBoat(Builder &b, bool chestBoat, bool raft) {
    b.cols = 128.f;
    b.rows = raft ? 64.f : 128.f;
    const auto root = entityRoot(12.f);
    if (raft) {
        b.box("Deck", {-14, -9, -4}, {28, 16, 4}, 0, 0, partPose(root, 0, 4, 0, 90.f, 0, 0));
        b.box("Keel", {-14, -8, -8}, {28, 16, 4}, 0, 0, partPose(root, 0, 8, 0, 90.f, 0, 0));
    } else {
        b.box("Bottom", {-14, -9, -3}, {28, 16, 3}, 0, 0, partPose(root, 0, 3, 1, 90.f, 0, 0));
        b.box("Back", {-13, -7, -1}, {18, 6, 2}, 0, 19, partPose(root, -15, 4, 4, 0, 270.f, 0));
        b.box("Front", {-8, -7, -1}, {16, 6, 2}, 0, 27, partPose(root, 15, 4, 0, 0, 90.f, 0));
        b.box("Right side", {-14, -7, -1}, {28, 6, 2}, 0, 35, partPose(root, 0, 4, -9, 0, 180.f, 0));
        b.box("Left side", {-14, -7, -1}, {28, 6, 2}, 0, 43, partPose(root, 0, 4, 9));
    }
    b.box("Left paddle", {-1, 0, -5}, {2, 2, 18}, 62, 0, partPose(root, 3, -2, 9, 45.f, 0, 15.f));
    b.box("Right paddle", {-1, 0, -5}, {2, 2, 18}, 62, 20, partPose(root, 3, -2, -9, -45.f, 0, -15.f));
    if (chestBoat) {
        b.box("Chest body", {-6, -8, -6}, {12, 8, 12}, 0, 76, partPose(root, -2, 3, 0));
        b.box("Chest lid", {-6, -12, -6}, {12, 4, 12}, 0, 59, partPose(root, -2, 3, 0));
        b.box("Chest lock", {-1, -9, 6}, {2, 4, 1}, 0, 59, partPose(root, -2, 3, 0));
    }
}

void addMinecart(Builder &b) {
    b.cols = 64.f;
    b.rows = 32.f;
    const auto root = entityRoot(12.f);
    b.box("Bottom", {-10, -8, -1}, {20, 16, 2}, 0, 10, partPose(root, 0, 4, 0, 90.f, 0, 0));
    b.box("Front", {-8, -9, -1}, {16, 8, 2}, 0, 0, partPose(root, -9, 4, 0, 0, 270.f, 0));
    b.box("Back", {-8, -9, -1}, {16, 8, 2}, 0, 0, partPose(root, 9, 4, 0, 0, 90.f, 0));
    b.box("Left", {-8, -9, -1}, {16, 8, 2}, 0, 0, partPose(root, 0, 4, -7, 0, 180.f, 0));
    b.box("Right", {-8, -9, -1}, {16, 8, 2}, 0, 0, partPose(root, 0, 4, 7));
}

void addTrident(Builder &b) {
    b.cols = 32.f;
    b.rows = 32.f;
    const auto root = entityRoot(22.f);
    b.box("Shaft", {-0.5f, 2, -0.5f}, {1, 25, 1}, 0, 6, root);
    b.box("Base", {-1.5f, 0, -0.5f}, {3, 2, 1}, 4, 0, root);
    b.box("Left spike", {-2.5f, -4, -0.5f}, {1, 4, 1}, 4, 3, root);
    b.box("Middle spike", {-0.5f, -5, -0.5f}, {1, 5, 1}, 0, 0, root);
    b.box("Right spike", {1.5f, -4, -0.5f}, {1, 4, 1}, 4, 3, root, 0.f, Builder::Mirror::X);
}

void addBook(Builder &b) {
    b.cols = 64.f;
    b.rows = 32.f;
    const auto root = entityRoot(12.f);
    b.box("Spine", {-1, -5, 0}, {2, 10, 0}, 12, 10, root);
    b.box("Right cover", {-6, -5, -0.005f}, {6, 10, 0.005f}, 0, 10,
          partPose(root, -1, 0, 0, 0, 35.f, 0));
    b.box("Left cover", {0, -5, -0.005f}, {6, 10, 0.005f}, 16, 10,
          partPose(root, 1, 0, 0, 0, -35.f, 0));
    b.box("Right pages", {-5, -4, -1}, {5, 8, 1}, 0, 10, partPose(root, -1, 0, 0, 0, 35.f, 0));
    b.box("Left pages", {0, -4, -1}, {5, 8, 1}, 12, 10, partPose(root, 1, 0, 0, 0, -35.f, 0));
}

void addBell(Builder &b) {
    b.cols = 32.f;
    b.rows = 32.f;
    const auto root = entityRoot(14.f);
    b.box("Bell body", {-3, -6, -3}, {6, 7, 6}, 0, 0, partPose(root, 0, 6, 0));
    b.box("Bell lip", {-4, 1, -4}, {8, 2, 8}, 0, 13, partPose(root, 0, 6, 0));
}

void addEndCrystal(Builder &b) {
    b.cols = 64.f;
    b.rows = 32.f;
    const auto root = entityRoot(12.f);
    b.box("Outer glass", {-4, -4, -4}, {8, 8, 8}, 0, 0, partPose(root, 0, 6, 0, 35.f, 45.f, 0));
    b.box("Inner glass", {-3.5f, -3.5f, -3.5f}, {7, 7, 7}, 0, 0,
          partPose(root, 0, 6, 0, 20.f, -25.f, 40.f));
    b.box("Core", {-2.5f, -2.5f, -2.5f}, {5, 5, 5}, 32, 0, partPose(root, 0, 6, 0, 45.f, 45.f, 45.f));
}

void addArrow(Builder &b) {
    b.cols = 32.f;
    b.rows = 32.f;
    const auto root = entityRoot(10.f);
    b.layer = EntityLayer::CutoutNoCull;
    b.box("Shaft vertical", {-8, -2.5f, 0}, {16, 5, 0}, 0, 0, partPose(root, 0, 4, 0, 45.f, 0, 0));
    b.box("Shaft horizontal", {-8, -2.5f, 0}, {16, 5, 0}, 0, 0,
          partPose(root, 0, 4, 0, -45.f, 0, 0));
    b.box("Fletching", {-7, -2.5f, -2.5f}, {0, 5, 5}, 0, 5, root);
}

void addDecoratedPot(Builder &b, const QString &textureAsset, bool isBase, const Library *library,
                     TexturePreviewResult &out) {
    b.layer = EntityLayer::Solid;
    const QString baseTex =
        isBase ? textureAsset
               : resolveCompanion(
                     "assets/minecraft/textures/entity/decorated_pot/decorated_pot_base.png",
                     "nova:synthetic/pot_base",
                     [] { return makeSolidEntityTone(32, 32, QColor(148, 86, 62), QColor(136, 76, 54)); },
                     library, out);
    b.cols = b.rows = 32.f;
    b.texture = baseTex;
    QMatrix4x4 neck;
    neck.translate(0, 37, 16);
    neck.rotate(180, 1, 0, 0);
    b.box("Collar", {4, 17, 4}, {8, 3, 8}, 0, 0, neck, -.1f);
    b.box("Lip", {5, 20, 5}, {6, 1, 6}, 0, 5, neck, .2f);
    b.box("Top", {1, 16, 1}, {14, 0, 14}, -14, 13);
    b.box("Bottom", {1, 0, 1}, {14, 0, 14}, -14, 13);

    const QString sideTex =
        !isBase ? textureAsset
                : resolveCompanion(
                      "assets/minecraft/textures/entity/decorated_pot/decorated_pot_side.png",
                      "nova:synthetic/pot_side",
                      [] { return makeSolidEntityTone(16, 16, QColor(156, 92, 66), QColor(144, 82, 58)); },
                      library, out);
    b.cols = b.rows = 16.f;
    b.texture = sideTex;
    constexpr struct {
        const char *name;
        float x, z, yAngle, xAngle, zAngle;
    } sides[] = {{"North", 15, 1, 0, 0, 180}, {"West", 1, 1, -90, 0, 180},
                 {"East", 15, 15, 90, 0, 180}, {"South", 1, 15, 0, 180, 0}};
    for (const auto &side : sides) {
        QMatrix4x4 pose;
        pose.translate(side.x, 16, side.z);
        pose.rotate(side.zAngle, 0, 0, 1);
        pose.rotate(side.yAngle, 0, 1, 0);
        pose.rotate(side.xAngle, 1, 0, 0);
        b.box(side.name, {}, {14, 16, 0}, 1, 0, pose, 0.f, Builder::Mirror::None, true);
    }
}

void addPainting(Builder &b, const QString &textureAsset, const QImage &activeImage,
                 const Library *library, TexturePreviewResult &out) {
    // Determine block dimensions from image aspect ratio (1 block = 16px, clamped to 4x4).
    float blocksW = 1.f, blocksH = 1.f;
    if (!activeImage.isNull() && activeImage.width() > 0 && activeImage.height() > 0) {
        const float ratio = float(activeImage.width()) / float(activeImage.height());
        if (ratio >= 1.f) {
            blocksW = std::clamp(std::round(ratio * 2.f) / 2.f, 1.f, 4.f);
            if (blocksW > 2.f && blocksW < 4.f)
                blocksW = std::round(blocksW);
        } else {
            blocksH = std::clamp(std::round((1.f / ratio) * 2.f) / 2.f, 1.f, 4.f);
            if (blocksH > 2.f && blocksH < 4.f)
                blocksH = std::round(blocksH);
        }
    }
    // Scale down slightly if wider/taller than 2 blocks so it frames cleanly in viewport.
    const float norm = 1.f / std::max(1.f, std::max(blocksW, blocksH) * 0.5f);
    const float hx = blocksW * norm;
    const float hy = blocksH * norm;
    const float hz = 0.0625f * norm; // 1 Minecraft pixel thick

    // Front face = the painting artwork.
    b.layer = EntityLayer::Solid;
    b.texture = textureAsset;
    b.quad("Painting front", "south",
           {{{-hx, hy, hz}, {-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}}},
           {{{0, 0}, {0, 1}, {1, 1}, {1, 0}}});

    // Back and wooden frame edges = painting/back.png or synthesized wood.
    b.texture = resolveCompanion("assets/minecraft/textures/painting/back.png",
                                 "nova:synthetic/painting_back", makeWoodBackTexture, library, out);
    b.quad("Frame back", "north",
           {{{hx, hy, -hz}, {hx, -hy, -hz}, {-hx, -hy, -hz}, {-hx, hy, -hz}}},
           {{{0, 0}, {0, 1}, {1, 1}, {1, 0}}});
    b.quad("Frame top", "up",
           {{{-hx, hy, -hz}, {-hx, hy, hz}, {hx, hy, hz}, {hx, hy, -hz}}},
           {{{0, 0}, {0, 0.125f}, {1, 0.125f}, {1, 0}}});
    b.quad("Frame bottom", "down",
           {{{-hx, -hy, hz}, {-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, -hy, hz}}},
           {{{0, 0}, {0, 0.125f}, {1, 0.125f}, {1, 0}}});
    b.quad("Frame left", "west",
           {{{-hx, hy, -hz}, {-hx, -hy, -hz}, {-hx, -hy, hz}, {-hx, hy, hz}}},
           {{{0, 0}, {0, 1}, {0.125f, 1}, {0.125f, 0}}});
    b.quad("Frame right", "east",
           {{{hx, hy, hz}, {hx, -hy, hz}, {hx, -hy, -hz}, {hx, hy, -hz}}},
           {{{0, 0}, {0, 1}, {0.125f, 1}, {0.125f, 0}}});
}

void addFullBlockCube(Builder &b, const QString &textureAsset, EntityLayer layer,
                      const QImage &activeImage) {
    // If animated vertical strip (height > width and height % width == 0), sample frame 0.
    float vMax = 1.f;
    if (!activeImage.isNull() && activeImage.width() > 0 &&
        activeImage.height() > activeImage.width() &&
        activeImage.height() % activeImage.width() == 0) {
        vMax = float(activeImage.width()) / float(activeImage.height());
    }
    b.layer = layer;
    b.texture = textureAsset;
    const std::array<QVector2D, 4> uvs{{{0, 0}, {0, vMax}, {1, vMax}, {1, 0}}};
    b.quad("Block", "south", {{{-1, 1, 1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}}}, uvs);
    b.quad("Block", "north", {{{1, 1, -1}, {1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}}}, uvs);
    b.quad("Block", "west", {{{-1, 1, -1}, {-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}}}, uvs);
    b.quad("Block", "east", {{{1, 1, 1}, {1, -1, 1}, {1, -1, -1}, {1, 1, -1}}}, uvs);
    b.quad("Block", "up", {{{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}}}, uvs);
    b.quad("Block", "down", {{{-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}}}, uvs);
}

void addCrossedPlanes(Builder &b, const QString &textureAsset, const QImage &activeImage) {
    float vMax = 1.f;
    if (!activeImage.isNull() && activeImage.width() > 0 &&
        activeImage.height() > activeImage.width() &&
        activeImage.height() % activeImage.width() == 0) {
        vMax = float(activeImage.width()) / float(activeImage.height());
    }
    b.layer = EntityLayer::CutoutNoCull;
    b.texture = textureAsset;
    const std::array<QVector2D, 4> uvs{{{0, 0}, {0, vMax}, {1, vMax}, {1, 0}}};
    const float k = 0.70710678f;
    b.quad("Cross A", "south", {{{-k, 1, -k}, {-k, -1, -k}, {k, -1, k}, {k, 1, k}}}, uvs);
    b.quad("Cross B", "east", {{{k, 1, -k}, {k, -1, -k}, {-k, -1, k}, {-k, 1, k}}}, uvs);
}

void addBillboardCard(Builder &b, const QString &textureAsset, const QImage &activeImage,
                      const QString &name) {
    float hx = 1.f, hy = 1.f;
    if (!activeImage.isNull() && activeImage.width() > 0 && activeImage.height() > 0) {
        if (activeImage.width() > activeImage.height())
            hy = std::max(0.25f, float(activeImage.height()) / float(activeImage.width()));
        else if (activeImage.height() > activeImage.width())
            hx = std::max(0.25f, float(activeImage.width()) / float(activeImage.height()));
    }
    b.layer = EntityLayer::CutoutNoCull;
    b.texture = textureAsset;
    b.quad(name, "south", {{{-hx, hy, 0}, {-hx, -hy, 0}, {hx, -hy, 0}, {hx, hy, 0}}},
           {{{0, 0}, {0, 1}, {1, 1}, {1, 0}}});
}

Mesh buildExtrudedItemMesh(const QString &textureAsset, const QImage &activeImage) {
    Model m;
    m.valid = true;
    m.generated = true;
    m.version = "26.1";
    m.id = "nova:builtin/item_extruded";
    m.guiLight = GuiLight::Front;
    m.textures["layer0"] = textureAsset;
    m.resolved["textures"] = QJsonObject{{"layer0", Location::fromAsset(textureAsset, "textures")}};
    if (activeImage.isNull())
        return mesh(m);
    // If animated vertical strip (height > width and divisible), extrude frame 0.
    QImage sample = activeImage;
    if (sample.width() > 0 && sample.height() > sample.width() &&
        sample.height() % sample.width() == 0) {
        sample = sample.copy(0, 0, sample.width(), sample.width());
    }
    return generatedMesh(m, {{textureAsset, sample}});
}

// ---- Registry of all built-in 3D texture previews ----

const QVector<TexturePreviewSpec> &allSpecs() {
    static const QVector<TexturePreviewSpec> specs = {
        // Armor & Equipment (worn on player / entities)
        {"armor_humanoid_outer", "Player wearing armor (helmet, chestplate, boots)",
         "Armor & Equipment"},
        {"armor_humanoid_leggings", "Player wearing armor leggings", "Armor & Equipment"},
        {"armor_trim_outer", "Player wearing armor trim (outer layer)", "Armor & Equipment"},
        {"armor_trim_leggings", "Player wearing armor trim (leggings)", "Armor & Equipment"},
        {"elytra", "Player wearing Elytra wings", "Armor & Equipment"},
        {"horse_armor", "Horse wearing armor / saddle", "Armor & Equipment"},
        {"wolf_armor", "Wolf wearing armor / collar", "Armor & Equipment"},
        {"llama_decor", "Llama wearing carpet decor", "Armor & Equipment"},
        {"pig_saddle", "Pig wearing saddle", "Armor & Equipment"},
        {"strider_saddle", "Strider wearing saddle", "Armor & Equipment"},
        {"camel_saddle", "Camel wearing saddle", "Armor & Equipment"},
        {"happy_ghast_harness", "Happy Ghast wearing harness", "Armor & Equipment"},
        // Players
        {"player_wide", "Player model (classic wide arms)", "Player"},
        {"player_slim", "Player model (slim arms)", "Player"},
        // Block Entities & Vehicles
        {"chest", "Chest (single)", "Block Entity"},
        {"chest_double", "Chest (double half)", "Block Entity"},
        {"bed", "Bed", "Block Entity"},
        {"shulker", "Shulker / Shulker Box", "Block Entity"},
        {"banner", "Banner (base)", "Block Entity"},
        {"banner_pattern", "Banner (with pattern)", "Block Entity"},
        {"shield", "Shield (base)", "Block Entity"},
        {"shield_pattern", "Shield (with pattern)", "Block Entity"},
        {"decorated_pot_base", "Decorated Pot (base)", "Block Entity"},
        {"decorated_pot_side", "Decorated Pot (sherd / side)", "Block Entity"},
        {"conduit", "Conduit", "Block Entity"},
        {"sign", "Standing Sign", "Block Entity"},
        {"hanging_sign", "Hanging Sign", "Block Entity"},
        {"boat", "Boat", "Vehicle"},
        {"chest_boat", "Chest Boat", "Vehicle"},
        {"raft", "Bamboo Raft", "Vehicle"},
        {"chest_raft", "Bamboo Chest Raft", "Vehicle"},
        {"minecart", "Minecart", "Vehicle"},
        {"armor_stand", "Armor Stand", "Block Entity"},
        {"trident", "Trident", "Item"},
        {"book", "Enchanting / Lectern Book", "Block Entity"},
        {"bell", "Bell", "Block Entity"},
        {"end_crystal", "End Crystal", "Entity"},
        {"arrow", "Arrow / Projectile", "Entity"},
        {"copper_golem", "Copper Golem", "Entity"},
        // Mobs
        {"zombie", "Zombie / Husk / Drowned", "Entity"},
        {"skeleton", "Skeleton / Wither Skeleton", "Entity"},
        {"skeleton_overlay", "Skeleton wearing Stray / Bogged overlay", "Entity"},
        {"piglin", "Piglin / Zombified Piglin", "Entity"},
        {"villager", "Villager", "Entity"},
        {"villager_overlay", "Villager wearing outfit / profession", "Entity"},
        {"enderman", "Enderman", "Entity"},
        {"iron_golem", "Iron Golem", "Entity"},
        {"creeper", "Creeper", "Entity"},
        {"creeper_armor", "Charged Creeper armor", "Entity"},
        {"pig", "Pig", "Entity"},
        {"cow", "Cow / Mooshroom", "Entity"},
        {"sheep", "Sheep", "Entity"},
        {"sheep_fur", "Sheep wearing wool fleece", "Entity"},
        {"chicken", "Chicken", "Entity"},
        {"wolf", "Wolf", "Entity"},
        {"cat", "Cat / Ocelot", "Entity"},
        {"fox", "Fox", "Entity"},
        {"horse", "Horse / Donkey / Mule", "Entity"},
        {"llama", "Llama", "Entity"},
        {"camel", "Camel", "Entity"},
        {"goat", "Goat", "Entity"},
        {"panda", "Panda", "Entity"},
        {"polar_bear", "Polar Bear", "Entity"},
        {"rabbit", "Rabbit", "Entity"},
        {"turtle", "Turtle", "Entity"},
        {"frog", "Frog", "Entity"},
        {"axolotl", "Axolotl", "Entity"},
        {"armadillo", "Armadillo", "Entity"},
        {"sniffer", "Sniffer", "Entity"},
        {"strider", "Strider", "Entity"},
        {"hoglin", "Hoglin / Zoglin", "Entity"},
        {"ravager", "Ravager", "Entity"},
        {"spider", "Spider / Cave Spider", "Entity"},
        {"silverfish", "Silverfish / Endermite", "Entity"},
        {"slime", "Slime", "Entity"},
        {"magma_cube", "Magma Cube", "Entity"},
        {"ghast", "Ghast", "Entity"},
        {"phantom", "Phantom", "Entity"},
        {"bat", "Bat", "Entity"},
        {"bee", "Bee", "Entity"},
        {"parrot", "Parrot", "Entity"},
        {"dolphin", "Dolphin", "Entity"},
        {"squid", "Squid / Glow Squid", "Entity"},
        {"guardian", "Guardian / Elder Guardian", "Entity"},
        {"cod", "Cod / Tropical Fish", "Entity"},
        {"salmon", "Salmon", "Entity"},
        {"blaze", "Blaze", "Entity"},
        {"breeze", "Breeze", "Entity"},
        {"allay", "Allay / Vex", "Entity"},
        {"warden", "Warden", "Entity"},
        {"creaking", "Creaking", "Entity"},
        {"ender_dragon", "Ender Dragon", "Entity"},
        {"wither", "Wither", "Entity"},
        {"wither_skull", "Wither Skull", "Entity"},
        // World, Items & Paintings
        {"painting", "Framed Painting", "World"},
        {"item_extruded", "Extruded 3D Item Sprite", "Item"},
        {"fluid_block", "Fluid / Translucent Block", "World"},
        {"flame_cross", "Crossed Flame / Portal", "World"},
        {"block_cube", "Full Block Cube", "World"},
        {"billboard", "3D Sprite Card", "World"},
    };
    return specs;
}

const QHash<QString, int> &specIndex() {
    static const QHash<QString, int> map = [] {
        QHash<QString, int> m;
        const auto &s = allSpecs();
        for (int i = 0; i < s.size(); ++i)
            m.insert(s[i].id, i);
        return m;
    }();
    return map;
}

QString detectId(const QString &path) {
    // 1. Armor & Equipment (1.21.4+ / 1.21.11 / 26.1+ and legacy paths)
    if (path.startsWith("entity/equipment/humanoid_leggings/") ||
        path.startsWith("entity/equipment/humanoid_baby_leggings/"))
        return "armor_humanoid_leggings";
    if (path.startsWith("entity/equipment/humanoid/") ||
        path.startsWith("entity/equipment/humanoid_baby/"))
        return "armor_humanoid_outer";
    if (path.startsWith("models/armor/")) {
        if (path.contains("layer_2"))
            return "armor_humanoid_leggings";
        return "armor_humanoid_outer";
    }
    if (path.startsWith("trims/entity/humanoid_leggings/") ||
        (path.startsWith("trims/models/armor/") && path.contains("leggings")))
        return "armor_trim_leggings";
    if (path.startsWith("trims/entity/humanoid/") || path.startsWith("trims/models/armor/"))
        return "armor_trim_outer";
    if (path.startsWith("entity/equipment/wings/") || path == "entity/elytra")
        return "elytra";
    if (path.startsWith("entity/equipment/horse_body/") ||
        path.startsWith("entity/equipment/horse_saddle/") ||
        path.startsWith("entity/equipment/donkey_saddle/") ||
        path.startsWith("entity/equipment/mule_saddle/") ||
        path.startsWith("entity/horse/armor/"))
        return "horse_armor";
    if (path.startsWith("entity/equipment/wolf_body/") || path == "entity/wolf/wolf_collar")
        return "wolf_armor";
    if (path.startsWith("entity/equipment/llama_body/") || path.startsWith("entity/llama/decor/"))
        return "llama_decor";
    if (path.startsWith("entity/equipment/pig_saddle/") || path == "entity/pig/pig_saddle")
        return "pig_saddle";
    if (path.startsWith("entity/equipment/strider_saddle/") ||
        path == "entity/strider/strider_saddle")
        return "strider_saddle";
    if (path.startsWith("entity/equipment/camel_saddle/"))
        return "camel_saddle";
    if (path.startsWith("entity/equipment/happy_ghast_body/"))
        return "happy_ghast_harness";

    // 2. Player skins
    if (path.startsWith("entity/player/slim/") || path == "entity/alex")
        return "player_slim";
    if (path.startsWith("entity/player/") || path == "entity/steve")
        return "player_wide";

    // 3. Block Entities & Vehicles
    if (path.startsWith("entity/chest/"))
        return (path.endsWith("_left") || path.endsWith("_right")) ? "chest_double" : "chest";
    if (path.startsWith("entity/bed/"))
        return "bed";
    if (path.startsWith("entity/shulker/"))
        return "shulker";
    if (path == "entity/banner_base")
        return "banner";
    if (path.startsWith("entity/banner/"))
        return "banner_pattern";
    if (path == "entity/shield_base" || path == "entity/shield_base_nopattern")
        return "shield";
    if (path.startsWith("entity/shield/"))
        return "shield_pattern";
    if (path.startsWith("entity/decorated_pot/"))
        return path.endsWith("base") ? "decorated_pot_base" : "decorated_pot_side";
    if (path.startsWith("entity/conduit/"))
        return "conduit";
    if (path.startsWith("entity/signs/hanging/"))
        return "hanging_sign";
    if (path.startsWith("entity/signs/"))
        return "sign";
    if (path.startsWith("entity/chest_boat/"))
        return path.contains("bamboo") ? "chest_raft" : "chest_boat";
    if (path.startsWith("entity/boat/"))
        return path.contains("bamboo") ? "raft" : "boat";
    if (path == "entity/minecart")
        return "minecart";
    if (path.startsWith("entity/armorstand/"))
        return "armor_stand";
    if (path == "entity/trident")
        return "trident";
    if (path == "entity/enchanting_table_book")
        return "book";
    if (path.startsWith("entity/bell/"))
        return "bell";
    if (path.startsWith("entity/end_crystal/"))
        return "end_crystal";
    if (path.startsWith("entity/projectiles/"))
        return "arrow";
    if (path.startsWith("entity/copper_golem/"))
        return "copper_golem";

    // 4. Mobs
    if (path.startsWith("entity/zombie/") || path == "entity/zombie" ||
        path.startsWith("entity/husk") || path.startsWith("entity/drowned"))
        return "zombie";
    if (path.startsWith("entity/skeleton/")) {
        if (path.contains("overlay"))
            return "skeleton_overlay";
        return "skeleton";
    }
    if (path.startsWith("entity/piglin/"))
        return "piglin";
    if (path.startsWith("entity/villager/")) {
        if (path == "entity/villager/villager")
            return "villager";
        return "villager_overlay";
    }
    if (path.startsWith("entity/zombie_villager/"))
        return path == "entity/zombie_villager/zombie_villager" ? "villager" : "villager_overlay";
    if (path.startsWith("entity/illager/ravager") || path.contains("ravager"))
        return "ravager";
    if (path.startsWith("entity/illager/vex"))
        return "allay";
    if (path.startsWith("entity/illager/") || path == "entity/witch")
        return "villager";
    if (path.startsWith("entity/enderman/"))
        return "enderman";
    if (path.startsWith("entity/iron_golem/"))
        return "iron_golem";
    if (path == "entity/snow_golem")
        return "player_wide";
    if (path.startsWith("entity/creeper/"))
        return path.contains("armor") ? "creeper_armor" : "creeper";
    if (path.startsWith("entity/pig/"))
        return "pig";
    if (path.startsWith("entity/cow/"))
        return "cow";
    if (path.startsWith("entity/sheep/"))
        return (path.contains("fur") || path.contains("wool")) ? "sheep_fur" : "sheep";
    if (path.startsWith("entity/chicken"))
        return "chicken";
    if (path.startsWith("entity/wolf/"))
        return "wolf";
    if (path.startsWith("entity/cat/"))
        return "cat";
    if (path.startsWith("entity/fox/"))
        return "fox";
    if (path.startsWith("entity/horse/"))
        return "horse";
    if (path.startsWith("entity/llama/"))
        return "llama";
    if (path.startsWith("entity/camel/"))
        return "camel";
    if (path.startsWith("entity/goat/"))
        return "goat";
    if (path.startsWith("entity/panda/"))
        return "panda";
    if (path.startsWith("entity/bear/"))
        return "polar_bear";
    if (path.startsWith("entity/rabbit/"))
        return "rabbit";
    if (path.startsWith("entity/turtle/"))
        return "turtle";
    if (path.startsWith("entity/frog/"))
        return "frog";
    if (path.startsWith("entity/axolotl/"))
        return "axolotl";
    if (path.startsWith("entity/armadillo"))
        return "armadillo";
    if (path.startsWith("entity/sniffer/"))
        return "sniffer";
    if (path.startsWith("entity/strider/"))
        return "strider";
    if (path.startsWith("entity/hoglin/"))
        return "hoglin";
    if (path.startsWith("entity/illager/ravager") || path.contains("ravager"))
        return "ravager";
    if (path.startsWith("entity/spider/"))
        return "spider";
    if (path == "entity/silverfish" || path == "entity/endermite")
        return "silverfish";
    if (path == "entity/slime/magmacube")
        return "magma_cube";
    if (path.startsWith("entity/slime/"))
        return "slime";
    if (path.startsWith("entity/ghast/"))
        return "ghast";
    if (path.startsWith("entity/phantom"))
        return "phantom";
    if (path == "entity/bat")
        return "bat";
    if (path.startsWith("entity/bee/"))
        return "bee";
    if (path.startsWith("entity/parrot/"))
        return "parrot";
    if (path == "entity/dolphin")
        return "dolphin";
    if (path.startsWith("entity/squid/"))
        return "squid";
    if (path.startsWith("entity/guardian"))
        return "guardian";
    if (path == "entity/fish/salmon")
        return "salmon";
    if (path.startsWith("entity/fish/") || path.startsWith("entity/tadpole/"))
        return "cod";
    if (path == "entity/blaze")
        return "blaze";
    if (path.startsWith("entity/breeze/"))
        return "breeze";
    if (path.startsWith("entity/allay/") || path.startsWith("entity/illager/vex"))
        return "allay";
    if (path.startsWith("entity/warden/"))
        return "warden";
    if (path.startsWith("entity/creaking/"))
        return "creaking";
    if (path.startsWith("entity/enderdragon/"))
        return "ender_dragon";
    if (path.startsWith("entity/wither/"))
        return "wither";
    if (path == "entity/beacon_beam" || path == "entity/lead_knot" ||
        path == "entity/fishing_hook" || path == "entity/experience_orb")
        return "billboard";
    // Any other entity texture (custom mob/skin): default to humanoid player or billboard.
    if (path.startsWith("entity/"))
        return "player_wide";

    // 5. Paintings, Maps, Items, Hardcoded Vanilla Blocks, Particles & Environment
    if (path.startsWith("painting/"))
        return "painting";
    if (path.startsWith("map/"))
        return "billboard";
    if (path.startsWith("item/"))
        return "item_extruded";
    if (path == "block/water_still" || path == "block/water_flow" ||
        path == "block/water_overlay" || path == "block/lava_still" || path == "block/lava_flow" ||
        path == "block/nether_portal")
        return "fluid_block";
    if (path == "block/fire_0" || path == "block/fire_1" || path == "block/soul_fire_0" ||
        path == "block/soul_fire_1")
        return "flame_cross";
    if (path.startsWith("block/destroy_stage_"))
        return "block_cube";
    if (path.startsWith("particle/") || path.startsWith("mob_effect/") ||
        path.startsWith("environment/") || path.startsWith("misc/") ||
        path.startsWith("gui/sprites/") || path.startsWith("trims/items/"))
        return "item_extruded";

    // Ordinary custom/vanilla block textures (e.g. block/fx_orphan.png) return empty so
    // they resolve through JSON models or preserve the explicit no-model notice.
    return {};
}

} // namespace

TexturePreviewSpec texturePreviewSpec(const QString &previewId) {
    const auto it = specIndex().constFind(previewId);
    return it == specIndex().cend() ? TexturePreviewSpec{} : allSpecs().at(*it);
}

TexturePreviewSpec detectTexturePreview(const QString &textureAsset) {
    const auto loc = Location::parse(Location::fromAsset(textureAsset, "textures"));
    if (!loc.valid())
        return {};
    const QString id = detectId(loc.path);
    return id.isEmpty() ? TexturePreviewSpec{} : texturePreviewSpec(id);
}

QVector<TexturePreviewSpec> manualTexturePreviews() {
    static const QStringList curated = {
        "armor_humanoid_outer", "armor_humanoid_leggings", "armor_trim_outer",
        "armor_trim_leggings",  "elytra",                  "horse_armor",
        "wolf_armor",           "llama_decor",             "pig_saddle",
        "player_wide",          "player_slim",             "item_extruded",
        "painting",             "chest",                   "bed",
        "shulker",              "banner",                  "shield",
        "sign",                 "boat",                    "minecart",
        "armor_stand",          "trident",                 "creeper",
        "zombie",               "skeleton",                "piglin",
        "villager",             "enderman",                "iron_golem",
        "pig",                  "cow",                     "sheep",
        "chicken",              "wolf",                    "cat",
        "horse",                "llama",                   "bee",
        "warden",               "ender_dragon",            "fluid_block",
    };
    QVector<TexturePreviewSpec> out;
    out.reserve(curated.size());
    for (const auto &id : curated) {
        const auto spec = texturePreviewSpec(id);
        if (!spec.id.isEmpty())
            out << spec;
    }
    return out;
}

TexturePreviewResult buildTexturePreview(const QString &textureAsset, const QImage &activeImage,
                                         const Library *library, const QString &previewId) {
    TexturePreviewResult result;
    const TexturePreviewSpec spec =
        previewId.isEmpty() ? detectTexturePreview(textureAsset) : texturePreviewSpec(previewId);
    if (spec.id.isEmpty())
        return result;

    result.id = spec.id;
    result.label = spec.label;

    const auto &id = spec.id;
    const bool legacySkin = !activeImage.isNull() && activeImage.width() > 0 &&
                            activeImage.height() > 0 &&
                            activeImage.width() == activeImage.height() * 2;

    if (id == "item_extruded") {
        result.mesh = buildExtrudedItemMesh(textureAsset, activeImage);
        return result;
    }

    Builder b;
    b.mesh.guiLight = GuiLight::Side;
    b.texture = textureAsset;

    if (id == "armor_humanoid_outer" || id == "armor_humanoid_leggings") {
        const bool isOuter = (id == "armor_humanoid_outer");
        const auto root = entityRoot(24.f);
        // 1. Standing player mannequin wearing the armor.
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion("assets/minecraft/textures/entity/player/wide/steve.png",
                                     "nova:synthetic/mannequin_humanoid", makeMannequinSkin,
                                     library, result);
        addPlayerBody(b, root, false, false, false, "Player ");

        // 2. Optional companion armor layer if present in the pack/stack.
        const QString companion = companionArmorAsset(textureAsset, isOuter);
        if (library && !companion.isEmpty() && library->hasAsset(companion)) {
            result.referencedAssets << companion;
            b.layer = EntityLayer::CutoutNoCull;
            b.texture = companion;
            if (isOuter)
                addArmorLeggings(b, root, 0.f, "Companion ");
            else
                addArmorOuter(b, root, 0.f, "Companion ");
        }

        // 3. The active armor texture itself (uses exact textureAsset so live edits & UVs work).
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        if (isOuter)
            addArmorOuter(b, root);
        else
            addArmorLeggings(b, root);
    } else if (id == "armor_trim_outer" || id == "armor_trim_leggings") {
        const bool isOuter = (id == "armor_trim_outer");
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion("assets/minecraft/textures/entity/player/wide/steve.png",
                                     "nova:synthetic/mannequin_humanoid", makeMannequinSkin,
                                     library, result);
        addPlayerBody(b, root, false, false, false, "Player ");

        b.layer = EntityLayer::CutoutNoCull;
        b.texture = resolveCompanion(
            isOuter ? "assets/minecraft/textures/entity/equipment/humanoid/iron.png"
                    : "assets/minecraft/textures/entity/equipment/humanoid_leggings/iron.png",
            isOuter ? "nova:synthetic/armor_base_outer" : "nova:synthetic/armor_base_inner",
            [isOuter] { return makeArmorBaseTexture(!isOuter); }, library, result);
        if (isOuter)
            addArmorOuter(b, root, 0.f, "Base ");
        else
            addArmorLeggings(b, root, 0.f, "Base ");

        b.texture = textureAsset;
        if (isOuter)
            addArmorOuter(b, root, .08f, "Trim ");
        else
            addArmorLeggings(b, root, .08f, "Trim ");
    } else if (id == "elytra") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion("assets/minecraft/textures/entity/player/wide/steve.png",
                                     "nova:synthetic/mannequin_humanoid", makeMannequinSkin,
                                     library, result);
        addPlayerBody(b, root, false, false, false, "Player ");
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        addElytraWings(b, root);
    } else if (id == "horse_armor") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/horse/horse_brown.png", "nova:synthetic/horse_base",
            [] { return makeSolidEntityTone(64, 64, QColor(138, 102, 74), QColor(124, 90, 64)); },
            library, result);
        addHorse(b, root, 0.f, "Horse ");
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        addHorse(b, root, .15f, "Armor ");
    } else if (id == "wolf_armor") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/wolf/wolf.png", "nova:synthetic/wolf_base",
            [] { return makeSolidEntityTone(64, 32, QColor(178, 174, 168), QColor(162, 158, 152)); },
            library, result);
        addWolf(b, root, 0.f, "Wolf ");
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        addWolf(b, root, .2f, "Armor ");
    } else if (id == "llama_decor") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/llama/creamy.png", "nova:synthetic/llama_base",
            [] { return makeSolidEntityTone(128, 64, QColor(196, 178, 148), QColor(182, 164, 136)); },
            library, result);
        addLlama(b, root, 0.f, "Llama ");
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        addLlama(b, root, .5f, "Decor ");
    } else if (id == "pig_saddle") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/pig/pig.png", "nova:synthetic/pig_base",
            [] { return makeSolidEntityTone(64, 32, QColor(220, 158, 158), QColor(206, 144, 144)); },
            library, result);
        addPig(b, root, 0.f, "Pig ");
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        addPig(b, root, .5f, "Saddle ");
    } else if (id == "strider_saddle") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/strider/strider.png", "nova:synthetic/strider_base",
            [] { return makeSolidEntityTone(64, 128, QColor(156, 76, 78), QColor(142, 66, 68)); },
            library, result);
        addStrider(b, root, 0.f, "Strider ");
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        addStrider(b, root, .5f, "Saddle ");
    } else if (id == "camel_saddle") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/camel/camel.png", "nova:synthetic/camel_base",
            [] { return makeSolidEntityTone(128, 128, QColor(202, 166, 114), QColor(188, 152, 102)); },
            library, result);
        addCamel(b, root, 0.f, "Camel ");
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        addCamel(b, root, .5f, "Saddle ");
    } else if (id == "happy_ghast_harness") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/ghast/happy_ghast.png", "nova:synthetic/ghast_base",
            [] { return makeSolidEntityTone(64, 32, QColor(224, 224, 224), QColor(210, 210, 210)); },
            library, result);
        addGhast(b, root, 0.f, "Ghast ");
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        addGhast(b, root, .5f, "Harness ");
    } else if (id == "player_wide" || id == "player_slim") {
        b.layer = EntityLayer::CutoutNoCull;
        addPlayerBody(b, entityRoot(24.f), id == "player_slim", legacySkin, true);
    } else if (id == "chest" || id == "chest_double") {
        addChest(b, id == "chest_double");
    } else if (id == "bed") {
        addBed(b);
    } else if (id == "shulker") {
        addShulker(b);
    } else if (id == "banner" || id == "banner_pattern") {
        addBanner(b, textureAsset, id == "banner_pattern", library, result);
    } else if (id == "shield" || id == "shield_pattern") {
        addShield(b, textureAsset, id == "shield_pattern", library, result);
    } else if (id == "decorated_pot_base" || id == "decorated_pot_side") {
        addDecoratedPot(b, textureAsset, id == "decorated_pot_base", library, result);
    } else if (id == "conduit") {
        b.cols = 32.f;
        b.rows = 16.f;
        b.box("Shell", {5, 5, 5}, {6, 6, 6}, 0, 0);
    } else if (id == "sign" || id == "hanging_sign") {
        addSign(b, id == "hanging_sign");
    } else if (id == "boat" || id == "chest_boat" || id == "raft" || id == "chest_raft") {
        addBoat(b, id == "chest_boat" || id == "chest_raft", id == "raft" || id == "chest_raft");
    } else if (id == "minecart") {
        addMinecart(b);
    } else if (id == "armor_stand") {
        addArmorStand(b, entityRoot(24.f));
    } else if (id == "trident") {
        addTrident(b);
    } else if (id == "book") {
        addBook(b);
    } else if (id == "bell") {
        addBell(b);
    } else if (id == "end_crystal") {
        b.layer = EntityLayer::CutoutNoCull;
        addEndCrystal(b);
    } else if (id == "arrow") {
        addArrow(b);
    } else if (id == "copper_golem") {
        b.layer = EntityLayer::CutoutNoCull;
        addCopperGolem(b, entityRoot(24.f));
    } else if (id == "zombie") {
        b.layer = EntityLayer::CutoutNoCull;
        addPlayerBody(b, entityRoot(24.f), false, legacySkin, !legacySkin);
    } else if (id == "skeleton") {
        b.layer = EntityLayer::CutoutNoCull;
        addSkeleton(b, entityRoot(24.f));
    } else if (id == "skeleton_overlay") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/skeleton/skeleton.png", "nova:synthetic/skeleton_base",
            [] { return makeSolidEntityTone(64, 32, QColor(196, 194, 186), QColor(178, 176, 168)); },
            library, result);
        addSkeleton(b, root, 0.f, "Skeleton ");
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        addPlayerBody(b, root, false, true, false, "Overlay ");
    } else if (id == "piglin") {
        b.layer = EntityLayer::CutoutNoCull;
        addPiglin(b, entityRoot(24.f));
    } else if (id == "villager") {
        b.layer = EntityLayer::CutoutNoCull;
        addVillager(b, entityRoot(24.f));
    } else if (id == "villager_overlay") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/villager/villager.png", "nova:synthetic/villager_base",
            [] { return makeSolidEntityTone(64, 64, QColor(168, 128, 102), QColor(154, 114, 90)); },
            library, result);
        addVillager(b, root, 0.f, "Villager ");
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        addVillager(b, root, .25f, "Outfit ");
    } else if (id == "enderman") {
        b.layer = EntityLayer::CutoutNoCull;
        addEnderman(b, entityRoot(24.f, .75f));
    } else if (id == "iron_golem") {
        addIronGolem(b, entityRoot(24.f, .75f));
    } else if (id == "creeper") {
        addCreeper(b, entityRoot(24.f));
    } else if (id == "creeper_armor") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/creeper/creeper.png", "nova:synthetic/creeper_base",
            [] { return makeSolidEntityTone(64, 32, QColor(92, 152, 82), QColor(76, 134, 68)); },
            library, result);
        addCreeper(b, root, 0.f, "Creeper ");
        b.layer = EntityLayer::Overlay;
        b.texture = textureAsset;
        addCreeper(b, root, 1.5f, "Charge ");
    } else if (id == "pig") {
        addPig(b, entityRoot(24.f));
    } else if (id == "cow") {
        addCow(b, entityRoot(24.f));
    } else if (id == "sheep") {
        addSheep(b, entityRoot(24.f), false);
    } else if (id == "sheep_fur") {
        const auto root = entityRoot(24.f);
        b.layer = EntityLayer::Cutout;
        b.texture = resolveCompanion(
            "assets/minecraft/textures/entity/sheep/sheep.png", "nova:synthetic/sheep_base",
            [] { return makeSolidEntityTone(64, 32, QColor(208, 178, 152), QColor(194, 164, 138)); },
            library, result);
        addSheep(b, root, false, 0.f, "Sheep ");
        b.layer = EntityLayer::CutoutNoCull;
        b.texture = textureAsset;
        addSheep(b, root, true, 0.f, "Fleece ");
    } else if (id == "chicken") {
        addChicken(b, entityRoot(24.f));
    } else if (id == "wolf") {
        addWolf(b, entityRoot(24.f));
    } else if (id == "cat") {
        addCat(b, entityRoot(24.f));
    } else if (id == "fox") {
        addFox(b, entityRoot(24.f));
    } else if (id == "horse") {
        addHorse(b, entityRoot(24.f));
    } else if (id == "llama") {
        addLlama(b, entityRoot(24.f));
    } else if (id == "camel") {
        addCamel(b, entityRoot(24.f, .75f));
    } else if (id == "goat") {
        addGoat(b, entityRoot(24.f));
    } else if (id == "panda") {
        addPanda(b, entityRoot(24.f));
    } else if (id == "polar_bear") {
        addPolarBear(b, entityRoot(24.f));
    } else if (id == "rabbit") {
        addRabbit(b, entityRoot(24.f));
    } else if (id == "turtle") {
        addTurtle(b, entityRoot(24.f));
    } else if (id == "frog") {
        addFrog(b, entityRoot(24.f));
    } else if (id == "axolotl") {
        b.layer = EntityLayer::CutoutNoCull;
        addAxolotl(b, entityRoot(24.f));
    } else if (id == "armadillo") {
        addArmadillo(b, entityRoot(24.f));
    } else if (id == "sniffer") {
        addSniffer(b, entityRoot(24.f, .6f));
    } else if (id == "strider") {
        addStrider(b, entityRoot(24.f));
    } else if (id == "hoglin") {
        addHoglin(b, entityRoot(24.f));
    } else if (id == "ravager") {
        addRavager(b, entityRoot(24.f, .65f));
    } else if (id == "spider") {
        addSpider(b, entityRoot(24.f));
    } else if (id == "silverfish") {
        addSilverfish(b, entityRoot(24.f));
    } else if (id == "slime" || id == "magma_cube") {
        addSlime(b, entityRoot(24.f), id == "magma_cube");
    } else if (id == "ghast") {
        addGhast(b, entityRoot(24.f));
    } else if (id == "phantom") {
        b.layer = EntityLayer::CutoutNoCull;
        addPhantom(b, entityRoot(20.f));
    } else if (id == "bat") {
        b.layer = EntityLayer::CutoutNoCull;
        addBat(b, entityRoot(24.f));
    } else if (id == "bee") {
        b.layer = EntityLayer::CutoutNoCull;
        addBee(b, entityRoot(24.f));
    } else if (id == "parrot") {
        b.layer = EntityLayer::CutoutNoCull;
        addParrot(b, entityRoot(24.f));
    } else if (id == "dolphin") {
        addDolphin(b, entityRoot(24.f));
    } else if (id == "squid") {
        addSquid(b, entityRoot(24.f));
    } else if (id == "guardian") {
        addGuardian(b, entityRoot(24.f));
    } else if (id == "cod" || id == "salmon") {
        b.layer = EntityLayer::CutoutNoCull;
        addFish(b, entityRoot(24.f), id == "salmon");
    } else if (id == "blaze") {
        addBlaze(b, entityRoot(24.f));
    } else if (id == "breeze") {
        b.layer = EntityLayer::CutoutNoCull;
        addBreeze(b, entityRoot(24.f));
    } else if (id == "allay") {
        b.layer = EntityLayer::CutoutNoCull;
        addAllay(b, entityRoot(24.f));
    } else if (id == "warden") {
        b.layer = EntityLayer::CutoutNoCull;
        addWarden(b, entityRoot(24.f, .7f));
    } else if (id == "creaking") {
        b.layer = EntityLayer::CutoutNoCull;
        addCreaking(b, entityRoot(24.f, .75f));
    } else if (id == "ender_dragon") {
        b.layer = EntityLayer::CutoutNoCull;
        addEnderDragon(b, entityRoot(24.f, .35f));
    } else if (id == "wither") {
        addWither(b, entityRoot(24.f));
    } else if (id == "wither_skull") {
        b.cols = 64.f;
        b.rows = 32.f;
        b.box("Skull", {-4, -8, -4}, {8, 8, 8}, 0, 0, entityRoot(12.f));
    } else if (id == "painting") {
        addPainting(b, textureAsset, activeImage, library, result);
    } else if (id == "fluid_block") {
        addFullBlockCube(b, textureAsset, EntityLayer::Overlay, activeImage);
    } else if (id == "flame_cross") {
        addCrossedPlanes(b, textureAsset, activeImage);
    } else if (id == "block_cube") {
        addFullBlockCube(b, textureAsset, EntityLayer::Cutout, activeImage);
    } else if (id == "billboard") {
        addBillboardCard(b, textureAsset, activeImage, spec.label);
    }

    result.mesh = std::move(b.mesh);
    return result;
}

} // namespace nova::mc
