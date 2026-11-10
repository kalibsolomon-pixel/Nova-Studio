// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 2-C8 production-widget tests: inline display editor, non-modal authoring,
// bulk UV controls, keyboard routing and accessibility metadata.
#include "batchdialog.h"
#include "creation.h"
#include "presets.h"
#include <QLineEdit>
#include <QSignalSpy>
#include <QTreeWidget>
#include "modelauthor.h"
#include "modelpanel.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QtTest>
using namespace nova;
namespace {
bool write(const QString &root, const QString &asset, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(root + '/' + asset).absolutePath());
    QFile f(root + '/' + asset);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
QJsonObject readJson(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        qWarning("readJson: cannot open %s", qPrintable(path));
    return QJsonDocument::fromJson(f.readAll()).object();
}
void evidence(QWidget *w, const QString &name) {
    const auto dir = qEnvironmentVariable("NOVA_C8_EVIDENCE_DIR");
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    w->grab().save(dir + '/' + name + ".png");
}
QVector3D firstPoint(Cube *cube) {
    const auto &m = cube->meshData();
    return m.faces.isEmpty() ? QVector3D() : m.faces[0].points[0];
}
} // namespace
class C8WorkspaceTest : public QObject {
    Q_OBJECT
    QTemporaryDir root;
    const QString model = "assets/nova/models/block/a.json";
    std::shared_ptr<mc::Library> library;
    Metrics metrics;
    DocumentSession session;
    QWidget window;
    Cube *cube = nullptr;
    ModelPanel *panel = nullptr;
    template <class T> T *child(const QString &name) {
        auto w = panel->findChild<T *>(name);
        if (!w)
            qWarning() << "missing widget" << name;
        return w;
    }

  private slots:
    void initTestCase() {
        auto source = mc::modelTemplate(mc::ModelTemplate::MultiElement, "nova:block/a");
        source.remove("display"); // starts without transforms, like a C7-authored model
        source["unknown_root"] = "kept";
        QVERIFY(write(root.path(), model, QJsonDocument(source).toJson()));
        QImage pixels(16, 16, QImage::Format_RGBA8888);
        pixels.fill(Qt::darkCyan);
        QDir().mkpath(root.path() + "/assets/nova/textures/block");
        QVERIFY(pixels.save(root.path() + "/assets/nova/textures/block/a.png"));
        library = std::make_shared<mc::Library>();
        library->update(root.path(), Index::scan(root.path()));
        session.setProject(root.path());
        auto layout = new QHBoxLayout(&window);
        cube = new Cube(&metrics);
        panel = new ModelPanel(cube, &session);
        layout->addWidget(cube, 2);
        layout->addWidget(panel, 1);
        panel->setLibrary(library, root.path());
        window.resize(1100, 760);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        panel->openAsset(model);
        QTest::qWait(60);
        QVERIFY(!cube->meshData().faces.isEmpty());
    }
    void displayEditorPreviewKeyboardAndHistory() {
        auto toggle = child<QToolButton>("displayToggle");
        QVERIFY(toggle && !child<QWidget>("displayEditor")->isVisible());
        toggle->click();
        QVERIFY(child<QWidget>("displayEditor")->isVisible());
        QCOMPARE(child<QComboBox>("modelContext")->currentText(), QString("gui"));
        QVERIFY(child<QLabel>("displaySource")->text().contains("Default"));
        const auto before = firstPoint(cube);
        auto rotationY = child<QDoubleSpinBox>("displayRotationY");
        rotationY->setValue(90);
        QVERIFY(child<QLabel>("displayStatus")->text().contains("Preview only"));
        QVERIFY(firstPoint(cube) != before); // main preview follows the staged transform
        QVERIFY(panel->documentSources().contains(model));
        // Escape inside the editor cancels; Enter applies one undo step.
        rotationY->setFocus();
        QTest::keyClick(rotationY, Qt::Key_Escape);
        QVERIFY(!panel->documentSources().contains(model));
        QCOMPARE(firstPoint(cube), before);
        rotationY->setValue(45);
        child<QDoubleSpinBox>("displayScaleX")->setValue(0.5); // uniform scale propagates
        QCOMPARE(child<QDoubleSpinBox>("displayScaleZ")->value(), 0.5);
        QTest::keyClick(rotationY, Qt::Key_Return);
        QVERIFY(child<QLabel>("displayStatus")->text().contains("Applied"));
        QVERIFY(!child<QPushButton>("displayApply")->isEnabled());
        // Ctrl+Z routing: model history when focus is in the panel.
        child<QPushButton>("displayCopy")->setFocus();
        QVERIFY(panel->historyShortcut(false));
        QVERIFY(!panel->documentSources().contains(model));
        QVERIFY(panel->historyShortcut(true));
        QVERIFY(panel->documentSources().contains(model));
        // Context switching never commits or discards a pending preview.
        child<QDoubleSpinBox>("displayTranslationY")->setValue(2);
        child<QComboBox>("modelContext")->setCurrentText("ground");
        QVERIFY(child<QPushButton>("displayApply")->isEnabled());
        QVERIFY(child<QLabel>("displaySource")->text().contains("Default"));
        child<QPushButton>("displayCancel")->click();
        child<QComboBox>("modelContext")->setCurrentText("gui");
        QCOMPARE(child<QDoubleSpinBox>("displayTranslationY")->value(), 0.0);
        QCOMPARE(child<QDoubleSpinBox>("displayRotationY")->value(), 45.0);
        // Face selection keeps the model target and the display state.
        const auto target = panel->selectedTarget();
        panel->selectFace(0);
        QCOMPARE(panel->selectedTarget(), target);
        QCOMPARE(child<QDoubleSpinBox>("displayRotationY")->value(), 45.0);
        // Save through the guarded path and verify JSON on disk.
        child<QPushButton>("displaySave")->click();
        const auto saved = readJson(root.path() + '/' + model);
        QCOMPARE(saved["display"].toObject()["gui"].toObject()["rotation"].toArray(), (QJsonArray{0, 45, 0}));
        QCOMPARE(saved["display"].toObject()["gui"].toObject()["scale"].toArray(), (QJsonArray{0.5, 0.5, 0.5}));
        QCOMPARE(saved["unknown_root"].toString(), QString("kept"));
        evidence(panel, "display-editor-panel");
    }
    void framingLinkingAndPresets() {
        auto framing = child<QCheckBox>("displayFraming");
        framing->setChecked(true);
        QVERIFY(cube->contextFraming);
        QCOMPARE(cube->referenceFrame, 2); // GUI slot
        child<QComboBox>("modelContext")->setCurrentText("thirdperson_righthand");
        QCOMPARE(cube->referenceFrame, 1); // block reference
        auto linked = child<QCheckBox>("displayLinked");
        QVERIFY(linked->isEnabled());
        QVERIFY(linked->isChecked()); // no left-hand value: mirrors the right hand
        child<QPushButton>("displayPreset")->click();
        child<QPushButton>("displayApply")->click();
        QVERIFY(child<QLabel>("displaySource")->text().contains("Local"));
        QVERIFY(child<QDoubleSpinBox>("displayScaleX")->value() < 0.5);
        evidence(&window, "display-framing-thirdperson");
        child<QComboBox>("modelContext")->setCurrentText("gui");
        evidence(&window, "display-framing-gui");
        framing->setChecked(false);
        QVERIFY(!cube->contextFraming);
        QCOMPARE(cube->referenceFrame, 0);
        child<QComboBox>("modelContext")->setCurrentText("raw");
        QVERIFY(!child<QDoubleSpinBox>("displayRotationX")->isEnabled());
        child<QComboBox>("modelContext")->setCurrentText("gui");
    }
    void authoringWindowIsNonModalAndExclusive() {
        panel->authorModel();
        QTest::qWait(60);
        QVERIFY(panel->authoringOpen());
        ModelAuthorDialog *dialog = nullptr;
        for (auto w : QApplication::topLevelWidgets())
            if (w->objectName() == "modelAuthorDialog" && w->isVisible())
                dialog = dynamic_cast<ModelAuthorDialog *>(w);
        QVERIFY(dialog);
        QVERIFY(!dialog->isModal());
        panel->authorModel(); // same model: the open window is reused, not duplicated
        QTest::qWait(30);
        int windows = 0;
        for (auto w : QApplication::topLevelWidgets())
            windows += w->objectName() == "modelAuthorDialog" && w->isVisible();
        QCOMPARE(windows, 1);
        QCOMPARE(QApplication::activeModalWidget(), nullptr);
        // The panel defers display edits for the same model while the window is open.
        child<QDoubleSpinBox>("displayRotationX")->setValue(10);
        QVERIFY(child<QLabel>("displayStatus")->text().contains("authoring window"));
        // Bulk UV across two elements with mixed widths.
        auto elements = dialog->findChild<QListWidget *>("authorElements");
        auto directions = dialog->findChild<QListWidget *>("authorDirections");
        QCOMPARE(elements->selectionMode(), QAbstractItemView::ExtendedSelection);
        elements->selectAll();
        directions->clearSelection();
        directions->item(2)->setSelected(true); // north
        dialog->findChild<QToolButton *>("authorBulkToggle")->click();
        auto summary = dialog->findChild<QLabel *>("authorBulkSummary");
        QVERIFY2(summary->text().contains("2 face(s) across 2 element(s)"), qPrintable(summary->text()));
        auto width = dialog->findChild<QDoubleSpinBox *>("authorBulkWidth");
        auto height = dialog->findChild<QDoubleSpinBox *>("authorBulkHeight");
        QCOMPARE(height->text(), QString("mixed")); // the two north faces differ in size
        QCOMPARE(width->text(), QString("mixed"));
        dialog->findChild<QDoubleSpinBox *>("authorBulkU")->setValue(3.5);
        dialog->findChild<QPushButton *>("authorBulkPosition")->click();
        QVERIFY(dialog->findChild<QLabel *>("authorStatus")->text().contains("Preview only"));
        QCOMPARE(dialog->findChild<QDoubleSpinBox *>("authorBulkU")->value(), 3.5);
        QCOMPARE(height->text(), QString("mixed")); // mixed axis left untouched
        if (auto tabs = dialog->findChild<QTabWidget *>())
            tabs->setCurrentIndex(1); // Faces / UV, where the bulk group lives
        if (auto bulk = dialog->findChild<QWidget *>("authorBulkUv"))
            if (auto area = qobject_cast<QScrollArea *>(bulk->parentWidget()->parentWidget()->parentWidget()))
                area->ensureWidgetVisible(bulk);
        QTest::qWait(30);
        evidence(dialog, "bulk-uv-authoring");
        dialog->findChild<QPushButton *>("authorCancel")->click();
        dialog->close();
        QTest::qWait(30);
        QVERIFY(!panel->authoringOpen());
    }
    void keyboardAndAccessibilityMetadata() {
        for (const auto &name : {"displayRotationX", "displayTranslationY", "displayScaleZ"}) {
            auto s = child<QDoubleSpinBox>(name);
            QVERIFY(!s->accessibleName().isEmpty());
            QVERIFY(s->focusPolicy() & Qt::TabFocus);
        }
        QVERIFY(!child<QWidget>("displayEditor")->accessibleName().isEmpty());
        // Tab moves through the transform grid in reading order.
        auto x = child<QDoubleSpinBox>("displayRotationX");
        x->setFocus();
        QTest::keyClick(x, Qt::Key_Tab);
        QCOMPARE(x->window()->focusWidget(), child<QDoubleSpinBox>("displayRotationY"));
    }
    void nativeBatchDialogScopesRunAndRecovery() {
        QTemporaryDir pack, outsideDir;
        const QString rel = "assets/nova/textures/block/layered.png";
        const auto png = pack.path() + '/' + rel;
        QDir().mkpath(QFileInfo(png).absolutePath());
        QImage base(8, 8, QImage::Format_RGBA8888);
        base.fill(QColor(40, 80, 120));
        QVERIFY(savePng(png, base));
        {
            Document doc(png, base);
            QImage top(8, 8, QImage::Format_RGBA8888);
            top.fill(QColor(200, 10, 10, 128));
            QVERIFY(doc.addLayerWithPixels("Glaze", top));
            QVERIFY(doc.saveNative(pack.path()));
        }
        const auto nvdoc = nativeDocumentPath(pack.path(), png);
        const auto original = Revision::read(nvdoc).hash;
        PresetStore presets(outsideDir.path() + "/presets.json");
        BatchContext context;
        context.root = pack.path();
        context.files = {rel, "assets/nova/textures/block/plain.png"};
        QVERIFY(savePng(pack.path() + "/assets/nova/textures/block/plain.png", base));
        context.presets = &presets;
        context.directory = "assets/nova/textures/block";
        BatchDialog dialog(context);
        dialog.setProperty("confirmed", true);
        dialog.show();
        dialog.findChild<QComboBox *>("batchSourceMode")->setCurrentIndex(int(batch::SourceMode::Native));
        QVERIFY(dialog.findChild<QWidget *>("batchNativeScope")->isVisible());
        dialog.findChild<QComboBox *>("batchLayerScope")->setCurrentIndex(int(batch::LayerScope::All));
        dialog.findChild<QLineEdit *>("batchTransaction")->setText(outsideDir.path() + "/tx");
        dialog.setScope(batch::Scope::Folder, "assets/nova/textures/block");
        QVERIFY(dialog.findChild<QLabel *>("batchTargetCount")->text().contains("1 with native documents"));
        batch::Pipeline p;
        p.steps = {{"hue_saturation", {{"hue", 60}}}, {"levels", {}}};
        dialog.setPipeline(p);
        QVERIFY(dialog.runDryRun());
        QCOMPARE(dialog.nativeDryRun().ready, 1);
        QCOMPARE(dialog.nativeDryRun().invalid, 1); // plain PNG has no native document
        auto results = dialog.findChild<QTreeWidget *>("batchResults");
        QVERIFY(results->topLevelItem(0)->text(2).contains("Glaze"));
        QCOMPARE(Revision::read(nvdoc).hash, original); // dry run writes nothing
        dialog.findChild<QPushButton *>("batchPreview")->click();
        QTest::qWait(50);
        evidence(&dialog, "native-batch-dry-run");
        QSignalSpy done(&dialog, &BatchDialog::nativeFinished);
        QVERIFY(dialog.start());
        QVERIFY(done.wait(30000));
        const auto report = done.first().first().value<batch::NativeReport>();
        QCOMPARE(report.done, 1);
        QVERIFY(Revision::read(nvdoc).hash != original);
        QVERIFY(dialog.findChild<QLabel *>("batchStatus")->text().contains("Journal"));
        evidence(&dialog, "native-batch-complete");
        // Recovery restores the verified original copy.
        auto inspect = dialog.recoverJournal(report.journal, true, false);
        QCOMPARE(inspect.restored, 1);
        QCOMPARE(dialog.recoverJournal(report.journal, true, true).restored, 1);
        QCOMPARE(Revision::read(nvdoc).hash, original);
        auto doc = Document::open(png);
        QVERIFY(doc && doc->loadNative(pack.path()));
        QCOMPARE(int(doc->layers().size()), 2);
    }
};
QTEST_MAIN(C8WorkspaceTest)
#include "c8_workspace_test.moc"
