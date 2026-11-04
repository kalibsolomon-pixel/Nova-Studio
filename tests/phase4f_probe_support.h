// SPDX-License-Identifier: GPL-3.0-or-later
// Shared helpers for the Phase 4-F probes: GUI-thread stack sampler, fixture packs,
// event-loop settling and process counters. Header-only; each probe is its own executable.
#pragma once
#include "core.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#include <qt_windows.h>
#include <psapi.h>
#include <dbghelp.h>

namespace phase4f {
using namespace nova;
class Sampler {
    static constexpr int Depth = 48, Capacity = 200000;
    struct Sample {
        int depth;
        DWORD64 frames[Depth];
    };
    std::vector<Sample> samples = std::vector<Sample>(Capacity);
    std::atomic_int count = 0;
    std::atomic_bool running = false;
    std::thread worker;
    HANDLE target = nullptr;

  public:
    void start() {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        running = true;
        worker = std::thread([this] {
            HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
            while (running && count < Capacity) {
                LARGE_INTEGER due{};
                due.QuadPart = -5000; // 0.5 ms
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 10);
                if (SuspendThread(target) == DWORD(-1))
                    continue;
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                auto &out = samples[size_t(count.load())];
                out.depth = 0;
                if (GetThreadContext(target, &ctx)) {
                    for (int i = 0; i < Depth && ctx.Rip; ++i) {
                        out.frames[out.depth++] = ctx.Rip;
                        DWORD64 base = 0;
                        auto entry = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                        if (!entry) {
                            ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                            ctx.Rsp += 8;
                            continue;
                        }
                        void *handler = nullptr;
                        DWORD64 frame = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, entry, &ctx, &handler, &frame, nullptr);
                    }
                }
                ResumeThread(target);
                ++count;
            }
            CloseHandle(timer);
        });
    }
    QJsonObject stop(int limit = 40) {
        running = false;
        worker.join();
        CloseHandle(target);
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_PUBLICS_ONLY);
        SymInitializeW(process, nullptr, TRUE);
        QHash<DWORD64, QString> names;
        auto name = [&](DWORD64 address) -> QString {
            auto it = names.find(address);
            if (it != names.end())
                return *it;
            alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW) + 1024] = {};
            auto symbol = reinterpret_cast<SYMBOL_INFOW *>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
            symbol->MaxNameLen = 500;
            DWORD64 displacement = 0;
            IMAGEHLP_MODULEW64 module{};
            module.SizeOfStruct = sizeof(module);
            QString moduleName = SymGetModuleInfoW64(process, address, &module)
                                     ? QString::fromWCharArray(module.ModuleName)
                                     : QStringLiteral("?");
            QString result;
            if (SymFromAddrW(process, address, &displacement, symbol) && displacement < 0x20000)
                result = moduleName + "!" + QString::fromWCharArray(symbol->Name, int(symbol->NameLen));
            else
                result = moduleName + "!+" + QString::number(address - module.BaseOfImage, 16);
            names.insert(address, result);
            return result;
        };
        QHash<QString, int> self, inclusive;
        const int n = count;
        for (int i = 0; i < n; ++i) {
            const auto &sample = samples[size_t(i)];
            if (!sample.depth)
                continue;
            ++self[name(sample.frames[0])];
            QSet<QString> seen;
            for (int f = 0; f < sample.depth; ++f) {
                const auto fn = name(sample.frames[f]);
                if (!seen.contains(fn)) {
                    seen.insert(fn);
                    ++inclusive[fn];
                }
            }
        }
        SymCleanup(process);
        auto top = [&](const QHash<QString, int> &table) {
            std::vector<std::pair<QString, int>> v;
            for (auto it = table.cbegin(); it != table.cend(); ++it)
                v.emplace_back(it.key(), it.value());
            std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
            QJsonArray rows;
            for (int i = 0; i < int(v.size()) && i < limit; ++i)
                rows.append(QJsonObject{{"fn", v[i].first}, {"pct", 100.0 * v[i].second / std::max(1, n)}});
            return rows;
        };
        return {{"samples", n}, {"self", top(self)}, {"inclusive", top(inclusive)}};
    }
};
inline void png(const QString &path, int size, int seed) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage im(size, size, QImage::Format_RGBA8888);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            im.setPixel(x, y, qRgba((x * 13 + seed * 7) & 255, (y * 11 + seed * 3) & 255, (seed * 29) & 255, 255));
    savePng(path, im);
}
inline void text(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    atomicReplace(path, [&](QIODevice &f) { return f.write(bytes) == bytes.size(); });
}
// A resource pack shaped like a real one: many textures over several folders.
inline void makePack(const QString &root, int textures) {
    text(root + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Resize probe"}})");
    for (int i = 0; i < textures; ++i)
        png(root + QString("/assets/nova/textures/block/set%1/t%2.png").arg(i % 24).arg(i), i % 5 ? 16 : 64, i);
    png(root + "/assets/nova/textures/block/anim.png", 16, 3);
    QImage sheet(16, 64, QImage::Format_RGBA8888);
    sheet.fill(Qt::darkCyan);
    savePng(root + "/assets/nova/textures/block/anim.png", sheet);
    text(root + "/assets/nova/textures/block/anim.png.mcmeta", R"({"animation":{"frametime":2}})");
    png(root + "/assets/nova/textures/block/cube.png", 16, 9);
    text(root + "/assets/nova/models/block/cube.json",
         R"({"textures":{"all":"nova:block/cube"},"elements":[{"from":[0,0,0],"to":[16,16,16],"faces":{"north":{"texture":"#all","uv":[0,0,16,16]},"south":{"texture":"#all","uv":[0,0,16,16]},"east":{"texture":"#all","uv":[0,0,16,16]},"west":{"texture":"#all","uv":[0,0,16,16]},"up":{"texture":"#all","uv":[0,0,16,16]},"down":{"texture":"#all","uv":[0,0,16,16]}}}]})");
}
// Pumps events for `ms`. The probes drive the window from main() without exec(), where
// Qt never delivers deleteLater(); deliver deferred deletes explicitly, as returning to
// the application's event loop does, so objects the product releases are released.
inline void settle(int ms = 150) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}
struct Stats {
    QJsonObject json(const std::vector<double> &v) const {
        if (v.empty())
            return {};
        auto s = v;
        std::sort(s.begin(), s.end());
        double sum = 0;
        for (double x : s)
            sum += x;
        return {{"n", int(s.size())},
                {"median_ms", s[s.size() / 2]},
                {"p95_ms", s[std::min(s.size() - 1, (95 * s.size() + 99) / 100 - 1)]},
                {"max_ms", s.back()},
                {"total_ms", sum}};
    }
};
inline FILETIME cpu() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULARGE_INTEGER a{k.dwLowDateTime, k.dwHighDateTime}, b{u.dwLowDateTime, u.dwHighDateTime};
    ULARGE_INTEGER t;
    t.QuadPart = a.QuadPart + b.QuadPart;
    return {t.LowPart, t.HighPart};
}
inline double cpuMs(FILETIME a, FILETIME b) {
    ULARGE_INTEGER x{a.dwLowDateTime, a.dwHighDateTime}, y{b.dwLowDateTime, b.dwHighDateTime};
    return (y.QuadPart - x.QuadPart) / 1e4;
}
inline qint64 privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
    return qint64(pmc.PrivateUsage);
}
} // namespace phase4f
