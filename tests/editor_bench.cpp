// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 2-C3 editor benchmarks: CPU function timings (not GPU or physical input
// latency). Usage: nova-editor-bench <output.json> [--quick]
#include "batch.h"
#include "session.h"
#include "index.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <algorithm>
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

using namespace nova;
namespace {
QJsonObject stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    double mean = 0, variance = 0;
    QJsonArray raw;
    for (auto x : v) { mean += x; raw.append(x); }
    if (!v.empty()) mean /= v.size();
    for (auto x : v) variance += (x - mean) * (x - mean);
    if (!v.empty()) variance /= v.size();
    auto pct = [&](double p) { return v.empty() ? 0.0 : v[size_t(std::min<double>(v.size() - 1, std::ceil(p * v.size()) - 1))]; };
    return {{"n", int(v.size())}, {"p50_ms", pct(.5)}, {"p95_ms", pct(.95)}, {"p99_ms", pct(.99)},
            {"max_ms", v.empty() ? 0 : v.back()}, {"mean_ms", mean}, {"variance_ms2", variance}, {"raw_ms", raw}};
}
template <class F> QJsonObject measure(int n, F f) {
    std::vector<double> times;
    for (int i = 0; i < n; ++i) {
        QElapsedTimer t;
        t.start();
        f(i);
        times.push_back(t.nsecsElapsed() / 1e6);
    }
    return stats(times);
}
QImage noise(int w, int h, int seed = 1) {
    QImage im(w, h, QImage::Format_RGBA8888);
    ops::Random r{quint64(seed)};
    for (int y = 0; y < h; ++y) {
        auto row = reinterpret_cast<quint32 *>(im.scanLine(y));
        for (int x = 0; x < w; ++x)
            row[x] = quint32(r.next()) | 0xff000000u;
    }
    return im;
}
qint64 privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX c{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&c), sizeof(c));
    return qint64(c.PrivateUsage);
}
qint64 peakWorkingSet() {
    PROCESS_MEMORY_COUNTERS c{};
    GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c));
    return qint64(c.PeakWorkingSetSize);
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const QString output = args.value(1, "editor-bench.json");
    const bool quick = args.contains("--quick");
    const int n = quick ? 5 : 30;
    QJsonObject out;
    out["note"] = "CPU function timings on this workstation; not GPU, display or physical input latency";
    if (args.contains("--index-only")) {
        const auto root = qEnvironmentVariable("NOVA_LARGE_FIXTURE");
        auto index = Index::scan(root);
        if (index.files.size() < 50000 || !index.error.isEmpty()) return 2;
        out["files"] = index.files.size();
        out["scan"] = measure(10, [&](int) { if (!Index::scan(root).error.isEmpty()) std::abort(); });
        out["refresh_file"] = measure(20, [&](int) {
            auto next = Index::refresh(root, index, {index.files.first()});
            if (next.files != index.files || !next.error.isEmpty()) std::abort();
        });
        out["refresh_assets_subtree"] = measure(8, [&](int) {
            auto next = Index::refresh(root, index, {"assets"});
            if (next.files != index.files || !next.error.isEmpty()) std::abort();
        });
        Navigation nav;
        nav.directory = QFileInfo(index.files.first()).path();
        out["folder_navigation"] = measure(100, [&](int) { nav.entries(index); });
        nav.query = "17";
        out["search"] = measure(100, [&](int) { nav.entries(index); });
        QFile file(output);
        if (!file.open(QIODevice::WriteOnly)) return 1;
        file.write(QJsonDocument(out).toJson());
        return 0;
    }
    out["build"] =
#ifdef NDEBUG
        "Release";
#else
        "Debug";
#endif
    // ---------------------------------------------------------------- strokes
    QJsonObject strokes;
    for (int size : {1, 8, 32})
        for (int canvas : {64, 512, 2048}) {
            auto doc = std::make_shared<Document>(QString(), noise(canvas, canvas));
            StrokeStyle s;
            s.color = qRgb(255, 0, 0);
            s.brush.size = size;
            s.brush.shape = size > 1 ? ops::BrushShape::Circle : ops::BrushShape::Square;
            const int segment = std::max(4, canvas / 16);
            strokes[QString("size%1_canvas%2_segment%3px").arg(size).arg(canvas).arg(segment)] =
                measure(quick ? 20 : 200, [&](int i) {
                    const int y = (i * 7) % canvas, x = (i * 13) % std::max(1, canvas - segment);
                    doc->stroke(doc->activeLayer(), {x, y}, {x + segment - 1, y}, s);
                    if (i % 20 == 19)
                        doc->finish();
                });
            doc->finish();
        }
    out["stroke_segment"] = strokes;
    // ---------------------------------------------------------------- selections
    {
        QJsonObject sel;
        auto image = noise(1024, 1024, 3);
        sel["magic_wand_1024_tol48"] = measure(n, [&](int i) {
            ops::colorMask(image, {i % 1024, (i * 31) % 1024}, 48, ops::Distance::Rgb, true, true);
        });
        sel["colour_range_1024_tol48"] = measure(n, [&](int i) {
            ops::colorMask(image, {i % 1024, 0}, 48, ops::Distance::Perceptual, false, true);
        });
        QVector<QPointF> polygon;
        for (int k = 0; k < 200; ++k) {
            const double t = 2 * 3.14159265 * k / 200, r = 300 + 150 * std::sin(t * 7);
            polygon.append({512 + r * std::cos(t), 512 + r * std::sin(t)});
        }
        sel["lasso_200pt_1024"] = measure(n, [&](int) { ops::polygonMask({1024, 1024}, polygon); });
        auto doc = std::make_shared<Document>(QString(), image);
        sel["select_commit_combine_1024"] = measure(n, [&](int i) {
            doc->select(ops::rectMask({1024, 1024}, {i * 7 % 900, 40, 120, 600}), ops::Combine::Add);
        });
        sel["marching_ants_outline_1024"] = measure(n, [&](int) { ops::maskOutline(doc->selection()); });
        out["selection"] = sel;
    }
    // ---------------------------------------------------------------- configurable drawing
    {
        QJsonObject drawing;
        for (int size : {64, 512, 2048}) {
            ops::ShapeSpec shape;
            shape.kind = ops::ShapeKind::Ellipse;
            shape.bounds = QRectF(size / 8., size / 8., size * .75, size * .75);
            shape.filled = true;
            for (bool smooth : {false, true}) {
                shape.smooth = smooth;
                drawing[QString("ellipse_%1_%2").arg(smooth ? "smooth" : "hard").arg(size)] =
                    measure(n, [&](int) { ops::rasterizeShape({size, size}, shape); });
            }
        }
        ops::Brush brush;
        brush.shape = ops::BrushShape::Diamond;
        brush.size = 32;
        brush.height = 64;
        drawing["diamond_32x64_footprint"] = measure(n, [&](int) { ops::footprint(brush); });
        out["drawing"] = drawing;
    }
    // ---------------------------------------------------------------- transforms
    {
        QJsonObject tr;
        auto content = noise(128, 128, 5);
        tr["floating_scale200_rotate30_128"] = measure(n, [&](int i) {
            auto img = ops::run({"resize_image", {{"width", 256}, {"height", 256}}}, content).image;
            img = ops::run({"resize_canvas", {{"width", 364}, {"height", 364}}}, img).image;
            ops::run({"rotate", {{"angle", 30.0 + i}}}, img);
        });
        auto doc = std::make_shared<Document>(QString(), noise(512, 512));
        for (int k = 0; k < 3; ++k)
            doc->addLayerWithPixels("L", noise(512, 512, k + 9));
        tr["preview_composite_512_4layers"] = measure(n, [&](int) { doc->compositeWith(doc->activeLayer(), content.scaled(512, 512)); });
        auto big = std::make_shared<Document>(QString(), noise(1024, 1024));
        tr["image_rotate_commit_1024"] = measure(quick ? 3 : 10, [&](int) { big->applyDocumentOperation({"image_rotate", {}}); });
        tr["resize_nearest_1024_to_2048"] = measure(quick ? 3 : 10, [&](int) {
            ops::run({"resize_image", {{"width", 2048}, {"height", 2048}}}, big->image);
        });
        out["transform"] = tr;
    }
    // ---------------------------------------------------------------- colour / filters / shading / generators
    {
        QJsonObject op;
        for (int size : {64, 512, 2048}) {
            const auto image = noise(size, size, size);
            const int reps = size >= 2048 ? (quick ? 2 : 6) : n;
            for (const char *id : {"hue_saturation", "levels", "curves", "gaussian_blur", "median", "palette_map",
                                   "directional_shading", "normal_map", "quantize"}) {
                if (QString(id) == "median" && size >= 2048)
                    continue;
                ops::Context ctx;
                ctx.palette = {qRgb(0, 0, 0), qRgb(80, 80, 80), qRgb(160, 160, 160), qRgb(255, 255, 255)};
                QJsonObject params;
                if (QString(id) == "hue_saturation")
                    params = {{"hue", 20}, {"saturation", 10}};
                if (QString(id) == "directional_shading")
                    params = {{"output", "Modify pixels"}};
                op[QString("%1_%2").arg(id).arg(size)] = measure(reps, [&](int) { ops::run({id, params}, image, ctx); });
            }
        }
        for (const char *id : {"gen_stone", "gen_bricks", "gen_ore", "gen_noise"})
            op[QString("%1_512").arg(id)] = measure(quick ? 3 : 10, [&](int i) {
                ops::run({id, {{"width", 512}, {"height", 512}, {"seed", i}}}, {});
            });
        out["operations"] = op;
    }
    // ---------------------------------------------------------------- compositing
    {
        QJsonObject comp;
        for (auto [layers, size] : {std::pair{16, 256}, {64, 256}, {16, 1024}, {64, 512}}) {
            auto doc = std::make_shared<Document>(QString(), noise(size, size));
            for (int k = 1; k < layers; ++k) {
                QImage px = noise(size, size, k);
                px.fill(QColor(k * 3, 100, 200, 128));
                doc->addLayerWithPixels("L", px);
            }
            const auto top = doc->activeLayer();
            comp[QString("%1layers_%2px_full_recomposite").arg(layers).arg(size)] =
                measure(quick ? 3 : 10, [&](int i) { doc->editLayer({LayerCommand::Visibility, top, {}, i % 2}); });
            StrokeStyle s;
            s.brush.size = 8;
            comp[QString("%1layers_%2px_stroke_segment").arg(layers).arg(size)] = measure(quick ? 10 : 60, [&](int i) {
                doc->editLayer({LayerCommand::Visibility, top, {}, 1});
                doc->stroke(top, {i % size, 5}, {(i + 10) % size, 5}, s);
            });
            doc->finish();
        }
        out["compositing"] = comp;
    }
    // ---------------------------------------------------------------- history retention
    {
        auto doc = std::make_shared<Document>(QString(), noise(512, 512));
        doc->setHistoryLimits(16LL * 1024 * 1024, 2048);
        const auto before = privateBytes();
        for (int i = 0; i < (quick ? 20 : 100); ++i)
            doc->applyOperation(doc->activeLayer(), {"noise", {{"seed", i}}});
        out["history"] = QJsonObject{{"operations_kept", doc->historyOperations()},
                                     {"history_bytes", double(doc->historyBytes())},
                                     {"budget_bytes", 16.0 * 1024 * 1024},
                                     {"private_bytes_delta", double(privateBytes() - before)}};
    }
    // ---------------------------------------------------------------- batch throughput
    {
        QTemporaryDir tmp;
        const auto root = tmp.path() + "/pack";
        QStringList files;
        for (int i = 0; i < (quick ? 40 : 200); ++i) {
            const auto rel = QString("assets/minecraft/textures/block/b%1.png").arg(i);
            QDir().mkpath(QFileInfo(root + '/' + rel).absolutePath());
            savePng(root + '/' + rel, noise(64, 64, i));
            files.append(rel);
        }
        batch::Pipeline p;
        p.steps = {{"hue_saturation", {{"hue", 30}}}, {"directional_shading", {{"output", "Modify pixels"}}},
                   {"quantize", {{"colors", 16}}}};
        QJsonObject b;
        for (int workers : {1, 4}) {
            batch::Output o;
            o.directory = tmp.path() + QString("/out%1").arg(workers);
            auto plan = batch::plan(p, root, files, o, workers);
            const auto peakBefore = peakWorkingSet();
            QElapsedTimer t;
            t.start();
            const auto report = batch::execute(p, plan, o, {}, nullptr);
            const double ms = t.nsecsElapsed() / 1e6;
            b[QString("workers%1").arg(workers)] =
                QJsonObject{{"files", int(files.size())}, {"done", report.done}, {"ms", ms},
                            {"files_per_second", report.done / (ms / 1000.0)}, {"plan_workers", plan.workers},
                            {"peak_working_set_growth_mib", double(peakWorkingSet() - peakBefore) / (1 << 20)}};
        }
        out["batch_64px_3steps"] = b;
    }
    if (!quick) {
        QTemporaryDir tmp;
        const auto root = tmp.filePath("pack");
        QDir().mkpath(root + "/assets/nova/textures/block");
        QStringList files;
        for (int i = 0; i < 24; ++i) {
            const auto rel = QString("assets/nova/textures/block/large%1.png").arg(i);
            if (!savePng(root + '/' + rel, noise(1024, 1024, i + 100))) return 3;
            files.append(rel);
        }
        batch::Pipeline pipeline;
        pipeline.steps = {{"hue_saturation", {{"hue", 30}}}, {"levels", {}}, {"invert", {}}};
        QJsonObject rows;
        for (int workers : {1, 4}) {
            batch::Output output; output.directory = tmp.filePath(QString("out%1").arg(workers));
            auto plan = batch::plan(pipeline, root, files, output, workers);
            QElapsedTimer timer; timer.start();
            const auto report = batch::execute(pipeline, plan, output, {}, nullptr);
            const double ms = timer.nsecsElapsed() / 1e6;
            if (report.done != files.size() || report.failed) return 4;
            rows[QString::number(workers)] = QJsonObject{{"ms", ms}, {"files", report.done},
                {"files_per_second", report.done * 1000.0 / ms}, {"workers", plan.workers},
                {"estimated_live_bytes", double(plan.peakItemBytes * plan.workers)}};
        }
        batch::Output output; output.directory = tmp.filePath("canceled");
        auto plan = batch::plan(pipeline, root, files, output, 4);
        std::atomic_bool cancel = false;
        std::atomic<qint64> requested = 0;
        QElapsedTimer timer; timer.start();
        auto report = batch::execute(pipeline, plan, output, {}, &cancel, [&](int) {
            qint64 expected = 0;
            if (requested.compare_exchange_strong(expected, timer.nsecsElapsed())) cancel.store(true);
        });
        rows["cancel_after_first_completion"] = QJsonObject{{"return_after_request_ms",
            (timer.nsecsElapsed() - requested.load()) / 1e6}, {"done", report.done},
            {"canceled", report.canceled}, {"failed", report.failed}};
        if (report.failed || report.done + report.canceled != files.size()) return 5;
        for (const auto &item : plan.items)
            if (item.status == batch::Item::Status::Done && loadPng(item.output).size() != QSize(1024, 1024)) return 6;
        out["batch_1024px_24images_3steps"] = rows;
    }
    out["process_private_mib_end"] = double(privateBytes()) / (1 << 20);
    QFile f(output);
    if (!f.open(QIODevice::WriteOnly))
        return 1;
    f.write(QJsonDocument(out).toJson());
    return 0;
}
