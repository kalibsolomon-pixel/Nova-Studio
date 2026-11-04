// SPDX-License-Identifier: GPL-3.0-or-later
// Inline item-display transform editor in the Models / UV panel (Phase 2-C8).
// Edits stage on the shared model document; Apply makes one model undo step and
// Save uses the same revision-checked path as every other model edit.
#include "display.h"
#include "modelpanel.h"
#include "creation.h"
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QtWidgets>

namespace nova {
namespace {
class KeyRelay : public QObject {
    std::function<bool(QKeyEvent *)> handler;

  public:
    KeyRelay(QObject *parent, std::function<bool(QKeyEvent *)> f)
        : QObject(parent), handler(std::move(f)) {}
    bool eventFilter(QObject *, QEvent *e) override {
        return e->type() == QEvent::KeyPress && handler(static_cast<QKeyEvent *>(e));
    }
};
const char *axisNames[] = {"X", "Y", "Z"};
} // namespace

QString ModelPanel::displayContext() const {
    const auto c = contexts ? contexts->currentText() : QString();
    return c == "raw" ? QString() : c;
}
QString ModelPanel::displayModelAsset() const {
    if (target.startsWith("model:"))
        return modelAsset();
    return contextModel.id.isEmpty() ? QString()
                                     : mc::Location::parse(contextModel.id).asset("models");
}
std::shared_ptr<mc::ModelEdit> ModelPanel::editableModel(const QString &asset, QString *error) {
    if (!library || asset.isEmpty()) {
        *error = "Choose a pack model first";
        return {};
    }
    if (!library->editable(asset)) {
        *error = "Read-only fallback: import a project-local override from Dependencies first";
        return {};
    }
    if (authorWindow && authorWindow->property("asset").toString() == asset) {
        *error = "This model is open in the authoring window; apply or close it first";
        return {};
    }
    auto edit = modelEdits.value(asset);
    if (edit && !edit->modified() && !edit->previewing() && edit->source() != library->source(asset) &&
        !edit->open(root, asset, error))
        return {};
    if (!edit) {
        if (modelEdits.size() >= 16) {
            *error = "Save pending model edits before opening another model (16-document limit)";
            return {};
        }
        edit = std::make_shared<mc::ModelEdit>();
        if (!edit->open(root, asset, error))
            return {};
        if (edit->source() != library->source(asset)) {
            *error = "Model changed externally; refresh the resource catalog before editing";
            return {};
        }
        modelEdits[asset] = edit;
        if (modelOpened)
            modelOpened(edit);
    }
    return edit;
}
QString ModelPanel::checkpointModel(const std::shared_ptr<mc::ModelEdit> &edit) {
    QElapsedTimer checkpoint;
    checkpoint.start();
    QString path, error;
    const auto key = QString::fromLatin1(
        QCryptographicHash::hash(edit->assetPath().toUtf8(), QCryptographicHash::Sha256).toHex());
    if (!mc::safeDestination(root, ".nova/model-recovery/" + key + ".json", &path, &error) ||
        !edit->writeRecovery(path, &error))
        displayStatus->setText("Edits kept in memory; recovery snapshot failed: " + error);
    else
        deferredModelRecovery.remove(edit->assetPath());
    if (modelCheckpointed)
        modelCheckpointed(checkpoint);
    return error;
}
void ModelPanel::buildDisplayEditor(QVBoxLayout *layout) {
    auto toggle = new QToolButton;
    toggle->setObjectName("displayToggle");
    toggle->setText("Item display transforms");
    toggle->setProperty("role", "disclosure");
    toggle->setCheckable(true);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setArrowType(Qt::RightArrow);
    toggle->setAccessibleName("Show item display transform editor");
    layout->addWidget(toggle);
    displayControls = new QWidget;
    displayControls->setObjectName("displayEditor");
    displayControls->setAccessibleName("Item display transform editor");
    displayControls->hide();
    layout->addWidget(displayControls);
    connect(toggle, &QToolButton::toggled, this, [this, toggle](bool open) {
        toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        displayControls->setVisible(open);
        if (open && contexts->currentText() == "raw")
            contexts->setCurrentText("gui"); // editing needs a context; raw has none
        updateDisplayControls();
    });
    auto v = new QVBoxLayout(displayControls);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    displaySource = new QLabel;
    displaySource->setObjectName("displaySource");
    displaySource->setWordWrap(true);
    displaySource->setTextFormat(Qt::PlainText);
    displaySource->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    v->addWidget(displaySource);
    auto grid = new QGridLayout;
    grid->setHorizontalSpacing(3);
    grid->setVerticalSpacing(2);
    const QString rows[] = {"Rotation °", "Translation", "Scale"};
    for (int c = 0; c < 3; ++c) {
        auto header = new QLabel(axisNames[c]);
        header->setAlignment(Qt::AlignCenter);
        grid->addWidget(header, 0, c + 1);
    }
    for (int r = 0; r < 3; ++r) {
        auto label = new QLabel(rows[r]);
        grid->addWidget(label, r + 1, 0);
        for (int c = 0; c < 3; ++c) {
            auto s = new QDoubleSpinBox;
            const int index = r * 3 + c;
            displayValues[index] = s;
            s->setObjectName(QString("display%1%2").arg(QStringList{"Rotation", "Translation", "Scale"}[r],
                                                          axisNames[c]));
            s->setAccessibleName(rows[r] + ' ' + axisNames[c]);
            s->setDecimals(r == 1 ? 3 : 4);
            s->setRange(r == 0   ? -mc::DisplayRotationLimit
                        : r == 1 ? -mc::DisplayTranslationLimit
                                 : -mc::DisplayScaleLimit,
                        r == 0   ? mc::DisplayRotationLimit
                        : r == 1 ? mc::DisplayTranslationLimit
                                 : mc::DisplayScaleLimit);
            s->setSingleStep(r == 0 ? 1 : r == 1 ? .25 : .025);
            s->setKeyboardTracking(false);
            s->setButtonSymbols(QAbstractSpinBox::NoButtons);
            s->setMinimumWidth(48);
            s->setToolTip(r == 0   ? "Degrees, applied X then Y then Z. Up/Down nudges; PgUp/PgDn ×10"
                          : r == 1 ? "1/16 block (pixels); Minecraft limits ±80"
                                   : "Minecraft limits ±4; negative values mirror");
            label->setBuddy(s);
            grid->addWidget(s, r + 1, c + 1);
            connect(s, &QDoubleSpinBox::valueChanged, this, [this, index](double value) {
                if (displaySyncing)
                    return;
                if (index >= 6 && displayUniform->isChecked()) {
                    displaySyncing = true;
                    for (int i = 6; i < 9; ++i)
                        displayValues[i]->setValue(value);
                    displaySyncing = false;
                }
                previewDisplayValues();
            });
        }
    }
    v->addLayout(grid);
    auto options = new QGridLayout;
    displayUniform = new QCheckBox("Uniform scale");
    displayUniform->setObjectName("displayUniform");
    displayUniform->setChecked(true);
    displaySnap = new QCheckBox("Snap");
    displaySnap->setObjectName("displaySnap");
    displaySnap->setToolTip("Rotation 15°, translation 0.5, scale 0.0625");
    displayLinked = new QCheckBox("Left mirrors right");
    displayLinked->setObjectName("displayLinked");
    displayLinked->setToolTip("Leave the left-hand context undefined so Minecraft mirrors the "
                              "right hand. Unlinking writes an explicit left-hand value.");
    contextFraming = new QCheckBox("Context framing");
    contextFraming->setObjectName("displayFraming");
    contextFraming->setToolTip("Fixed camera with a one-block (or GUI slot) reference so scale "
                               "and translation are visible. Not a player-pose simulation.");
    options->addWidget(displayUniform, 0, 0);
    options->addWidget(displaySnap, 0, 1);
    options->addWidget(displayLinked, 1, 0);
    options->addWidget(contextFraming, 1, 1);
    v->addLayout(options);
    connect(displaySnap, &QCheckBox::toggled, this, [this](bool snap) {
        for (int i = 0; i < 9; ++i)
            displayValues[i]->setSingleStep(i < 3 ? (snap ? 15 : 1) : i < 6 ? (snap ? .5 : .25)
                                                                           : (snap ? .0625 : .025));
    });
    connect(contextFraming, &QCheckBox::toggled, this, [this](bool) { updateDisplayFraming(); });
    connect(displayLinked, &QCheckBox::toggled, this, [this](bool linked) {
        if (displaySyncing)
            return;
        QString error;
        auto edit = editableModel(displayModelAsset(), &error);
        const auto ctx = displayContext();
        const auto model = library ? library->model(mc::Location::fromAsset(displayModelAsset(), "models"))
                                   : mc::Model{};
        const auto right = mc::resolveDisplay(model, mc::leftHandContext(ctx) ? mc::pairedHand(ctx) : ctx);
        if (!edit || !edit->linkHands(ctx, linked, right.value, &error)) {
            updateDisplayControls();
            displayStatus->setText(error);
            return;
        }
        displayDocument = edit;
        refreshDisplayModel();
    });
    auto button = [&](QBoxLayout *row, const QString &text, const QString &name, const QString &tip,
                      auto callback) {
        auto b = new QPushButton(text);
        b->setObjectName(name);
        b->setToolTip(tip);
        b->setAutoDefault(false);
        row->addWidget(b);
        connect(b, &QPushButton::clicked, this, callback);
        return b;
    };
    auto commitRow = new QHBoxLayout;
    displayApply = button(commitRow, "Apply", "displayApply", "Commit the preview as one model undo step (Enter)",
                          [this] { finishDisplayEdit(true); });
    displayCancel = button(commitRow, "Cancel", "displayCancel", "Discard the preview (Escape)",
                           [this] { finishDisplayEdit(false); });
    button(commitRow, "Save", "displaySave", "Write the model JSON with the revision check", [this] {
        if (!displayDocument)
            return;
        if (displayDocument->previewing())
            finishDisplayEdit(true);
        if (saveModel(displayDocument))
            displayStatus->setText("Saved " + displayDocument->assetPath());
        updateDisplayControls();
    });
    v->addLayout(commitRow);
    auto editRow = new QHBoxLayout;
    button(editRow, "Inherit", "displayReset",
           "Remove this model's value so the parent (or mirrored right hand) applies", [this] {
               QString error;
               auto edit = editableModel(displayModelAsset(), &error);
               if (!edit || !edit->resetDisplay(displayContext(), &error)) {
                   displayStatus->setText(error);
                   return;
               }
               displayDocument = edit;
               refreshDisplayModel();
           });
    button(editRow, "Copy", "displayCopy", "Copy this context's transform as JSON", [this] {
        displayClipboard = mc::displayTransformJson(displayValuesNow());
        QApplication::clipboard()->setText(
            QString::fromUtf8(QJsonDocument(displayClipboard).toJson(QJsonDocument::Compact)));
        displayStatus->setText("Copied " + displayContext());
    });
    button(editRow, "Paste", "displayPaste", "Paste a copied transform into this context", [this] {
        auto json = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8());
        const auto value = json.isObject() ? json.object() : displayClipboard;
        QString error;
        auto edit = editableModel(displayModelAsset(), &error);
        if (value.isEmpty() || !edit || !edit->pasteDisplay(displayContext(), value, &error)) {
            displayStatus->setText(error.isEmpty() ? "Nothing to paste" : error);
            return;
        }
        displayDocument = edit;
        refreshDisplayModel();
    });
    button(editRow, "Block preset", "displayPreset",
           "Stage Nova's block-like transforms for GUI, ground, frame and hands", [this] {
               QString error;
               auto edit = editableModel(displayModelAsset(), &error);
               if (!edit || !edit->applyDisplayPreset(mc::blockDisplayPreset(), &error)) {
                   displayStatus->setText(error);
                   return;
               }
               displayDocument = edit;
               refreshDisplayModel();
           });
    v->addLayout(editRow);
    auto historyRow = new QHBoxLayout;
    button(historyRow, "Undo model", "displayUndo", "Model JSON history (separate from texture history)",
           [this] { modelHistory(false); });
    button(historyRow, "Redo model", "displayRedo", {}, [this] { modelHistory(true); });
    v->addLayout(historyRow);
    displayStatus = new QLabel;
    displayStatus->setObjectName("displayStatus");
    displayStatus->setWordWrap(true);
    displayStatus->setTextFormat(Qt::PlainText);
    displayStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    v->addWidget(displayStatus);
    displayControls->installEventFilter(new KeyRelay(displayControls, [this](QKeyEvent *e) {
        if (e->key() == Qt::Key_Escape && displayDocument && displayDocument->previewing()) {
            finishDisplayEdit(false);
            return true;
        }
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && displayDocument &&
            displayDocument->previewing()) {
            finishDisplayEdit(true);
            return true;
        }
        return false;
    }));
    connect(cube, &Cube::displayDragged, this, [this](QPointF d, Qt::KeyboardModifiers keys) {
        if (!displayControls->isVisible() || displayContext().isEmpty())
            return;
        auto t = displayValuesNow();
        if (keys == Qt::ShiftModifier) {
            t.rotation[1] += d.x() * .5;
            t.rotation[0] += d.y() * .5;
        } else if (keys == Qt::ControlModifier) {
            t.translation[0] += d.x() * .05;
            t.translation[1] -= d.y() * .05;
        } else {
            const double f = std::clamp(1 - d.y() * .005, .5, 2.);
            for (auto &s : t.scale)
                s = std::clamp(s * f, -mc::DisplayScaleLimit, mc::DisplayScaleLimit);
        }
        for (int i = 0; i < 3; ++i) {
            t.rotation[i] = std::clamp(t.rotation[i], -mc::DisplayRotationLimit, mc::DisplayRotationLimit);
            t.translation[i] =
                std::clamp(t.translation[i], -mc::DisplayTranslationLimit, mc::DisplayTranslationLimit);
        }
        setDisplayValues(t);
        previewDisplayValues();
    });
}
mc::DisplayTransform ModelPanel::displayValuesNow() const {
    mc::DisplayTransform t;
    for (int i = 0; i < 3; ++i) {
        t.rotation[i] = displayValues[i]->value();
        t.translation[i] = displayValues[3 + i]->value();
        t.scale[i] = displayValues[6 + i]->value();
    }
    return t;
}
void ModelPanel::setDisplayValues(const mc::DisplayTransform &t) {
    displaySyncing = true;
    for (int i = 0; i < 3; ++i) {
        displayValues[i]->setValue(t.rotation[i]);
        displayValues[3 + i]->setValue(t.translation[i]);
        displayValues[6 + i]->setValue(t.scale[i]);
    }
    displaySyncing = false;
}
void ModelPanel::previewDisplayValues() {
    const auto ctx = displayContext();
    if (ctx.isEmpty()) {
        displayStatus->setText("Choose a display context (not raw) to edit transforms");
        return;
    }
    auto t = displayValuesNow();
    if (displaySnap->isChecked()) {
        auto snap = [](double v, double step) { return std::round(v / step) * step; };
        for (int i = 0; i < 3; ++i) {
            t.rotation[i] = snap(t.rotation[i], 15);
            t.translation[i] = snap(t.translation[i], .5);
            t.scale[i] = snap(t.scale[i], .0625);
        }
        setDisplayValues(t);
    }
    QString error;
    auto edit = editableModel(displayModelAsset(), &error);
    if (!edit || !edit->previewDisplay(ctx, t, &error)) {
        updateDisplayControls(); // restore the authoritative values first
        displayStatus->setText(error);
        return;
    }
    displayDocument = edit;
    refreshDisplayModel();
}
void ModelPanel::refreshDisplayModel() {
    if (!displayDocument || !library)
        return;
    if (!displayDocument->previewing() && displayDocument->modified())
        checkpointModel(displayDocument);
    if (modelChanged)
        modelChanged();
    library->previewSource(displayDocument->assetPath(), displayDocument->source());
    rebuild(); // updates the main preview and, through it, these controls
}
void ModelPanel::finishDisplayEdit(bool commit) {
    if (!displayDocument)
        return;
    if (commit) {
        if (displayDocument->previewing() &&
            !displayDocument->commitPreview("Display · " + displayContext())) {
            displayStatus->setText("Commit rejected: model or history budget exceeded");
            return;
        }
        checkpointModel(displayDocument);
    } else
        displayDocument->cancelPreview();
    refreshDisplayModel();
}
std::shared_ptr<mc::ModelEdit> ModelPanel::openModelDocument(const QString &asset) const {
    if (!library || !library->editable(asset) ||
        (authorWindow && authorWindow->property("asset").toString() == asset))
        return {};
    return modelEdits.value(asset);
}
bool ModelPanel::registerProvisionalModel(const std::shared_ptr<mc::ModelEdit> &edit, QString *error) {
    if (!library || !edit->provisional() || modelEdits.contains(edit->assetPath()) || modelEdits.size() >= 16) {
        *error = "The model path is already open or the model document limit was reached";
        return false;
    }
    *error = checkpointModel(edit);
    if (!error->isEmpty())
        return false;
    modelEdits[edit->assetPath()] = edit;
    library->setProvisionalSource(edit->assetPath(), edit->source());
    if (modelRegistered)
        modelRegistered(edit->assetPath());
    return true;
}
QString ModelPanel::publishModelEdit(const std::shared_ptr<mc::ModelEdit> &edit) {
    const auto warning = checkpointModel(edit);
    library->previewSource(edit->assetPath(), edit->source());
    rebuild();
    if (modelChanged)
        modelChanged();
    if (canvas)
        canvas->update();
    return warning;
}

bool ModelPanel::modelHistory(bool redo, std::shared_ptr<mc::ModelEdit> edit) {
    if (!edit)
        edit = modelEdits.value(modelAsset());
    if (!edit || authoringModel(edit->assetPath()))
        return false;
    if (!redo && edit->previewing())
        edit->cancelPreview();
    else if (!(redo ? edit->redo() : edit->undo()))
        return false;
    publishModelEdit(edit);
    return true;
}
bool ModelPanel::historyShortcut(bool redo) {
    // Ctrl+Z / Ctrl+Y act on model JSON while keyboard focus is inside this panel.
    auto focus = QApplication::focusWidget();
    if (!focus && window()) // inactive window: last focused child (no shortcut can arrive then)
        focus = window()->focusWidget();
    return focus && isAncestorOf(focus) && modelHistory(redo);
}
void ModelPanel::updateDisplayFraming() {
    const auto ctx = displayContext();
    const bool framing = contextFraming && contextFraming->isChecked() && !ctx.isEmpty();
    cube->referenceFrame = framing ? (ctx == "gui" ? 2 : 1) : 0;
    // Re-frame only when framing or the context changes, so orbiting survives edits.
    if (framing != cube->contextFraming || (framing && ctx != framedContext))
        cube->setContextFraming(framing, ctx == "gui");
    framedContext = framing ? ctx : QString();
    cube->update();
}
void ModelPanel::updateDisplayControls() {
    if (!displayControls)
        return;
    cube->displayManipulation = displayControls->isVisible();
    if (!displayControls->isVisible())
        return;
    updateDisplayFraming();
    const auto ctx = displayContext();
    const auto asset = displayModelAsset();
    const bool usable = library && !ctx.isEmpty() && !asset.isEmpty();
    for (auto s : displayValues)
        s->setEnabled(usable);
    if (!usable) {
        displaySource->setText(ctx.isEmpty() ? "Choose a display context above (raw shows no transform)"
                                             : "Choose a block or item model");
        displayApply->setEnabled(false);
        displayCancel->setEnabled(false);
        return;
    }
    const auto model = library->model(mc::Location::fromAsset(asset, "models"));
    const auto r = mc::resolveDisplay(model, ctx);
    QString text = mc::displayContextLabel(ctx) + " · " + r.describe();
    if (!r.valid)
        text += "\nUnsupported value in source JSON: reset (Inherit) before editing";
    if (r.translationClamped || r.scaleClamped)
        text += "\nMinecraft clamps this value (translation ±80, scale ±4)";
    if (!library->editable(asset))
        text += "\nRead-only fallback model";
    displaySource->setText(text);
    setDisplayValues(r.value);
    if (r.value.scale[0] != r.value.scale[1] || r.value.scale[1] != r.value.scale[2])
        displayUniform->setChecked(false); // never force non-uniform data to one value
    displaySyncing = true;
    displayLinked->setEnabled(mc::handContext(ctx));
    const auto left = mc::leftHandContext(ctx) ? ctx : mc::pairedHand(ctx);
    const auto leftResolution = mc::handContext(ctx) ? mc::resolveDisplay(model, left) : mc::DisplayResolution{};
    displayLinked->setChecked(mc::handContext(ctx) &&
                              (leftResolution.source == mc::DisplaySource::LocalFromRightHand ||
                               leftResolution.source == mc::DisplaySource::InheritedFromRightHand ||
                               leftResolution.source == mc::DisplaySource::Default));
    displaySyncing = false;
    const bool pending = displayDocument && displayDocument->assetPath() == asset &&
                         displayDocument->previewing();
    displayApply->setEnabled(pending);
    displayCancel->setEnabled(pending);
    if (pending)
        displayStatus->setText("Preview only · Enter applies one undo step, Escape cancels");
    else if (displayDocument && displayDocument->assetPath() == asset && displayDocument->modified())
        displayStatus->setText("Applied · unsaved model JSON (Save writes the checked revision)");
    else if (displayStatus->text().startsWith("Preview only") || displayStatus->text().startsWith("Applied"))
        displayStatus->clear();
}
} // namespace nova
