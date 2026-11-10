#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QObject>
#include <QScreen>
#include <QWidget>
#include <QWindow>
#include <cstdio>
#include <cstdlib>

namespace {
class TestDisplay final : public QObject {
    QString requested;
    bool placing = false;

    QScreen *screen() const {
        for (auto candidate : QGuiApplication::screens())
            if (candidate->name() == requested)
                return candidate;
        std::fprintf(stderr, "Requested test display is unavailable; refusing GUI execution\n");
        std::_Exit(78);
    }

    bool eventFilter(QObject *object, QEvent *event) override {
        if (placing || (event->type() != QEvent::Show && event->type() != QEvent::Move &&
                        event->type() != QEvent::Resize && event->type() != QEvent::Polish))
            return false;
        auto widget = qobject_cast<QWidget *>(object);
        if (!widget || !widget->isWindow() || widget->windowType() == Qt::Desktop)
            return false;
        placing = true;
        auto selected = screen();
        const auto area = selected->availableGeometry().adjusted(12, 32, -12, -12);
        auto geometry = widget->geometry();
        geometry.setSize(geometry.size().boundedTo(area.size()));
        geometry.moveLeft(qBound(area.left(), geometry.left(), area.right() - geometry.width() + 1));
        geometry.moveTop(qBound(area.top(), geometry.top(), area.bottom() - geometry.height() + 1));
        widget->setScreen(selected);
        if (widget->geometry() != geometry)
            widget->setGeometry(geometry);
        placing = false;
        return false;
    }

  public:
    explicit TestDisplay(QString name, QObject *parent) : QObject(parent), requested(std::move(name)) {}
};

void installTestDisplay() {
    const auto requested = qEnvironmentVariable("NOVA_TEST_SCREEN");
    if (!requested.isEmpty())
        QCoreApplication::instance()->installEventFilter(new TestDisplay(requested, QCoreApplication::instance()));
}
}

Q_COREAPP_STARTUP_FUNCTION(installTestDisplay)
