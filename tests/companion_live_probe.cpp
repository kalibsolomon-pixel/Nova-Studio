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
// Usage: nova-companion-live-probe <scratch folder>. Set NOVA_VANILLA_BASELINE_ROOT to a scratch
// folder: the probe downloads the official 1.21.11 baseline there for Nova's side of the
// comparison. Set NOVA_TEST_SCREEN to keep Nova's windows on a chosen screen.
#include "companion.h"
#include "fabric_testing.h"
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
#include <QLabel>
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

    QObject::connect(engine, &FabricRuntime::gameExited, [] { say("Minecraft exited"); });
    engine->requestStop();
    if (!waitFor([&] { return !engine->running(); }, 90000))
        engine->forceStop();
    waitFor([&] { return !engine->running(); }, 30000);
    restoreInstance(); // after the game wrote its options on exit
    return finish(reloaded ? 0 : 1);
}
