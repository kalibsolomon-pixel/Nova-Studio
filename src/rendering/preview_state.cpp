// SPDX-License-Identifier: GPL-3.0-or-later
#include "cube.h"
#include <QJsonArray>
#include <cmath>
namespace nova {
QJsonObject Cube::resourceCounts() const {
    return {{"textures", materialTextures.size() + (texture ? 1 : 0)},
            {"buffers",
             int(buffer.isCreated()) + int(lineBuffer.isCreated()) + int(guideBuffer.isCreated())},
            {"programs", program && program->isLinked() ? 1 : 0}};
}
QJsonObject Cube::viewSettings() const {
    return {{"pitch", pitch},
            {"yaw", yaw},
            {"scale", scale},
            {"pan", QJsonArray{cameraPan.x(), cameraPan.y()}},
            {"perspective", perspective},
            {"wireframe", wireframe},
            {"outlines", outlines},
            {"axes", axes},
            {"grid", groundGrid},
            {"neutral", neutral},
            {"ambient", ambient},
            {"light", QJsonArray{lightDirection.x(), lightDirection.y(), lightDirection.z()}}};
}
void Cube::restoreView(const QJsonObject &state) {
    auto number = [&](const char *name, double fallback, double lo, double hi) {
        const auto v = state[name];
        return v.isDouble() && std::isfinite(v.toDouble()) ? std::clamp(v.toDouble(), lo, hi)
                                                           : fallback;
    };
    pitch = float(number("pitch", 25, -89, 89));
    yaw = float(number("yaw", 38, -36000, 36000));
    scale = float(number("scale", 1, .05, 8));
    cameraPan = {};
    const auto pan = state["pan"].toArray();
    if (pan.size() == 2 && std::isfinite(pan[0].toDouble()) && std::isfinite(pan[1].toDouble()))
        cameraPan = {std::clamp(pan[0].toDouble(), -1000., 1000.),
                     std::clamp(pan[1].toDouble(), -1000., 1000.)};
    perspective = state["perspective"].toBool();
    wireframe = state["wireframe"].toBool();
    outlines = state["outlines"].toBool();
    axes = state["axes"].toBool();
    groundGrid = state["grid"].toBool();
    neutral = state["neutral"].toBool();
    ambient = float(number("ambient", .55, 0, 1));
    lightDirection = {-.4f, .8f, .6f};
    const auto light = state["light"].toArray();
    if (light.size() == 3) {
        QVector3D direction;
        bool valid = true;
        for (int i = 0; i < 3; ++i) {
            valid &= light[i].isDouble() && std::isfinite(light[i].toDouble()) &&
                     std::abs(light[i].toDouble()) <= 10000;
            direction[i] = float(light[i].toDouble());
        }
        if (valid && direction.lengthSquared() > 1e-12)
            lightDirection = direction.normalized();
    }
    update();
}
} // namespace nova
