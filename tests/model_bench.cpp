// SPDX-License-Identifier: GPL-3.0-or-later
#include "modelpanel.h"
#include "playback.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QOpenGLTimerQuery>
#include <QtTest>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#endif
using namespace nova;
// Optional benchmark-only GPU scope. Interactive rendering never waits for a
// query. The readback benchmark already synchronizes; collect the previous draw
// only when available, and keep it separate from readback/wall timings.
class TimedCube : public Cube {
    std::unique_ptr<QOpenGLTimerQuery> query;
    bool enabled, attempted = false, pending = false;
  protected:
    void paintGL() override {
        if (enabled && !attempted) {
            attempted = true;
            query = std::make_unique<QOpenGLTimerQuery>();
            if (!query->create()) query.reset();
        }
        if (query && pending && query->isResultAvailable()) {
            gpuMs.append(double(query->waitForResult()) / 1e6);
            pending = false;
        }
        const bool measure = query && !pending;
        if (measure) query->begin();
        Cube::paintGL();
        if (measure) { query->end(); pending = true; }
    }
  public:
    QJsonArray gpuMs;
    TimedCube(Metrics *metrics, bool gpu) : Cube(metrics), enabled(gpu) {}
    ~TimedCube() override {
        if (query) { makeCurrent(); query.reset(); doneCurrent(); }
    }
};
qint64 workingSet() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX m{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m),
                         sizeof(m));
    return qint64(m.WorkingSetSize);
#else
    return 0;
#endif
}
double processCpu() {
#ifdef Q_OS_WIN
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULARGE_INTEGER kernel{}, user{};
    kernel.LowPart = k.dwLowDateTime;
    kernel.HighPart = k.dwHighDateTime;
    user.LowPart = u.dwLowDateTime;
    user.HighPart = u.dwHighDateTime;
    return double(kernel.QuadPart + user.QuadPart) / 10000;
#else
    return 0;
#endif
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc != 3 && argc != 4)
        return 2;
    const QString root = QString::fromLocal8Bit(argv[1]);
    QElapsedTimer t;
    t.start();
    auto index = Index::scan(root);
    const double indexing = t.nsecsElapsed() / 1e6;
    mc::Library library;
    t.restart();
    library.update(root, index);
    const double parse = t.nsecsElapsed() / 1e6;
    auto model = library.model("nova:block/complex");
    if (!model.valid)
        return 3;
    t.restart();
    for (int i = 0; i < 1000; ++i)
        if (library.model("nova:block/complex").id.isEmpty())
            return 4;
    const double cached = t.nsecsElapsed() / 1e6;
    t.restart();
    for (int i = 0; i < 1000; ++i)
        if (mc::mesh(model).faces.size() != 18)
            return 5;
    const double meshes = t.nsecsElapsed() / 1e6;
    t.restart();
    for (int i = 0; i < 1000; ++i) {
        auto snapshot = library;
        if (!snapshot.model("nova:block/inherited").valid)
            return 8;
    }
    const double parent = t.nsecsElapsed() / 1e6;
    QJsonObject largeIndex;
    const auto largeRoot = QDir(root).absoluteFilePath("../generated/pack-50000");
    if (QFileInfo::exists(largeRoot)) {
        t.restart();
        auto large = Index::scan(largeRoot);
        const double indexTime = t.nsecsElapsed() / 1e6;
        mc::Library catalog;
        t.restart();
        catalog.update(largeRoot, large);
        largeIndex = {{"assets", large.files.size()},
                      {"index_ms", indexTime},
                      {"dependency_catalog_ms", t.nsecsElapsed() / 1e6},
                      {"parsed_json", double(catalog.parsedFiles)}};
    }
    Metrics metrics;
    auto doc = std::make_shared<Document>(root + "/assets/nova/textures/block/check.png",
                                          loadPng(root + "/assets/nova/textures/block/check.png"));
    TimedCube cube(&metrics, app.arguments().contains("--gpu"));
    cube.resize(360, 320);
    cube.setDocument(doc);
    auto geometry = std::make_shared<mc::Mesh>(mc::mesh(model));
    cube.setModel(geometry, "assets/nova/textures/block/check.png");
    cube.show();
    QTest::qWait(80);
    cube.grabFramebuffer();
    QJsonArray frames, updates;
    for (int i = 0; i < 40; ++i) {
        t.restart();
        cube.perspective = (i % 2) == 0;
        cube.update();
        QApplication::processEvents();
        auto frame = cube.grabFramebuffer();
        if (frame.isNull())
            return 6;
        frames.append(t.nsecsElapsed() / 1e6);
    }
    for (int i = 0; i < 40; ++i) {
        doc->draw({i % 16, (i / 16) % 16}, {i % 16, (i / 16) % 16}, qRgb(i * 5, 80, 160));
        doc->finish();
        t.restart();
        cube.setDocument(doc);
        cube.update();
        QApplication::processEvents();
        cube.grabFramebuffer();
        updates.append(t.nsecsElapsed() / 1e6);
    }
    const auto primaryCounters = metrics.counters;
    const auto beforeModels = workingSet();
    DocumentSession session;
    session.setProject(root);
    session.adopt(doc);
    Canvas canvas(doc, &metrics);
    ModelPanel panel(&cube, &session);
    panel.setLibrary(std::make_shared<mc::Library>(library), root);
    panel.setDocument(doc, &canvas);
    for (int i = 0; i < 25; ++i) {
        panel.openAsset(i % 2 ? "assets/nova/models/block/check.json"
                              : "assets/nova/models/block/complex.json");
        cube.grabFramebuffer();
    }
    const auto afterModels = workingSet();
    t.restart();
    for (int i = 0; i < 100; ++i)
        panel.selectFace(i % 6);
    const double uv = t.nsecsElapsed() / 1e6;
    auto targets = panel.findChild<QComboBox *>("modelTarget");
    targets->setCurrentIndex(0);
    cube.grabFramebuffer();
    QTest::qWait(50);
    const auto afterClose = workingSet();
    QString error;
    auto animated = session.open(root + "/assets/nova/textures/block/animated.png", &error);
    if (!animated || !animated->animated())
        return 9;
    cube.setDocument(animated);
    canvas.document = animated;
    panel.setDocument(animated, &canvas);
    panel.openAsset("assets/nova/models/block/animated.json");
    PlaybackController playback;
    QObject::connect(&playback, &PlaybackController::displayed, &canvas,
                     [&](const QImage &image, int) {
                         canvas.setPlaybackPreview(image);
                         cube.setPlaybackPreview(image);
                     });
    playback.setDocument(animated);
    const double cpuBefore = processCpu();
    t.restart();
    playback.play();
    QTest::qWait(1100);
    playback.stop();
    const double playbackWall = t.nsecsElapsed() / 1e6, playbackCpu = processCpu() - cpuBefore;
    QJsonObject output{{"index_ms", indexing},
                       {"catalog_parse_ms", parse},
                       {"json_files", double(library.parsedFiles)},
                       {"cached_model_1000_ms", cached},
                       {"mesh_1000_ms", meshes},
                       {"uncached_parent_resolution_1000_ms", parent},
                       {"large_project", largeIndex},
                       {"uv_selection_100_ms", uv},
                       {"working_set_before_model_cycles", double(beforeModels)},
                       {"working_set_after_25_model_cycles", double(afterModels)},
                       {"working_set_after_generic_close", double(afterClose)},
                       {"animated_model_playback_wall_ms", playbackWall},
                       {"animated_model_playback_cpu_ms", playbackCpu},
                       {"faces", 18},
                       {"camera_readback_ms", frames},
                       {"pixel_update_readback_ms", updates},
                       {"counters", primaryCounters},
                       {"lifecycle_counters", metrics.counters},
                       {"environment", metrics.environment},
                       {"render_submit_cpu_ms", metrics.samples["preview_submit_cpu"]},
                       {"gpu_draw_ms", cube.gpuMs},
                       {"scope", "Widget readback timings include CPU/GPU synchronization; they "
                                 "are not presentation frame-rate or GPU-only timings"}};
    QFile out(QString::fromLocal8Bit(argv[2]));
    if (!out.open(QIODevice::WriteOnly))
        return 7;
    out.write(QJsonDocument(output).toJson());
    return 0;
}
