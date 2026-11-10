// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-F keyboard navigation on the real main window: focus traversal, opening assets
// from the tree, switching documents, and leaving dialogs and prompts with Escape.
#include "phase4f_probe_support.h"
#include "recovery.h"
#include "window.h"
#include <QAbstractButton>
#include <QDialog>
#include <QMessageBox>
#include <QPointer>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QtTest>
#include <functional>

namespace nova {
class EditorTestAccess {
  public:
    static bool indexing(const Window &w) { return w.indexBusy; }
    static void openTexture(Window &w, const QString &path) { w.openTexture(path); }
    static void setMode(Window &w, bool grid) { w.setMode(grid); }
    static void setDirectory(Window &w, const QString &d) { w.setDirectory(d); }
    static QTreeView *tree(Window &w) { return w.tree; }
    static QTabWidget *tabs(Window &w) { return w.tabs; }
    static void showReview(Window &w) { w.showMcpAccess(); }
    static void showComparison(Window &w, const QString &path) {
        Window::Comparison c;
        c.path = path;
        c.beforeRevision = "before";
        c.afterRevision = "after";
        c.detail = "Keyboard comparison";
        c.owner = "test";
        c.instance = QUuid::createUuid();
        c.before = QImage(16, 16, QImage::Format_RGBA8888);
        c.before.fill(Qt::red);
        c.after = QImage(16, 16, QImage::Format_RGBA8888);
        c.after.fill(Qt::blue);
        w.rememberComparison(c);
        w.showComparison(path, "after");
    }
    static QDialog *comparisonDialog(Window &w) { return w.comparisonDialog; }
    static RecoveryStore &recovery(Window &w) { return *w.recovery; }
};
} // namespace nova

using namespace nova;
using namespace phase4f;

QMessageBox *visiblePrompt(const QString &name = {}) {
    for (auto w : QApplication::topLevelWidgets())
        if (auto box = qobject_cast<QMessageBox *>(w); box && box->isVisible() && (name.isEmpty() || box->objectName() == name))
            return box;
    return nullptr;
}
class Phase4fKeyboardTests : public QObject {
    Q_OBJECT
    QTemporaryDir tmp;
    QString root;
    int run = 0;
    std::unique_ptr<Window> open(const std::function<void(const QString &)> &beforeStartup = {}) {
        const auto settingsFile = tmp.filePath(QString("settings-%1/studio.ini").arg(++run));
        qApp->setProperty("settingsFile", settingsFile);
        if (beforeStartup) {
            beforeStartup(QFileInfo(settingsFile).absolutePath() + "/recovery");
            if (QTest::currentTestFailed())
                return {};
        }
        QElapsedTimer started;
        started.start();
        auto window = std::make_unique<Window>(root, QString(), started);
        window->resize(1300, 850);
        window->show();
        window->activateWindow();
        [&] { QVERIFY(QTest::qWaitForWindowActive(window.get())); }();
        QElapsedTimer wait;
        wait.start();
        while ((EditorTestAccess::indexing(*window) || wait.elapsed() < 500) && wait.elapsed() < 20000)
            settle(20);
        return window;
    }
    // Moves the tree's current row to `file` with the arrow keys only.
    bool selectWithKeys(QTreeView *tree, const QString &file) {
        tree->setFocus();
        QTest::keyClick(tree, Qt::Key_Home);
        for (int i = 0; i < 400; ++i) {
            if (tree->currentIndex().data(Qt::UserRole).toString().endsWith(file))
                return true;
            QTest::keyClick(tree, Qt::Key_Down);
        }
        return false;
    }
  private slots:
    void initTestCase() {
        root = tmp.filePath("Keyboard Pack");
        makePack(root, 40);
    }
    // Enter on a tree row opens the asset, as double-click does.
    void enterOpensAssetFromTree() {
        auto window = open();
        auto tree = EditorTestAccess::tree(*window);
        EditorTestAccess::setMode(*window, false);
        EditorTestAccess::setDirectory(*window, "assets/nova/textures/block");
        settle(100);
        tree->expandAll();
        QVERIFY(selectWithKeys(tree, "/cube.png"));
        const int before = EditorTestAccess::tabs(*window)->count();
        QTest::keyClick(tree, Qt::Key_Return);
        settle(200);
        auto tabs = EditorTestAccess::tabs(*window);
        QCOMPARE(tabs->count(), before + 1);
        QVERIFY(tabs->tabText(tabs->currentIndex()).startsWith("cube.png"));
        // Enter again focuses the existing tab instead of opening a duplicate.
        tree->setFocus();
        QTest::keyClick(tree, Qt::Key_Return);
        settle(100);
        QCOMPARE(tabs->count(), before + 1);
    }
    // Tab moves focus through many controls and never traps it; Shift+Tab walks back.
    void tabTraversalVisitsControlsWithoutTrapping() {
        auto window = open();
        EditorTestAccess::openTexture(*window, root + "/assets/nova/textures/block/cube.png");
        settle(200);
        auto tree = EditorTestAccess::tree(*window);
        tree->setFocus();
        settle(50);
        QWidget *start = QApplication::focusWidget();
        QVERIFY(start);
        QList<QWidget *> path{start};
        QSet<QWidget *> seen{start};
        int stuck = 0;
        for (int i = 0; i < 150; ++i) {
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
            auto now = QApplication::focusWidget();
            QVERIFY2(now, "focus was lost");
            QVERIFY2(window->isAncestorOf(now) || now == window.get(), "focus left the window");
            stuck = now == path.last() ? stuck + 1 : 0;
            QVERIFY2(stuck < 3, qPrintable(QString("Tab is trapped in %1").arg(now->metaObject()->className())));
            path.append(now);
            seen.insert(now);
        }
        qInfo("Tab visited %d distinct controls in 150 presses", int(seen.size()));
        QVERIFY(seen.size() >= 15);
        // Shift+Tab retraces the same widgets in reverse.
        int retraced = 0;
        for (int i = 0; i < 20; ++i) {
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab, Qt::ShiftModifier);
            retraced += QApplication::focusWidget() == path[path.size() - 2 - i] ? 1 : 0;
        }
        QVERIFY2(retraced >= 18, qPrintable(QString::number(retraced)));
    }
    // Ctrl+Tab and Ctrl+Shift+Tab switch between open documents.
    void ctrlTabSwitchesDocuments() {
        auto window = open();
        EditorTestAccess::openTexture(*window, root + "/assets/nova/textures/block/cube.png");
        EditorTestAccess::openTexture(*window, root + "/assets/nova/textures/block/anim.png");
        settle(200);
        auto tabs = EditorTestAccess::tabs(*window);
        QVERIFY(tabs->count() >= 2);
        tabs->currentWidget()->setFocus();
        const int first = tabs->currentIndex();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab, Qt::ControlModifier);
        settle(50);
        QVERIFY2(tabs->currentIndex() != first, "Ctrl+Tab did not change document");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab, Qt::ControlModifier | Qt::ShiftModifier);
        settle(50);
        QCOMPARE(tabs->currentIndex(), first);
    }
    // Escape closes the workflow review and comparison windows.
    void escapeClosesReviewAndComparison() {
        auto window = open();
        EditorTestAccess::showReview(*window);
        settle(200);
        // The dialogs delete themselves on close; hold them through QPointer.
        QPointer<QDialog> review = window->findChild<QDialog *>("mcpAccessDialog");
        QVERIFY(review && review->isVisible());
        review->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(review));
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : review, Qt::Key_Escape);
        settle(100);
        QVERIFY(!review || !review->isVisible());
        EditorTestAccess::showComparison(*window, "assets/nova/textures/block/cube.png");
        settle(200);
        QPointer<QDialog> comparison = EditorTestAccess::comparisonDialog(*window);
        QVERIFY(comparison && comparison->isVisible());
        comparison->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(comparison));
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : comparison, Qt::Key_Escape);
        settle(100);
        QVERIFY(!comparison || !comparison->isVisible());
    }
    // Escape on the recovery prompt means Later: the snapshot is kept.
    void escapeOnRecoveryPromptKeepsSnapshot() {
        const auto texture = root + "/assets/nova/textures/block/cube.png";
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::green);
        const auto id = QUuid::createUuid();
        bool answered = false;
        QStringList prompts;
        QStringList tabsBefore;
        QTimer poll;
        connect(&poll, &QTimer::timeout, [&] {
            if (answered)
                return;
            auto box = visiblePrompt("recoveryDialog");
            if (!box)
                return;
            auto owner = dynamic_cast<Window *>(box->parentWidget());
            if (!owner)
                return;
            auto tabs = EditorTestAccess::tabs(*owner);
            for (int i = 0; i < tabs->count(); ++i)
                tabsBefore << tabs->tabText(i);
            prompts << box->text();
            answered = true;
            QTest::keyClick(box, Qt::Key_Escape);
        });
        poll.start(30);
        QString snapshot;
        QByteArray originalSnapshot;
        auto window = open([&](const QString &folder) {
            // This is an earlier session only if its file predates this window's RecoveryStore.
            RecoveryStore earlier(folder);
            QVERIFY(earlier.write(id, texture, QDir(root).canonicalPath(), Revision::read(texture), image));
            snapshot = earlier.pathFor(id);
            QFile file(snapshot);
            QVERIFY(file.open(QIODevice::ReadOnly));
            originalSnapshot = file.readAll();
            QVERIFY(!originalSnapshot.isEmpty());
        });
        QVERIFY(window);
        // Use the ordinary startup check once, without racing an explicit second discovery.
        QTRY_VERIFY_WITH_TIMEOUT(answered, 10000);
        settle(200);
        poll.stop();
        QCOMPARE(prompts, QStringList{"Unsaved texture recovered: " + texture});
        QFile file(snapshot);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), originalSnapshot);
        file.close();
        const auto pending = EditorTestAccess::recovery(*window).earlierEntries();
        QCOMPARE(pending.size(), 1);
        QCOMPARE(pending.front().id, id);
        auto tabs = EditorTestAccess::tabs(*window);
        QStringList open;
        for (int i = 0; i < tabs->count(); ++i)
            open << tabs->tabText(i);
        QCOMPARE(open, tabsBefore);
        EditorTestAccess::recovery(*window).discard(id);
    }
    // Escape on the unsaved-changes prompt cancels closing: nothing is saved or lost.
    void escapeOnCloseConfirmationCancels() {
        auto window = open();
        const auto texture = root + "/assets/nova/textures/block/cube.png";
        EditorTestAccess::openTexture(*window, texture);
        settle(150);
        std::shared_ptr<Document> doc;
        for (const auto &d : window->documents().all())
            if (pathIdentity(d->path) == pathIdentity(texture))
                doc = d;
        QVERIFY(doc);
        QVERIFY(doc->paint(doc->activeLayer(), {0, 0}, {3, 3}, qRgb(9, 9, 9)));
        const auto bytes = Revision::read(texture);
        bool answered = false;
        QTimer poll;
        connect(&poll, &QTimer::timeout, [&] {
            auto box = visiblePrompt();
            if (!box)
                return;
            answered = true;
            QTest::keyClick(box, Qt::Key_Escape);
        });
        poll.start(30);
        auto tabs = EditorTestAccess::tabs(*window);
        const int tabsBefore = tabs->count();
        emit tabs->tabCloseRequested(tabs->currentIndex());
        QTRY_VERIFY_WITH_TIMEOUT(answered, 10000);
        poll.stop();
        settle(100);
        QCOMPARE(tabs->count(), tabsBefore);
        QVERIFY(doc->modified());
        QVERIFY(bytes.sameContent(Revision::read(texture)));
        while (doc->modified() && doc->canUndo())
            doc->undo();
    }
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setApplicationName("Nova Phase 4-F Keyboard");
    Phase4fKeyboardTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "phase4f_keyboard_test.moc"
