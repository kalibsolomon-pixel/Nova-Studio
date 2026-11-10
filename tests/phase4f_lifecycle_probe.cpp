// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-F long-session resource lifecycle probe.
//
// Repeats a full editing session in one process and samples process resources after
// every cycle: project switches, texture/animation/model documents with edits and
// undo/redo, the 3D preview moving between dock and model tabs, comparisons, recovery
// snapshots, MCP authorization/connection/revocation and preview captures.
//
//   nova-phase4f-lifecycle-probe <work-dir> <output.json> [cycles] [--assert]
//
// With --assert the process exits non-zero if resources grow after warm-up beyond
// bounds that separate cache retention from leaks (used by the CTest entry).
#include "assets.h"
#include "canvas.h"
#include "cube.h"
#include "phase4f_probe_support.h"
#include "recovery.h"
#include "service.h"
#include "window.h"
#include <QDialog>
#include <QDockWidget>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLocalSocket>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTreeView>
#include <QtTest>
#include <tlhelp32.h>
#include <winternl.h>

namespace nova {
class EditorTestAccess {
  public:
    static bool indexing(const Window &w) { return w.indexBusy; }
    static void openTexture(Window &w, const QString &path) { w.openTexture(path); }
    static void openModel(Window &w, const QString &path) { w.openModelAsset(path); }
    static QTabWidget *tabs(Window &w) { return w.tabs; }
    static QDockWidget *modelsDock(Window &w) { return w.modelsDock; }
    static Cube *cube(Window &w) { return w.cube; }
    static Metrics &metrics(Window &w) { return w.metrics; }
    static AssetModel *model(Window &w) { return w.model; }
    static RecoveryStore &recovery(Window &w) { return *w.recovery; }
    static QString project(Window &w) { return w.project; }
    static void switchProject(Window &w, const QString &root) {
        w.pathText->setText(root);
        emit w.pathText->returnPressed();
    }
    static void closeTab(Window &w, int index) { emit w.tabs->tabCloseRequested(index); }
    static void showComparison(Window &w, const QString &path) {
        Window::Comparison c;
        c.path = path;
        c.beforeRevision = "before";
        c.afterRevision = "after";
        c.detail = "Lifecycle comparison";
        c.owner = "probe";
        c.instance = QUuid::createUuid();
        c.before = QImage(64, 64, QImage::Format_RGBA8888);
        c.before.fill(Qt::red);
        c.after = QImage(64, 64, QImage::Format_RGBA8888);
        c.after.fill(Qt::blue);
        w.rememberComparison(c);
        w.showComparison(path, "after");
    }
    static void clearComparisons(Window &w) { w.clearComparisons(); }
    static QDialog *comparisonDialog(Window &w) { return w.comparisonDialog; }
    static void showReview(Window &w) { w.showMcpAccess(); }
    static mcp::Service *mcp(Window &w) { return w.mcpService; }
};
} // namespace nova

using namespace nova;
using namespace phase4f;
namespace {
int threadCount() {
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{sizeof(entry)};
    for (bool ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry))
        n += entry.th32OwnerProcessID == GetCurrentProcessId();
    CloseHandle(snap);
    return n;
}

// Handle census by object type (Event, File, Thread, ...) for leak attribution.
QJsonObject handleTypes() {
    using NtQuerySystemInformationFn = LONG(WINAPI *)(ULONG, PVOID, ULONG, PULONG);
    using NtQueryObjectFn = LONG(WINAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static auto ntdll = GetModuleHandleW(L"ntdll.dll");
    static auto querySystem = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    static auto queryObject = reinterpret_cast<NtQueryObjectFn>(GetProcAddress(ntdll, "NtQueryObject"));
    struct Entry {
        PVOID object;
        ULONG_PTR pid, handle;
        ULONG access;
        USHORT creator, typeIndex;
        ULONG attributes, reserved;
    };
    struct Info {
        ULONG_PTR count, reserved;
        Entry entries[1];
    };
    std::vector<char> buffer(1 << 22);
    ULONG needed = 0;
    while (querySystem(64 /* SystemExtendedHandleInformation */, buffer.data(), ULONG(buffer.size()), &needed) == LONG(0xC0000004))
        buffer.resize(buffer.size() * 2);
    auto info = reinterpret_cast<Info *>(buffer.data());
    QMap<QString, int> counts;
    const auto pid = ULONG_PTR(GetCurrentProcessId());
    for (ULONG_PTR i = 0; i < info->count; ++i) {
        const auto &e = info->entries[i];
        if (e.pid != pid)
            continue;
        alignas(8) char typeBuffer[1024];
        ULONG length = 0;
        QString type = QString("type#%1").arg(e.typeIndex);
        if (queryObject(HANDLE(e.handle), 2 /* ObjectTypeInformation */, typeBuffer, sizeof typeBuffer, &length) == 0) {
            auto name = reinterpret_cast<UNICODE_STRING *>(typeBuffer);
            type = QString::fromWCharArray(name->Buffer, name->Length / 2);
        }
        ++counts[type];
    }
    QJsonObject out;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        out[it.key()] = it.value();
    return out;
}
qint64 directoryBytes(const QString &path) {
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext())
        total += QFileInfo(it.next()).size();
    return total;
}
qint64 counter(Window &w, const char *name) {
    return qint64(EditorTestAccess::metrics(w).counters.value(name).toDouble());
}
QJsonObject sample(Window &w, int cycle, double cycleMs) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    const auto liveTextures = counter(w, "texture_allocations") - counter(w, "texture_releases");
    const auto liveMaterials = counter(w, "material_texture_allocations") - counter(w, "material_texture_releases");
    return {{"cycle", cycle},
            {"cycle_ms", cycleMs},
            {"private_mb", pmc.PrivateUsage / 1048576.0},
            {"working_set_mb", pmc.WorkingSetSize / 1048576.0},
            {"handles", int(handles)},
            {"handle_types", qEnvironmentVariableIsSet("NOVA_LC_HANDLE_TYPES") ? handleTypes() : QJsonObject()},
            {"threads", threadCount()},
            {"gdi_objects", int(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS))},
            {"user_objects", int(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS))},
            {"gl_live_textures", liveTextures},
            {"gl_live_materials", liveMaterials},
            {"material_uploads", counter(w, "material_texture_uploads")},
            {"thumbnail_cache_bytes", EditorTestAccess::model(w)->cachedBytes()},
            {"recovery_bytes", double(directoryBytes(EditorTestAccess::recovery(w).root()))},
            {"open_tabs", EditorTestAccess::tabs(w)->count()},
            {"top_level_widgets", int(QApplication::topLevelWidgets().size())}};
}
void waitIndexed(Window &w) {
    QElapsedTimer t;
    t.start();
    settle(150);
    while (EditorTestAccess::indexing(w) && t.elapsed() < 30000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    settle(150);
}
// A project folder that Nova has closed must not be held open: renaming it fails if any
// handle (file, directory watch or thumbnail read) remains inside.
bool releasedFolder(const QString &root) {
    const auto moved = root + "-moved";
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExW(QDir::toNativeSeparators(root).toStdWString().c_str(),
                        QDir::toNativeSeparators(moved).toStdWString().c_str(), 0)) {
            MoveFileExW(QDir::toNativeSeparators(moved).toStdWString().c_str(),
                        QDir::toNativeSeparators(root).toStdWString().c_str(), 0);
            return true;
        }
        settle(50);
    }
    return false;
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setSwapInterval(0); // as src/app/main.cpp
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setApplicationName("Nova Studio Lifecycle Probe");
    QFont font = app.font();
    font.setPointSize(9);
    app.setFont(font);
    const auto args = app.arguments();
    if (args.size() < 3)
        return 2;
    const QString work = args[1], output = args[2];
    const int cycles = args.size() > 3 && args[3] != "--assert" ? args[3].toInt() : 40;
    const bool assertBounds = args.contains("--assert");
    QDir().mkpath(work);
    qApp->setProperty("settingsFile", work + "/settings/studio.ini");
    const QString packA = work + "/pack-a", packB = work + "/pack-b";
    if (!QFileInfo::exists(packA + "/pack.mcmeta"))
        makePack(packA, 400);
    if (!QFileInfo::exists(packB + "/pack.mcmeta"))
        makePack(packB, 150);
    QElapsedTimer started;
    started.start();
    Window window(packA, {}, started);
    window.resize(1400, 900);
    window.show();
    if (!QTest::qWaitForWindowExposed(&window))
        qWarning("lifecycle probe: window was not exposed in time");
    waitIndexed(window);
    QJsonArray samples;
    QJsonArray problems;
    // Leak bisection: NOVA_LC_SKIP=project,texture,animation,model,tabs,comparison,mcp
    const auto skip = qEnvironmentVariable("NOVA_LC_SKIP").split(',', Qt::SkipEmptyParts);
    auto on = [&](const char *part) { return !skip.contains(QLatin1String(part)); };
    int folderChecks = 0, folderReleased = 0;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        QElapsedTimer cycleTimer;
        cycleTimer.start();
        const QString root = cycle % 2 ? packB : packA;
        const QString other = cycle % 2 ? packA : packB;
        if (on("project")) {
            EditorTestAccess::switchProject(window, root);
            waitIndexed(window);
        }
        // The previous project is fully released after a switch.
        if (cycle > 0 && on("project")) {
            ++folderChecks;
            if (releasedFolder(other))
                ++folderReleased;
            else
                problems.append(QString("cycle %1: previous project folder still held open").arg(cycle));
        }
        // Texture: strokes, undo/redo, recovery snapshot, preview.
        const auto texture = root + "/assets/nova/textures/block/cube.png";
        if (on("texture"))
            EditorTestAccess::openTexture(window, texture);
        settle(60);
        auto &docs = window.documents().all();
        std::shared_ptr<Document> doc;
        for (const auto &d : docs)
            if (pathIdentity(d->path) == pathIdentity(texture))
                doc = d;
        if (doc && on("texture")) {
            for (int i = 0; i < 12; ++i)
                doc->paint(doc->activeLayer(), {i % 16, 0}, {15 - i % 16, 15}, qRgb(i * 20, 40, 200));
            for (int i = 0; i < 6; ++i)
                doc->undo();
            for (int i = 0; i < 3; ++i)
                doc->redo();
            window.activeCanvas() ? window.activeCanvas()->update() : void();
            settle(40);
            EditorTestAccess::recovery(window).writeDocument(*doc, root);
            EditorTestAccess::cube(window)->grabFramebuffer();
            while (doc->modified() && doc->canUndo())
                doc->undo();
            EditorTestAccess::recovery(window).remove(doc->id());
        } else if (on("texture")) {
            problems.append(QString("cycle %1: texture document did not open").arg(cycle));
        }
        // Animation document.
        if (on("animation"))
            EditorTestAccess::openTexture(window, root + "/assets/nova/textures/block/anim.png");
        settle(80);
        // Model document: the preview host moves into the model tab and back.
        if (on("model"))
            EditorTestAccess::openModel(window, "assets/nova/models/block/cube.json");
        if (auto dock = EditorTestAccess::modelsDock(window))
            dock->show();
        settle(80);
        EditorTestAccess::cube(window)->grabFramebuffer();
        auto tabs = EditorTestAccess::tabs(window);
        for (int i = 0; on("tabs") && i < tabs->count() * 2; ++i) {
            tabs->setCurrentIndex(i % tabs->count());
            settle(15);
        }
        // Comparison opened and closed.
        if (on("comparison"))
            EditorTestAccess::showComparison(window, "assets/nova/textures/block/cube.png");
        settle(60);
        if (auto dialog = EditorTestAccess::comparisonDialog(window))
            dialog->close();
        EditorTestAccess::clearComparisons(window);
        // MCP: authorize, connect a client, revoke (disconnects), reconnect is refused.
        if (on("mcp"))
            EditorTestAccess::showReview(window);
        settle(60);
        if (auto service = on("mcp") ? EditorTestAccess::mcp(window) : nullptr) {
            if (service->authorize(EditorTestAccess::project(window), cycle % 3 == 0)) {
                QLocalSocket client;
                if (qEnvironmentVariable("NOVA_LC_MCP") != "authorize") {
                    client.connectToServer("nova-mcp-" + service->connectionCode().left(32));
                    client.waitForConnected(1000);
                }
                settle(30);
                service->revoke();
                settle(30);
                client.abort();
            } else {
                problems.append(QString("cycle %1: MCP authorization failed").arg(cycle));
            }
        }
        for (auto top : QApplication::topLevelWidgets())
            if (top != &window && top->isVisible())
                top->close();
        // Close every document tab (all clean, so no prompt).
        while (tabs->count()) {
            const int before = tabs->count();
            EditorTestAccess::closeTab(window, 0);
            settle(20);
            if (tabs->count() == before) {
                problems.append(QString("cycle %1: a tab refused to close").arg(cycle));
                break;
            }
        }
        settle(100);
        samples.append(sample(window, cycle, cycleTimer.nsecsElapsed() / 1e6));
        fprintf(stderr, "[lifecycle] cycle %d\n", cycle);
    }
    // Summarize growth after warm-up: first quarter vs last sample.
    const int warm = std::max(2, cycles / 4);
    auto at = [&](int i, const char *key) { return samples[i].toObject()[key].toDouble(); };
    QJsonObject growth;
    for (const auto key : {"private_mb", "working_set_mb", "handles", "threads", "gdi_objects", "user_objects",
                           "gl_live_textures", "gl_live_materials", "thumbnail_cache_bytes", "recovery_bytes",
                           "top_level_widgets"})
        growth[key] = at(cycles - 1, key) - at(warm, key);
    QJsonObject report{{"cycles", cycles}, {"warmup_cycle", warm}, {"growth_after_warmup", growth},
                       {"folder_release_checks", folderChecks}, {"folder_released", folderReleased},
                       {"problems", problems}, {"samples", samples}};
    window.close();
    settle(200);
    QFile out(output);
    if (out.open(QIODevice::WriteOnly))
        out.write(QJsonDocument(report).toJson());
    if (!assertBounds)
        return 0;
    // Bounds: a leak of one object per cycle would exceed these over the measured span.
    const int span = cycles - 1 - warm;
    bool ok = problems.isEmpty() && folderReleased == folderChecks;
    ok &= growth["handles"].toDouble() < std::max(20, span);
    ok &= growth["threads"].toDouble() <= 2;
    ok &= growth["gdi_objects"].toDouble() < std::max(10, span / 2);
    ok &= growth["user_objects"].toDouble() < std::max(10, span / 2);
    ok &= growth["gl_live_textures"].toDouble() <= 0 && growth["gl_live_materials"].toDouble() <= 0;
    ok &= growth["top_level_widgets"].toDouble() <= 0;
    ok &= growth["recovery_bytes"].toDouble() <= 0;
    ok &= growth["private_mb"].toDouble() < 40;
    if (!ok)
        fprintf(stderr, "[lifecycle] bounds exceeded: %s\n", QJsonDocument(growth).toJson(QJsonDocument::Compact).constData());
    return ok ? 0 : 1;
}
