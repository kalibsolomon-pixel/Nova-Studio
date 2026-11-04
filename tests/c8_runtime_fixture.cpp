// SPDX-License-Identifier: GPL-3.0-or-later
// Authors the C8 runtime comparison models with Nova's own creation and display-editing
// engine (the code paths behind Create Minecraft asset and the display editor).
//   c8_preset: model/texture pair from the cube template (template display preset)
//   c8_custom: cube model whose display was edited: GUI 45° roll at 0.8, small first-person
//              right hand (left hand inherits it mirrored), raised third-person right hand.
#include "creation.h"
#include "display.h"
#include "edit.h"
#include <QCoreApplication>
#include <QDir>
#include <cstdio>
using namespace nova;
static int fail(const QString &message) {
    std::fprintf(stderr, "%s\n", qPrintable(message));
    return 1;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2)
        return fail("usage: nova-c8-runtime-fixture <pack root>");
    const QString root = QString::fromLocal8Bit(argv[1]);
    mc::CreationRequest pair;
    pair.type = mc::AssetType::ModelTexturePair;
    pair.id = "nova:block/c8_preset";
    pair.texture = "nova:block/c8_check";
    pair.version = "1.21.11";
    auto plan = mc::planCreation(root, pair);
    if (!plan.valid())
        return fail("preset plan: " + plan.error);
    auto created = mc::executeCreation(plan);
    if (!created.complete)
        return fail("preset create: " + created.error);
    mc::CreationRequest model;
    model.type = mc::AssetType::BlockModel;
    model.id = "nova:block/c8_custom";
    model.texture = "nova:block/c8_check";
    model.version = "1.21.11";
    plan = mc::planCreation(root, model);
    if (!plan.valid())
        return fail("custom plan: " + plan.error);
    created = mc::executeCreation(plan);
    if (!created.complete)
        return fail("custom create: " + created.error);
    mc::ModelEdit edit;
    QString error;
    if (!edit.open(root, "assets/nova/models/block/c8_custom.json", &error))
        return fail("open: " + error);
    mc::DisplayTransform gui, first, third;
    gui.rotation = {0, 0, 45};
    gui.scale = {0.8, 0.8, 0.8};
    first.rotation = {0, 45, 0};
    first.scale = {0.25, 0.25, 0.25};
    third.rotation = {75, 45, 0};
    third.translation = {0, 6, 0};
    third.scale = {0.375, 0.375, 0.375};
    if (!edit.previewDisplay("gui", gui, &error) || !edit.previewDisplay("firstperson_righthand", first, &error) ||
        !edit.resetDisplay("firstperson_lefthand", &error) ||
        !edit.previewDisplay("thirdperson_righthand", third, &error) || !edit.commitPreview("Runtime fixture") ||
        !edit.save(&error))
        return fail("display edit: " + error);
    std::printf("%s\n", qPrintable(QDir(root).absolutePath()));
    return 0;
}
