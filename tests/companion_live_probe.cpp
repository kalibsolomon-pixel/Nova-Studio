// SPDX-License-Identifier: GPL-3.0-or-later
// Manual live probe of Nova's companion mod against the real managed Fabric runtime; never
// registered with CTest. It prepares (when needed) and starts Minecraft 1.21.11 in Nova's test
// environment, a real game window, then:
//   1. has the game draw the stone icon from a generated pack whose stone is cyan,
//   2. recolours the stone orange, syncs (which reloads by itself) and draws it again: an orange
//      icon proves the reload reached the game,
//   3. runs Compare Icons with the reference set and saves the dialog and its report.
// For a clean comparison only vanilla and the probe pack are enabled during the run; afterwards the
// instance's options.txt is restored (without the probe pack) and the probe pack folder is removed.
// With --full it then compares every vanilla item's icon at GUI scales 1-4 (each scale the game
// window allows) and writes out/parity-1.21.11.json: per scale, the counts and every item that
// differs or cannot be drawn. Images of the differing items go to out/scale-<n>/ for review only.
// --warnings then checks live sync and warning links in the real game: a stone model with a missing
// texture is saved, live sync reloads it, the warning must name models/block/stone.json; the model is
// fixed with two quick saves, which must give one reload and clear the warning.
// --rerender <folder> <scale> skips the game: it draws Nova's icon again for every
// <item>-minecraft.png a full run saved in <folder> and counts what still differs (for renderer work).
// Usage: nova-companion-live-probe <scratch folder> [--full | --rerender <folder> <scale>]. Set NOVA_VANILLA_BASELINE_ROOT to a scratch
// folder: the probe downloads the official 1.21.11 baseline there for Nova's side of the
// comparison. Set NOVA_TEST_SCREEN to keep Nova's windows on a chosen screen.
#include "companion.h"
#include "fabric_testing.h"
#include "assets.h"
#include "icon_parity.h"
#include "index.h"
#include "models.h"
#include "resources.h"
#include "settings.h"
#include "vanilla/cache.h"
#include "vanilla/installer.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QTableWidget>
#include <QThread>
#include <cstdio>
#include <functional>

using namespace nova;
using namespace nova::testing;

namespace {
void say(const QString &text) {
    std::fprintf(stdout, "%s\n", text.toUtf8().constData());
    std::fflush(stdout);
}
bool waitFor(const std::function<bool()> &done, int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > milliseconds)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    return true;
}
QImage stone(const QColor &color) {
    QImage image(16, 16, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}
// Mean colour of the pixels that differ from the slot background.
QColor meanIcon(const QImage &icon, const QColor &background) {
    qint64 r = 0, g = 0, b = 0, n = 0;
    for (int y = 0; y < icon.height(); ++y)
        for (int x = 0; x < icon.width(); ++x) {
            const auto c = icon.pixelColor(x, y);
            if (qAbs(c.red() - background.red()) + qAbs(c.green() - background.green()) +
                    qAbs(c.blue() - background.blue()) < 12)
                continue;
            r += c.red(), g += c.green(), b += c.blue(), ++n;
        }
    return n ? QColor(int(r / n), int(g / n), int(b / n)) : QColor();
}
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc < 2) {
        say("Usage: nova-companion-live-probe <scratch folder>");
        return 2;
    }
    const QString scratch = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    const bool full = argc > 2 && QByteArray(argv[2]) == "--full";
    const bool warnings = argc > 2 && QByteArray(argv[2]) == "--warnings";
    if (argc > 4 && QByteArray(argv[2]) == "--rerender") {
        auto library = std::make_shared<mc::Library>("1.21.11");
        const auto empty = scratch + "/empty-pack";
        QDir().mkpath(empty);
        library->update(empty, Index::scan(empty));
        if (auto entry = vanilla::BaselineCache().latest("1.21.11"); entry && entry->hasModelData())
            library->setFallbacks({mc::ResourceSource::fromBaseline(*entry)}, "1.21.11");
        const QDir folder(QString::fromLocal8Bit(argv[3]));
        const int scale = QByteArray(argv[4]).toInt();
        int matching = 0, total = 0;
        for (const auto &name : folder.entryList({"*-minecraft.png"}, QDir::Files)) {
            const auto item = name.chopped(14);
            const QImage game(folder.filePath(name));
            const auto asset = "assets/minecraft/items/" + item + ".json";
            auto comparison = compareIcons(renderInventoryIcon(*library, asset, scale), game, QColor("#8b8b8b"));
            const int frames = qMin(inventoryIconAnimationLength(*library, asset), 64);
            for (int k = 1; k < frames && !comparison.matches(); ++k) {
                auto attempt = compareIcons(renderInventoryIcon(*library, asset, scale, k), game, QColor("#8b8b8b"));
                if (attempt.differing < comparison.differing)
                    comparison = attempt;
            }
            ++total;
            matching += comparison.matches();
            if (!comparison.matches())
                say(QString("%1: %2 px, max %3").arg(item).arg(comparison.differing).arg(comparison.maxDelta));
        }
        say(QString("rerender: %1 of %2 now match").arg(matching).arg(total));
        return 0;
    }
    const auto out = scratch + "/out";
    const auto pack = scratch + "/Nova Probe Pack";
    QDir().mkpath(out);
    QDir().mkpath(pack + "/assets/minecraft/textures/block");
    QJsonObject results;
    auto finish = [&](int code) {
        QFile file(out + "/results.json");
        if (file.open(QIODevice::WriteOnly))
            file.write(QJsonDocument(results).toJson());
        say(QString("results: %1").arg(out + "/results.json"));
        return code;
    };
    {
        QFile meta(pack + "/pack.mcmeta");
        meta.open(QIODevice::WriteOnly);
        meta.write(R"({"pack":{"description":"Nova companion probe","min_format":[75,0],"max_format":[75,0]}})");
    }
    stone(QColor(40, 200, 220)).save(pack + "/assets/minecraft/textures/block/stone.png");

    // Nova's side needs vanilla models: the official baseline, downloaded into the scratch root.
    say("Vanilla baseline: " + vanilla::BaselineCache::defaultRoot());
    {
        vanilla::BaselineInstaller installer;
        QString error = "timeout";
        bool done = false;
        QObject::connect(&installer, &vanilla::BaselineInstaller::finished, [&](const QString &e) {
            error = e;
            done = true;
        });
        installer.start("1.21.11");
        waitFor([&] { return done; }, 20 * 60 * 1000);
        results["baseline"] = error.isEmpty() ? QString("ready") : error;
        say("Baseline: " + (error.isEmpty() ? QString("ready") : error));
    }
    auto library = std::make_shared<mc::Library>("1.21.11");
    library->update(pack, Index::scan(pack));
    if (auto entry = vanilla::BaselineCache().latest("1.21.11"); entry && entry->hasModelData())
        library->setFallbacks({mc::ResourceSource::fromBaseline(*entry)}, "1.21.11");

    Preferences preferences;
    FabricTestDialog::Context context;
    context.projectRoot = [pack] { return pack; };
    context.packName = [] { return QString("Nova Probe Pack"); };
    context.unsavedDocuments = [] { return QStringList{}; };
    context.activity = [](const QString &message) { say("activity: " + message); };
    context.preferences = &preferences;
    context.interactive = false;
    context.modelLibrary = [library] { return library; };
    FabricTestDialog dialog(context);
    auto engine = dialog.runtime();
    QObject::connect(engine, &FabricRuntime::changed, [engine] {
        static QString last;
        const auto line = stateText(engine->state()) + " · " + engine->status();
        if (line != last)
            say("runtime: " + (last = line));
    });
    say("Runtime: " + engine->layout().base);
    const auto game = engine->layout().game;
    const auto probeEntry = QString("file/Nova Test - Nova Probe Pack");
    QByteArray originalOptions;
    {
        QFile options(game + "/options.txt");
        if (options.open(QIODevice::ReadOnly))
            originalOptions = options.readAll();
    }
    auto writeOptions = [&](const QByteArray &bytes) {
        QFile options(game + "/options.txt");
        if (options.open(QIODevice::WriteOnly | QIODevice::Truncate))
            options.write(bytes);
    };
    auto withPacks = [&](QByteArray bytes, const QString &packs) {
        QStringList lines = QString::fromUtf8(bytes).split('\n');
        lines.removeIf([](const QString &line) { return line.startsWith("resourcePacks:"); });
        if (!packs.isEmpty())
            lines.insert(0, "resourcePacks:" + packs);
        return lines.join('\n').toUtf8();
    };
    auto restoreInstance = [&] {
        // The user's own pack list, minus the probe pack an earlier run may have added.
        auto restored = originalOptions;
        restored.replace(",\"" + probeEntry.toUtf8() + "\"", "").replace("\"" + probeEntry.toUtf8() + "\",", "");
        writeOptions(restored);
        const auto probeFolder = game + "/resourcepacks/Nova Test - Nova Probe Pack";
        if (QFileInfo(probeFolder).isDir() && !QFileInfo(probeFolder).isSymLink())
            QDir(probeFolder).removeRecursively();
        say("Test instance restored");
    };
    writeOptions(withPacks(originalOptions, "[\"vanilla\",\"" + probeEntry + "\"]"));
    dialog.testInMinecraft();
    if (!waitFor([&] { return engine->readyForInput(); }, 30 * 60 * 1000)) {
        results["launch"] = "Minecraft did not reach its menu: " + engine->error();
        engine->forceStop();
        waitFor([&] { return !engine->running(); }, 30000);
        restoreInstance();
        return finish(1);
    }
    results["launch"] = "ready";
    if (!waitFor([&] { return engine->companion()->available(); }, 120000)) {
        results["companion"] = "not active (see " + engine->runLogPath() + ")";
        engine->requestStop();
        waitFor([&] { return !engine->running(); }, 90000);
        restoreInstance();
        return finish(1);
    }
    results["companion"] = "active";
    say("Companion active for launch " + engine->companion()->launchId());

    auto drawStone = [&](const QString &label) -> QColor {
        bool done = false;
        QColor mean;
        auto connection = QObject::connect(engine->companion(), &CompanionLink::iconsFinished,
            [&](bool ok, const QJsonObject &result, const QString &error) {
                done = true;
                const QImage icon(result.value("files").toArray().at(0).toString());
                if (!ok || icon.isNull()) {
                    say(label + ": no icon (" + error + ")");
                    return;
                }
                icon.save(out + "/stone-" + label + ".png");
                mean = meanIcon(icon, QColor(result.value("background").toString()));
                say(QString("%1: stone icon %2x%3 at GUI scale %4, mean colour %5")
                        .arg(label).arg(icon.width()).arg(icon.height())
                        .arg(result.value("guiScale").toInt()).arg(mean.name()));
            });
        QString error;
        if (!engine->companion()->requestIcons({"minecraft:stone"}, &error))
            say(label + ": request refused: " + error);
        else
            waitFor([&] { return done; }, 90000);
        QObject::disconnect(connection);
        return mean;
    };
    const auto before = drawStone("before");
    results["stoneBefore"] = before.name();

    QString reloadMessage;
    QObject::connect(&dialog, &FabricTestDialog::reloadReported, [&](const QString &m) { reloadMessage = m; });
    stone(QColor(230, 120, 30)).save(pack + "/assets/minecraft/textures/block/stone.png");
    dialog.syncPack();
    waitFor([&] { return !reloadMessage.isEmpty(); }, 180000);
    results["reload"] = reloadMessage.isEmpty() ? QString("no reload reported") : reloadMessage;
    say("Reload: " + results["reload"].toString());
    const auto after = drawStone("after");
    results["stoneAfter"] = after.name();
    const bool reloaded = before.isValid() && after.isValid() && before.blue() > before.red() &&
                          after.red() > after.green() && after.green() > after.blue();
    results["reloadReachedGame"] = reloaded;
    say(QString("Reload reached the game: %1").arg(reloaded ? "yes" : "NO"));

    IconParityDialog::Context parity;
    parity.companion = engine->companion();
    parity.library = [library] { return library; };
    parity.projectRoot = [pack] { return pack; };
    parity.folder = [engine] { return engine->layout().game + '/' + companionFolder; };
    IconParityDialog compare(parity);
    compare.show();
    compare.compare();
    auto table = compare.findChild<QTableWidget *>("parityResults");
    waitFor([&] { return table->rowCount() > 0; }, 180000);
    waitFor([] { return false; }, 500); // let the rows paint
    compare.grab().save(out + "/compare-icons.png");
    QJsonArray rows;
    for (int row = 0; row < table->rowCount(); ++row) {
        rows.append(table->item(row, 0)->text() + " — " + table->item(row, 4)->text());
        say("compare: " + rows.last().toString());
    }
    results["compare"] = rows;
    results["compareStatus"] = compare.findChild<QLabel *>("parityStatus")->text();
    say("Compare: " + results["compareStatus"].toString());

    if (warnings) {
        const auto model = QString("assets/minecraft/models/block/stone.json");
        auto save = [&](const QByteArray &json) {
            QDir().mkpath(QFileInfo(pack + '/' + model).path());
            QFile file(pack + '/' + model);
            file.open(QIODevice::WriteOnly | QIODevice::Truncate);
            file.write(json);
        };
        int reloads = 0;
        QObject::connect(&dialog, &FabricTestDialog::reloadReported, [&](const QString &m) {
            reloads += m.startsWith("Reloaded in Minecraft"); // sync progress is reported too
            say("live sync: " + m);
        });
        dialog.findChild<QCheckBox *>("fabricLiveReload")->setChecked(true);
        auto findings = dialog.findChild<QListWidget *>("fabricFindings");
        auto stoneRows = [&] {
            QStringList rows;
            for (int i = 0; i < findings->count(); ++i)
                if (findings->item(i)->data(Qt::UserRole).toStringList().contains(model))
                    rows << findings->item(i)->text();
            return rows;
        };
        save(R"({"parent":"minecraft:block/cube_all","textures":{"all":"nova:block/missing_live_reload"}})");
        dialog.packFilesChanged({model});
        waitFor([&] { return reloads >= 1; }, 180000);
        waitFor([&] { return !stoneRows().isEmpty(); }, 20000);
        const auto linked = stoneRows();
        say(QString("warning linked to %1: %2").arg(model, linked.isEmpty() ? QString("NONE") : linked.join(" | ")));
        for (int i = 0; i < findings->count() && i < 12; ++i)
            say("  finding: " + findings->item(i)->text() + " -> " +
                findings->item(i)->data(Qt::UserRole).toStringList().join(", "));
        results["warningLinked"] = !linked.isEmpty();
        // Two quick saves while fixing it: one reload, and the warning goes.
        const int before = reloads;
        save(R"({"parent":"minecraft:block/cube_all","textures":{"all":"minecraft:block/stone"}})");
        dialog.packFilesChanged({model});
        waitFor([] { return false; }, 400);
        dialog.packFilesChanged({model});
        waitFor([&] { return reloads > before; }, 180000);
        waitFor([] { return false; }, 6000); // a second reload would have started by now
        results["liveSyncReloads"] = reloads - before;
        results["warningCleared"] = stoneRows().isEmpty();
        say(QString("two quick saves gave %1 reload(s); stone warning cleared: %2")
                .arg(reloads - before).arg(stoneRows().isEmpty() ? "yes" : "NO"));
        QFile::remove(pack + '/' + model);
    }
    if (full) {
        QStringList items;
        if (auto entry = vanilla::BaselineCache().latest("1.21.11"))
            for (auto it = entry->files.cbegin(); it != entry->files.cend(); ++it)
                if (it.key().startsWith("assets/minecraft/items/") && it.key().endsWith(".json") &&
                    !it.key().mid(23).contains('/'))
                    items << "minecraft:" + it.key().mid(23).chopped(5);
        items.sort();
        say(QString("Full sweep: %1 vanilla items").arg(items.size()));
        QJsonObject scales;
        QList<int> done;
        for (int requested = 1; requested <= 4; ++requested) {
            int used = 0, matching = 0, compared = 0;
            QJsonArray differing, undrawn, absent, animatedMatches;
            bool skip = false;
            for (int first = 0; first < items.size() && !skip; first += 16) {
                const auto batch = items.mid(first, 16);
                bool finished = false, ok = false;
                QJsonObject answer;
                QString failure;
                auto connection = QObject::connect(engine->companion(), &CompanionLink::iconsFinished,
                    [&](bool success, const QJsonObject &result, const QString &error) {
                        finished = true, ok = success, answer = result, failure = error;
                    });
                QString error;
                if (engine->companion()->requestIcons(batch, &error, requested, false))
                    waitFor([&] { return finished; }, 90000);
                else
                    failure = error;
                QObject::disconnect(connection);
                if (!ok) {
                    say(QString("scale %1 batch %2: %3").arg(requested).arg(first).arg(failure));
                    for (const auto &item : batch)
                        absent.append(item + " (" + failure + ")");
                    continue;
                }
                used = answer.value("guiScale").toInt();
                if (used != requested && done.contains(used)) {
                    say(QString("GUI scale %1 is not available in this window (got %2); skipped").arg(requested).arg(used));
                    skip = true;
                    break;
                }
                const QColor background(answer.value("background").toString());
                const auto files = answer.value("files").toArray();
                for (int i = 0; i < batch.size(); ++i) {
                    const QImage game(files.at(i).toString());
                    const auto asset = "assets/minecraft/items/" + batch[i].mid(10) + ".json";
                    const auto icon = renderInventoryIcon(*library, asset, used);
                    if (game.isNull()) {
                        absent.append(batch[i]);
                        continue;
                    }
                    if (icon.isNull()) {
                        undrawn.append(batch[i]);
                        continue;
                    }
                    auto comparison = compareIcons(icon, game, background);
                    ++compared;
                    // Animated textures: the game shows whichever frame is current, so try each one.
                    int entry = 0;
                    const int frames = comparison.matches() ? 1 : qMin(inventoryIconAnimationLength(*library, asset), 64);
                    for (int k = 1; k < frames && !comparison.matches(); ++k) {
                        auto attempt = compareIcons(renderInventoryIcon(*library, asset, used, k), game, background);
                        if (attempt.differing < comparison.differing)
                            comparison = attempt, entry = k;
                    }
                    if (comparison.matches()) {
                        ++matching;
                        if (entry)
                            animatedMatches.append(QString("%1 (entry %2)").arg(batch[i]).arg(entry));
                        continue;
                    }
                    differing.append(QJsonObject{{"item", batch[i]}, {"differingPixels", comparison.differing},
                                                 {"pixels", comparison.total}, {"maxDelta", comparison.maxDelta},
                                                 {"animationEntries", frames}});
                    const auto folder = QString("%1/scale-%2").arg(out).arg(used);
                    QDir().mkpath(folder);
                    const auto name = batch[i].mid(10);
                    comparison.nova.save(folder + '/' + name + "-nova.png");
                    comparison.minecraft.save(folder + '/' + name + "-minecraft.png");
                    comparison.difference.save(folder + '/' + name + "-difference.png");
                }
            }
            if (skip || used == 0)
                continue;
            done << used;
            say(QString("GUI scale %1: %2 of %3 compared icons match, %4 differ, %5 not drawn by Nova, %6 not drawn by the game")
                    .arg(used).arg(matching).arg(compared).arg(differing.size()).arg(undrawn.size()).arg(absent.size()));
            scales[QString::number(used)] = QJsonObject{{"compared", compared}, {"matching", matching},
                                                        {"differing", differing}, {"notDrawnByNova", undrawn},
                                                        {"matchedOnAnimationEntry", animatedMatches},
                                                        {"notDrawnByGame", absent}};
        }
        QFile report(out + "/parity-1.21.11.json");
        if (report.open(QIODevice::WriteOnly))
            report.write(QJsonDocument(QJsonObject{{"minecraft", "1.21.11"}, {"items", items.size()},
                                                   {"tolerance", 2}, {"scales", scales}})
                             .toJson());
        results["parity"] = out + "/parity-1.21.11.json";
    }

    QObject::connect(engine, &FabricRuntime::gameExited, [] { say("Minecraft exited"); });
    engine->requestStop();
    if (!waitFor([&] { return !engine->running(); }, 90000))
        engine->forceStop();
    waitFor([&] { return !engine->running(); }, 30000);
    restoreInstance(); // after the game wrote its options on exit
    return finish(reloaded ? 0 : 1);
}
