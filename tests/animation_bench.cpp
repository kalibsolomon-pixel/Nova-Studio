// SPDX-License-Identifier: GPL-3.0-or-later
#include "animation_export.h"
#include "canvas.h"
#include "cube.h"
#include "minecraft_animation.h"
#include "recovery.h"
#include "timeline.h"
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <array>
#include <cmath>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
using namespace nova;
namespace {
double elapsed(QElapsedTimer &timer) { return double(timer.nsecsElapsed()) / 1e6; }
QJsonObject distribution(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return {{"n", int(values.size())}, {"p50", values[values.size() / 2]}, {"p95", values[size_t(std::ceil(values.size() * .95)) - 1]}};
}
qint64 resident() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX memory{}; GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)); return qint64(memory.WorkingSetSize);
#else
    return 0;
#endif
}
double cpuMilliseconds() {
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user; GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    ULARGE_INTEGER k{}, u{}; k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime; return double(k.QuadPart + u.QuadPart) / 10000;
#else
    return 0;
#endif
}
QImage pattern(int size, int frame, int layer) {
    QImage image(size, size, QImage::Format_RGBA8888);
    for (int y = 0; y < size; ++y) {
        auto row = image.scanLine(y);
        for (int x = 0; x < size; ++x) {
            auto p = row + x * 4; p[0] = uchar((x * 17 + frame * 13) % 256); p[1] = uchar((y * 19 + frame * 7) % 256);
            p[2] = uchar((x * y + layer * 83) % 256); p[3] = layer ? 90 : 255;
        }
    }
    return image;
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const bool playbackTiming = argc == 3 && QString::fromLocal8Bit(argv[2]) == "--playback-timing";
    if (argc != 2 && !playbackTiming) return 2;
    QJsonArray results;
    for (const auto workload : {std::array{16, 32, 1}, std::array{32, 100, 1}, std::array{64, 256, 1},
         std::array{128, 100, 1}, std::array{256, 64, 4}, std::array{64, 1024, 2}, std::array{256, 128, 3}}) {
        const int side = workload[0], count = workload[1], layerCount = workload[2];
        if (playbackTiming && count != 1024) continue;
        QTemporaryDir temp; const auto path = temp.filePath("texture.png"); QElapsedTimer timer; timer.start();
        const auto memoryBefore = resident(); auto doc = std::make_shared<Document>(path, pattern(side, 0, 0));
        if (count == 1024) { auto limits = doc->frameLimits(); limits.sourceFrames = 2048; limits.playbackEntries = 32768; doc->setAnimationLimits(limits); }
        auto state = doc->animationState(); state.enabled = true; state.frames.clear(); state.entries.clear(); state.defaultTicks = 2;
        for (int i = 0; i < count; ++i) {
            SourceFrame frame; frame.name = QString("Frame %1").arg(i + 1);
            for (int j = 0; j < layerCount; ++j) {
                RasterLayer layer; layer.id = QUuid::createUuid(); layer.name = QString("Layer %1").arg(j + 1); layer.pixels = pattern(side, i, j); frame.layers.push_back(layer);
            }
            frame.activeLayer = frame.layers.back().id; state.entries.push_back({frame.id, 0}); state.frames.push_back(frame);
        }
        if (count == 1024) while (state.entries.size() < 16384) state.entries.push_back(state.entries[state.entries.size() % size_t(count)]);
        state.active = state.frames[0].id; QString error;
        if (!doc->replaceAnimation(state, "Benchmark creation", &error)) { qCritical() << error; return 3; }
        QJsonObject row{{"side", side}, {"source_frames", count}, {"layers_per_frame", layerCount},
                        {"playback_entries", int(state.entries.size())}, {"creation_ms", elapsed(timer)},
                        {"logical_pixels_bytes", double(doc->pixelBytes())}, {"working_set_delta_bytes", double(resident() - memoryBefore)}};
        std::vector<double> times;
        for (int i = 0; i < 7; ++i) {
            timer.restart(); if (!doc->editFrame(FrameCommand::Duplicate, {}, -1, {}, &error)) { qCritical() << error; return 4; }
            times.push_back(elapsed(timer)); if (!doc->canUndo()) return 5; doc->undo();
        }
        row["duplicate_ms"] = distribution(times); times.clear();
        for (int i = 0; i < 100; ++i) { timer.restart(); doc->selectFrame(state.frames[size_t((i * 17) % count)].id); times.push_back(elapsed(timer)); }
        row["switch_ms"] = distribution(times); times.clear(); doc->setAnimationLimits(doc->frameLimits());
        for (const auto &frame : state.frames) { timer.restart(); auto image = doc->frameComposite(frame.id); if (image.isNull()) return 6; times.push_back(elapsed(timer)); }
        row["composite_ms"] = distribution(times); row["composite_cache_bytes"] = double(doc->frameCacheBytes());
        timer.restart(); auto bytes = doc->encodeNative("texture.png", &error); row["native_encode_ms"] = elapsed(timer); row["native_bytes"] = double(bytes.size());
        if (bytes.isEmpty()) { qCritical() << error; return 7; }
        Document loaded(path, QImage{}); timer.restart(); if (!loaded.decodeNative(bytes, "texture.png", &error)) return 8; row["native_decode_ms"] = elapsed(timer);
        timer.restart(); if (!doc->saveNative(temp.path(), &error)) return 14; row["native_guarded_save_ms"] = elapsed(timer);
        Document fromDisk(path, QImage{}); timer.restart(); if (!fromDisk.loadNative(temp.path(), &error)) return 15; row["native_guarded_load_ms"] = elapsed(timer);
        RecoveryStore recovery(temp.filePath("recovery")); timer.restart(); if (!recovery.writeDocument(*doc->recoverySnapshot(), temp.path(), &error)) return 9;
        row["recovery_serialization_ms"] = elapsed(timer);
        timer.restart(); auto plan = planAnimationExport(*doc, std::min(16, count), "26.1", &error); row["export_plan_ms"] = elapsed(timer);
        timer.restart(); auto journal = stageAnimationPair(path, plan.png, plan.mcmeta, {}, {}, &error);
        if (journal.isEmpty() || !finishAnimationPair(journal, PairAction::Retry, &error)) { qCritical() << error; return 10; }
        row["paired_write_ms"] = elapsed(timer);
        auto processed = doc->recoverySnapshot(); std::vector<QUuid> ids; for (const auto &frame : state.frames) ids.push_back(frame.id);
        timer.restart(); if (!processed->applyFrames(ids, {"invert", {}}, &error)) return 11; row["all_frames_invert_ms"] = elapsed(timer);
        Metrics metrics; DocumentSession session; if (!session.adopt(doc)) return 12;
        TimelinePanel timeline(&session); timeline.resize(1000, 300); timeline.setDocument(doc); timeline.show(); QTest::qWait(80);
        timer.restart(); auto view = timeline.timelineView();
        for (int i = 0; i < view->model()->rowCount(); i += std::max(1, count / 16)) { view->scrollTo(view->model()->index(i, 0)); QApplication::processEvents(); }
        row["timeline_scroll_and_thumbnails_ms"] = elapsed(timer);
        row["thumbnail_cache_bytes"] = double(timeline.findChild<TimelineModel *>()->thumbnailBytes());
        Canvas canvas(doc, &metrics); canvas.resize(700, 500); canvas.zoom = 384.0 / side; canvas.grid = false; canvas.show();
        doc->selectFrame(state.frames[size_t(count / 2)].id); canvas.onion = {true, 2, 2, 25, true}; times.clear();
        for (int i = 0; i < 5; ++i) { timer.restart(); auto pixmap = canvas.grab(); times.push_back(elapsed(timer)); }
        row["onion_render_ms"] = distribution(times); canvas.onion.enabled = false;
        Cube cube(&metrics); cube.resize(280, 240); cube.setDocument(doc); cube.show(); QTest::qWait(100);
        PlaybackController playback;
        QElapsedTimer cadence;
        QJsonArray displayTimes;
        QObject::connect(&playback, &PlaybackController::displayed, &canvas, [&](const QImage &image, int) {
            if (cadence.isValid()) displayTimes.append(elapsed(cadence));
            canvas.setPlaybackPreview(image); cube.setPlaybackPreview(image);
        });
        QObject::connect(&playback, &PlaybackController::stopped, &canvas, [&] { canvas.setPlaybackPreview({}); cube.setPlaybackPreview({}); });
        playback.setDocument(doc); timer.restart(); const auto cpuBefore = cpuMilliseconds();
        cadence.start(); playback.play(); QTest::qWait(playbackTiming ? 5000 : 1100); playback.stop();
        row["playback_wall_ms"] = elapsed(timer); row["playback_process_cpu_ms"] = cpuMilliseconds() - cpuBefore;
        QJsonArray intervals;
        for (qsizetype i = 1; i < displayTimes.size(); ++i)
            intervals.append(displayTimes[i].toDouble() - displayTimes[i - 1].toDouble());
        row["playback_display_times_ms"] = displayTimes;
        row["playback_display_intervals_ms"] = intervals;
        row["playback_default_entry_duration_ms"] = state.defaultTicks * 50;
        row["playback_timing_scope"] = "displayed-signal CPU timestamps, not physical presentation";
        row["gpu_upload_counts"] = metrics.counters; row["gpu_environment"] = metrics.environment;
        row["canvas_paint_cpu_ms"] = metrics.samples["canvas_paint_cpu"];
        row["preview_submit_cpu_ms"] = metrics.samples["preview_submit_cpu"];
        QTest::qWait(100); const auto idle = cpuMilliseconds(); QTest::qWait(500); row["stopped_idle_cpu_ms_per_500ms"] = cpuMilliseconds() - idle;
        row["history_bytes"] = double(doc->historyBytes()); row["working_set_bytes"] = double(resident());
        cube.setDocument({}); timeline.hide(); canvas.hide(); cube.hide();
        results.append(row); qInfo() << side << count << layerCount << "complete";
        QFile checkpoint(QString::fromLocal8Bit(argv[1]));
        if (!checkpoint.open(QIODevice::WriteOnly)) return 13;
        checkpoint.write(QJsonDocument(QJsonObject{{"workloads", results}, {"timing", "CPU measurements; GPU presentation is not measured"}}).toJson(QJsonDocument::Indented));
    }
    QFile output(QString::fromLocal8Bit(argv[1])); if (!output.open(QIODevice::WriteOnly)) return 13;
    output.write(QJsonDocument(QJsonObject{{"workloads", results}, {"timing", "CPU measurements; GPU presentation is not measured"}}).toJson(QJsonDocument::Indented)); return 0;
}
