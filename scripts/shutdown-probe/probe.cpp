// Diagnostic executable only. Links the unchanged, verified Release libraries.
// Every run owns its settings, recovery files and workload fixtures.
#include "window.h"
#include "batchdialog.h"
#include "opdialog.h"
#include <QFutureWatcher>
#include <QSurfaceFormat>
#include <QtWidgets>
#include <cstdio>

static QElapsedTimer elapsed;
static void record(const QString &event, QJsonObject fields = {}) {
    fields["event"] = event;
    fields["ms"] = elapsed.nsecsElapsed() / 1e6;
    fields["global_workers"] = QThreadPool::globalInstance()->activeThreadCount();
    const auto data = QJsonDocument(fields).toJson(QJsonDocument::Compact);
    fprintf(stdout, "%s\n", data.constData());
    fflush(stdout);
}
namespace nova {
// Window already exposes this friend for diagnostic/test access.
class EditorTestAccess {
  public:
    static QJsonObject state(Window &w) {
        return {{"index_busy", w.indexBusy}, {"io_busy", w.ioBusy},
                {"io_workers", w.ioPool.activeThreadCount()},
                {"documents", w.session.count()}, {"visible", w.isVisible()}};
    }
    static void rescan(Window &w) { w.fullRescan = true; w.updateIndex(); }
    static void snapshot(Window &w) { w.backgroundSafety(); }
};
}
class ProbeWindow : public nova::Window {
  public:
    using Window::Window;
    ~ProbeWindow() override { record("window_destructor_enter", nova::EditorTestAccess::state(*this)); }
  protected:
    void closeEvent(QCloseEvent *event) override {
        record("close_enter", nova::EditorTestAccess::state(*this));
        Window::closeEvent(event);
        auto fields = nova::EditorTestAccess::state(*this);
        fields["accepted"] = event->isAccepted();
        record("close_return", fields);
    }
};
int main(int argc, char **argv) {
    elapsed.start();
    int result = 0;
    {
        QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
        QSurfaceFormat format;
        format.setDepthBufferSize(24);
        format.setVersion(2, 1);
        format.setProfile(QSurfaceFormat::CompatibilityProfile);
        QSurfaceFormat::setDefaultFormat(format);
        QApplication app(argc, argv);
        app.setStyle("Fusion");
        app.setFont(QFont(app.font().family(), 9));
        auto args = app.arguments();
        auto arg = [&](const QString &key) { int i = args.indexOf(key); return i >= 0 ? args.value(i + 1) : QString(); };
        const auto mode = arg("--mode"), run = arg("--run-dir");
        QDir().mkpath(run);
        app.setProperty("settingsFile", run + "/settings.ini");
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [] { record("about_to_quit"); });
        {
            ProbeWindow window(arg("--project"), run + "/telemetry.json", elapsed);
            window.show();
            for (auto child : window.findChildren<QObject *>()) {
                const QString type = child->metaObject()->className();
                if (dynamic_cast<nova::Cube *>(child) || qobject_cast<nova::AssetModel *>(child) ||
                    qobject_cast<nova::DirectoryMonitor *>(child) || qobject_cast<QDockWidget *>(child))
                    QObject::connect(child, &QObject::destroyed, &app, [type] { record("child_destroyed", {{"type", type}}); });
            }
            // Resolve only the probe's deliberate unsaved edit, never owner data.
            QTimer discard;
            discard.setInterval(5);
            QObject::connect(&discard, &QTimer::timeout, &app, [] {
                if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    if (box->windowTitle() == "Unsaved texture") {
                        record("discard_probe_edit");
                        box->button(QMessageBox::Discard)->click();
                    }
            });
            discard.start();
            bool prepared = false;
            QTimer prepare;
            prepare.setInterval(10);
            QObject::connect(&prepare, &QTimer::timeout, &window, [&] {
                if (prepared || !window.activeCanvas()) return;
                prepared = true;
                prepare.stop();
                record("ready", {{"mode", mode}});
                auto close = [&window] { record("close_requested"); window.close(); };
                if (mode == "settled") {
                    QTimer::singleShot(3000, &window, close);
                } else if (mode == "index") {
                    nova::EditorTestAccess::rescan(window);
                    record("rescan_started", nova::EditorTestAccess::state(window));
                    close();
                } else if (mode == "thumbnails") {
                    auto model = window.findChild<nova::AssetModel *>();
                    auto index = nova::Index::scan(arg("--project"));
                    int requested = 0;
                    for (const auto &path : index.files)
                        if (path.endsWith(".png")) {
                            model->data(model->indexFor(path), Qt::DecorationRole);
                            if (++requested == 128) break;
                        }
                    record("thumbnails_requested", {{"count", requested}});
                    close();
                } else if (mode == "recovery") {
                    auto doc = window.activeCanvas()->document;
                    window.documents().apply(doc, doc->activeLayer(), {"noise", {{"seed", 123}}});
                    nova::EditorTestAccess::snapshot(window);
                    record("snapshot_started", nova::EditorTestAccess::state(window));
                    close();
                } else if (mode == "preview") {
                    auto presets = new nova::PresetStore(run + "/presets.json");
                    auto dialog = new nova::OperationDialog(&window.documents(), window.activeCanvas()->document,
                        window.activeCanvas(), presets, {}, {"gaussian_blur"}, [] { return QString(); }, &window);
                    dialog->parameters()->setParams({{"sigma", 6.0}});
                    dialog->show();
                    auto poll = new QTimer(&window);
                    QObject::connect(poll, &QTimer::timeout, &window, [&, dialog, presets, poll, close] {
                        bool running = false;
                        for (auto watcher : dialog->findChildren<QFutureWatcherBase *>())
                            running = running || watcher->isRunning();
                        if (!running) return;
                        poll->stop();
                        record("preview_worker_active");
                        delete dialog;
                        delete presets;
                        record("preview_destroyed");
                        close();
                    });
                    poll->start(1);
                } else if (mode == "batch") {
                    nova::BatchContext context;
                    context.root = arg("--batch-root");
                    context.files = nova::Index::scan(context.root).files;
                    auto dialog = new nova::BatchDialog(context, &window);
                    dialog->setScope(nova::batch::Scope::Pack, {});
                    nova::batch::Pipeline pipeline;
                    pipeline.steps = {{"resize_image", {{"width", 512}, {"height", 512}}},
                                      {"gaussian_blur", {{"sigma", 6.0}}}, {"median", {{"radius", 2}}}};
                    dialog->setPipeline(pipeline);
                    dialog->findChild<QLineEdit *>("batchDirectory")->setText(run + "/batch-output");
                    dialog->show();
                    if (!dialog->runDryRun() || !dialog->start()) {
                        record("setup_failed"); app.exit(2); return;
                    }
                    record("batch_started", {{"ready_items", dialog->plan().ready}, {"workers", dialog->plan().workers}});
                    QTimer::singleShot(30, &window, [dialog, close] {
                        record("batch_destroy_begin");
                        delete dialog;
                        record("batch_destroy_end");
                        close();
                    });
                } else if (mode == "floating") {
                    for (auto dock : window.findChildren<QDockWidget *>()) dock->setFloating(true);
                    record("docks_floated");
                    QTimer::singleShot(2100, &window, close);
                } else { record("setup_failed"); app.exit(2); }
            });
            prepare.start();
            result = app.exec();
            record("event_loop_return", {{"code", result}});
        }
        record("window_destroyed");
    }
    // Do not call the global pool here: QApplication and its global pool are gone.
    fprintf(stdout, "{\"event\":\"application_destroyed\",\"ms\":%.6f,\"code\":%d}\n",
            elapsed.nsecsElapsed() / 1e6, result);
    fflush(stdout);
    return result;
}
