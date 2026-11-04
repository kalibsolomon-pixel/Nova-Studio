// SPDX-License-Identifier: GPL-3.0-or-later
#include "metrics.h"
#include "branding.h"
#include "build_identity.h"
#include "diagnostics.h"
#include "window.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif
int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    // Plugins may resolve dependencies only from the application, system and
    // explicitly registered directories, never the current working directory.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#endif
    QElapsedTimer started;
    started.start();
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    // The 3D preview is a QOpenGLWidget, so Qt composites the whole main window through
    // OpenGL and presents it with SwapBuffers on every flush. With vsync that present
    // blocks until the next refresh, and a resize step or scroll frame flushes more than
    // once, which made resizing and scrolling stall (Phase 4-F). The preview renders into
    // an offscreen framebuffer and DWM composes windowed presents, so disabling vsync for
    // the presents changes no rendered pixels. Every repaint is still requested by an
    // event; nothing renders continuously.
    format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    // Select the application style before constructing the workspace. Changing it
    // afterwards repolishes every existing widget during the startup path.
    app.setStyle("Fusion");
    app.setProperty("applicationInitMs", started.nsecsElapsed() / 1e6);
    app.setApplicationName("Nova Studio");
    app.setWindowIcon(nova::applicationIcon());
    app.setApplicationVersion(nova::AppVersion);
    app.setOrganizationName("Nova Studio");
    QFont font = app.font();
    font.setPointSize(9);
    app.setFont(font);
    auto args = app.arguments();
    auto arg = [&](QString key, QString fallback = QString()) {
        int i = args.indexOf(key);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : fallback;
    };
    if (args.contains("--bench"))
        return nova::benchmark(arg("--bench"), arg("--output", "qt-bench.json")) ? 0 : 1;
    app.setProperty("settingsFile", arg("--settings"));
    const auto diagnosticRoot = arg("--settings").isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/diagnostics"
        : QFileInfo(arg("--settings")).absolutePath() + "/diagnostics";
    if (!nova::diagnostics::start(diagnosticRoot))
        qWarning("Local diagnostic log is unavailable");
    if (args.contains("--diagnostics-export")) {
        QString error;
        if (nova::diagnostics::exportReport(arg("--diagnostics-export"), &error)) return 0;
        qCritical().noquote() << "Diagnostic export failed:" << error;
        return 1;
    }
    auto project = arg("--project");
    if (project.isEmpty()) {
        auto prefs = nova::Settings(arg("--settings")).load();
        auto recent = prefs.reopenLast ? prefs.recentProjects : QStringList{};
        if (!recent.isEmpty())
            project = recent.first();
    }
    nova::Window window(project, arg("--telemetry"), started);
    window.show();
    if (args.contains("--cycle-project"))
        window.performanceCycle(arg("--cycle-project"));
    if (args.contains("--qa-dir"))
        window.qa(arg("--qa-dir"));
    if (args.contains("--exercise"))
        window.startExercise();
    if (args.contains("--smoke-seconds"))
        QTimer::singleShot(int(arg("--smoke-seconds").toDouble() * 1000), &window, &QWidget::close);
    return app.exec();
}
