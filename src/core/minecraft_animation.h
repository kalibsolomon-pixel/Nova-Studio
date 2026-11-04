// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
namespace nova {
struct AnimationExport {
    QImage sheet;
    QJsonObject metadata;
    QByteArray png, mcmeta;
    QStringList diagnostics;
    bool ok() const { return !png.isEmpty() && !mcmeta.isEmpty(); }
};
bool supportedAnimationTarget(const QString &target);
bool parseAnimation(const QImage &sheet, const QJsonObject &metadata, const QString &target,
                    AnimationState *state, QString *error, const AnimationLimits &limits = {});
AnimationExport planAnimationExport(const Document &document, int columns = 1,
                                    const QString &target = {}, QString *error = nullptr);
std::shared_ptr<Document> openMinecraftAnimation(const QString &png, const QString &target,
                                               QString *error = nullptr);
QImage interpolateFrames(const QImage &from, const QImage &to, double fraction);
} // namespace nova
