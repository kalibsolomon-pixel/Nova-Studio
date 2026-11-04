// SPDX-License-Identifier: GPL-3.0-or-later
#include "batch.h"
#include "core.h"
#include "presets.h"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace nova;
namespace {
QImage pattern(int w, int h, int seed) {
    QImage im(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            im.setPixel(x, y, qRgba((x * 31 + seed * 7) % 256, (y * 17 + seed) % 256, (x * y + seed * 3) % 256, 255));
    return im;
}
QByteArray fileHash(const QString &path) { return Revision::read(path).hash; }
batch::Pipeline stoneBlue() {
    batch::Pipeline p;
    p.name = "Stone to blue";
    p.steps = {{"hue_saturation", {{"hue", 30}, {"saturation", -20}}},
               {"directional_shading", {{"output", "Modify pixels"}, {"steps", 2}}},
               {"quantize", {{"colors", 8}}}};
    return p;
}
} // namespace

class BatchTests : public QObject {
    Q_OBJECT
    QTemporaryDir tmp;
    QString root;
    QStringList files;
  private slots:
    void initTestCase() {
        root = tmp.path() + "/pack";
        files = {"assets/minecraft/textures/block/stone.png", "assets/minecraft/textures/block/cobblestone.png",
                 "assets/minecraft/textures/item/stick.png", "assets/demo/textures/block/stone.png",
                 "assets/minecraft/sounds/x.ogg", "pack.png"};
        int seed = 1;
        for (const auto &f : files) {
            QDir().mkpath(QFileInfo(root + '/' + f).absolutePath());
            if (f.endsWith(".png"))
                QVERIFY(savePng(root + '/' + f, pattern(16, 16, seed++)));
            else {
                QFile o(root + '/' + f);
                QVERIFY(o.open(QIODevice::WriteOnly));
                o.write("ogg");
            }
        }
    }
    void pipelineSchema() {
        auto p = stoneBlue();
        QString error;
        batch::Pipeline back;
        QVERIFY(batch::Pipeline::fromJson(p.toJson(), &back, &error));
        QCOMPARE(back.steps.size(), size_t(3));
        QCOMPARE(back.steps[0].params["hue"].toInt(), 30);
        auto json = p.toJson();
        json["schema"] = 99;
        QVERIFY(!batch::Pipeline::fromJson(json, &back, &error));
        QVERIFY(error.contains("schema"));
        json = p.toJson();
        json["format"] = "other";
        QVERIFY(!batch::Pipeline::fromJson(json, &back, &error));
        batch::Pipeline bad;
        bad.steps = {{"gaussian_blur", {{"sigma", 500}}}};
        QVERIFY(!bad.validate(&error));
        QVERIFY(error.startsWith("Step 1"));
        batch::Pipeline disabled = stoneBlue();
        for (auto &s : disabled.steps)
            s.enabled = false;
        QVERIFY(!disabled.validate(&error));
        // Presets store pipelines in a versioned file.
        PresetStore store(tmp.path() + "/presets.json");
        store.put({"pipeline", "Stone to blue", {}, p.toJson()});
        QVERIFY(store.save(&error));
        PresetStore again(tmp.path() + "/presets.json");
        QVERIFY(again.load(&error));
        QVERIFY(again.find("pipeline", "Stone to blue"));
        QVERIFY(again.remove("pipeline", "Stone to blue"));
    }
    void targetScopes() {
        using batch::Scope;
        QCOMPARE(batch::resolveTargets(files, Scope::Pack, {}).size(), 5);
        QCOMPARE(batch::resolveTargets(files, Scope::Folder, "assets/minecraft/textures/block").size(), 2);
        QCOMPARE(batch::resolveTargets(files, Scope::FolderTree, "assets/minecraft").size(), 3);
        QCOMPARE(batch::resolveTargets(files, Scope::Category, "Blocks").size(), 3);
        QCOMPARE(batch::resolveTargets(files, Scope::Namespace, "demo").size(), 1);
        QCOMPARE(batch::resolveTargets(files, Scope::Search, "STONE").size(), 3);
        QCOMPARE(batch::resolveTargets(files, Scope::Selected, files[2]), QStringList{files[2]});
        QCOMPARE(batch::resolveTargets(files, Scope::Selected, "assets/minecraft/sounds/x.ogg").size(), 0);
    }
    void outputPathSafety() {
        batch::Output out;
        out.directory = tmp.path() + "/out";
        QString path, error;
        QVERIFY(batch::safeOutputPath(out, "a/b/c.png", 3, &path, &error));
        QVERIFY(path.endsWith("/out/a/b/c.png"));
        out.pattern = "{dir}/{name}_blue";
        QVERIFY(batch::safeOutputPath(out, "a/b/c.png", 3, &path, &error));
        QVERIFY(path.endsWith("/out/a/b/c_blue.png"));
        out.pattern = "{index}-{name}";
        QVERIFY(batch::safeOutputPath(out, "a/b/c.png", 3, &path, &error));
        QVERIFY(path.endsWith("/out/0003-c.png"));
        for (const char *bad : {"../{name}", "{name}?", "C:/x/{name}", "{dir}/../../{name}", ".nova/{name}", "{name}."}) {
            out.pattern = bad;
            QVERIFY2(!batch::safeOutputPath(out, "a/b/c.png", 0, &path, &error), bad);
        }
    }
    void dryRunAccuracy() {
        auto targets = batch::resolveTargets(files, batch::Scope::Pack, {});
        batch::Output out;
        out.directory = tmp.path() + "/dry";
        auto plan = batch::plan(stoneBlue(), root, targets, out);
        QCOMPARE(int(plan.items.size()), 5);
        QCOMPARE(plan.ready, 5);
        QVERIFY(plan.peakItemBytes > 0);
        QVERIFY(plan.summary().contains("5 ready"));
        QVERIFY(!QDir(out.directory).exists()); // dry run writes nothing
        // Name-only pattern makes two "stone.png" targets collide.
        out.pattern = "{name}";
        plan = batch::plan(stoneBlue(), root, targets, out);
        QCOMPARE(plan.conflicts, 1);
        // Existing destination files conflict unless overwrite is enabled.
        out.pattern = "{path}";
        QDir().mkpath(out.directory + "/pack");
        QVERIFY(savePng(out.directory + "/pack.png", pattern(2, 2, 0)));
        plan = batch::plan(stoneBlue(), root, targets, out);
        QCOMPARE(plan.conflicts, 1);
        out.overwrite = true;
        plan = batch::plan(stoneBlue(), root, targets, out);
        QCOMPARE(plan.conflicts, 0);
        // Output directories inside the pack are refused.
        out.directory = root + "/generated";
        plan = batch::plan(stoneBlue(), root, targets, out);
        QCOMPARE(plan.invalid, 5);
        // Missing sources are invalid, not fatal.
        out.directory = tmp.path() + "/dry2";
        plan = batch::plan(stoneBlue(), root, {"assets/missing.png"}, out);
        QCOMPARE(plan.invalid, 1);
    }
    void executeIsDeterministicAndLeavesSourcesUntouched() {
        auto targets = batch::resolveTargets(files, batch::Scope::Category, "Blocks");
        QMap<QString, QByteArray> before;
        for (const auto &t : targets)
            before[t] = fileHash(root + '/' + t);
        batch::Output out;
        out.directory = tmp.path() + "/run";
        out.pattern = "{dir}/{name}_blue";
        auto pipeline = stoneBlue();
        auto plan = batch::plan(pipeline, root, targets, out, 3);
        std::atomic_bool cancel = false;
        std::atomic_int progressed = 0;
        auto report = batch::execute(pipeline, plan, out, {}, &cancel, [&](int) { ++progressed; });
        for (const auto &item : plan.items)
            if (item.status != batch::Item::Status::Done)
                qWarning() << item.relative << batch::statusName(item.status) << item.message;
        QCOMPARE(report.done, 3);
        QCOMPARE(progressed.load(), 3);
        for (const auto &item : plan.items) {
            QCOMPARE(fileHash(item.source), before[item.relative]);
            const auto expected = ops::runPipeline(pipeline.steps, loadPng(item.source), {}).image;
            QCOMPARE(loadPng(item.output), expected);
        }
        QVERIFY(QFileInfo::exists(report.manifest));
        QFile m(report.manifest);
        QVERIFY(m.open(QIODevice::ReadOnly));
        const auto manifest = QJsonDocument::fromJson(m.readAll()).object();
        QCOMPARE(manifest["done"].toInt(), 3);
        QCOMPARE(manifest["items"].toArray().size(), 3);
        QCOMPARE(manifest["items"].toArray()[0].toObject()["status"].toString(), QString("Done"));
        // A second identical run conflicts instead of silently overwriting.
        auto again = batch::plan(pipeline, root, targets, out);
        QCOMPARE(again.conflicts, 3);
    }
    void partialFailureAndLateConflicts() {
        // A truncated PNG keeps a readable header but fails to decode.
        const auto broken = root + "/assets/minecraft/textures/block/broken.png";
        QVERIFY(savePng(broken, pattern(16, 16, 9)));
        QFile f(broken);
        QVERIFY(f.open(QIODevice::ReadWrite));
        const auto bytes = f.readAll();
        f.resize(0);
        f.write(bytes.left(60));
        f.close();
        QStringList targets = {"assets/minecraft/textures/block/stone.png", "assets/minecraft/textures/block/broken.png",
                               "assets/minecraft/textures/item/stick.png"};
        batch::Output out;
        out.directory = tmp.path() + "/partial";
        auto plan = batch::plan(stoneBlue(), root, targets, out);
        // Destination appears after planning: guarded write fails without clobbering it.
        QDir().mkpath(QFileInfo(plan.items[2].output).absolutePath());
        QVERIFY(savePng(plan.items[2].output, pattern(3, 3, 3)));
        const auto intruder = fileHash(plan.items[2].output);
        std::atomic_bool cancel = false;
        auto report = batch::execute(stoneBlue(), plan, out, {}, &cancel);
        QCOMPARE(report.done, 1);
        QCOMPARE(report.failed + report.skipped, 2);
        QCOMPARE(fileHash(plan.items[2].output), intruder);
        QVERIFY(plan.items[2].status == batch::Item::Status::Failed);
        QFile::remove(broken);
    }
    void cancellation() {
        auto targets = batch::resolveTargets(files, batch::Scope::Pack, {});
        batch::Output out;
        out.directory = tmp.path() + "/canceled";
        auto plan = batch::plan(stoneBlue(), root, targets, out, 1);
        std::atomic_bool cancel = false;
        int seen = 0;
        auto report = batch::execute(stoneBlue(), plan, out, {}, &cancel, [&](int) {
            if (++seen == 2)
                cancel = true; // cancel mid-job
        });
        QCOMPARE(report.done, 2);
        QCOMPARE(report.canceled, 3);
        int written = 0;
        for (const auto &item : plan.items)
            written += QFileInfo::exists(item.output);
        QCOMPARE(written, 2);
        QVERIFY(QFileInfo::exists(report.manifest)); // canceled jobs still leave an audit trail
    }
    void inPlaceKeepsRecoverableOriginals() {
        QTemporaryDir local;
        const auto pack = local.path() + "/pack";
        const QString rel = "assets/minecraft/textures/block/a.png";
        QDir().mkpath(QFileInfo(pack + '/' + rel).absolutePath());
        QVERIFY(savePng(pack + '/' + rel, pattern(8, 8, 4)));
        const auto original = fileHash(pack + '/' + rel);
        batch::Output out;
        out.inPlace = true;
        out.backupDirectory = local.path() + "/backup";
        batch::Pipeline invert;
        invert.steps = {{"invert", {}}};
        auto plan = batch::plan(invert, pack, {rel}, out);
        QCOMPARE(plan.ready, 1);
        auto report = batch::execute(invert, plan, out, {}, nullptr);
        qWarning() << batch::statusName(plan.items[0].status) << plan.items[0].message;
        QCOMPARE(report.done, 1);
        QCOMPARE(fileHash(local.path() + "/backup/" + rel), original);
        QVERIFY(fileHash(pack + '/' + rel) != original);
        QVERIFY(report.manifest.startsWith(out.backupDirectory));
        // Changing the source between dry run and execution blocks the update.
        auto second = batch::plan(invert, pack, {rel}, out);
        QCOMPARE(second.conflicts, 1); // backup already exists
        out.backupDirectory = local.path() + "/backup2";
        second = batch::plan(invert, pack, {rel}, out);
        QVERIFY(savePng(pack + '/' + rel, pattern(8, 8, 5)));
        const auto changed = fileHash(pack + '/' + rel);
        report = batch::execute(invert, second, out, {}, nullptr);
        QCOMPARE(report.failed, 1);
        QCOMPARE(fileHash(pack + '/' + rel), changed);
        // Backups inside the pack are refused.
        out.backupDirectory = pack + "/backups";
        QCOMPARE(batch::plan(invert, pack, {rel}, out).invalid, 1);
    }
    void resourceBudgetsBoundWorkers() {
        batch::Pipeline heavy;
        heavy.steps = {{"resize_image", {{"width", 4096}, {"height", 4096}}}, {"gaussian_blur", {{"sigma", 2.0}}}};
        batch::Output out;
        out.directory = tmp.path() + "/heavy";
        auto plan = batch::plan(heavy, root, {files[0]}, out, 8);
        QVERIFY(plan.peakItemBytes >= 4096LL * 4096 * 4 * 6);
        QVERIFY(qint64(plan.workers) * plan.peakItemBytes <= 1024LL * 1024 * 1024 || plan.workers == 1);
        QVERIFY(plan.workers <= 2);
    }
};
QTEST_GUILESS_MAIN(BatchTests)
#include "batch_test.moc"
