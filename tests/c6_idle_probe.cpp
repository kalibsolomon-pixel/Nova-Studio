// SPDX-License-Identifier: GPL-3.0-or-later
#include "modelpanel.h"
#include "timeline.h"
#include "window.h"
#include <QAbstractEventDispatcher>
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QTimer>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace nova {
class EditorTestAccess {
  public:
    static void configure(Window &w, const QString &scenario) {
        if (scenario == "empty")
            return;
        const auto file = scenario.startsWith("animation") || scenario == "timeline-hidden"
                              ? "/assets/nova/textures/block/animated.png"
                              : "/assets/nova/textures/block/check.png";
        if (QFile::exists(w.project + file))
            w.openTexture(w.project + file);
        if (scenario == "model" || scenario == "floating" || scenario == "hidden")
            w.modelPanel->openAsset("assets/nova/models/block/complex.json");
        if (scenario == "hidden")
            w.previewDock->hide();
        if (scenario == "floating")
            w.previewDock->setFloating(true);
        if (scenario == "timeline-hidden" && w.animationDock)
            w.animationDock->hide();
        if (scenario == "animation-playing" && w.timeline)
            w.timeline->playback.play();
    }
    static QJsonObject counters(Window &w) {
        auto counters = w.metrics.counters;
        counters["gpu_resources"] = w.cube->resourceCounts();
        return counters;
    }
    static void discardProbeDocuments(Window &w) {
        // Probe-only imported animations are intentionally unsaved. No source writes.
        for (auto doc : w.session.all())
            while (doc->modified() && doc->historyCursor() > 0)
                doc->undo();
    }
};
} // namespace nova
class Activity : public QObject, public QAbstractNativeEventFilter {
  public:
    QMap<QString, qint64> counts;
    bool nativeEvents = false;
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override {
#ifdef Q_OS_WIN
        ++counts["native_" + QString::number(static_cast<MSG *>(message)->message, 16)];
#endif
        return false;
    }
    bool eventFilter(QObject *o, QEvent *e) override {
        const auto type = e->type();
        if (nativeEvents)
            ++counts["qt_event_" + QString::number(type)];
        if (type == QEvent::Timer || type == QEvent::Paint || type == QEvent::UpdateRequest ||
            type == QEvent::MetaCall || type == QEvent::DeferredDelete)
            ++counts[QString::number(type) + ':' + o->metaObject()->className() + ':' +
                     o->objectName()];
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
    activity.nativeEvents = args.contains("--native-events");
    if (activity.nativeEvents)
        app.installNativeEventFilter(&activity);
    app.installEventFilter(&activity);
    auto dispatcher = QAbstractEventDispatcher::instance();
    QObject::connect(dispatcher, &QAbstractEventDispatcher::awake, &activity,
                     [&] { ++activity.counts["dispatcher_awake"]; });
    QObject::connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, &activity,
                     [&] { ++activity.counts["dispatcher_block"]; });
    window.show();
    QTimer::singleShot(3000, &window,
                       [&] { nova::EditorTestAccess::configure(window, arg("--scenario")); });
    QTimer::singleShot(10000, &window, [&] { activity.counts.clear(); });
    QTimer::singleShot(arg("--seconds", "75").toInt() * 1000, &window, [&] {
        QFile file(arg("--activity"));
        if (file.open(QIODevice::WriteOnly))
            file.write(QJsonDocument(QJsonObject{{"events", activity.json()},
                                                 {"render_counters",
                                                  nova::EditorTestAccess::counters(window)}})
                           .toJson());
        nova::EditorTestAccess::discardProbeDocuments(window);
        window.close();
    });
    return app.exec();
}
