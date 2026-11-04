#include "metrics.h"
#include "window.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QTimer>
int main(int argc, char **argv) {
    QElapsedTimer started;
    started.start();
    QApplication app(argc, argv);
    app.setApplicationName("Nova Studio Qt prototype");
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
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(format);
    nova::Window window(arg("--project", "fixtures/generated/pack-1500"), arg("--telemetry"),
                        started);
    window.show();
    if (args.contains("--qa-dir"))
        window.qa(arg("--qa-dir"));
    if (args.contains("--exercise"))
        window.startExercise();
    if (args.contains("--smoke-seconds"))
        QTimer::singleShot(int(arg("--smoke-seconds").toDouble() * 1000), &window, &QWidget::close);
    return app.exec();
}
