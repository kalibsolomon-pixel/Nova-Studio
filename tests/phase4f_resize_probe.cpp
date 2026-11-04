// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-F resize and scroll profiler.
//
// Drives the real main window with native SetWindowPos resizes (the messages an edge or
// corner drag produces) and wheel scrolling of the asset views, and attributes
// exclusive GUI-thread time to (event type, receiver class) by overriding notify().
//
//   nova-phase4f-resize-probe <work-dir> <output.json> [scenario ...]
//
// Scenarios: empty, many-tree, many-grid, texture, animation, model, review, comparison,
// scroll-tree, scroll-grid. Run with QT_SCALE_FACTOR for DPI variants.
#include "assets.h"
#include "canvas.h"
#include "core.h"
#include "cube.h"
#include "window.h"
#include "phase4f_probe_support.h"
#include <QAbstractItemView>
#include <QDialog>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QPixmapCache>
#include <QScrollBar>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QWheelEvent>
#include <QtTest>
#include <algorithm>
#include <map>
#include <qt_windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <atomic>
#include <thread>

namespace nova {
class EditorTestAccess {
  public:
    static bool indexing(const Window &w) { return w.indexBusy; }
    static void openTexture(Window &w, const QString &path) { w.openTexture(path); }
    static void openModel(Window &w, const QString &path) { w.openModelAsset(path); }
    static void setMode(Window &w, bool grid) { w.setMode(grid); }
    static QTreeView *tree(Window &w) { return w.tree; }
    static QListView *grid(Window &w) { return w.grid; }
    static QTabWidget *tabs(Window &w) { return w.tabs; }
    static QDockWidget *modelsDock(Window &w) { return w.modelsDock; }
    static void setDirectory(Window &w, const QString &d) { w.setDirectory(d); }
    static void showReview(Window &w) { w.showMcpAccess(); }
    static void showComparison(Window &w, const QString &path) {
        Window::Comparison c;
        c.path = path;
        c.beforeRevision = "before";
        c.afterRevision = "after";
        c.detail = "Probe comparison";
        c.owner = "probe";
        c.instance = QUuid::createUuid();
        c.before = QImage(64, 64, QImage::Format_RGBA8888);
        c.before.fill(Qt::red);
        c.after = QImage(64, 64, QImage::Format_RGBA8888);
        c.after.fill(Qt::blue);
        w.rememberComparison(c);
        w.showComparison(path, "after");
    }
    static QDialog *comparisonDialog(Window &w) { return w.comparisonDialog; }
    static Cube *cube(Window &w) { return w.cube; }
};
} // namespace nova

using namespace nova;
using namespace phase4f;
namespace {
void mark(const char *m) { fprintf(stderr, "[probe] %s\n", m); fflush(stderr); }
struct Cost {
    double ms = 0;
    int count = 0;
};
class ProfilingApp : public QApplication {
  public:
    using QApplication::QApplication;
    bool profiling = false;
    bool detail = qEnvironmentVariableIntValue("NOVA_PROBE_DETAIL") == 1;
    std::map<QString, Cost> costs;
    struct Frame {
        qint64 start, child;
    };
    std::vector<Frame> stack;
    QElapsedTimer clock;
    bool notify(QObject *receiver, QEvent *event) override {
        if (!profiling)
            return QApplication::notify(receiver, event);
        // The receiver may be destroyed by the event (DeferredDelete): read it first.
        const auto type = event->type();
        QString className = QLatin1StringView(receiver->metaObject()->className());
        if (detail && (type == QEvent::Paint || type == QEvent::Resize || type == QEvent::LayoutRequest)) {
            // Name the widget: its object name, or the nearest named ancestor.
            QObject *named = receiver;
            QString chain;
            for (int depth = 0; named && depth < 8; ++depth, named = named->parent())
                if (!named->objectName().isEmpty() && !named->objectName().startsWith("qt_")) {
                    chain += (chain.isEmpty() ? "" : "<") + named->objectName();
                    break;
                } else if (!named->objectName().isEmpty()) {
                    chain += (chain.isEmpty() ? "" : "<") + named->objectName();
                }
            if (!chain.isEmpty())
                className += "@" + chain;
        }
        stack.push_back({clock.nsecsElapsed(), 0});
        const bool result = QApplication::notify(receiver, event);
        const auto frame = stack.back();
        stack.pop_back();
        const auto total = clock.nsecsElapsed() - frame.start;
        if (!stack.empty())
            stack.back().child += total;
        const auto exclusive = total - frame.child;
        // Bucket by event and receiver class; ignore sub-microsecond noise.
        if (exclusive > 2000) {
            auto key = QString("%1 %2").arg(int(type)).arg(className);
            auto &c = costs[key];
            c.ms += exclusive / 1e6;
            ++c.count;
        }
        return result;
    }
};
ProfilingApp *app() { return static_cast<ProfilingApp *>(qApp); }
QString eventName(int type) {
    switch (type) {
    case QEvent::Paint: return "Paint";
    case QEvent::Resize: return "Resize";
    case QEvent::LayoutRequest: return "LayoutRequest";
    case QEvent::UpdateRequest: return "UpdateRequest";
    case QEvent::Move: return "Move";
    case QEvent::Timer: return "Timer";
    case QEvent::MetaCall: return "MetaCall";
    case QEvent::Expose: return "Expose";
    case QEvent::Wheel: return "Wheel";
    case QEvent::Show: return "Show";
    case QEvent::Polish: return "Polish";
    case QEvent::PolishRequest: return "PolishRequest";
    case QEvent::UpdateLater: return "UpdateLater";
    case QEvent::DevicePixelRatioChange: return "DevicePixelRatioChange";
    default: return QString::number(type);
    }
}
// In-process sampling profiler for the GUI thread. Unwinds with the x64 .pdata tables, so
// no PDBs are needed; names come from DLL export tables (Qt exports most of its API).
// No allocation happens while the target thread is suspended.
QJsonObject breakdown(int limit = 24) {
    std::vector<std::pair<QString, Cost>> v(app()->costs.begin(), app()->costs.end());
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second.ms > b.second.ms; });
    QJsonArray rows;
    for (int i = 0; i < int(v.size()) && i < limit; ++i) {
        const auto parts = v[i].first.split(' ');
        rows.append(QJsonObject{{"event", eventName(parts[0].toInt())}, {"class", parts.value(1)},
                                {"ms", v[i].second.ms}, {"count", v[i].second.count}});
    }
    return {{"top", rows}};
}
// One native resize step: what Windows delivers for each mouse move of an edge drag.
double resizeStep(HWND hwnd, QSize size) {
    QElapsedTimer t;
    t.start();
    SetWindowPos(hwnd, nullptr, 0, 0, size.width(), size.height(),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // Deliver the posted layout and update requests the resize produced, as the
    // sizing loop does before the next mouse move.
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    return t.nsecsElapsed() / 1e6;
}
QJsonObject sweep(Window &window, const QString &kind) {
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    const auto screen = window.screen()->availableGeometry().size();
    const double dpr = window.devicePixelRatioF();
    // Sweep between 90 % of the available screen and a compact size above the window's
    // minimum, so no step is clamped by Windows. Native sizes are physical pixels.
    const auto minimum = window.minimumSizeHint().expandedTo(window.minimumSize());
    const QSize largeLogical(int(screen.width() * 0.9), int(screen.height() * 0.9));
    const QSize compactLogical(std::max(minimum.width() + 40, int(largeLogical.width() * 0.62)),
                               std::max(minimum.height() + 40, int(largeLogical.height() * 0.66)));
    const QSize large(int(largeLogical.width() * dpr), int(largeLogical.height() * dpr));
    const QSize compact(int(compactLogical.width() * dpr), int(compactLogical.height() * dpr));
    std::vector<QSize> path;
    const int steps = qEnvironmentVariableIsSet("NOVA_PROBE_STEPS") ? qEnvironmentVariableIntValue("NOVA_PROBE_STEPS") : 120;
    for (int cycle = 0; cycle < 2; ++cycle)
        for (int i = 0; i <= 2 * steps; ++i) {
            const double f = i <= steps ? double(i) / steps : double(2 * steps - i) / steps;
            int w = large.width(), h = large.height();
            if (kind == "horizontal" || kind == "corner")
                w = int(large.width() + (compact.width() - large.width()) * f);
            if (kind == "vertical" || kind == "corner")
                h = int(large.height() + (compact.height() - large.height()) * f);
            path.push_back({w, h});
        }
    resizeStep(hwnd, large);
    settle(200);
    std::vector<double> times;
    app()->costs.clear();
    const auto memBefore = privateBytes();
    const auto cpuBefore = cpu();
    QElapsedTimer wall;
    wall.start();
    const bool sample = qEnvironmentVariableIntValue("NOVA_PROBE_SAMPLE") == 1;
    Sampler sampler;
    if (sample)
        sampler.start();
    else
        app()->profiling = true;
    int clamped = 0;
    QSize previous = window.size();
    std::map<QString, Cost> tail;
    int tailSteps = 0;
    for (const auto &s : path) {
        const auto before = app()->costs;
        const double t = resizeStep(hwnd, s);
        if (t > 28.0 && app()->profiling) {
            // Attribute only the slow steps: the difference this step made to each bucket.
            ++tailSteps;
            for (const auto &[key, cost] : app()->costs) {
                const auto it = before.find(key);
                const double delta = cost.ms - (it == before.end() ? 0.0 : it->second.ms);
                if (delta > 0.05) {
                    tail[key].ms += delta;
                    tail[key].count += cost.count - (it == before.end() ? 0 : it->second.count);
                }
            }
        }
        // Count only steps that changed the window; a clamped step costs nothing.
        if (window.size() != previous)
            times.push_back(t);
        else
            ++clamped;
        previous = window.size();
    }
    app()->profiling = false;
    QJsonObject sampled;
    if (sample)
        sampled = sampler.stop();
    const double wallMs = wall.nsecsElapsed() / 1e6;
    const auto cpuAfter = cpu();
    settle(200);
    auto out = Stats().json(times);
    out["kind"] = kind;
    out["clamped_steps"] = clamped;
    out["sweep_logical"] = QString("%1x%2 to %3x%4").arg(largeLogical.width()).arg(largeLogical.height()).arg(compactLogical.width()).arg(compactLogical.height());
    out["wall_ms"] = wallMs;
    out["cpu_ms"] = cpuMs(cpuBefore, cpuAfter);
    out["private_bytes_delta"] = double(privateBytes() - memBefore);
    out["breakdown"] = breakdown();
    {
        const auto all = std::exchange(app()->costs, tail);
        auto slow = breakdown(16);
        slow["steps_over_28ms"] = tailSteps;
        out["slow_steps"] = slow;
        app()->costs = all;
    }
    if (sample)
        out["samples"] = sampled;
    return out;
}
QJsonObject maximizeCycle(Window &window) {
    std::vector<double> times;
    app()->costs.clear();
    app()->profiling = true;
    for (int i = 0; i < 6; ++i) {
        QElapsedTimer t;
        t.start();
        window.showMaximized();
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        times.push_back(t.nsecsElapsed() / 1e6);
        t.restart();
        window.showNormal();
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        times.push_back(t.nsecsElapsed() / 1e6);
        settle(60);
    }
    app()->profiling = false;
    auto out = Stats().json(times);
    out["kind"] = "maximize-restore";
    out["breakdown"] = breakdown(8);
    return out;
}
// Wheel scrolling an asset view, one notch per frame, measuring each notch until painted.
QJsonObject scroll(QAbstractItemView *view, const QString &kind) {
    view->setFocus();
    view->verticalScrollBar()->setValue(0);
    settle(300);
    std::vector<double> times;
    app()->costs.clear();
    const auto cpuBefore = cpu();
    const bool sample = qEnvironmentVariableIntValue("NOVA_PROBE_SAMPLE") == 1;
    Sampler sampler;
    if (sample)
        sampler.start();
    else
        app()->profiling = true;
    const auto vp = view->viewport();
    const QPointF pos(vp->width() / 2.0, vp->height() / 2.0);
    const int notches = qEnvironmentVariableIsSet("NOVA_PROBE_NOTCHES") ? qEnvironmentVariableIntValue("NOVA_PROBE_NOTCHES") : 160;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < notches; ++i) {
            const int delta = pass == 0 ? -120 : 120;
            QWheelEvent wheel(pos, vp->mapToGlobal(pos), {}, {0, delta}, Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QElapsedTimer t;
            t.start();
            QCoreApplication::sendEvent(vp, &wheel);
            vp->repaint(); // present the scrolled frame, as the next vsync would
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            times.push_back(t.nsecsElapsed() / 1e6);
        }
    app()->profiling = false;
    QJsonObject sampled;
    if (sample)
        sampled = sampler.stop();
    const auto cpuAfter = cpu();
    auto out = Stats().json(times);
    if (sample)
        out["samples"] = sampled;
    out["kind"] = kind;
    out["cpu_ms"] = cpuMs(cpuBefore, cpuAfter);
    out["max_scroll"] = view->verticalScrollBar()->maximum();
    out["breakdown"] = breakdown();
    return out;
}

// Correctness after resizing: on-screen pixels (what DWM composited), the 3D preview's
// on-screen region against its framebuffer (stale output check), hit testing of the
// preview and canvas, and unchanged selection and focus.
QJsonObject verifyAfterResize(Window &window, const QString &captureDir, const QString &label) {
    QJsonArray checks;
    bool ok = true;
    auto check = [&](const QString &name, bool passed, const QString &detail = {}) {
        checks.append(QJsonObject{{"name", name}, {"passed", passed}, {"detail", detail}});
        ok &= passed;
    };
    auto tree = EditorTestAccess::tree(window);
    const auto selected = tree->currentIndex().data(Qt::UserRole).toString();
    const auto focus = QApplication::focusWidget();
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    const auto avail = window.screen()->availableGeometry();
    const double dpr = window.devicePixelRatioF();
    const QList<QSize> sizes = {{int(avail.width() * 0.85), int(avail.height() * 0.85)},
                                {int(avail.width() * 0.6), int(avail.height() * 0.7)},
                                {int(avail.width() * 0.75), int(avail.height() * 0.6)}};
    int index = 0;
    for (const auto &logical : sizes) {
        // Drag-like approach to the size, then let it settle as when the mouse stops.
        const QSize from = window.size();
        for (int i = 1; i <= 24; ++i) {
            const double f = i / 24.0;
            const QSize step(int(from.width() + (logical.width() - from.width()) * f),
                             int(from.height() + (logical.height() - from.height()) * f));
            SetWindowPos(hwnd, nullptr, 0, 0, int(step.width() * dpr), int(step.height() * dpr),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
        window.raise();
        window.activateWindow();
        settle(500);
        const auto tag = QString("%1-%2").arg(label).arg(index++);
        auto cube = EditorTestAccess::cube(window);
        const auto screen = window.screen();
        const auto frame = window.frameGeometry();
        const QPixmap shot = screen->grabWindow(0, frame.x() - screen->geometry().x(), frame.y() - screen->geometry().y(),
                                                frame.width(), frame.height());
        shot.save(captureDir + "/" + tag + ".png");
        if (cube && cube->isVisible() && cube->width() > 40 && cube->height() > 40) {
            const QRect onWindow(cube->mapTo(&window, QPoint(0, 0)) - (frame.topLeft() - window.geometry().topLeft()) * -1,
                                 cube->size());
            const QPoint topLeft = cube->mapToGlobal(QPoint(0, 0)) - frame.topLeft();
            const QImage onScreen = shot.toImage().copy(QRect(topLeft * shot.devicePixelRatio(), cube->size() * shot.devicePixelRatio()))
                                        .convertToFormat(QImage::Format_RGB32);
            const QImage framebuffer = cube->grabFramebuffer().convertToFormat(QImage::Format_RGB32)
                                           .scaled(onScreen.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
            Q_UNUSED(onWindow);
            double diff = 0;
            int distinct = 0;
            QSet<QRgb> colours;
            for (int y = 0; y < onScreen.height(); y += 3)
                for (int x = 0; x < onScreen.width(); x += 3) {
                    const QRgb a = onScreen.pixel(x, y), b = framebuffer.pixel(x, y);
                    diff += std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
                    ++distinct;
                    if (colours.size() < 64)
                        colours.insert(a & 0xF0F0F0);
                }
            const double mean = distinct ? diff / distinct / 3.0 : 255;
            check(tag + " preview rendered", colours.size() > 3, QString("%1 colour groups").arg(colours.size()));
            check(tag + " preview current", mean < 12.0, QString("mean channel difference %1").arg(mean, 0, 'f', 2));
            const QPoint centre = cube->mapToGlobal(cube->rect().center());
            check(tag + " preview hit test", QApplication::widgetAt(centre) == cube,
                  QApplication::widgetAt(centre) ? QApplication::widgetAt(centre)->metaObject()->className() : "none");
        }
        if (auto canvas = window.activeCanvas(); canvas && canvas->isVisible()) {
            const QPoint centre = canvas->mapToGlobal(canvas->rect().center());
            check(tag + " canvas hit test", QApplication::widgetAt(centre) == canvas);
            check(tag + " canvas inside window", window.rect().contains(QRect(canvas->mapTo(&window, QPoint()), canvas->size())));
        }
        // Every visible docked panel lies inside the window and no two overlap (layout
        // corruption). Tabified panels share one area; only the raised one is visible.
        QList<QPair<QString, QRect>> docks;
        for (auto dock : window.findChildren<QDockWidget *>())
            if (dock->isVisible() && !dock->isFloating() && !dock->visibleRegion().isEmpty())
                docks.append({dock->objectName(), QRect(dock->mapTo(&window, QPoint()), dock->size())});
        QStringList outside, overlapping;
        for (int i = 0; i < docks.size(); ++i) {
            if (!window.rect().contains(docks[i].second))
                outside << QString("%1 %2,%3 %4x%5").arg(docks[i].first).arg(docks[i].second.x()).arg(docks[i].second.y())
                               .arg(docks[i].second.width()).arg(docks[i].second.height());
            for (int j = i + 1; j < docks.size(); ++j) {
                const auto common = docks[i].second.intersected(docks[j].second);
                if (common.width() > 1 && common.height() > 1)
                    overlapping << docks[i].first + "/" + docks[j].first;
            }
        }
        check(tag + " docks inside window", outside.isEmpty(), outside.join("; "));
        check(tag + " docks do not overlap", overlapping.isEmpty(), overlapping.join("; "));
    }
    check(label + " selection unchanged", tree->currentIndex().data(Qt::UserRole).toString() == selected, selected);
    check(label + " focus unchanged", QApplication::focusWidget() == focus);
    return {{"label", label}, {"passed", ok}, {"checks", checks}};
}
} // namespace

int main(int argc, char **argv) {
    // Same application setup as src/app/main.cpp, so the measured window is the product's.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    // Mirrors src/app/main.cpp (swap interval 0). NOVA_PROBE_SWAP_INTERVAL=1 reproduces the
    // previous presentation for A/B attribution.
    format.setSwapInterval(qEnvironmentVariableIsSet("NOVA_PROBE_SWAP_INTERVAL")
                               ? qEnvironmentVariableIntValue("NOVA_PROBE_SWAP_INTERVAL")
                               : 0);
    QSurfaceFormat::setDefaultFormat(format);
    ProfilingApp application(argc, argv);
    application.setStyle("Fusion");
    application.setApplicationName("Nova Studio Resize Probe");
    QFont font = application.font();
    font.setPointSize(9);
    application.setFont(font);
    application.clock.start();
    if (qEnvironmentVariableIsSet("NOVA_PROBE_PIXMAP_CACHE_KB")) // attribution experiment only
        QPixmapCache::setCacheLimit(qEnvironmentVariableIntValue("NOVA_PROBE_PIXMAP_CACHE_KB"));
    fprintf(stderr, "[probe] pixmap cache limit %d KB\n", QPixmapCache::cacheLimit());
    const auto args = application.arguments();
    if (args.size() < 3) {
        qCritical("usage: nova-phase4f-resize-probe <work-dir> <output.json> [scenario ...]");
        return 2;
    }
    const QString work = args[1], output = args[2];
    QStringList scenarios = args.mid(3);
    if (scenarios.isEmpty())
        scenarios = {"empty", "many-tree", "many-grid", "texture", "animation", "model", "review", "comparison",
                     "scroll-tree", "scroll-grid", "dense-grid", "dense-tree"};
    mark("start");
    QDir().mkpath(work);
    qApp->setProperty("settingsFile", work + "/studio.ini");
    const int textures = qEnvironmentVariableIsSet("NOVA_PROBE_TEXTURES") ? qEnvironmentVariableIntValue("NOVA_PROBE_TEXTURES") : 2400;
    const QString emptyRoot = work + "/empty-pack", fullRoot = work + "/many-pack", denseRoot = work + "/dense-pack";
    if (!QFileInfo::exists(emptyRoot + "/pack.mcmeta"))
        makePack(emptyRoot, 0);
    if (!QFileInfo::exists(fullRoot + "/pack.mcmeta"))
        makePack(fullRoot, textures);
    // One folder with as many textures as vanilla's textures/block, scrolled end to end.
    if (!QFileInfo::exists(denseRoot + "/pack.mcmeta")) {
        makePack(denseRoot, 0);
        for (int i = 0; i < 1500; ++i)
            png(denseRoot + QString("/assets/minecraft/textures/block/b%1.png").arg(i, 4, 10, QChar('0')), 16, i);
    }
    mark("fixtures ready");
    QJsonObject report{{"qt", qVersion()}, {"scale", qEnvironmentVariable("QT_SCALE_FACTOR", "1")},
                       {"textures", textures}};
    QJsonArray results;
    for (const auto &scenario : scenarios) {
        const bool empty = scenario == "empty";
        QElapsedTimer started;
        started.start();
        mark("constructing window");
        const bool dense = scenario.startsWith("dense-");
        Window window(empty ? emptyRoot : dense ? denseRoot : fullRoot, {}, started);
        mark("window constructed");
        window.resize(1400, 900);
        window.show();
        QTest::qWaitForWindowExposed(&window);
        mark("exposed");
        QElapsedTimer wait;
        wait.start();
        while ((EditorTestAccess::indexing(window) || wait.elapsed() < 800) && wait.elapsed() < 30000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const QString texture = "assets/nova/textures/block/cube.png";
        if (scenario == "many-grid" || scenario == "scroll-grid") {
            EditorTestAccess::setMode(window, true);
            EditorTestAccess::setDirectory(window, "assets/nova/textures/block/set0");
        }
        if (scenario == "dense-grid") {
            EditorTestAccess::setMode(window, true);
            EditorTestAccess::setDirectory(window, "assets/minecraft/textures/block");
        }
        if (scenario == "many-tree" || scenario == "scroll-tree" || scenario == "dense-tree") {
            EditorTestAccess::setMode(window, false);
            EditorTestAccess::tree(window)->expandAll();
        }
        if (scenario == "texture" || scenario == "comparison")
            EditorTestAccess::openTexture(window, fullRoot + '/' + texture);
        if (scenario == "animation")
            EditorTestAccess::openTexture(window, fullRoot + "/assets/nova/textures/block/anim.png");
        if (scenario == "model") {
            EditorTestAccess::openModel(window, "assets/nova/models/block/cube.json");
            if (auto dock = EditorTestAccess::modelsDock(window))
                dock->show();
        }
        if (scenario == "review")
            EditorTestAccess::showReview(window);
        if (scenario == "comparison")
            EditorTestAccess::showComparison(window, texture);
        if (qEnvironmentVariableIntValue("NOVA_PROBE_NO_STYLESHEET") == 1)
            qApp->setStyleSheet({}); // attribution experiment only
        mark("scenario prepared");
        settle(600);
        mark("settled");
        QJsonObject entry{{"scenario", scenario}, {"documents", EditorTestAccess::tabs(window)->count()},
                          {"effective_scale", window.devicePixelRatioF()}};
        if (scenario.startsWith("verify-")) {
            const auto captures = QFileInfo(output).absolutePath() + "/captures";
            QDir().mkpath(captures);
            const auto label = scenario + "-" + QString::number(window.devicePixelRatioF(), 'f', 2);
            if (scenario == "verify-model") {
                // Texture and model open, model tab active: the preview lives in the tab.
                EditorTestAccess::openTexture(window, fullRoot + '/' + texture);
                EditorTestAccess::openModel(window, "assets/nova/models/block/cube.json");
                if (auto dock = EditorTestAccess::modelsDock(window))
                    dock->show();
            } else {
                // Texture tab active: the preview lives in its dock.
                EditorTestAccess::openTexture(window, fullRoot + '/' + texture);
            }
            EditorTestAccess::setMode(window, false);
            EditorTestAccess::tree(window)->setFocus();
            settle(600);
            entry["verify"] = verifyAfterResize(window, captures, label);
        } else if (scenario.startsWith("scroll-") || dense) {
            // Scroll in a short window, where many rows are off screen.
            window.resize(1200, 760);
            settle(400);
            QAbstractItemView *view = scenario == "scroll-grid" || scenario == "dense-grid"
                                          ? static_cast<QAbstractItemView *>(EditorTestAccess::grid(window))
                                          : EditorTestAccess::tree(window);
            QJsonArray runs;
            runs.append(scroll(view, "cold"));
            runs.append(scroll(view, "warm"));
            entry["scroll"] = runs;
        } else {
            QJsonArray runs;
            for (const auto &kind : {"horizontal", "vertical", "corner"})
                runs.append(sweep(window, kind));
            runs.append(maximizeCycle(window));
            entry["resize"] = runs;
        }
        results.append(entry);
        if (auto dialog = EditorTestAccess::comparisonDialog(window))
            dialog->close();
        for (auto top : QApplication::topLevelWidgets())
            if (top != &window && top->isVisible())
                top->close();
        window.close();
        settle(200);
        qInfo().noquote() << scenario << "done";
    }
    report["results"] = results;
    QFile out(output);
    if (!out.open(QIODevice::WriteOnly))
        return 1;
    out.write(QJsonDocument(report).toJson());
    return 0;
}
