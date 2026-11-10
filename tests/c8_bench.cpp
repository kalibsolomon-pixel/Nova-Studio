// SPDX-License-Identifier: GPL-3.0-or-later
// C8 workload benchmark: display preview, bulk UV, native batch, large animation storage.
#include "creation.h"
#include "display.h"
#include "edit.h"
#include "native_batch.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
using namespace nova;
namespace {
double mib() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX m{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m));
    return double(m.PeakWorkingSetSize) / 1048576.0;
#else
    return 0;
#endif
}
QJsonObject stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + .5))]; };
    return {{"n", double(v.size())}, {"median_ms", at(.5)}, {"p95_ms", at(.95)}, {"max_ms", v.back()}};
}
QImage solid(QRgb c, int size) {
    QImage i(size, size, QImage::Format_RGBA8888);
    i.fill(QColor::fromRgba(c));
    return i;
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2)
        return 2;
    QTemporaryDir root;
    QJsonObject out;
    // ---- display preview: stage + resolve + mesh + pose (what one control change costs)
    {
        mc::CreationRequest r;
        r.type = mc::AssetType::ModelTexturePair;
        r.shape = mc::ModelTemplate::MultiElement;
        r.id = "nova:block/bench";
        auto plan = mc::planCreation(root.path(), r);
        if (!plan.valid() || !mc::executeCreation(plan).complete)
            return 3;
        mc::ModelEdit edit;
        if (!edit.open(root.path(), "assets/nova/models/block/bench.json"))
            return 4;
        mc::Library lib;
        lib.update(root.path(), Index::scan(root.path()));
        std::vector<double> times;
        QElapsedTimer t;
        for (int i = 0; i < 500; ++i) {
            t.restart();
            mc::DisplayTransform d;
            d.rotation = {double(i % 90), double(i % 360), 0};
            d.scale = {0.5, 0.5, 0.5};
            if (!edit.previewDisplay("gui", d))
                return 5;
            lib.previewSource("assets/nova/models/block/bench.json", edit.source());
            auto model = lib.model("nova:block/bench");
            auto mesh = mc::applyTransform(mc::mesh(model), mc::itemPose(mc::displayTransform(model, "gui"), {}));
            if (mesh.faces.isEmpty())
                return 6;
            times.push_back(t.nsecsElapsed() / 1e6);
        }
        out["display_preview_update"] = stats(times);
        // ---- bulk UV: 12 faces across 2 elements per operation
        QVector<mc::UvTarget> targets;
        for (int e = 0; e < 2; ++e)
            for (const auto &f : {"down", "up", "north", "south", "west", "east"})
                targets.append({e, f});
        std::vector<double> uv;
        for (int i = 0; i < 300; ++i) {
            mc::UvBulk op;
            op.kind = i % 3 == 0 ? mc::UvBulk::Offset : i % 3 == 1 ? mc::UvBulk::MirrorU : mc::UvBulk::Snap;
            op.value = {0.25, 0};
            t.restart();
            if (!edit.bulkUv(targets, op, [](const mc::UvTarget &) { return QSize(16, 16); }))
                return 7;
            uv.push_back(t.nsecsElapsed() / 1e6);
            if (i % 10 == 9)
                edit.cancelPreview();
        }
        out["bulk_uv_12_faces"] = stats(uv);
    }
    // ---- native batch throughput: 24 static (128 px, 4 layers) + 6 animated (64 px, 16 frames)
    {
        QStringList files;
        const auto base = root.path() + "/assets/nova/textures/block/";
        QDir().mkpath(base);
        for (int i = 0; i < 30; ++i) {
            const auto rel = QString("assets/nova/textures/block/n%1.png").arg(i);
            const bool animated = i >= 24;
            const int size = animated ? 64 : 128;
            savePng(root.path() + '/' + rel, solid(qRgba(10, 20, 30, 255), size));
            Document doc(root.path() + '/' + rel, solid(qRgba(10, 20, 30, 255), size));
            if (animated) {
                doc.convertToAnimation();
                for (int f = 1; f < 16; ++f)
                    doc.editFrame(FrameCommand::Duplicate);
            } else
                for (int l = 0; l < 3; ++l)
                    doc.addLayerWithPixels(QString("L%1").arg(l), solid(qRgba(l * 60, 0, 0, 128), size));
            if (!doc.saveNative(root.path()))
                return 8;
            files << rel;
        }
        batch::Pipeline p;
        p.steps = {{"hue_saturation", {{"hue", 30}}}, {"levels", {}}};
        batch::NativeOptions o;
        o.layers = batch::LayerScope::All;
        o.transactionRoot = root.path() + "-tx";
        QElapsedTimer t;
        t.start();
        auto plan = batch::planNative(p, root.path(), files, o);
        const double planMs = t.nsecsElapsed() / 1e6;
        std::atomic_bool cancel = false;
        t.restart();
        auto report = batch::executeNative(p, plan, root.path(), o, {}, &cancel);
        const double runMs = t.nsecsElapsed() / 1e6;
        out["native_batch"] = QJsonObject{{"documents", double(files.size())}, {"ready", plan.ready},
                                          {"done", report.done}, {"plan_ms", planMs}, {"execute_ms", runMs},
                                          {"documents_per_second", report.done / (runMs / 1000)},
                                          {"workers", plan.workers},
                                          {"peak_estimate_mib", double(plan.peakBytes) / 1048576.0},
                                          {"process_peak_working_set_mib", mib()}};
        QDir(root.path() + "-tx").removeRecursively(); // task-owned temporary transactions
    }
    // ---- large animation native save/load: 64 frames x 2 layers x 256 px
    {
        const auto png = root.path() + "/assets/nova/textures/block/large_anim.png";
        savePng(png, solid(qRgba(1, 2, 3, 255), 256));
        Document doc(png, solid(qRgba(1, 2, 3, 255), 256));
        doc.addLayerWithPixels("Top", solid(qRgba(200, 0, 0, 100), 256));
        doc.convertToAnimation();
        for (int f = 1; f < 64; ++f)
            doc.editFrame(FrameCommand::Duplicate);
        QElapsedTimer t;
        std::vector<double> save, load;
        for (int i = 0; i < 3; ++i) {
            t.restart();
            if (!doc.saveNative(root.path()))
                return 9;
            save.push_back(t.nsecsElapsed() / 1e6);
            Document reload(png, solid(0, 256));
            t.restart();
            if (!reload.loadNative(root.path()))
                return 10;
            load.push_back(t.nsecsElapsed() / 1e6);
        }
        out["large_animation_native"] = QJsonObject{{"frames", 64}, {"size", 256}, {"save", stats(save)},
                                                    {"load", stats(load)},
                                                    {"bytes", double(QFileInfo(nativeDocumentPath(root.path(), png)).size())}};
    }
    out["peak_working_set_mib"] = mib();
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::WriteOnly))
        return 11;
    f.write(QJsonDocument(out).toJson());
    return 0;
}
