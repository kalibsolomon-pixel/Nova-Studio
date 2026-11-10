// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include <QtWidgets>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: the Windows process types must precede PSAPI declarations.
#include <windows.h>
#include <psapi.h>
// clang-format on
#endif
namespace nova {
void Window::performanceCycle(const QString &root) {
    auto timer = new QTimer(this);
    timer->setInterval(1000);
    auto stage = std::make_shared<int>(0);
    connect(timer, &QTimer::timeout, this, [this, timer, stage, root] {
        auto measure = [&](const QString &label) {
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS_EX counters{};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                     sizeof(counters))) {
                metrics.samples[label + "_working_set_mib"].append(double(counters.WorkingSetSize) /
                                                                   (1024 * 1024));
                metrics.samples[label + "_private_mib"].append(double(counters.PrivateUsage) /
                                                               (1024 * 1024));
            }
#endif
            qint64 history = 0;
            qint64 pixels = 0, layers = 0;
            for (auto &doc : session.all()) {
                history += doc->historyBytes();
                pixels += doc->pixelBytes();
                layers += doc->layers().size();
            }
            metrics.count(label + "_history_bytes", history);
            metrics.count(label + "_thumbnail_bytes", model->cachedBytes());
            metrics.count(label + "_documents", session.count());
            metrics.count(label + "_canvases", tabs->count());
            metrics.count(label + "_qobjects", findChildren<QObject *>().size());
            metrics.count(label + "_pixel_bytes", pixels);
            metrics.count(label + "_layers", layers);
            if (!telemetry.isEmpty())
                metrics.write(telemetry);
        };
        switch (*stage) {
        case 0:
            measure("empty");
            pathText->setText(root);
            emit pathText->returnPressed();
            ++*stage;
            break;
        case 1:
            if (indexBusy || !current())
                return;
            measure("pack_open");
            setMode(true);
            ++*stage;
            break;
        case 2:
            measure("pack_browsed");
            for (const auto &path : collection.files) {
                if (path.endsWith(".png", Qt::CaseInsensitive))
                    openTexture(root + '/' + path);
                if (session.count() >= 8)
                    break;
            }
            measure("eight_documents_open");
            for (int i = tabs->count() - 1; i >= 0 && session.count() > 1; --i)
                if (auto canvas = dynamic_cast<Canvas *>(tabs->widget(i))) {
                    session.close(canvas->document);
                    removeDocumentTab(i);
                }
            if (!current())
                for (int i = 0; i < tabs->count(); ++i)
                    if (dynamic_cast<Canvas *>(tabs->widget(i))) {
                        tabs->setCurrentIndex(i);
                        break;
                    }
            refresh();
            measure("seven_documents_closed");
            for (int i = 0; i < 6000; ++i) {
                auto doc = current()->document;
                doc->draw({0, 0}, {0, 0}, qRgba(i % 255, 80, 90, 255));
                doc->finish();
            }
            refresh();
            ++*stage;
            break;
        case 3:
            measure("repeated_edit");
            if (qApp->arguments().contains("--layer-cycle")) {
                auto doc = current()->document;
                for (int n = 1; n < 16; ++n) {
                    doc->editLayer({LayerCommand::Add, {}, QString("Layer %1").arg(n)});
                    doc->draw({0, 0}, {10, 10}, qRgba(n * 10, 80, 120, 128));
                    doc->finish();
                }
                refresh();
                measure("sixteen_layers");
                QTimer::singleShot(50, this, [] {
                    if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                        box->button(QMessageBox::Discard)->click();
                });
            } else
                current()->document->reload();
            refresh();
            findChild<QAction *>("closeProjectCommand")->trigger();
            ++*stage;
            timer->setInterval(5000);
            break;
        case 4:
            measure("pack_closed");
            timer->stop();
            close();
            break;
        }
    });
    timer->start();
}
} // namespace nova
