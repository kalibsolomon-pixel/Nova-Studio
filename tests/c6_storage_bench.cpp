// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include "recovery.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#ifdef Q_OS_WIN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#endif
using namespace nova;
qint64 privateBytes() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX m{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m),
                         sizeof(m));
    return qint64(m.PrivateUsage);
#else
    return 0;
#endif
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 4 && argc != 5)
        return 2;
    const QString output = QString::fromLocal8Bit(argv[1]), kind = QString::fromLocal8Bit(argv[2]);
    const int side = QString::fromLocal8Bit(argv[3]).toInt();
    const int count = 64, layers = 4;
    QTemporaryDir temp;
    QImage base(side, side, QImage::Format_RGBA8888);
    base.fill(Qt::red);
    auto doc = std::make_shared<Document>(temp.filePath("texture.png"), base);
    AnimationState state;
    state.enabled = true;
    quint32 random = 0x13579bdu;
    for (int frame = 0; frame < count; ++frame) {
        SourceFrame source;
        source.name = QString("Frame %1").arg(frame);
        for (int layer = 0; layer < layers; ++layer) {
            QImage pixels = base;
            if (kind == "unique") {
                pixels = QImage(side, side, QImage::Format_RGBA8888);
                for (int y = 0; y < side; ++y) {
                    auto row = pixels.scanLine(y);
                    for (int x = 0; x < side * 4; ++x) {
                        random ^= random << 13;
                        random ^= random >> 17;
                        random ^= random << 5;
                        row[x] = uchar(random);
                    }
                }
            }
            RasterLayer raster;
            raster.id = QUuid::createUuid();
            raster.name = QString("Layer %1").arg(layer);
            raster.pixels = pixels;
            source.layers.push_back(raster);
        }
        source.activeLayer = source.layers[0].id;
        state.entries.push_back({source.id, frame % 4 + 1});
        state.frames.push_back(source);
    }
    state.active = state.frames[0].id;
    QString error;
    if (!doc->replaceAnimation(state, "benchmark", &error)) {
        qCritical("%s", qPrintable(error));
        return 3;
    }
    doc->setHistoryLimits(16 * 1024 * 1024, 2048);
    QJsonObject row{{"kind", kind},
                    {"side", side},
                    {"source_frames", count},
                    {"layers", layers},
                    {"decoded_bytes", double(doc->pixelBytes())},
                    {"private_before", double(privateBytes())}};
    QElapsedTimer t;
    t.start();
    auto bytes = doc->encodeNative("texture.png", &error);
    row["cold_encode_ms"] = t.nsecsElapsed() / 1e6;
    row["native_bytes"] = double(bytes.size());
    row["private_after_encode"] = double(privateBytes());
    if (bytes.isEmpty()) {
        qCritical("%s", qPrintable(error));
        return 4;
    }
    Document loaded(doc->path, {});
    t.restart();
    if (!loaded.decodeNative(bytes, "texture.png", &error))
        return 5;
    row["decode_ms"] = t.nsecsElapsed() / 1e6;
    row["private_after_decode"] = double(privateBytes());
    loaded.close();
    bytes.clear();
    bytes.squeeze();
    t.restart();
    if (!doc->saveNative(temp.path(), &error))
        return 6;
    row["guarded_save_ms"] = t.nsecsElapsed() / 1e6;
    row["private_after_save"] = double(privateBytes());
    RecoveryStore recovery(temp.filePath("recovery"));
    t.restart();
    if (!recovery.writeDocument(*doc->recoverySnapshot(), temp.path(), &error))
        return 7;
    row["recovery_ms"] = t.nsecsElapsed() / 1e6;
    row["private_after_recovery"] = double(privateBytes());
    if (app.arguments().contains("--cycles")) {
        doc->close();
        QJsonArray samples, opening, editing, saving;
        for (int cycle = 0; cycle < 32; ++cycle) {
            {
                Document current(temp.filePath("texture.png"), {});
                t.restart();
                if (!current.loadNative(temp.path(), &error)) return 9;
                opening.append(t.nsecsElapsed() / 1e6);
                t.restart();
                current.draw({cycle % side, 0}, {cycle % side, 0}, qRgba(cycle, 80, 190, 255));
                current.finish();
                editing.append(t.nsecsElapsed() / 1e6);
                if (cycle % 2) current.undo();
                else {
                    t.restart();
                    if (!current.saveNative(temp.path(), &error)) return 10;
                    saving.append(t.nsecsElapsed() / 1e6);
                }
                current.close();
            }
            QJsonObject sample{{"cycle", cycle}, {"private_mib", double(privateBytes()) / 1048576.0}};
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS_EX m{};
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m));
            DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
            sample["working_set_mib"] = double(m.WorkingSetSize) / 1048576.0;
            sample["peak_working_set_mib"] = double(m.PeakWorkingSetSize) / 1048576.0;
            sample["handles"] = int(handles);
#endif
            samples.append(sample);
        }
        row["lifecycle_closed_samples"] = samples;
        row["lifecycle_open_ms"] = opening;
        row["lifecycle_edit_ms"] = editing;
        row["lifecycle_save_ms"] = saving;
    }
    QFile file(output);
    if (!file.open(QIODevice::WriteOnly))
        return 8;
    file.write(QJsonDocument(row).toJson());
    return 0;
}
