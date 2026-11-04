// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "timeline.h"
#include "minecraft_animation.h"
#include "animation_export.h"
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>
#include <qpa/qwindowsysteminterface.h>
using namespace nova;
class AnimationGuiTest : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QString evidence;
    void capture(QWidget &widget, const QString &name) {
        QTest::qWait(90); QVERIFY(widget.grab().save(evidence + '/' + name + ".png"));
    }
    void trigger(Window &window, const char *name) {
        auto action = window.findChild<QAction *>(name); QVERIFY(action); action->trigger();
    }
    void button(Window &window, const char *name) {
        auto widget = window.findChild<QAbstractButton *>(name); QVERIFY(widget); QVERIFY(widget->isEnabled()); QTest::mouseClick(widget, Qt::LeftButton);
    }
    void modal(const QString &name, std::function<void(QDialog *)> body) {
        auto timer = new QTimer(this); timer->setInterval(20); auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
        connect(timer, &QTimer::timeout, this, [=] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog && dialog->objectName() == name) { timer->stop(); timer->deleteLater(); body(dialog); if (dialog->isVisible()) dialog->reject(); }
            else if (elapsed->elapsed() > 15000) { timer->stop(); timer->deleteLater(); QFAIL("Expected animation modal did not appear"); }
        }); timer->start();
    }
  private slots:
    void restoreNativeAnimationBeforeFirstExport() {
        const auto root = temporary.filePath("Native Only Pack"), path = root + "/unexported.png";
        QDir().mkpath(root);
        QImage image(16, 16, QImage::Format_RGBA8888); image.fill(QColor(12, 34, 56));
        auto source = std::make_shared<Document>(path, image); QVERIFY(source->convertToAnimation());
        QVERIFY(source->editFrame(FrameCommand::Duplicate));
        QVERIFY(source->editLayer({LayerCommand::Add, {}, "Overlay"})); source->draw({1, 1}, {1, 1}, qRgb(120, 140, 160)); source->finish();
        QString error; QVERIFY2(source->saveNative(root, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(path));
        const auto settings = temporary.filePath("native-only-restore.ini");
        Preferences preferences; preferences.projectPath = root; preferences.openDocuments = {path, root + "/missing.png"}; preferences.activeDocument = path;
        QVERIFY(Settings(settings).save(preferences)); qApp->setProperty("settingsFile", settings);
        QElapsedTimer started; started.start(); Window window(root, {}, started); window.show();
        QTRY_VERIFY2(window.activeCanvas(), "A saved native animation must reopen before any Minecraft PNG has been exported");
        auto restored = window.activeCanvas()->document;
        QCOMPARE(restored->animationState().frames.size(), size_t(2));
        QCOMPARE(restored->activeFrame(), source->activeFrame());
        QCOMPARE(restored->layers().size(), size_t(2)); QCOMPARE(restored->image, source->image);
        QCOMPARE(window.findChild<QTabWidget *>("editorTabs")->count(), 1);
        QVERIFY(!QFileInfo::exists(path)); QVERIFY(!restored->modified());
        QVERIFY(window.close());
    }
    void completedFramePreviewCanApply() {
        const auto root = temporary.filePath("Completed Preview Pack"), path = root + "/texture.png";
        QDir().mkpath(root);
        QImage image(16, 16, QImage::Format_RGBA8888); image.fill(QColor(40, 80, 120));
        QVERIFY(savePng(path, image));
        qApp->setProperty("settingsFile", temporary.filePath("completed-preview.ini"));
        QElapsedTimer started; started.start(); Window window(root, {}, started); window.show();
        QTRY_VERIFY(window.activeCanvas());
        trigger(window, "convertAnimation"); button(window, "duplicateFrame");
        auto doc = window.activeCanvas()->document;
        auto timeline = window.findChild<TimelinePanel *>(); QVERIFY(timeline);
        // Include a repeated reference: a source must be processed exactly once.
        timeline->duplicateEntry();
        auto view = timeline->timelineView();
        view->selectionModel()->select(QItemSelection(view->model()->index(0, 0), view->model()->index(2, 0)), QItemSelectionModel::ClearAndSelect);
        const auto before = doc->animationState(); bool applied = false;
        modal("animationScopeDialog", [&](QDialog *dialog) {
            dialog->findChild<QComboBox *>("frameOperationScope")->setCurrentIndex(1);
            for (auto combo : dialog->findChildren<QComboBox *>()) if (combo->findData("invert") >= 0) combo->setCurrentIndex(combo->findData("invert"));
            QPushButton *preview = nullptr, *apply = nullptr;
            for (auto button : dialog->findChildren<QPushButton *>()) {
                if (button->text() == "Preview") preview = button;
                if (button->text() == "Apply") apply = button;
            }
            QVERIFY(preview); QVERIFY(apply); QVERIFY(!apply->isEnabled());
            preview->click();
            QStringList labels; // the dialog's status line says why a preview was not accepted
            for (auto label : dialog->findChildren<QLabel *>()) labels << label->text();
            QVERIFY2(apply->isEnabled(), qPrintable("Successful worker completion must not be treated as user cancellation: " + labels.join(" | ")));
            QCOMPARE(doc->frameComposite(before.frames[0].id), image);
            apply->click(); applied = true;
        }); trigger(window, "animationScope");
        QVERIFY(applied);
        for (const auto &frame : before.frames) QCOMPARE(doc->frameComposite(frame.id).pixelColor(0, 0), QColor(215, 175, 135));
        window.documents().dispatch(doc, Command::Undo);
        for (const auto &frame : before.frames) QCOMPARE(doc->frameComposite(frame.id), image);
        window.documents().dispatch(doc, Command::Redo);
        trigger(window, "saveNativeCommand"); QVERIFY(!doc->modified());
        QVERIFY(window.close());
    }
    void nativeTimelineEditingPlayback() {
        evidence = qEnvironmentVariable("NOVA_C4_EVIDENCE_DIR", temporary.filePath("evidence")); QDir().mkpath(evidence);
        const auto root = temporary.filePath("Animation Pack"), path = root + "/assets/minecraft/textures/block/animation.png";
        QDir().mkpath(QFileInfo(path).absolutePath());
        QImage image(16, 16, QImage::Format_RGBA8888); image.fill(Qt::transparent); QVERIFY(savePng(path, image));
        QFile metadata(root + "/pack.mcmeta"); QVERIFY(metadata.open(QIODevice::WriteOnly));
        metadata.write("{\"pack\":{\"pack_format\":75,\"description\":\"C4 acceptance\"}}"); metadata.close();
        qApp->setProperty("settingsFile", temporary.filePath("settings/studio.ini"));
        QElapsedTimer started; started.start(); Window window(root, {}, started); window.show();
        auto tabs = window.findChild<QTabWidget *>("editorTabs"); QTRY_COMPARE(tabs->count(), 1);
        auto canvas = window.activeCanvas(); auto doc = canvas->document;
        QVERIFY(!window.findChild<TimelinePanel *>());
        trigger(window, "convertAnimation");
        auto timeline = window.findChild<TimelinePanel *>(); QVERIFY(timeline); QVERIFY(doc->animated());
        QTRY_VERIFY(timeline->isVisible());
        const auto first = doc->activeFrame(); window.tools().setForeground(qRgb(230, 65, 100));
        QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->widgetPosition({2.5, 2.5}).toPoint());
        QCOMPARE(doc->image.pixelColor(2, 2), QColor(230, 65, 100));
        button(window, "duplicateFrame"); const auto second = doc->activeFrame(); QVERIFY(first != second);
        window.tools().setForeground(qRgb(65, 180, 250));
        QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->widgetPosition({8.5, 8.5}).toPoint());
        QCOMPARE(doc->frameComposite(first).pixelColor(8, 8), QColor(Qt::transparent));
        button(window, "addFrame"); const auto third = doc->activeFrame();
        QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->widgetPosition({12.5, 5.5}).toPoint());
        auto duration = window.findChild<QSpinBox *>("entryDuration"); duration->setValue(8);
        QCOMPARE(doc->animationState().entries[2].ticks, 8);
        window.findChild<QSpinBox *>("defaultDuration")->setValue(2);
        QCOMPARE(doc->animationState().defaultTicks, 2);
        timeline->duplicateEntry(); QCOMPARE(doc->animationState().entries.size(), size_t(4));
        QCOMPARE(doc->animationState().frames.size(), size_t(3));
        capture(window, "01-animation-workspace");
        timeline->moveEntry(3, 0); QCOMPARE(doc->animationState().entries[0].frame, third);
        capture(*window.findChild<QDockWidget *>("animationDock"), "02-timeline-reordered");
        // Source references survive model drag/drop and source-order changes.
        auto view = timeline->timelineView(); auto model = view->model();
        auto mime = std::unique_ptr<QMimeData>(model->mimeData({model->index(0, 0)}));
        QVERIFY(model->dropMimeData(mime.get(), Qt::MoveAction, 3, 0, {}));
        QCOMPARE(doc->animationState().entries[2].frame, third);
        window.findChild<QCheckBox *>("onionEnabled")->setChecked(true);
        const auto before = doc->image;
        capture(window, "03-onion-skin"); QCOMPARE(doc->image, before);
        auto source = window.findChild<QComboBox *>("sourceFrameSelector"); source->setCurrentIndex(1);
        QCOMPARE(doc->activeFrame(), second); button(window, "addLayer");
        const auto overlay = doc->activeLayer();
        QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->widgetPosition({4.5, 10.5}).toPoint());
        QCOMPARE(doc->layers().size(), size_t(2));
        capture(window, "04-layered-frame");
        // Canvas and cube receive the exact same transient playback image.
        auto cube = static_cast<Cube *>(window.findChild<QOpenGLWidget *>()); QVERIFY(cube);
        QSignalSpy frameSpy(&timeline->playback, &PlaybackController::displayed);
        // Playback pauses by design whenever Nova stops being the active application
        // (playbackPausesWhenNovaIsInactive covers that). Parallel GUI suites take the foreground at
        // random moments, which made the Play/Pause checks below fail intermittently (Phase 4-G,
        // docs/phase-4g/ANIMATION_GUI_INVESTIGATION.md), so this flow ignores foreground changes.
        QVERIFY(QObject::disconnect(qApp, &QGuiApplication::applicationStateChanged, &window, nullptr));
        button(window, "animationPlay"); QVERIFY(timeline->playback.playing()); QTRY_VERIFY(frameSpy.count() > 1);
        QVERIFY(canvas->playbackVisible());
        const auto serial = doc->changeSerial();
        QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->widgetPosition({1.5, 1.5}).toPoint());
        QCOMPARE(doc->changeSerial(), serial);
        capture(window, "05-playback-cube");
        button(window, "animationPlay"); QVERIFY(!timeline->playback.playing()); QVERIFY(!timeline->playback.timerActive());
        button(window, "animationStop"); QVERIFY(!canvas->playbackVisible()); QCOMPARE(doc->activeFrame(), second);
        // Brush/selection context stays in the selected frame.
        QVERIFY(window.documents().selection(doc, DocumentSession::SelectionCommand::All));
        QVERIFY(window.documents().apply(doc, doc->activeLayer(), {"invert", {}}));
        window.documents().dispatch(doc, Command::Undo); QCOMPARE(doc->activeFrame(), second);
        QVERIFY(window.documents().animationEdit(doc, doc->animationState(), "Scope baseline"));
        auto prepared = doc->recoverySnapshot(); std::vector<QUuid> ids{first, third};
        QVERIFY(prepared->applyFrames(ids, {"invert", {}}));
        QVERIFY(window.documents().animationEdit(doc, prepared->animationState(), "Selected Frames Filter"));
        QCOMPARE(doc->frameComposite(first).pixelColor(2, 2), QColor(25, 190, 155));
        // Metadata dialog is a native widget and retains the editable layers.
        QTimer::singleShot(150, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) { capture(*dialog, "06-duration-metadata"); QVERIFY(dialog->findChild<QPlainTextEdit *>("animationRawMetadata")); dialog->reject(); }
        }); trigger(window, "animationMetadata");
        trigger(window, "saveNativeCommand"); QVERIFY(!doc->modified()); QVERIFY(QFileInfo::exists(doc->editablePath()));
        auto reopened = std::make_shared<Document>(path, QImage{}); QString error;
        QVERIFY2(reopened->loadNative(root, &error), qPrintable(error));
        QCOMPARE(reopened->animationState().frames.size(), size_t(3)); QCOMPARE(reopened->activeFrame(), second);
        QCOMPARE(reopened->layers()[1].id, overlay); QCOMPARE(reopened->layers()[1].pixels, doc->layers()[1].pixels);
        modal("animationExportDialog", [&](QDialog *dialog) {
            capture(*dialog, "09-minecraft-export"); auto commit = dialog->findChild<QPushButton *>("commitAnimationExport");
            QVERIFY(commit); QVERIFY(commit->isEnabled()); commit->click();
        }); trigger(window, "exportAnimation");
        QVERIFY(!doc->exportModified()); QVERIFY(QFileInfo::exists(path + ".mcmeta"));
        auto exported = openMinecraftAnimation(path, "26.1", &error); QVERIFY2(exported, qPrintable(error));
        QCOMPARE(exported->animationState().entries.size(), doc->animationState().entries.size());
        auto planned = planAnimationExport(*doc); QVERIFY(planned.sheet.save(evidence + "/10-exported-spritesheet.png"));
        const auto metadataBytes = planned.mcmeta;
        QFile changed(path + ".mcmeta"); QVERIFY(changed.open(QIODevice::WriteOnly)); changed.write("{\"animation\":{\"frametime\":40}}"); changed.close();
        modal("animationExportDialog", [&](QDialog *dialog) {
            QVERIFY(!dialog->findChild<QPushButton *>("commitAnimationExport")->isEnabled());
            QVERIFY(dialog->findChild<QLabel *>("exportDiagnostics")->text().contains("CONFLICT")); capture(*dialog, "11-export-conflict");
        }); trigger(window, "exportAnimation");
        QVERIFY(atomicWrite(path + ".mcmeta", metadataBytes, Revision::read(path + ".mcmeta")));
        window.tools().setForeground(qRgb(250, 200, 50)); QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->widgetPosition({6.5, 6.5}).toPoint());
        auto pendingPlan = planAnimationExport(*doc); auto journal = stageAnimationPair(path, pendingPlan.png, pendingPlan.mcmeta, doc->revision(), doc->mcmetaRevision(), &error);
        QVERIFY2(!journal.isEmpty(), qPrintable(error)); QVERIFY(!finishAnimationPair(journal, PairAction::Retry, &error, [] { return false; }));
        modal("animationExportRecovery", [&](QDialog *dialog) {
            capture(*dialog, "12-export-recovery");
            for (auto b : dialog->findChildren<QPushButton *>()) if (b->text() == "Retry saved pair") { b->click(); break; }
            QVERIFY(pendingAnimationPairs(path).isEmpty()); capture(*dialog, "13-export-recovered");
        }); trigger(window, "recoverAnimationExport");
        QVERIFY(!doc->exportModified()); trigger(window, "saveNativeCommand");
        window.resize(900, 650); capture(window, "07-compact-animation");
        auto dock = window.findChild<QDockWidget *>("animationDock"); dock->setFloating(true); dock->resize(950, 300);
        capture(*dock, "08-floating-timeline"); dock->setFloating(false);
        QVERIFY(window.close());
    }
    // Phase 4-G regression: the mechanism behind the intermittent parallel-run failures. Another
    // application taking the foreground pauses playback, so the next Play/Pause press resumes.
    void playbackPausesWhenNovaIsInactive() {
        const auto root = temporary.filePath("Inactive Playback Pack"), path = root + "/texture.png";
        QDir().mkpath(root);
        QImage image(16, 16, QImage::Format_RGBA8888); image.fill(QColor(90, 30, 60));
        QVERIFY(savePng(path, image));
        qApp->setProperty("settingsFile", temporary.filePath("inactive-playback.ini"));
        QElapsedTimer started; started.start(); Window window(root, {}, started); window.show();
        QTRY_VERIFY(window.activeCanvas());
        trigger(window, "convertAnimation"); button(window, "duplicateFrame");
        auto timeline = window.findChild<TimelinePanel *>(); QVERIFY(timeline);
        // On Windows Qt derives the application state from the focus window (the platform has no
        // ApplicationState capability): no focus window means another application is active.
        auto setState = [&](Qt::ApplicationState state) {
            QWindowSystemInterface::handleFocusWindowChanged(
                state == Qt::ApplicationActive ? window.windowHandle() : nullptr, Qt::ActiveWindowFocusReason);
            QWindowSystemInterface::flushWindowSystemEvents();
            QCOMPARE(QGuiApplication::applicationState(), state);
        };
        setState(Qt::ApplicationInactive);
        setState(Qt::ApplicationActive);
        button(window, "animationPlay"); QVERIFY(timeline->playback.playing()); QVERIFY(timeline->playback.timerActive());
        setState(Qt::ApplicationInactive);
        QVERIFY(!timeline->playback.playing()); QVERIFY(!timeline->playback.timerActive());
        QVERIFY(timeline->playback.previewing()); // paused on the current frame, not stopped
        setState(Qt::ApplicationActive);
        // Exactly the failure signature: the toggle now resumes rather than pauses.
        button(window, "animationPlay"); QVERIFY(timeline->playback.playing());
        button(window, "animationPlay"); QVERIFY(!timeline->playback.playing());
        button(window, "animationStop"); QVERIFY(!timeline->playback.previewing());
        trigger(window, "saveNativeCommand"); QVERIFY(!window.activeCanvas()->document->modified());
        QVERIFY(window.close());
    }
    void onionAlignmentAndExportExclusion() {
        Metrics metrics; QImage image(8, 4, QImage::Format_RGBA8888); image.fill(Qt::transparent);
        auto doc = std::make_shared<Document>("test.png", image); doc->convertToAnimation();
        doc->draw({1, 1}, {1, 1}, qRgb(255, 0, 0)); doc->finish(); doc->editFrame(FrameCommand::Add);
        Canvas canvas(doc, &metrics); canvas.resize(500, 320); canvas.zoom = 24; canvas.grid = false; canvas.show(); QTest::qWait(100);
        auto point = canvas.widgetPosition({1.5, 1.5}).toPoint(); auto base = canvas.grab().toImage();
        canvas.onion = {true, 1, 0, 50, false}; auto overlay = canvas.grab().toImage();
        const auto sample = (QPointF(point) * base.devicePixelRatio()).toPoint();
        QVERIFY(base.pixelColor(sample) != overlay.pixelColor(sample));
        QCOMPARE(doc->image.pixelColor(1, 1), QColor(Qt::transparent));
        auto plan = planAnimationExport(*doc); QVERIFY(plan.ok()); QCOMPARE(plan.sheet.pixelColor(1, 5), QColor(Qt::transparent));
        QCOMPARE(canvas.pixelPosition(point).toPoint(), QPoint(2, 2));
        canvas.hide();
    }
};
QTEST_MAIN(AnimationGuiTest)
#include "animation_gui_test.moc"
