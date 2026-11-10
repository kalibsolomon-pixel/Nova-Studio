// SPDX-License-Identifier: GPL-3.0-or-later
// C8 memory attribution probe. Stages the work of the standalone render benchmark and
// samples process memory plus enumerated heap payload (busy vs free entries):
//   start -> 50k index -> 50k catalog -> both destroyed -> heap compaction ->
//   OpenGL preview of a small model -> 25 model cycles -> close.
// Output: JSON array of samples. Read-only on fixtures.
#include "cube.h"
#include "index.h"
#include "models.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <cstring>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <malloc.h>
#include <psapi.h>
#endif
using namespace nova;
namespace {
QJsonObject sample(const QString &label) {
    QJsonObject o{{"label", label}};
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX m{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m));
    o["working_set_mib"] = double(m.WorkingSetSize) / 1048576.0;
    o["private_mib"] = double(m.PrivateUsage) / 1048576.0;
    HANDLE heaps[256];
    const DWORD count = std::min<DWORD>(GetProcessHeaps(256, heaps), 256);
    qint64 busyAll = 0, freeAll = 0, busyDefault = 0, freeDefault = 0;
    for (DWORD i = 0; i < count; ++i) {
        qint64 busy = 0, free = 0;
        if (!HeapLock(heaps[i]))
            continue;
        PROCESS_HEAP_ENTRY e{};
        while (HeapWalk(heaps[i], &e)) {
            if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY)
                busy += e.cbData;
            else if (!(e.wFlags & (PROCESS_HEAP_REGION | PROCESS_HEAP_UNCOMMITTED_RANGE)))
                free += e.cbData;
        }
        HeapUnlock(heaps[i]);
        busyAll += busy;
        freeAll += free;
        if (heaps[i] == GetProcessHeap()) {
            busyDefault = busy;
            freeDefault = free;
        }
    }
    o["heaps"] = double(count);
    o["heap_busy_mib"] = double(busyAll) / 1048576.0;
    // Preserve the legacy JSON key. This sums HeapWalk free-entry payloads;
    // it is not an independently validated component of process private commit.
    o["heap_free_committed_mib"] = double(freeAll) / 1048576.0;
    o["default_heap_busy_mib"] = double(busyDefault) / 1048576.0;
    o["default_heap_free_committed_mib"] = double(freeDefault) / 1048576.0;
#endif
    return o;
}
void compact() {
#ifdef Q_OS_WIN
    _heapmin();
    HANDLE heaps[256];
    const DWORD count = std::min<DWORD>(GetProcessHeaps(256, heaps), 256);
    for (DWORD i = 0; i < count; ++i)
        HeapCompact(heaps[i], 0);
#endif
}
} // namespace
int main(int argc, char **argv) {
    bool sharedContexts = false, fusion = false, emptyFirst = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shared-contexts") == 0) sharedContexts = true;
        if (std::strcmp(argv[i], "--fusion") == 0) fusion = true;
        if (std::strcmp(argv[i], "--empty-first") == 0) emptyFirst = true;
    }
    if (sharedContexts) QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    if (fusion) app.setStyle("Fusion");
    if (argc < 4 || argc > 6)
        return 2;
    const QString models = QString::fromLocal8Bit(argv[1]), large = QString::fromLocal8Bit(argv[2]);
    QJsonArray samples;
    auto initial = sample("start");
    initial["shared_contexts"] = sharedContexts;
    initial["fusion"] = fusion;
    initial["empty_first"] = emptyFirst;
    samples.append(initial);
    {
        auto index = Index::scan(large);
        samples.append(sample("50k-index"));
        mc::Library catalog;
        catalog.update(large, index);
        auto s = sample("50k-catalog");
        s["assets"] = double(index.files.size());
        samples.append(s);
    }
    samples.append(sample("50k-destroyed"));
    compact();
    samples.append(sample("heap-compacted"));
    auto index = Index::scan(models);
    mc::Library library;
    library.update(models, index);
    auto model = library.model("nova:block/complex");
    Metrics metrics;
    Cube cube(&metrics);
    cube.resize(360, 320);
    if (emptyFirst) {
        cube.show();
        QTest::qWait(80);
        cube.grabFramebuffer();
        samples.append(sample("empty-preview-first"));
    }
    cube.setModel(std::make_shared<mc::Mesh>(mc::mesh(model)));
    cube.show();
    QTest::qWait(80);
    cube.grabFramebuffer();
    auto gl = sample("gl-preview");
    gl["gpu"] = cube.resourceCounts();
    samples.append(gl);
    for (int i = 0; i < 25; ++i) {
        auto m = library.model(i % 2 ? "nova:block/check" : "nova:block/complex");
        cube.setModel(std::make_shared<mc::Mesh>(mc::mesh(m)));
        cube.grabFramebuffer();
    }
    auto cycles = sample("25-model-cycles");
    cycles["gpu"] = cube.resourceCounts();
    samples.append(cycles);
    cube.setModel({});
    cube.grabFramebuffer();
    cube.hide();
    QTest::qWait(30);
    samples.append(sample("generic-close"));
    compact();
    samples.append(sample("generic-close-compacted"));
    QFile file(QString::fromLocal8Bit(argv[3]));
    if (!file.open(QIODevice::WriteOnly))
        return 3;
    file.write(QJsonDocument(samples).toJson());
    return 0;
}
