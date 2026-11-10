#include "window.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QScreen>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QToolTip>
#include <QWindow>
#include <QtTest>
#include <windows.h>

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QApplication::setStyle("Fusion");
    QTemporaryDir temporary;
    application.setProperty("settingsFile", temporary.filePath("studio.ini"));
    const QString output = application.arguments().value(1);
    if (output.isEmpty() || !QDir().mkpath(output))
        return 2;
    QElapsedTimer started;
    started.start();
    nova::Window window({}, {}, started);
    window.resize(1100, 720);
    window.show();
    if (!QTest::qWaitForWindowExposed(&window))
        return 3;
    QJsonArray rows;
    bool passed = true;
    int index = 0;
    for (auto screen : application.screens()) {
        if (!qEnvironmentVariable("NOVA_TEST_SCREEN").isEmpty() &&
            screen->name() != qEnvironmentVariable("NOVA_TEST_SCREEN"))
            continue;
        window.windowHandle()->setScreen(screen);
        window.setGeometry(QRect(screen->availableGeometry().topLeft() + QPoint(20, 20), QSize(1100, 720)));
        window.activateWindow();
        QTest::qWait(500);
        auto theme = window.findChild<QAction *>("darkThemeCommand");
        if (!theme)
            return 4;
        for (bool dark : {false, true}) {
            theme->setChecked(dark);
            for (const QString accent : {QString("blue"), QString("violet")}) {
                auto action = window.findChild<QAction *>("accent-" + accent);
                if (!action)
                    return 5;
                action->trigger();
                SendMessageW(reinterpret_cast<HWND>(window.winId()), WM_SETTINGCHANGE, 0,
                             reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
                QTest::qWait(200);
                QTest::keyClick(&window, Qt::Key_Tab);
                QOffscreenSurface surface;
                surface.create();
                QOpenGLContext context;
                const bool gl = context.create() && context.makeCurrent(&surface);
                if (gl)
                    context.doneCurrent();
                const QString label = QString("screen-%1-%2-%3").arg(index).arg(dark ? "dark" : "light", accent);
                const bool image = window.grab().save(output + '/' + label + ".png");
                const bool sameScreen = window.screen() == screen;
                rows.append(QJsonObject{{"screen", screen->name()}, {"logical_dpi", screen->logicalDotsPerInch()},
                                        {"device_pixel_ratio", window.devicePixelRatioF()}, {"dark", dark},
                                        {"accent", accent}, {"window_on_requested_screen", sameScreen},
                                        {"opengl_after_notification", gl}, {"screenshot", label + ".png"},
                                        {"focus_present", application.focusWidget() != nullptr}});
                passed = passed && gl && image && sameScreen;
            }
        }
        auto menu = window.menuBar()->actions().first()->menu();
        menu->popup(window.mapToGlobal(QPoint(10, 30)));
        QTest::qWait(150);
        passed = passed && menu->grab().save(output + QString("/screen-%1-menu.png").arg(index));
        menu->close();
        QToolTip::showText(window.mapToGlobal(QPoint(90, 90)), "Native tooltip acceptance", &window);
        QTest::qWait(150);
        QToolTip::hideText();
        ++index;
    }
    QJsonObject result{{"passed", passed}, {"screen_count", application.screens().size()}, {"rows", rows},
                       {"system_color_scheme", int(application.styleHints()->colorScheme())},
                       {"system_theme_changed", false}, {"synthetic_notification", "ImmersiveColorSet"}};
    QFile report(output + "/native-probe.json");
    if (!report.open(QIODevice::WriteOnly))
        return 6;
    report.write(QJsonDocument(result).toJson());
    window.close();
    return passed ? 0 : 1;
}
