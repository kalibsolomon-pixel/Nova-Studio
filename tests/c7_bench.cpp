// SPDX-License-Identifier: GPL-3.0-or-later
#include "batch.h"
#include "edit.h"
#include "modelauthor.h"
#include "project.h"
#include "resourcetools.h"
#include "validation.h"
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace nova;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc != 2)
        return 2;
    QTemporaryDir pack, exported;
    mc::CreationRequest request;
    request.id = "nova:block/original";
    request.type = mc::AssetType::ModelTexturePair;
    auto plan = mc::planCreation(pack.path(), request);
    if (!mc::executeCreation(plan).complete)
        return 3;
    QElapsedTimer timer;
    timer.start();
    auto index = Index::scan(pack.path());
    mc::Library library;
    library.update(pack.path(), index);
    const double dependencies = timer.nsecsElapsed() / 1e6;
    auto document = std::make_shared<mc::ModelEdit>();
    timer.restart();
    if (!document->open(pack.path(), plan.files[0].relative))
        return 4;
    const double opening = timer.nsecsElapsed() / 1e6;
    QJsonArray edits, meshes, uvs;
    for (int i = 0; i < 100; ++i) {
        mc::ElementTransform t;
        t.from = {float(i % 8), 0, 0};
        timer.restart();
        if (!document->previewElement(0, t) || !document->commitPreview())
            return 5;
        edits.append(timer.nsecsElapsed() / 1e6);
        library.previewSource(plan.files[0].relative, document->source());
        timer.restart();
        auto mesh = mc::mesh(library.model(request.id));
        if (mesh.faces.size() != 6)
            return 6;
        meshes.append(timer.nsecsElapsed() / 1e6);
        timer.restart();
        if (!document->transformUvs({{0, "south"}}, mc::UvOperation::Move, {.125, .25}) ||
            !document->commitPreview())
            return 7;
        uvs.append(timer.nsecsElapsed() / 1e6);
    }
    if (!document->save())
        return 8;
    index = Index::scan(pack.path());
    library.update(pack.path(), index, {plan.files[0].relative});
    mc::ValidationEngine engine;
    timer.restart();
    auto report = engine.run(pack.path(), index, library);
    const double validation = timer.nsecsElapsed() / 1e6;
    timer.restart();
    auto cached = engine.run(pack.path(), index, library);
    const double revalidation = timer.nsecsElapsed() / 1e6;
    ResourceTools problems;
    problems.context = [&] {
        return ResourceContext{
            pack.path(), plan.files[0].relative, {}, index, std::make_shared<mc::Library>(library)};
    };
    problems.show();
    timer.restart();
    problems.validate();
    while (!problems.findChild<QPushButton *>("runValidation")->isEnabled() &&
           timer.elapsed() < 10000) {
        app.processEvents();
        QTest::qWait(1);
    }
    const double population = timer.nsecsElapsed() / 1e6;
    batch::Pipeline pipeline;
    pipeline.steps = {{"hsv", {{"hue", 20}}}};
    QString error;
    if (!pipeline.validate(&error)) {
        pipeline.steps = {{"invert", {}}};
        if (!pipeline.validate(&error))
            return 9;
    }
    batch::Output output;
    output.directory = exported.path() + "/textures";
    auto textures = mc::modelTextureScope(library, plan.files[0].relative, false);
    auto batchPlan = batch::plan(pipeline, pack.path(), textures, output);
    timer.restart();
    auto result = batch::execute(pipeline, batchPlan, output, {}, nullptr);
    const double batchMs = timer.nsecsElapsed() / 1e6;
    if (result.done != 1)
        return 10;
    timer.restart();
    if (!Project::exportMinecraft(pack.path(), exported.path() + "/pack", &error))
        return 11;
    const double exporting = timer.nsecsElapsed() / 1e6;
    // Inject a verified obsolete shade field to time the real backup/repair path.
    auto source = document->authoritative();
    auto elements = source["elements"].toArray();
    auto element = elements[0].toObject();
    element["shade"] = false;
    elements[0] = element;
    source["elements"] = elements;
    auto path = pack.path() + '/' + plan.files[0].relative;
    if (!atomicWrite(path, QJsonDocument(source).toJson(), Revision::read(path), &error))
        return 12;
    index = Index::scan(pack.path());
    library.setVersion("26.3");
    library.update(pack.path(), index, {plan.files[0].relative});
    auto repairReport = engine.run(pack.path(), index, library);
    double repairMs = -1;
    for (const auto &d : repairReport.diagnostics)
        if (d.code == "shade_migration") {
            timer.restart();
            auto fix = mc::applyRepair(pack.path(), d);
            repairMs = timer.nsecsElapsed() / 1e6;
            if (!fix.applied)
                return 13;
            break;
        }
    QJsonObject json{{"model_open_ms", opening},
                     {"dependency_resolution_ms", dependencies},
                     {"edit_commit_ms", edits},
                     {"mesh_update_ms", meshes},
                     {"uv_operation_ms", uvs},
                     {"validation_ms", validation},
                     {"cached_validation_ms", revalidation},
                     {"files", report.files},
                     {"cached_files", cached.reused},
                     {"problems_async_population_ms", population},
                     {"quick_fix_ms", repairMs},
                     {"model_texture_batch_ms", batchMs},
                     {"resource_pack_export_ms", exporting}};
    const QString large = qEnvironmentVariable("NOVA_LARGE_FIXTURE");
    if (!large.isEmpty()) {
        auto largeIndex = Index::scan(large);
        mc::Library largeLibrary;
        largeLibrary.update(large, largeIndex);
        mc::ValidationEngine largeEngine;
        timer.restart();
        auto initial = largeEngine.run(large, largeIndex, largeLibrary);
        const double initialMs = timer.nsecsElapsed() / 1e6;
        timer.restart();
        auto warm = largeEngine.run(large, largeIndex, largeLibrary);
        const double warmMs = timer.nsecsElapsed() / 1e6;
        json["large_validation"] = QJsonObject{{"files", initial.files},
                                               {"cold_ms", initialMs},
                                               {"warm_ms", warmMs},
                                               {"cached", warm.reused},
                                               {"diagnostics", initial.diagnostics.size()}};
    }
    QFile out(QString::fromLocal8Bit(argv[1]));
    if (!out.open(QIODevice::WriteOnly))
        return 14;
    out.write(QJsonDocument(json).toJson());
    return 0;
}
