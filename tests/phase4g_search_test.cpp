// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-G: application-wide search (Ctrl+K). Coverage, ranking for the example queries,
// keyboard-only use from every workspace, activation through the existing handlers, settings
// navigation, context-dependent availability, shortcut conflicts and responsiveness.
#include "search.h"
#include "window.h"
#include <QDockWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>

using namespace nova;

class Phase4gSearchTest : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QString evidence = qEnvironmentVariable("NOVA_PHASE4G_EVIDENCE");
    QString root;
    std::unique_ptr<Window> window;

    QString scaleLabel() const { return qEnvironmentVariable("QT_SCALE_FACTOR", "1"); }
    static QStringList titles(const SearchIndex &index, const QString &query, int count) {
        QStringList out;
        for (const auto &result : index.query(query, count))
            out << (result.entry >= 0 ? index.entries[result.entry].title : "asset:" + result.asset);
        return out;
    }
    // Runs body against the next modal dialog with this object name.
    void onModal(const QString &name, std::function<void(QDialog *)> body) {
        auto timer = new QTimer(this);
        timer->setInterval(10);
        auto started = std::make_shared<QElapsedTimer>();
        started->start();
        connect(timer, &QTimer::timeout, this, [=] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog && dialog->objectName() == name && dialog->isVisible()) {
                timer->stop();
                timer->deleteLater();
                body(dialog);
                // A failed check returns from body early; never leave the modal open.
                if (dialog->isVisible())
                    dialog->reject();
            } else if (started->elapsed() > 10000) {
                timer->stop();
                timer->deleteLater();
                if (auto stray = QApplication::activeModalWidget())
                    stray->close();
                QFAIL(qPrintable("Modal did not appear: " + name));
            }
        });
        timer->start();
    }
    // Opens Search with the keyboard and types a query; body inspects the open dialog.
    void search(const QString &query, std::function<void(QDialog *, QLineEdit *, QListWidget *)> body) {
        onModal("commandPalette", [=](QDialog *dialog) {
            auto edit = dialog->findChild<QLineEdit *>("commandQuery");
            auto list = dialog->findChild<QListWidget *>("commandList");
            QVERIFY(edit && list);
            QCOMPARE(dialog->focusWidget(), edit);
            QTest::keyClicks(edit, query);
            body(dialog, edit, list);
            if (dialog->isVisible())
                QTest::keyClick(edit, Qt::Key_Escape);
            QVERIFY(!dialog->isVisible());
        });
        window->activateWindow();
        QTest::keyClick(window.get(), Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(!QApplication::activeModalWidget());
    }

  private slots:
    void initTestCase() {
        root = temporary.filePath("Search Pack");
        QDir().mkpath(root + "/assets/nova/textures/block");
        QDir().mkpath(root + "/assets/nova/models/block");
        QFile meta(root + "/pack.mcmeta");
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write(R"({"pack":{"pack_format":75,"description":"search"}})");
        meta.close();
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(QColor(80, 120, 160));
        for (int i = 0; i < 3000; ++i)
            QVERIFY(image.save(QString("%1/assets/nova/textures/block/stone_variant_%2.png").arg(root).arg(i)));
        QVERIFY(image.save(root + "/assets/nova/textures/block/ruby_ore.png"));
        qApp->setProperty("settingsFile", temporary.filePath("search.ini"));
        QElapsedTimer started;
        started.start();
        window = std::make_unique<Window>(root, QString{}, started);
        window->resize(1280, 800);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(window->searchIndex().assets.size() >= 3001, 30000);
    }
    void cleanupTestCase() {
        if (window)
            window->close();
        window.reset();
    }

    void indexCoversTheApplication() {
        const auto index = window->searchIndex();
        QMap<QString, int> categories;
        for (const auto &entry : index.entries)
            ++categories[entry.category];
        for (const auto &category : {"Command", "Tool", "Setting", "Workspace", "Panel"})
            QVERIFY2(categories.value(category) > 0, category);
        QVERIFY(categories["Workspace"] == 5);
        QVERIFY(categories["Tool"] >= 8);
        QVERIFY(index.entries.size() > 120);
        // Search lists commands from the menus themselves; it never lists itself.
        QStringList objects;
        for (const auto &entry : index.entries)
            if (entry.action)
                objects << entry.action->objectName();
        for (const auto &name : {"editUndo", "editRedo", "saveCommand", "exportAnimation", "testInMinecraft",
                                 "syncTestPack", "mcpAccess", "validateResources", "workspacePreset-uv", "tool-pencil"})
            QVERIFY2(objects.contains(name), name);
        QVERIFY(!objects.contains("commandPaletteCommand"));
        // Every curated alias names an action or panel that exists, so aliases cannot drift silently.
        for (const auto &name : searchAliasNames())
            QVERIFY2(window->findChild<QAction *>(name) || window->findChild<QDockWidget *>(name), qPrintable(name));
        if (!evidence.isEmpty()) {
            QJsonArray rows;
            for (const auto &entry : index.entries)
                rows.append(QJsonObject{{"title", entry.title}, {"category", entry.category}, {"location", entry.location},
                                        {"shortcut", entry.shortcut}, {"keywords", QJsonArray::fromStringList(entry.keywords)},
                                        {"unavailable", entry.unavailable}});
            QDir().mkpath(evidence);
            QFile file(evidence + "/search-index.json");
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(QJsonObject{{"entries", rows}, {"assets", index.assets.size()},
                                                 {"categories", QJsonObject::fromVariantMap(QVariantMap(
                                                     [&] { QVariantMap m; for (auto it = categories.begin(); it != categories.end(); ++it) m[it.key()] = it.value(); return m; }()))}})
                           .toJson());
        }
    }
    void exampleQueriesFindTheirDestination() {
        const auto index = window->searchIndex();
        const QList<std::tuple<QString, QString, int>> cases{
            {"export animation", "Export Minecraft Animation…", 1},
            {"minecraft", "Test in Minecraft…", 3},
            {"layers", "Layers", 3},
            {"opacity", "Layer opacity", 3},
            {"uv", "UV Editing", 3},
            {"undo", "Undo", 1},
            {"mcp", "MCP Integration…", 3},
            {"appearance", "Theme", 3},
            {"test in minecraft", "Test in Minecraft…", 1},
            {"ctrl+z", "Undo", 1},
            {"F8", "Validate Resources", 1},
            {"PREFERENCES", "Preferences…", 1},
            {"agent", "Set up an AI agent (MCP)", 3},
            {"flip image hor", "Flip Image Horizontally", 1},
            {"ruby", "asset:assets/nova/textures/block/ruby_ore.png", 1},
        };
        QJsonArray rows;
        for (const auto &[query, expected, within] : cases) {
            const auto found = titles(index, query, 8);
            rows.append(QJsonObject{{"query", query}, {"expected", expected}, {"top", QJsonArray::fromStringList(found)}});
            QVERIFY2(found.mid(0, within).contains(expected),
                     qPrintable(QString("%1 → %2").arg(query, found.join(" | "))));
        }
        QVERIFY(titles(index, "zzzz-no-such-thing", 5).isEmpty());
        if (!evidence.isEmpty()) {
            QFile file(evidence + "/search-examples.json");
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(rows).toJson());
        }
    }
    void opensFromEveryWorkspaceWithTheKeyboard() {
        for (const auto &preset : {"essentials", "texture", "animation", "model", "uv"}) {
            window->findChild<QAction *>(QString("workspacePreset-") + preset)->trigger();
            QCOMPARE(window->workspacePreset(), QString(preset));
            bool sawCurrent = false;
            search("workspace", [&](QDialog *, QLineEdit *, QListWidget *list) {
                for (int i = 0; i < list->count(); ++i)
                    if (list->item(i)->text().endsWith("(current)"))
                        sawCurrent = true;
            });
            QVERIFY2(sawCurrent, preset);
        }
        // Switching workspace through Search uses the existing preset action.
        window->findChild<QAction *>("workspacePreset-essentials")->trigger();
        search("uv editing", [](QDialog *, QLineEdit *edit, QListWidget *list) {
            QCOMPARE(list->currentItem()->text(), QString("UV Editing"));
            QTest::keyClick(edit, Qt::Key_Return);
        });
        QCOMPARE(window->workspacePreset(), QString("uv"));
        window->findChild<QAction *>("workspacePreset-essentials")->trigger();
        // Ctrl+Shift+P keeps working.
        onModal("commandPalette", [](QDialog *dialog) { dialog->reject(); });
        QTest::keyClick(window.get(), Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(!QApplication::activeModalWidget());
    }
    void resultsRunTheSameHandlersAsMenus() {
        auto validate = window->findChild<QAction *>("validateResources");
        QSignalSpy triggered(validate, &QAction::triggered);
        search("validate res", [](QDialog *, QLineEdit *edit, QListWidget *list) {
            // Arrow keys move through results; Enter runs the selected one.
            QTest::keyClick(edit, Qt::Key_Down);
            QTest::keyClick(edit, Qt::Key_Up);
            QCOMPARE(list->currentRow(), 0);
            QTest::keyClick(edit, Qt::Key_Return);
        });
        QCOMPARE(triggered.count(), 1);
        QTRY_VERIFY(window->findChild<QDockWidget *>("resourceProblems")->isVisible());
        // A panel result shows the panel; it never hides an open one.
        auto layers = window->findChild<QDockWidget *>("layers");
        layers->hide();
        search("layers", [](QDialog *, QLineEdit *edit, QListWidget *list) {
            QCOMPARE(list->currentItem()->text(), QString("Layers"));
            QTest::keyClick(edit, Qt::Key_Return);
        });
        QTRY_VERIFY(layers->isVisible());
        search("layers", [](QDialog *, QLineEdit *edit, QListWidget *) { QTest::keyClick(edit, Qt::Key_Return); });
        QVERIFY(layers->isVisible());
        // Assets open through the same path as related-asset navigation.
        search("ruby_ore", [](QDialog *, QLineEdit *edit, QListWidget *) { QTest::keyClick(edit, Qt::Key_Return); });
        QTRY_VERIFY(window->activeCanvas());
        QVERIFY(window->activeCanvas()->document->path.endsWith("ruby_ore.png"));
    }
    void settingsResultsFocusTheControl() {
        QString focused;
        onModal("preferencesDialog", [&](QDialog *dialog) {
            QTest::qWait(50); // the focus request runs on the next event-loop turn
            QVERIFY(dialog->focusWidget());
            focused = dialog->focusWidget()->objectName();
            if (!evidence.isEmpty())
                dialog->grab().save(QString("%1/preferences-from-search-%2.png").arg(evidence, scaleLabel()));
            dialog->reject();
        });
        search("accent colour", [](QDialog *, QLineEdit *edit, QListWidget *list) {
            QCOMPARE(list->currentItem()->text(), QString("Accent colour"));
            QTest::keyClick(edit, Qt::Key_Return);
        });
        QTRY_COMPARE(focused, QString("preferenceAccent"));
        // MCP results open the matching page of the MCP Integration window.
        search("set up an ai agent", [](QDialog *, QLineEdit *edit, QListWidget *) { QTest::keyClick(edit, Qt::Key_Return); });
        auto mcp = window->findChild<QDialog *>("mcpAccessDialog");
        QTRY_VERIFY(mcp && mcp->isVisible());
        QCOMPARE(mcp->findChild<QTabWidget *>("mcpTabs")->currentIndex(), 1);
        mcp->close();
    }
    void unavailableCommandsExplainWhy() {
        // With every texture closed, image commands are listed but explain what they need.
        while (window->activeCanvas())
            window->findChild<QAction *>("closeDocumentCommand")->trigger();
        auto entryFor = [&](const QString &title) {
            for (const auto &entry : window->searchIndex().entries)
                if (entry.title == title)
                    return entry;
            return SearchEntry{};
        };
        QCOMPARE(entryFor("Flip Image Horizontally").unavailable, QString("Open a texture to use this command."));
        // Availability mirrors the menu exactly: search never invents its own rules for an action.
        QCOMPARE(entryFor("Undo").unavailable.isEmpty(), window->findChild<QAction *>("editUndo")->isEnabled());
        search("flip image horizontal", [](QDialog *dialog, QLineEdit *edit, QListWidget *) {
            QTest::keyClick(edit, Qt::Key_Return);
            QVERIFY(dialog->isVisible()); // nothing ran
            QVERIFY(dialog->findChild<QLabel *>("searchDetail")->text().contains("Unavailable: Open a texture"));
        });
        if (!evidence.isEmpty())
            search("flip image", [&](QDialog *dialog, QLineEdit *, QListWidget *) {
                QTest::qWait(50);
                QVERIFY(dialog->grab().save(QString("%1/search-unavailable-%2.png").arg(evidence, scaleLabel())));
            });
        // Context changes are reflected the next time Search opens.
        search("ruby_ore", [](QDialog *, QLineEdit *edit, QListWidget *) { QTest::keyClick(edit, Qt::Key_Return); });
        QTRY_VERIFY(window->activeCanvas());
        QVERIFY(entryFor("Flip Image Horizontally").unavailable.isEmpty());
    }
    void shortcutsDoNotConflict() {
        QMultiMap<QString, QString> owners;
        for (auto action : window->findChildren<QAction *>()) {
            if (action->objectName().isEmpty() || action->shortcutContext() == Qt::WidgetShortcut ||
                action->shortcutContext() == Qt::WidgetWithChildrenShortcut)
                continue;
            for (const auto &key : action->shortcuts())
                if (!owners.values(key.toString()).contains(action->objectName()))
                    owners.insert(key.toString(), action->objectName());
        }
        QStringList conflicts;
        for (const auto &key : owners.uniqueKeys())
            if (owners.count(key) > 1)
                conflicts << key + ": " + owners.values(key).join(", ");
        QVERIFY2(conflicts.isEmpty(), qPrintable(conflicts.join("; ")));
        QCOMPARE(owners.values("Ctrl+K"), QStringList{"commandPaletteCommand"});
        QCOMPARE(owners.values("Ctrl+Shift+P"), QStringList{"commandPaletteCommand"});
    }
    void staysResponsiveWithLargeAssetCollections() {
        // The live window index (3,001 assets) is built when Search opens.
        QList<double> builds;
        for (int i = 0; i < 9; ++i) {
            QElapsedTimer timer;
            timer.start();
            const auto index = window->searchIndex();
            builds << timer.nsecsElapsed() / 1e6;
            QVERIFY(index.assets.size() >= 3001);
        }
        // A synthetic 50,000-asset project, queried one keystroke at a time.
        auto index = window->searchIndex();
        index.assets.clear();
        for (int i = 0; i < 50000; ++i)
            index.assets << QString("assets/minecraft/textures/%1/item_%2_%3.png")
                                .arg(i % 7 ? "block" : "item").arg(i).arg(i % 13 ? "plain" : "glowing");
        index.finish();
        QList<double> keystrokes;
        for (const auto &query : {"g", "gl", "glo", "glow", "glowi", "glowing", "glowing 4", "glowing 42"}) {
            QElapsedTimer timer;
            timer.start();
            const auto results = index.query(query);
            keystrokes << timer.nsecsElapsed() / 1e6;
            QVERIFY(!results.isEmpty());
        }
        std::sort(builds.begin(), builds.end());
        std::sort(keystrokes.begin(), keystrokes.end());
        const auto buildMedian = builds[builds.size() / 2], keyMedian = keystrokes[keystrokes.size() / 2];
        qInfo("search index build median %.2f ms (max %.2f); 50k-asset keystroke median %.2f ms (max %.2f)",
              buildMedian, builds.last(), keyMedian, keystrokes.last());
        // One frame at 60 Hz is 16.7 ms. Bounds leave room for a loaded test machine.
        QVERIFY2(buildMedian < 50, qPrintable(QString::number(buildMedian)));
        QVERIFY2(keyMedian < 33, qPrintable(QString::number(keyMedian)));
        if (!evidence.isEmpty()) {
            QFile file(QString("%1/search-performance-%2.json").arg(evidence, scaleLabel()));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QJsonArray b, k;
            for (auto v : builds) b.append(v);
            for (auto v : keystrokes) k.append(v);
            file.write(QJsonDocument(QJsonObject{{"windowAssets", 3001}, {"indexBuildMs", b},
                                                 {"syntheticAssets", 50000}, {"keystrokeMs", k}}).toJson());
        }
    }
    void screenshots() {
        if (evidence.isEmpty())
            QSKIP("Set NOVA_PHASE4G_EVIDENCE to capture screenshots.");
        for (const auto &[query, name] : {std::pair{"minecraft", "minecraft"}, std::pair{"opacity", "opacity"},
                                          std::pair{"mcp", "mcp"}}) {
            search(query, [&, name = QString(name)](QDialog *dialog, QLineEdit *, QListWidget *) {
                QTest::qWait(50);
                QVERIFY(dialog->grab().save(QString("%1/search-%2-%3.png").arg(evidence, name, scaleLabel())));
            });
        }
    }
};

QTEST_MAIN(Phase4gSearchTest)
#include "phase4g_search_test.moc"
