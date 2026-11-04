// SPDX-License-Identifier: GPL-3.0-or-later
// Native-widget workflow tests for the Phase 2-C3 editor. Pointer input is
// delivered to the real canvas; results are verified on document pixels.
#include "batchdialog.h"
#include "canvas.h"
#include "opdialog.h"
#include "panels.h"
#include "recovery.h"
#include <QProcess>
#include <QScopeGuard>
#include "window.h"
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>

namespace {
void mouse(QWidget *w, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
           Qt::KeyboardModifiers mods = {}) {
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, mods);
    QApplication::sendEvent(w, &e);
}
QPointF at(nova::Canvas *c, double x, double y) { return c->widgetPosition(QPointF(x + .5, y + .5)); }
void drag(nova::Canvas *c, QPointF from, QPointF to, Qt::KeyboardModifiers mods = {}, int steps = 6) {
    mouse(c, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, mods);
    for (int i = 1; i <= steps; ++i)
        mouse(c, QEvent::MouseMove, from + (to - from) * (double(i) / steps), Qt::NoButton, Qt::LeftButton, mods);
    mouse(c, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, mods);
}
void click(nova::Canvas *c, QPointF p, Qt::KeyboardModifiers mods = {}) {
    mouse(c, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, mods);
    mouse(c, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, mods);
}
void key(QWidget *w, int k, Qt::KeyboardModifiers mods = {}) { QTest::keyClick(w, Qt::Key(k), mods); }
void trigger(QWidget &window, const char *name) {
    auto a = window.findChild<QAction *>(name);
    QVERIFY2(a, name);
    a->trigger();
}
// Runs `body` against the next modal dialog, then lets the caller continue.
void onModal(std::function<void(QWidget *)> body) {
    QTimer::singleShot(80, [body] {
        auto w = QApplication::activeModalWidget();
        if (w) {
            QPointer<QDialog> dialog = qobject_cast<QDialog *>(w);
            body(w);
            // A failed assertion inside the callback must not strand exec().
            if (dialog && dialog->isVisible())
                dialog->reject();
        }
    });
}
} // namespace

class EditorGuiTests : public QObject {
    Q_OBJECT
    QString evidence;
    QTemporaryDir tmp;
    void capture(QWidget &w, const QString &name) {
        QTest::qWait(120);
        QVERIFY(w.grab().save(evidence + "/" + name + ".png"));
    }
  private slots:
    void initTestCase() {
        evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        if (evidence.isEmpty())
            evidence = tmp.path() + "/evidence";
        QDir().mkpath(evidence);
    }
    void brushPresetsRetainStampPixels() {
        nova::ToolState tools;
        auto &brush = tools.brush(nova::Tool::Brush);
        brush.shape = nova::ops::BrushShape::Custom;
        brush.size = 7;
        brush.height = 11;
        brush.stamp = QImage(3, 2, QImage::Format_RGBA8888);
        brush.stamp.fill(Qt::transparent);
        brush.stamp.setPixel(1, 0, qRgba(20, 50, 90, 128));
        brush.stamp.setPixel(2, 1, qRgba(240, 80, 50, 255));
        const auto expected = nova::ops::footprint(brush);
        const auto path = tmp.path() + "/stamp-presets.json";
        nova::PresetStore presets(path);
        presets.put({"brush", "Tall stamp", {}, tools.brushPreset(nova::Tool::Brush)});
        QVERIFY(presets.save());
        nova::PresetStore reopened(path);
        QVERIFY(reopened.load());
        QVERIFY(reopened.find("brush", "Tall stamp"));
        nova::ToolState restored;
        restored.applyBrushPreset(nova::Tool::Brush, reopened.find("brush", "Tall stamp")->data);
        const auto loaded = restored.brush(nova::Tool::Brush);
        QCOMPARE(loaded.stamp, brush.stamp);
        QCOMPARE(int(loaded.shape), int(nova::ops::BrushShape::Custom));
        QCOMPARE(loaded.size, 7);
        QCOMPARE(loaded.height, 11);
        const auto actual = nova::ops::footprint(loaded);
        QCOMPARE(actual.size(), expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            QCOMPARE(actual[i].offset, expected[i].offset);
            QCOMPARE(actual[i].coverage, expected[i].coverage);
        }
        restored.applyBrushPreset(nova::Tool::Brush, {{"schema", 1}, {"size", 5}, {"shape", 1}});
        QCOMPARE(restored.brush(nova::Tool::Brush).height, 0); // schema-1 default preserved
        restored.applyBrushPreset(nova::Tool::Brush, {{"shape", 5}, {"stamp_png", "invalid"}});
        QCOMPARE(int(restored.brush(nova::Tool::Brush).shape), int(nova::ops::BrushShape::Square));
        // Size metadata is checked before decoding potentially large PNG data.
        QImage large(257, 1, QImage::Format_RGBA8888);
        large.fill(Qt::black);
        QByteArray encoded;
        QBuffer buffer(&encoded);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(large.save(&buffer, "PNG"));
        restored.applyBrushPreset(nova::Tool::Brush, {{"shape", 5}, {"stamp_png", QString::fromLatin1(encoded.toBase64())}});
        QVERIFY(restored.brush(nova::Tool::Brush).stamp.isNull());
        // High-entropy stamps exceed the old settings limit; settings must keep
        // complete JSON rather than truncating it in the middle of base64 data.
        brush.stamp = QImage(256, 256, QImage::Format_RGBA8888);
        nova::ops::Random random(42);
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x)
                brush.stamp.setPixel(x, y, QRgb(random.next()) | 0xff000000u);
        nova::Preferences preferences;
        preferences.toolState = QJsonDocument(tools.brushPreset(nova::Tool::Brush)).toJson();
        QVERIFY(preferences.toolState.size() > 64 * 1024);
        nova::Settings settings(tmp.path() + "/stamp-settings.ini");
        QVERIFY(settings.save(preferences));
        QCOMPARE(settings.load().toolState, preferences.toolState);
        restored.applyBrushPreset(nova::Tool::Brush, QJsonDocument::fromJson(settings.load().toolState).object());
        QCOMPARE(restored.brush(nova::Tool::Brush).stamp, brush.stamp);
        const auto savedRevision = nova::Revision::read(path);
        presets.put({"brush", "Oversize", {}, {{"value", QString(8 * 1024 * 1024, 'x')}}});
        QString error;
        QVERIFY(!presets.save(&error));
        QVERIFY(savedRevision.sameContent(nova::Revision::read(path)));
    }
    void smoothShapeAndRectangularBrushWorkflow() {
        const auto root = tmp.path() + "/Smooth Pack", path = root + "/assets/minecraft/textures/block/test.png";
        QDir().mkpath(QFileInfo(path).absolutePath());
        QImage blank(16, 16, QImage::Format_RGBA8888);
        blank.fill(Qt::transparent);
        QVERIFY(nova::savePng(path, blank));
        qApp->setProperty("settingsFile", tmp.path() + "/smooth-prefs/studio.ini");
        QElapsedTimer time;
        time.start();
        nova::Window window(root, {}, time);
        window.resize(1440, 940);
        window.show();
        auto tabs = window.findChild<QTabWidget *>("editorTabs");
        QTRY_VERIFY(tabs->count() >= 1);
        auto canvas = static_cast<nova::Canvas *>(tabs->currentWidget());
        auto doc = canvas->document;
        auto &tools = window.tools();
        canvas->zoom = 24;
        tools.setTool(nova::Tool::Brush);
        auto size = window.findChild<QSpinBox *>("brushSize");
        auto height = window.findChild<QSpinBox *>("brushHeight");
        QTRY_VERIFY(size && height);
        size->setValue(3);
        height->setValue(7);
        window.findChild<QComboBox *>("brushShape")->setCurrentIndex(2); // diamond
        tools.setForeground(qRgb(200, 100, 50));
        click(canvas, at(canvas, 3, 4));
        const auto brushed = doc->image;
        QCOMPARE(qAlpha(brushed.pixel(3, 1)), 255);
        QCOMPARE(qAlpha(brushed.pixel(3, 7)), 255);
        QCOMPARE(qAlpha(brushed.pixel(1, 4)), 0);
        capture(window, "c3-16-rectangular-brush");
        tools.setTool(nova::Tool::Shape);
        auto smooth = window.findChild<QCheckBox *>("shapeSmooth");
        QVERIFY(smooth);
        QVERIFY(!smooth->isChecked());
        smooth->setChecked(true);
        window.findChild<QComboBox *>("shapeKind")->setCurrentIndex(2);
        window.findChild<QCheckBox *>("shapeFilled")->setChecked(true);
        tools.setForeground(qRgba(50, 180, 220, 192));
        mouse(canvas, QEvent::MouseButtonPress, at(canvas, 6, 3), Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, at(canvas, 13, 12), Qt::NoButton, Qt::LeftButton);
        QCOMPARE(doc->image, brushed); // preview never mutates source
        const auto preview = canvas->displayed();
        QVERIFY(preview != brushed);
        capture(window, "c3-17-smooth-shape-preview");
        canvas->cancelInteraction();
        QCOMPARE(canvas->displayed(), brushed);
        drag(canvas, at(canvas, 6, 3), at(canvas, 13, 12));
        QCOMPARE(doc->image, preview);
        trigger(window, "editUndo");
        QCOMPARE(doc->image, brushed);
        trigger(window, "editRedo");
        QCOMPARE(doc->image, preview);
        trigger(window, "saveCommand");
        QVERIFY(!doc->modified());
        QCOMPARE(nova::loadPng(path), doc->image);
        capture(window, "c3-18-smooth-shape-committed");
        QVERIFY(window.documents().select(doc, nova::ops::rectMask({16, 16}, {4, 4, 4, 4}), nova::ops::Combine::Replace));
        nova::PresetStore presets;
        {
            nova::OperationDialog dialog(&window.documents(), doc, canvas, &presets, {}, {"gen_bricks"}, {}, &window);
            dialog.show();
            auto apply = dialog.findChild<QPushButton *>("operationApply");
            QTRY_VERIFY(apply->isEnabled());
            const auto before = doc->image;
            QString error;
            QVERIFY2(dialog.applyOperation(&error), qPrintable(error));
            QCOMPARE(doc->layers().size(), size_t(2));
            const auto layer = doc->layer(doc->activeLayer());
            QVERIFY(layer);
            QCOMPARE(qAlpha(layer->pixels.pixel(5, 5)), 255);
            QCOMPARE(layer->pixels.pixel(3, 3), QRgb(0));
            QCOMPARE(doc->layers()[0].pixels, before);
            dialog.close();
        }
        trigger(window, "editUndo");
        QCOMPARE(doc->image, preview);
        QVERIFY(window.documents().select(doc, nova::ops::emptyMask({16, 16}), nova::ops::Combine::Replace));
        tools.setTool(nova::Tool::Move);
        drag(canvas, at(canvas, 3, 3), at(canvas, 7, 7));
        QVERIFY(!canvas->hasFloating());
        QCOMPARE(doc->image, preview);
        QApplication::clipboard()->setText("Empty selection sentinel");
        trigger(window, "editCopy");
        QCOMPARE(QApplication::clipboard()->text(), QString("Empty selection sentinel"));
        const auto layers = doc->layers().size();
        trigger(window, "editDuplicateSelection");
        QCOMPARE(doc->layers().size(), layers);
        trigger(window, "saveCommand");
        QVERIFY(window.close());
    }
    void editorWorkflow() {
        const auto root = tmp.path() + "/Editor Pack", path = root + "/assets/minecraft/textures/block/stone.png",
                   second = root + "/assets/minecraft/textures/block/brick.png";
        QDir().mkpath(QFileInfo(path).absolutePath());
        QImage base(16, 16, QImage::Format_RGBA8888);
        base.fill(Qt::transparent);
        QVERIFY(nova::savePng(path, base));
        QVERIFY(nova::savePng(second, base));
        QFile meta(root + "/pack.mcmeta");
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write("{\"pack\":{\"pack_format\":34,\"description\":\"Editor\"}}");
        meta.close();
        qApp->setProperty("settingsFile", tmp.path() + "/prefs/studio.ini");
        QElapsedTimer time;
        time.start();
        nova::Window window(root, {}, time);
        window.resize(1400, 900);
        window.show();
        auto tabs = window.findChild<QTabWidget *>("editorTabs");
        QTRY_VERIFY(tabs->count() >= 1);
        auto canvas = static_cast<nova::Canvas *>(tabs->currentWidget());
        auto doc = canvas->document;
        auto &tools = window.tools();
        canvas->zoom = 24;
        canvas->update();
        QTest::qWait(50);
        // 1. Procedural stone fills the texture through the generator dialog (replace layer).
        onModal([](QWidget *d) {
            d->findChild<QComboBox *>("generatorOutput")->setCurrentIndex(1);
            QTRY_VERIFY_WITH_TIMEOUT(d->findChild<QPushButton *>("operationApply")->isEnabled(), 10000);
            d->findChild<QPushButton *>("operationApply")->click();
        });
        trigger(window, "op-gen_stone");
        QTRY_VERIFY(qAlpha(doc->image.pixel(5, 5)) == 255);
        const auto stone = doc->image;
        capture(window, "c3-01-generated-stone");
        // 2. Rectangle selection with the mouse, then drawing is clipped to it.
        window.findChild<QAction *>("tool-rect-select")->trigger();
        QCOMPARE(int(tools.tool), int(nova::Tool::RectSelect));
        trigger(window, "saveCommand"); // plain PNG save: content clean before selecting
        QVERIFY(!doc->modified());
        drag(canvas, at(canvas, 2, 2), at(canvas, 7, 7));
        QVERIFY(doc->hasSelection());
        QCOMPARE(nova::ops::maskBounds(doc->selection()), QRect(2, 2, 6, 6));
        QVERIFY(!doc->modified()); // selection alone never dirties content
        window.findChild<QAction *>("tool-pencil")->trigger();
        tools.setForeground(qRgb(255, 0, 0));
        drag(canvas, at(canvas, 0, 4), at(canvas, 15, 4));
        QCOMPARE(doc->image.pixel(4, 4), qRgb(255, 0, 0));
        QCOMPARE(doc->image.pixel(10, 4), stone.pixel(10, 4)); // clipped
        capture(window, "c3-02-selection-clipped-stroke");
        // 3. Copy, paste as floating placement, nudge, commit.
        trigger(window, "editCopy");
        trigger(window, "editPaste");
        QVERIFY(canvas->hasFloating());
        QCOMPARE(int(tools.tool), int(nova::Tool::Move));
        canvas->setFocus();
        for (int i = 0; i < 8; ++i)
            key(canvas, Qt::Key_Right);
        QCOMPARE(doc->image.pixel(12, 4), stone.pixel(12, 4)); // not committed yet
        capture(window, "c3-03-floating-paste");
        key(canvas, Qt::Key_Return);
        QVERIFY(!canvas->hasFloating());
        QCOMPARE(doc->image.pixel(12, 4), qRgb(255, 0, 0));
        // 4. Grid-snapped rectangle selection, stamp definition, magic wand.
        trigger(window, "selectNone");
        QVERIFY(!doc->hasSelection());
        window.findChild<QAction *>("tool-rect-select")->trigger();
        tools.snap = 4;
        drag(canvas, at(canvas, 1, 1), at(canvas, 5, 5));
        QCOMPARE(nova::ops::maskBounds(doc->selection()), QRect(0, 0, 8, 8));
        tools.snap = 1;
        trigger(window, "defineStamp");
        QCOMPARE(int(tools.brush(nova::Tool::Brush).shape), int(nova::ops::BrushShape::Custom));
        QCOMPARE(tools.brush(nova::Tool::Brush).stamp.size(), QSize(8, 8));
        tools.brush(nova::Tool::Brush).shape = nova::ops::BrushShape::Square;
        trigger(window, "selectNone");
        window.findChild<QAction *>("tool-magic-wand")->trigger();
        click(canvas, at(canvas, 4, 4));
        QVERIFY(doc->hasSelection());
        QCOMPARE(doc->selection().constScanLine(4)[4], uchar(255));
        // 5. Lasso replaces the selection; ellipse selection subtracts with Alt.
        window.findChild<QAction *>("tool-lasso")->trigger();
        mouse(canvas, QEvent::MouseButtonPress, at(canvas, 1, 1), Qt::LeftButton, Qt::LeftButton);
        for (auto p : {QPointF(12, 1), QPointF(12, 12), QPointF(1, 12)})
            mouse(canvas, QEvent::MouseMove, canvas->widgetPosition(p), Qt::NoButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, at(canvas, 1, 12), Qt::LeftButton, Qt::NoButton);
        QVERIFY(nova::ops::maskBounds(doc->selection()).contains(QRect(2, 2, 9, 9)));
        window.findChild<QAction *>("tool-ellipse-select")->trigger();
        drag(canvas, at(canvas, 4, 4), at(canvas, 8, 8), Qt::AltModifier);
        QCOMPARE(doc->selection().constScanLine(6)[6], uchar(0));
        QCOMPARE(doc->selection().constScanLine(2)[2], uchar(255));
        capture(window, "c3-04-lasso-minus-ellipse");
        trigger(window, "selectNone");
        // 6. Brush with a circle footprint and symmetry; shape tool draws one undo step.
        window.findChild<QAction *>("tool-brush")->trigger();
        tools.brush(nova::Tool::Brush).size = 3;
        tools.brush(nova::Tool::Brush).shape = nova::ops::BrushShape::Diamond;
        tools.symmetry = nova::ops::Symmetry::Horizontal;
        tools.setForeground(qRgb(0, 200, 255));
        click(canvas, at(canvas, 2, 12));
        QCOMPARE(doc->image.pixel(2, 12), qRgb(0, 200, 255));
        QCOMPARE(doc->image.pixel(13, 12), qRgb(0, 200, 255)); // mirrored
        tools.symmetry = nova::ops::Symmetry::None;
        const int before = doc->historyOperations();
        window.findChild<QAction *>("tool-shape")->trigger();
        tools.shape = nova::ops::ShapeKind::Ellipse;
        tools.filled = true;
        tools.setForeground(qRgb(250, 250, 0));
        drag(canvas, at(canvas, 9, 9), at(canvas, 14, 14));
        QCOMPARE(doc->image.pixel(11, 11), qRgb(250, 250, 0));
        QCOMPARE(doc->historyOperations(), before + 1);
        capture(window, "c3-05-brush-symmetry-shape");
        // 7. Fill bucket (global) and eyedropper.
        window.findChild<QAction *>("tool-eyedropper")->trigger();
        click(canvas, at(canvas, 11, 11));
        QCOMPARE(tools.foreground, qRgb(250, 250, 0));
        window.findChild<QAction *>("tool-fill")->trigger();
        tools.setForeground(qRgb(10, 120, 40));
        tools.contiguous = true;
        tools.tolerance = 0;
        click(canvas, at(canvas, 11, 11));
        QCOMPARE(doc->image.pixel(11, 11), qRgb(10, 120, 40));
        // 8. Transform the selected pixels: scale 200% then apply as one step.
        window.findChild<QAction *>("tool-rect-select")->trigger();
        drag(canvas, at(canvas, 10, 10), at(canvas, 11, 11));
        window.findChild<QAction *>("tool-transform")->trigger();
        canvas->setFocus();
        key(canvas, Qt::Key_Left); // lift + nudge
        QVERIFY(canvas->hasFloating());
        tools.scaleX = tools.scaleY = 200;
        emit tools.transformChanged();
        QCOMPARE(canvas->floatingState().cached.size(), QSize(4, 4));
        capture(window, "c3-06-transform-preview");
        const int ops = doc->historyOperations();
        emit tools.commitRequested();
        QVERIFY(!canvas->hasFloating());
        QCOMPARE(doc->historyOperations(), ops + 1);
        trigger(window, "selectNone");
        // 9. Rotate the image and undo it.
        const auto beforeRotate = doc->image;
        trigger(window, "imageRotateCW");
        QCOMPARE(doc->image.pixel(15, 0), beforeRotate.pixel(0, 0));
        trigger(window, "editUndo");
        QCOMPARE(doc->image, beforeRotate);
        // 10. HSL via the colour panel hex field.
        auto hex = window.findChild<QLineEdit *>("colorHex");
        hex->setText("#FF3366CC");
        emit hex->editingFinished();
        QCOMPARE(tools.foreground, qRgba(0x33, 0x66, 0xCC, 0xFF));
        auto palette = window.findChild<nova::ColorPanel *>();
        QVERIFY(palette->createPalette("Test palette", {qRgb(0, 0, 0), qRgb(255, 255, 255)}));
        QCOMPARE(palette->activePaletteName(), QString("Test palette"));
        QCOMPARE(tools.palette.size(), size_t(2));
        capture(window, "c3-07-colour-panel");
        // 11. Blur through the shared operation dialog with live preview.
        const auto beforeBlur = doc->image;
        onModal([this](QWidget *d) {
            QTRY_VERIFY_WITH_TIMEOUT(d->findChild<QPushButton *>("operationApply")->isEnabled(), 10000);
            QVERIFY(d->grab().save(evidence + "/c3-08-blur-dialog.png"));
            d->findChild<QPushButton *>("operationApply")->click();
        });
        trigger(window, "op-gaussian_blur");
        QVERIFY(doc->image != beforeBlur);
        QCOMPARE(doc->historyLabels().last(), QString("Gaussian Blur"));
        // 12. Directional shading creates a separate shading layer.
        const int layers = int(doc->layers().size());
        onModal([](QWidget *d) {
            QTRY_VERIFY_WITH_TIMEOUT(d->findChild<QPushButton *>("operationApply")->isEnabled(), 10000);
            d->findChild<QPushButton *>("operationApply")->click();
        });
        trigger(window, "op-directional_shading");
        QCOMPARE(int(doc->layers().size()), layers + 1);
        capture(window, "c3-09-shading-layer");
        // 13. History panel jumps back two steps; command palette finds and runs an action.
        auto history = window.findChild<QListWidget *>("historyList");
        QTRY_VERIFY(history->count() == doc->historyOperations() + 1);
        history->setCurrentRow(history->count() - 3);
        QCOMPARE(doc->historyCursor(), doc->historyOperations() - 2);
        history->setCurrentRow(history->count() - 1);
        onModal([this](QWidget *d) {
            auto q = d->findChild<QLineEdit *>("commandQuery");
            QTest::keyClicks(q, "flip image hor");
            QTest::qWait(50);
            QVERIFY(d->grab().save(evidence + "/c3-10-command-palette.png"));
            QTest::keyClick(q, Qt::Key_Return);
        });
        const auto beforeFlip = doc->image;
        trigger(window, "commandPaletteCommand");
        QCOMPARE(doc->image.pixel(0, 5), beforeFlip.pixel(15, 5));
        // 14. Save native, export PNG, reopen to verify exact pixels.
        trigger(window, "saveNativeCommand");
        trigger(window, "exportPngCommand");
        const auto exported = nova::loadPng(doc->path);
        QCOMPARE(exported, doc->image);
        // 15. Workspace layout persistence for the new docks.
        auto colors = window.findChild<QDockWidget *>("colors");
        colors->setFloating(true);
        onModal([](QWidget *d) {
            auto input = qobject_cast<QInputDialog *>(d);
            input->setTextValue("Floating colours");
            input->accept();
        });
        trigger(window, "saveLayoutCommand");
        capture(window, "c3-11-workspace");
        QTimer::singleShot(50, [] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Discard)->click();
        });
        QVERIFY(window.close());
        QSettings settings(tmp.path() + "/prefs/studio.ini", QSettings::IniFormat);
        settings.beginGroup("layouts");
        QCOMPARE(settings.childKeys().size(), 1);
        settings.endGroup();
        const auto prefs = nova::Settings(tmp.path() + "/prefs/studio.ini").load();
        QVERIFY(prefs.layouts.contains("Floating colours"));
        QVERIFY(!prefs.toolState.isEmpty());
        QCOMPARE(prefs.activePalette, QString("Test palette"));
        QVERIFY(!prefs.recentColors.isEmpty());
    }
    void batchWorkflow() {
        const auto root = tmp.path() + "/Batch Pack";
        QStringList made;
        for (int i = 0; i < 120; ++i) {
            const auto rel = QString("assets/minecraft/textures/block/stone_%1.png").arg(i, 3, 10, QChar('0'));
            QDir().mkpath(QFileInfo(root + '/' + rel).absolutePath());
            QImage im(64, 64, QImage::Format_RGBA8888);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                    im.setPixel(x, y, qRgb((x * 3 + i) % 256, (y * 5) % 256, (x + y + i) % 256));
            QVERIFY(nova::savePng(root + '/' + rel, im));
            made.append(rel);
        }
        nova::PresetStore presets(tmp.path() + "/batch-presets.json");
        nova::BatchContext context;
        context.root = root;
        context.files = made;
        context.directory = "assets/minecraft/textures/block";
        context.presets = &presets;
        const auto original = nova::Revision::read(root + '/' + made[0]).hash;
        // Quick job: three-step pipeline over the folder, previewed, dry-run and executed.
        {
            nova::BatchDialog dialog(context);
            dialog.show();
            dialog.setScope(nova::batch::Scope::Folder, "assets/minecraft/textures/block");
            nova::batch::Pipeline p;
            p.steps = {{"hue_saturation", {{"hue", 40}}}, {"directional_shading", {{"output", "Modify pixels"}}},
                       {"quantize", {{"colors", 12}}}};
            dialog.setPipeline(p);
            dialog.findChild<QPushButton *>("batchPreview")->click();
            QTest::qWait(500);
            QVERIFY(dialog.runDryRun());
            QCOMPARE(dialog.plan().ready, 120);
            QVERIFY(dialog.grab().save(evidence + "/c3-12-batch-dry-run.png"));
            QSignalSpy done(&dialog, &nova::BatchDialog::finished);
            QVERIFY(dialog.start());
            QVERIFY(done.wait(60000));
            const auto report = done.first().first().value<nova::batch::Report>();
            QCOMPARE(report.done, 120);
            QVERIFY(QFileInfo::exists(report.manifest));
            QVERIFY(dialog.grab().save(evidence + "/c3-13-batch-complete.png"));
        }
        QCOMPARE(nova::Revision::read(root + '/' + made[0]).hash, original); // sources untouched
        // Long job canceled mid-way while measuring GUI event-loop responsiveness.
        {
            nova::BatchDialog dialog(context);
            dialog.show();
            dialog.setScope(nova::batch::Scope::Pack, {});
            nova::batch::Pipeline heavy;
            heavy.steps = {{"resize_image", {{"width", 512}, {"height", 512}}}, {"gaussian_blur", {{"sigma", 6.0}}},
                           {"median", {{"radius", 2}}}};
            dialog.setPipeline(heavy);
            dialog.findChild<QLineEdit *>("batchDirectory")->setText(tmp.path() + "/heavy-out");
            QVERIFY(dialog.runDryRun());
            QSignalSpy done(&dialog, &nova::BatchDialog::finished);
            QVERIFY(dialog.start());
            QElapsedTimer gap;
            gap.start();
            qint64 worst = 0;
            QTimer tick;
            QObject::connect(&tick, &QTimer::timeout, [&] {
                worst = std::max(worst, gap.restart());
            });
            tick.start(10);
            QTest::qWait(700);
            dialog.findChild<QPushButton *>("batchCancel")->click();
            QVERIFY(done.wait(60000));
            tick.stop();
            const auto report = done.first().first().value<nova::batch::Report>();
            QVERIFY2(report.canceled > 0, "cancellation should stop remaining items");
            QVERIFY(report.done < 120);
            qInfo() << "batch-responsiveness worst-gap-ms" << worst << "done" << report.done << "canceled"
                    << report.canceled;
#ifdef NDEBUG
            const qint64 limit = 500; // Release
#else
            const qint64 limit = 1500; // unoptimised Debug build
#endif
            QVERIFY2(worst < limit, qPrintable(QString("GUI event loop stalled for %1 ms").arg(worst)));
            QVERIFY(dialog.grab().save(evidence + "/c3-14-batch-canceled.png"));
        }
    }
    void recoveryDecisionKeepsOrDiscardsSnapshot_data() {
        QTest::addColumn<QString>("decision");
        QTest::newRow("later") << QString("Later");
        QTest::newRow("restore-fails") << QString("Restore");
        QTest::newRow("explicit-discard") << QString("Discard snapshot");
        QTest::newRow("shutdown-during-prompt") << QString("Shutdown");
    }
    void recoveryDecisionKeepsOrDiscardsSnapshot() {
        QFETCH(QString, decision);
        QTemporaryDir fixture;
        const auto root = fixture.filePath("pack");
        const auto path = root + "/assets/minecraft/textures/block/a.png";
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QImage pixels(16, 16, QImage::Format_RGBA8888);
        pixels.fill(Qt::transparent);
        QVERIFY(nova::savePng(path, pixels));
        nova::Document seed(path, pixels);
        QVERIFY(seed.editLayer({nova::LayerCommand::Add, {}, "Snapshot ink"}));
        QVERIFY(seed.saveNative(root));
        seed.draw({1, 1}, {1, 1}, qRgb(10, 20, 30));
        seed.finish();
        const auto settings = fixture.filePath("settings/studio.ini");
        const nova::RecoveryStore store(fixture.filePath("settings/recovery"));
        QVERIFY(store.writeDocument(seed, root));
        const auto snapshot = store.pathFor(seed.id());
        const auto before = nova::Revision::read(snapshot);
        auto expected = before;

        const auto oldSettings = qApp->property("settingsFile");
        qApp->setProperty("settingsFile", settings);
        const auto resetSettings = qScopeGuard([&] { qApp->setProperty("settingsFile", oldSettings); });
        QElapsedTimer time;
        time.start();
        nova::Window window(root, {}, time);
        window.show();
        auto timer = window.findChild<QTimer *>("recoveryAndRevision");
        QVERIFY(timer);
        QTimer drive;
        bool matchingDocument = false, handled = false, failureShown = false, closed = false;
        connect(&drive, &QTimer::timeout, this, [&] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            if (box->windowTitle() == "Recovery unavailable") {
                failureShown = true;
                box->accept();
                return;
            }
            if (box->objectName() != "recoveryDialog" || handled || window.documents().all().empty())
                return;
            matchingDocument = window.documents().all().front()->id() == seed.id();
            QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
            handled = true;
            if (decision == "Shutdown") {
                closed = window.close();
                box->reject(); // unwind the nested dialog after dispatching the close event
                return;
            }
            if (decision == "Restore") {
                // Make a discovered snapshot fail at the subsequent load.
                QFile corrupt(snapshot);
                if (corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    corrupt.write("invalid recovery payload");
                corrupt.close();
                expected = nova::Revision::read(snapshot);
            }
            for (auto button : box->buttons())
                if (button->text() == decision) {
                    button->click();
                    return;
                }
            box->reject();
        });
        drive.start(20);
        QTRY_VERIFY_WITH_TIMEOUT(handled, 10000);
        drive.stop();
        QVERIFY(matchingDocument);
        if (decision == "Restore") {
            QVERIFY(failureShown);
            QVERIFY(!before.sameContent(expected));
        }
        if (decision == "Shutdown")
            QVERIFY(closed);
        else
            QVERIFY(window.close());
        if (decision == "Discard snapshot")
            QVERIFY(!QFileInfo::exists(snapshot));
        else
            QVERIFY(expected.sameContent(nova::Revision::read(snapshot)));
    }
    void recoveryAfterForcedTermination() {
        // C3 edits (generator, resize, blur, shading layer, selection) in a real child
        // process, forced termination after the background snapshot, then restore.
        const auto root = tmp.path() + "/Recovery Pack", path = root + "/assets/minecraft/textures/block/a.png";
        QDir().mkpath(QFileInfo(path).absolutePath());
        QImage base(16, 16, QImage::Format_RGBA8888);
        base.fill(Qt::transparent);
        QVERIFY(nova::savePng(path, base));
        const auto settings = tmp.path() + "/crash/studio.ini";
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--editor-child", root, settings});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForReadyRead(30000));
        QCOMPARE(child.readAllStandardOutput().trimmed(), QByteArray("snapshot-ready"));
        child.kill();
        QVERIFY(child.waitForFinished());
        nova::RecoveryStore store(QFileInfo(settings).absolutePath() + "/recovery");
        const auto entries = store.entries();
        QCOMPARE(entries.size(), 1);
        const auto entry = store.load(entries[0].id);
        nova::Document expected(entry.path, QImage{});
        QVERIFY(expected.decodeNative(entry.nativeData, "recovery.png"));
        QCOMPARE(expected.image.size(), QSize(32, 32));
        QCOMPARE(int(expected.layers().size()), 2);
        QVERIFY(expected.layers()[1].extensions.contains("nova.effect"));
        QVERIFY(!expected.hasSelection()); // selections are never part of recovery
        const auto disk = nova::Revision::read(path);
        qApp->setProperty("settingsFile", settings);
        const auto snapshot = nova::Revision::read(entry.file);
        bool survivedTimer = false;
        QTimer restore;
        restore.setInterval(50);
        connect(&restore, &QTimer::timeout, this, [&] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || box->objectName() != "recoveryDialog")
                return;
            auto window = box->parentWidget();
            auto tabs = window->findChild<QTabWidget *>("editorTabs");
            if (!tabs || tabs->count() == 0)
                return; // exercise cleanup against the reopened clean document
            auto timer = window->findChild<QTimer *>("recoveryAndRevision");
            QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
            survivedTimer = snapshot.sameContent(nova::Revision::read(entry.file));
            restore.stop();
            for (auto b : box->buttons())
                if (b->text() == "Restore")
                    b->click();
        });
        restore.start();
        QElapsedTimer time;
        time.start();
        nova::Window recovered(root, {}, time);
        recovered.show();
        auto tabs = recovered.findChild<QTabWidget *>("editorTabs");
        QTRY_VERIFY_WITH_TIMEOUT(!restore.isActive(), 15000);
        QVERIFY(survivedTimer);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 5000);
        auto doc = static_cast<nova::Canvas *>(tabs->currentWidget())->document;
        QTRY_VERIFY_WITH_TIMEOUT(doc->modified(), 5000);
        QCOMPARE(doc->image, expected.image);
        QVERIFY(disk.sameContent(nova::Revision::read(path))); // original PNG untouched
        capture(recovered, "c3-15-recovered-after-kill");
        QTimer::singleShot(50, [] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Discard)->click();
        });
        QVERIFY(recovered.close());
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setApplicationName("Nova Editor Acceptance");
    if (app.arguments().contains("--editor-child")) {
        const auto args = app.arguments();
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
            edit.stop();
            auto canvas = static_cast<nova::Canvas *>(tabs->currentWidget());
            auto doc = canvas->document;
            auto &session = window.documents();
            session.apply(doc, doc->activeLayer(), {"gen_bricks", {{"width", 16}, {"height", 16}}});
            session.apply(doc, doc->activeLayer(), {"resize_image", {{"width", 32}, {"height", 32}}});
            session.apply(doc, doc->activeLayer(), {"gaussian_blur", {{"sigma", 0.6}}});
            session.apply(doc, doc->activeLayer(), {"directional_shading", {}});
            session.select(doc, nova::ops::rectMask({32, 32}, {0, 0, 8, 8}), nova::ops::Combine::Replace);
        });
        edit.start();
        QTimer ready;
        ready.setInterval(100);
        QObject::connect(&ready, &QTimer::timeout, &window, [&] {
            nova::RecoveryStore store(QFileInfo(args[3]).absolutePath() + "/recovery");
            const auto entries = store.entries();
            if (entries.isEmpty())
                return;
            const auto e = store.load(entries[0].id);
            nova::Document probe(e.path, QImage{});
            if (e.nativeData.isEmpty() || !probe.decodeNative(e.nativeData, "recovery.png") ||
                probe.layers().size() != 2)
                return;
            ready.stop();
            fprintf(stdout, "snapshot-ready\n");
            fflush(stdout);
        });
        ready.start();
        return app.exec();
    }
    EditorGuiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "editor_gui_test.moc"
