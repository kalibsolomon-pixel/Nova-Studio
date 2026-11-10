// SPDX-License-Identifier: GPL-3.0-or-later
// C8 idle laboratory probe: the production Window with a fixed scenario, counting Qt
// events, timers, repaints, dispatcher wakeups and native messages over a settled
// window. Writes JSON; never writes project sources (probe documents are discarded).
#include "modelpanel.h"
#include "timeline.h"
#include "window.h"
#include <QAbstractEventDispatcher>
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTimer>
#include <QToolButton>
#include <QDir>
#include <QJsonArray>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
// Process memory plus a walk of the default process heap (used by the CRT allocator):
// busy = live allocations, free = committed but unused (allocator-retained) bytes.
QJsonObject memorySample(const QString &label) {
    QJsonObject o{{"label", label}};
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX m{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m));
    o["working_set_mib"] = double(m.WorkingSetSize) / 1048576.0;
    o["private_mib"] = double(m.PrivateUsage) / 1048576.0;
    o["peak_working_set_mib"] = double(m.PeakWorkingSetSize) / 1048576.0;
    HANDLE heap = GetProcessHeap();
    qint64 busy = 0, freeBytes = 0, blocks = 0;
    if (HeapLock(heap)) {
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heap, &entry)) {
            if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                busy += entry.cbData;
                ++blocks;
            } else if (!(entry.wFlags & (PROCESS_HEAP_REGION | PROCESS_HEAP_UNCOMMITTED_RANGE)))
                freeBytes += entry.cbData;
        }
        HeapUnlock(heap);
    }
    o["heap_busy_mib"] = double(busy) / 1048576.0;
    o["heap_free_committed_mib"] = double(freeBytes) / 1048576.0;
    o["heap_busy_blocks"] = double(blocks);
#endif
    return o;
}
namespace nova {
class EditorTestAccess {
  public:
    static void configure(Window &w, const QString &scenario) {
        if (scenario == "empty")
            { discard(w); closeAll(w); return; }
        const bool animation = scenario.startsWith("animation");
        const auto file = animation ? "/assets/nova/textures/block/animated.png" : "/assets/nova/textures/block/check.png";
        if (QFile::exists(w.project + file))
            w.openTexture(w.project + file);
        if (scenario.startsWith("model") || scenario == "display-editor")
            w.modelPanel->openAsset(scenario == "model-paused" ? "assets/nova/models/block/animated.json"
                                                               : "assets/nova/models/block/complex.json");
        if (scenario == "model-paused")
            if (auto toggle = w.modelPanel->findChild<QCheckBox *>("animateMaterials")) toggle->setChecked(false);
        if (scenario == "model-hidden")
            w.previewDock->hide();
        if (scenario == "display-editor")
            if (auto toggle = w.modelPanel->findChild<QToolButton *>("displayToggle"))
                toggle->click();
        if (scenario == "animation-playing" && w.timeline)
            w.timeline->playback.play();
        if (scenario == "problems")
            w.showProblems(true);
        if (scenario == "layered" && w.activeCanvas()) {
            auto doc = w.activeCanvas()->document;
            for (int i = 0; i < 15; ++i) {
                QImage layer(doc->image.size(), QImage::Format_RGBA8888);
                layer.fill(QColor(i * 16, 80, 120, 128));
                w.session.addLayer(doc, QString("Layer %1").arg(i), layer, {}, nullptr);
            }
        }
    }
    static QJsonObject counters(Window &w) {
        auto counters = w.metrics.counters;
        counters["gpu_resources"] = w.cube->resourceCounts();
        return counters;
    }
    static int documents(Window &w) { return int(w.session.all().size()); }
    static QJsonObject assets(Window &w) {
        QJsonObject out;
        auto measure = [](int n, const auto &f) {
            QJsonArray times;
            for (int i = 0; i < n; ++i) {
                QElapsedTimer timer; timer.start(); f(i);
                times.append(timer.nsecsElapsed() / 1e6);
            }
            return times;
        };
        out["files"] = w.collection.files.size();
        out["install_cpu_ms"] = measure(10, [&](int) { w.model->setIndex(w.collection, w.project); });
        out["apply_file_cpu_ms"] = measure(20, [&](int) { w.model->applyIndex(w.collection, {w.collection.files.first()}); });
        out["category_cpu_ms"] = measure(20, [&](int i) { w.model->setCategory(i % 2 ? QString() : QString("Blocks")); });
        out["filter_cpu_ms"] = measure(20, [&](int i) { w.proxy->setFilterFixedString(i % 2 ? "" : "17"); w.proxy->rowCount(); });
        const auto dirs = w.collection.directories.keys();
        out["folder_cpu_ms"] = measure(50, [&](int i) { w.setDirectory(dirs[i % dirs.size()]); });
        w.setMode(true);
        out["thumbnail_folder_repaint_ms"] = measure(20, [&](int i) {
            w.setDirectory(dirs[i % dirs.size()]); QApplication::processEvents();
        });
        QStringList pngs;
        for (const auto &file : w.collection.files) if (file.endsWith(".png")) pngs.append(file);
        out["open_close_texture_cpu_ms"] = measure(20, [&](int i) {
            w.openTexture(w.project + '/' + pngs[i % pngs.size()]); closeAll(w);
        });
        out["thumbnail_cache_bytes"] = w.model->cachedBytes();
        out["thumbnail_pending"] = w.model->pendingCount();
        return out;
    }
    static void closeAll(Window &w) {
        while (w.tabs->count() > 0) {
            const int before = w.tabs->count();
            emit w.tabs->tabCloseRequested(0);
            if (w.tabs->count() == before)
                break; // a document refused to close (would need confirmation)
        }
    }
    static void openModel(Window &w, const QString &asset) { w.modelPanel->openAsset(asset); }
    static void openTexture(Window &w, const QString &png) { w.openTexture(png); }
    static void openLarge(Window &w, const QString &png) {
        w.openTexture(png);
        for (const auto &doc : w.session.all())
            if (QFileInfo(doc->path) == QFileInfo(png))
                for (int i = 0; i < 15; ++i) {
                    QImage layer(doc->image.size(), QImage::Format_RGBA8888);
                    layer.fill(QColor(i * 16, 255 - i * 16, 128, 128));
                    w.session.addLayer(doc, QString("Layer %1").arg(i), layer, {}, nullptr);
                }
    }
    static void discard(Window &w) {
        for (auto doc : w.session.all())
            while (doc->modified() && doc->historyCursor() > 0)
                doc->undo();
    }
};
} // namespace nova
class Activity : public QObject, public QAbstractNativeEventFilter {
  public:
    QMap<QString, qint64> counts;
    QElapsedTimer paintClock;
    double lastPaintMs = 0;
    int paints = 0;
    bool classes = false;
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override {
#ifdef Q_OS_WIN
        const auto msg = static_cast<MSG *>(message);
        ++counts["native_0x" + QString::number(msg->message, 16)];
        // Attribute the messages C6 could not: Qt-private WM_USER range and accessibility.
        if (classes && msg->hwnd && (msg->message == 0x003D || (msg->message >= 0x0400 && msg->message <= 0x0410))) {
            wchar_t name[128] = {};
            GetClassNameW(msg->hwnd, name, 128);
            ++counts["class_0x" + QString::number(msg->message, 16) + "@" + QString::fromWCharArray(name)];
        }
#endif
        return false;
    }
    bool eventFilter(QObject *o, QEvent *e) override {
        const auto type = e->type();
        if (type == QEvent::Paint && paintClock.isValid()) {
            lastPaintMs = paintClock.nsecsElapsed() / 1e6;
            ++paints;
        }
        ++counts["qt_event_" + QString::number(type)];
        if (type == QEvent::Timer || type == QEvent::Paint || type == QEvent::UpdateRequest ||
            type == QEvent::MetaCall || type == QEvent::DeferredDelete)
            ++counts[QString::number(type) + ':' + o->metaObject()->className() + ':' + o->objectName()];
        return false;
    }
    QJsonObject json() const {
        QJsonObject out;
        for (auto i = counts.begin(); i != counts.end(); ++i)
            out[i.key()] = double(i.value());
        return out;
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    auto args = app.arguments();
    auto arg = [&](QString key, QString fallback = QString()) {
        int i = args.indexOf(key);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : fallback;
    };
    app.setProperty("settingsFile", arg("--settings"));
    QElapsedTimer started;
    started.start();
    nova::Window window(arg("--project"), arg("--telemetry"), started);
    Activity activity;
    activity.classes = args.contains("--native-classes");
    app.installNativeEventFilter(&activity);
    app.installEventFilter(&activity);
    auto dispatcher = QAbstractEventDispatcher::instance();
    QObject::connect(dispatcher, &QAbstractEventDispatcher::awake, &activity,
                     [&] { ++activity.counts["dispatcher_awake"]; });
    QObject::connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, &activity,
                     [&] { ++activity.counts["dispatcher_block"]; });
    window.show();
    // Probe-owned scratch documents only: answer close prompts with Discard so a probe
    // never blocks on exit (imported Minecraft animations open as unsaved documents).
    QJsonArray dialogs;
    QTimer watchdog;
    QObject::connect(&watchdog, &QTimer::timeout, [&] {
        if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            dialogs.append(box->windowTitle() + ": " + box->text());
            if (auto discard = box->button(QMessageBox::Discard))
                discard->click();
            else
                box->reject();
        }
    });
    watchdog.start(250);
    const int settle = arg("--settle", "15").toInt() * 1000, seconds = arg("--seconds", "30").toInt() * 1000;
    if (arg("--scenario") == "assets") {
        QTimer::singleShot(5000, &window, [&] {
            watchdog.stop();
            QFile file(arg("--activity"));
            const auto data = nova::EditorTestAccess::assets(window);
            if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(data).toJson());
            window.close();
        });
        return app.exec();
    }
    if (arg("--scenario") == "workspaces") {
        QTimer::singleShot(2500, &window, [&] {
            watchdog.stop();
            QJsonArray rows;
            const auto before = nova::EditorTestAccess::counters(window);
            for (int cycle = 0; cycle < 12; ++cycle)
                for (const auto name : {"texture", "animation", "model", "uv", "essentials"}) {
                    activity.lastPaintMs = 0;
                    activity.paints = 0;
                    activity.paintClock.start();
                    window.findChild<QAction *>(QString("workspacePreset-") + name)->trigger();
                    const double blocking = activity.paintClock.nsecsElapsed() / 1e6;
                    // Count the final observed Qt paint after 30 ms of quiet. This is
                    // a software repaint proxy, not DWM presentation or input-to-photon.
                    int oldPaints = -1, quiet = 0;
                    while (quiet < 6 && activity.paintClock.elapsed() < 2000) {
                        QTest::qWait(5);
                        if (activity.paints == oldPaints) ++quiet;
                        else quiet = 0;
                        oldPaints = activity.paints;
                    }
                    rows.append(QJsonObject{{"cycle", cycle}, {"preset", name}, {"blocking_ms", blocking},
                                            {"last_paint_ms", activity.lastPaintMs}, {"paints", activity.paints}});
                }
            activity.paintClock.invalidate();
            QFile file(arg("--activity"));
            if (file.open(QIODevice::WriteOnly))
                file.write(QJsonDocument(QJsonObject{{"rows", rows}, {"before", before},
                    {"after", nova::EditorTestAccess::counters(window)}}).toJson());
            window.close();
        });
        return app.exec();
    }
    if (arg("--scenario") == "cycles") {
        // Lifecycle laboratory: open, close, reopen; memory after each phase.
        QTemporaryDir scratch;
        const auto large = scratch.path() + "/large.png";
        QImage pixels(1024, 1024, QImage::Format_RGBA8888);
        pixels.fill(QColor(30, 60, 90));
        pixels.save(large);
        QJsonArray samples;
        samples.append(memorySample("window-shown"));
        const int cycles = arg("--cycles", "12").toInt();
        const auto project = arg("--project");
        QTimer::singleShot(2000, &window, [&] {
            samples.append(memorySample("empty-workspace"));
            for (int c = 0; c < cycles; ++c) {
                auto pump = [](int ms) {
                    QElapsedTimer t;
                    t.start();
                    while (t.elapsed() < ms)
                        QApplication::processEvents(QEventLoop::AllEvents, 20);
                };
                nova::EditorTestAccess::openTexture(window, project + "/assets/nova/textures/block/check.png");
                pump(150);
                if (c == 0)
                    samples.append(memorySample("static-texture"));
                for (const auto &m : {"assets/nova/models/block/check.json", "assets/nova/models/block/complex.json",
                                      "assets/nova/models/block/animated.json", "assets/nova/models/block/complex.json"})
                    nova::EditorTestAccess::openModel(window, m), pump(80);
                if (c == 0)
                    samples.append(memorySample("models-previewed"));
                nova::EditorTestAccess::openTexture(window, project + "/assets/nova/textures/block/animated.png");
                pump(200);
                if (c == 0)
                    samples.append(memorySample("animated-document"));
                nova::EditorTestAccess::openLarge(window, large);
                pump(300);
                auto open = memorySample(QString("cycle-%1-open").arg(c));
                open["documents"] = nova::EditorTestAccess::documents(window);
                open["gpu"] = nova::EditorTestAccess::counters(window)["gpu_resources"];
                samples.append(open);
                nova::EditorTestAccess::discard(window);
                nova::EditorTestAccess::closeAll(window);
                pump(300);
                auto closed = memorySample(QString("cycle-%1-closed").arg(c));
                closed["documents"] = nova::EditorTestAccess::documents(window);
                closed["gpu"] = nova::EditorTestAccess::counters(window)["gpu_resources"];
                samples.append(closed);
            }
            QFile file(arg("--activity"));
            if (file.open(QIODevice::WriteOnly))
                file.write(QJsonDocument(QJsonObject{{"scenario", "cycles"},
                                                     {"samples", samples},
                                                     {"dismissed_dialogs", dialogs},
                                                     {"render_counters", nova::EditorTestAccess::counters(window)}})
                               .toJson());
            window.close();
        });
        return app.exec();
    }
    QTimer::singleShot(2000, &window, [&] { nova::EditorTestAccess::configure(window, arg("--scenario")); });
    QElapsedTimer measured;
    QTimer::singleShot(settle, &window, [&] {
        watchdog.stop(); // no probe timer contaminating settled idle counters
        activity.counts.clear();
        measured.start();
        QFile marker(arg("--activity") + ".start");
        if (marker.open(QIODevice::WriteOnly))
            marker.write("start");
    });
    QTimer::singleShot(settle + seconds, &window, [&] {
        const double ms = double(measured.elapsed());
        QFile file(arg("--activity"));
        if (file.open(QIODevice::WriteOnly))
            file.write(QJsonDocument(QJsonObject{{"scenario", arg("--scenario")},
                                                 {"measured_ms", ms},
                                                 {"events", activity.json()},
                                                 {"memory", memorySample("end")},
                                                 {"dismissed_dialogs", dialogs},
                                                 {"render_counters", nova::EditorTestAccess::counters(window)}})
                           .toJson());
        nova::EditorTestAccess::discard(window);
        watchdog.start();
        window.close();
    });
    return app.exec();
}
