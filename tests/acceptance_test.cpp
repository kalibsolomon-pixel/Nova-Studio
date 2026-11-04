// SPDX-License-Identifier: GPL-3.0-or-later
#include "project.h"
#include "window.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>
class AcceptanceTests : public QObject {
    Q_OBJECT
    QString evidence;
    void capture(QWidget *widget, const QString &name) {
        QTest::qWait(120);
        QVERIFY(widget->grab().save(evidence + '/' + name + ".png"));
    }
    void png(const QString &path, QRgb color) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QImage image(32, 32, QImage::Format_RGBA8888);
        image.fill(QColor::fromRgba(color));
        QVERIFY(nova::savePng(path, image));
    }
    void dismiss(QMessageBox::StandardButton choice) {
        QTimer::singleShot(40, [] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box)
                box->button(QMessageBox::Discard)->click();
        });
        Q_UNUSED(choice);
    }
  private slots:
    void completeWorkflow() {
        QTemporaryDir tmp;
        evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        if (evidence.isEmpty())
            evidence = tmp.path() + "/screenshots";
        QDir().mkpath(evidence);
        auto settings = tmp.path() + "/preferences/studio.ini";
        qApp->setProperty("settingsFile", settings);
        QElapsedTimer time;
        time.start();
        nova::Window window({}, {}, time);
        window.show();
        QTest::qWait(300);
        capture(&window, "01-welcome");
        auto root = tmp.path() + "/Example Pack";
        QTimer::singleShot(60, [&] {
            auto dialog = QApplication::activeModalWidget();
            QVERIFY(dialog);
            capture(dialog, "02-new-project");
            dialog->findChild<QLineEdit *>("projectDestination")->setText(root);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        window.findChild<QAction *>("newProjectCommand")->trigger();
        QVERIFY(QFileInfo::exists(root + "/pack.mcmeta"));
        png(root + "/assets/minecraft/textures/block/stone.png", qRgba(100, 120, 90, 255));
        png(root + "/assets/example/textures/block/moss.png", qRgba(40, 140, 80, 255));
        png(root + "/assets/example/textures/item/tool.png", qRgba(90, 70, 160, 255));
        auto model = window.findChild<nova::AssetModel *>();
        QTRY_VERIFY_WITH_TIMEOUT(
            model->indexFor("assets/minecraft/textures/block/stone.png").isValid(), 10000);
        auto tree = window.findChild<QTreeView *>("assetTree");
        auto proxy = qobject_cast<QSortFilterProxyModel *>(tree->model());
        emit tree->doubleClicked(
            proxy->mapFromSource(model->indexFor("assets/minecraft/textures/block/stone.png")));
        auto tabs = window.findChild<QTabWidget *>("editorTabs");
        QCOMPARE(tabs->count(), 1);
        capture(&window, "03-pack-workspace");
        QTimer::singleShot(60, [&] {
            auto dialog = QApplication::activeModalWidget();
            QVERIFY(dialog);
            capture(dialog, "04-project-properties");
            auto source = dialog->findChild<QPlainTextEdit *>("metadataSource");
            auto original = source->toPlainText();
            source->setPlainText("{ invalid metadata");
            capture(dialog, "08-metadata-validation");
            source->setPlainText(original);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        auto before = nova::Revision::read(root + "/pack.mcmeta");
        window.findChild<QAction *>("projectPropertiesCommand")->trigger();
        QVERIFY(before.sameContent(nova::Revision::read(root + "/pack.mcmeta")));
        QTimer::singleShot(60, [&] {
            auto dialog = QApplication::activeModalWidget();
            QVERIFY(dialog);
            dialog->findChild<QLineEdit *>()->setText("Edited resource pack description");
            auto version=dialog->findChild<QComboBox *>();
            const auto target=version->findText("1.21.11",Qt::MatchContains);
            QVERIFY(target>=0);
            version->setCurrentIndex(target);
            for (auto button : dialog->findChildren<QPushButton *>())
                if (button->text().startsWith("Apply description"))
                    button->click();
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        window.findChild<QAction *>("projectPropertiesCommand")->trigger();
        auto edited = nova::Project::inspect(root).json.value("pack").toObject();
        QCOMPARE(edited.value("description").toString(),
                 QString("Edited resource pack description"));
        QCOMPARE(edited.value("min_format").toArray()[0].toInt(), 75);
        auto category = window.findChild<QComboBox *>("assetCategory");
        category->setCurrentText("Blocks");
        QCOMPARE(model->rowCount(), 2);
        capture(&window, "05-categories");
        category->setCurrentIndex(0);
        tree->expandAll();
        capture(&window, "06-tree");
        for (auto b : window.findChildren<QPushButton *>())
            if (b->text() == "Grid")
                b->click();
        emit tree->doubleClicked(
            proxy->mapFromSource(model->indexFor("assets/minecraft/textures/block/stone.png")));
        capture(&window, "07-thumbnails");
        auto canvas = static_cast<nova::Canvas *>(tabs->currentWidget());
        canvas->document->draw({2, 2}, {20, 20}, qRgba(170, 80, 200, 255));
        canvas->document->finish();
        canvas->changed();
        capture(&window, "12-editor-preview");
        png(canvas->document->path, qRgba(220, 60, 50, 255));
        QTimer::singleShot(60, [&] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            capture(box, "09-external-conflict");
            for (auto b : box->buttons())
                if (b->text() == "Keep current")
                    b->click();
        });
        window.findChild<QAction *>("saveCommand")->trigger();
        QVERIFY(canvas->document->modified());
        QCOMPARE(nova::loadPng(canvas->document->path).pixel(0, 0), qRgba(220, 60, 50, 255));
        auto copy = root + "/local-copy.png";
        QVERIFY(canvas->document->saveAs(copy));
        QVERIFY(!canvas->document->modified());
        QVERIFY(window.close());
        // Force-terminate the actual native application test child only after its async snapshot is
        // on disk.
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--crash-child", root, settings});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForReadyRead(15000));
        QCOMPARE(child.readAllStandardOutput().trimmed(), QByteArray("snapshot-ready"));
        child.kill();
        QVERIFY(child.waitForFinished());
        auto disk = nova::Revision::read(root + "/assets/minecraft/textures/block/stone.png");
        QTimer restoreTimer;
        restoreTimer.setInterval(50);
        connect(&restoreTimer, &QTimer::timeout, this, [&] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || box->objectName() != "recoveryDialog")
                return;
            restoreTimer.stop();
            capture(box, "10-recovery");
            for (auto b : box->buttons())
                if (b->text() == "Restore")
                    b->click();
        });
        restoreTimer.start();
        nova::Window recovered(root, {}, time);
        recovered.show();
        auto recoveredTabs = recovered.findChild<QTabWidget *>("editorTabs");
        QTRY_VERIFY_WITH_TIMEOUT(!restoreTimer.isActive(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(
            static_cast<nova::Canvas *>(recoveredTabs->currentWidget())->document->modified(),
            5000);
        QVERIFY(disk.sameContent(
            nova::Revision::read(root + "/assets/minecraft/textures/block/stone.png")));
        dismiss(QMessageBox::Discard);
        QVERIFY(recovered.close());
        auto large = qEnvironmentVariable("NOVA_LARGE_FIXTURE");
        if (!large.isEmpty()) {
            qApp->setProperty("settingsFile", tmp.path() + "/large.ini");
            nova::Window big(large, {}, time);
            big.show();
            auto bigTabs = big.findChild<QTabWidget *>("editorTabs");
            QTRY_COMPARE_WITH_TIMEOUT(bigTabs->count(), 1, 15000);
            for (auto b : big.findChildren<QPushButton *>())
                if (b->text() == "Grid")
                    b->click();
            QTRY_VERIFY_WITH_TIMEOUT(
                big.findChild<nova::AssetModel *>()->cachedBytes() > 10 * 64 * 64 * 4, 10000);
            QTest::qWait(500);
            capture(&big, "11-large-project");
            QVERIFY(big.close());
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setApplicationName("Nova Studio Acceptance");
    app.setOrganizationName("Nova Studio");
    if (app.arguments().contains("--crash-child")) {
        auto args = app.arguments();
        app.setProperty("settingsFile", args[3]);
        QElapsedTimer time;
        time.start();
        nova::Window window(args[2], {}, time);
        window.show();
        QTimer edit;
        edit.setInterval(100);
        QObject::connect(&edit, &QTimer::timeout, &window, [&] {
            auto tabs = window.findChild<QTabWidget *>("editorTabs");
            if (!tabs->count())
                return;
            auto canvas = static_cast<nova::Canvas *>(tabs->currentWidget());
            canvas->document->draw({0, 0}, {10, 10}, qRgba(9, 8, 7, 255));
            canvas->document->finish();
            canvas->changed();
            edit.stop();
        });
        edit.start();
        QTimer ready;
        ready.setInterval(100);
        QObject::connect(&ready, &QTimer::timeout, &window, [&] {
            nova::RecoveryStore store(QFileInfo(args[3]).absolutePath() + "/recovery");
            if (!store.entries().isEmpty()) {
                ready.stop();
                fprintf(stdout, "snapshot-ready\n");
                fflush(stdout);
            }
        });
        ready.start();
        return app.exec();
    }
    AcceptanceTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "acceptance_test.moc"
