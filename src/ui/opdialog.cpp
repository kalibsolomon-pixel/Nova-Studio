// SPDX-License-Identifier: GPL-3.0-or-later
#include "opdialog.h"
#include "canvas.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace nova {
namespace {
QPixmap checkerPixmap(const QImage &image, QSize box) {
    if (image.isNull())
        return {};
    const double k = std::max(1.0, std::floor(std::min(box.width() / double(image.width()),
                                                       box.height() / double(image.height()))));
    const QSize size = (image.width() * k > box.width() || image.height() * k > box.height())
                           ? image.size().scaled(box, Qt::KeepAspectRatio)
                           : image.size() * int(k);
    QPixmap pm(size);
    QPainter p(&pm);
    for (int y = 0; y < size.height(); y += 8)
        for (int x = 0; x < size.width(); x += 8)
            p.fillRect(x, y, 8, 8, ((x + y) / 8) % 2 ? QColor(70, 70, 70) : QColor(100, 100, 100));
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(pm.rect(), image);
    return pm;
}
QIcon colorIcon(QRgb c) {
    QPixmap pm(28, 16);
    pm.fill(QColor::fromRgba(c));
    QPainter p(&pm);
    p.setPen(Qt::black);
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    return QIcon(pm);
}
} // namespace

ParamEditor::ParamEditor(QWidget *parent) : QWidget(parent) {
    form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
}
void ParamEditor::setOperation(const QString &operation, const QJsonObject &params) {
    id = operation;
    editors.clear();
    while (form->rowCount())
        form->removeRow(0);
    const auto info = ops::find(id);
    if (!info)
        return;
    auto description = new QLabel(info->description);
    description->setWordWrap(true);
    description->setEnabled(false);
    form->addRow(description);
    for (const auto &p : info->params) {
        QWidget *w = nullptr;
        switch (p.type) {
        case ops::ParamType::Int: {
            auto box = new QWidget;
            auto h = new QHBoxLayout(box);
            h->setContentsMargins(0, 0, 0, 0);
            auto spin = new QSpinBox;
            spin->setRange(int(p.minimum), int(p.maximum));
            spin->setKeyboardTracking(false);
            spin->setObjectName("param-" + p.key);
            if (p.maximum - p.minimum <= 1000) {
                auto slider = new QSlider(Qt::Horizontal);
                slider->setRange(int(p.minimum), int(p.maximum));
                connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
                connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
                h->addWidget(slider, 1);
            }
            h->addWidget(spin);
            connect(spin, &QSpinBox::valueChanged, this, [this] {
                if (!updating)
                    emit edited();
            });
            editors[p.key] = spin;
            w = box;
            break;
        }
        case ops::ParamType::Double: {
            auto spin = new QDoubleSpinBox;
            spin->setRange(p.minimum, p.maximum);
            spin->setSingleStep(p.step);
            spin->setDecimals(p.step < 1 ? 2 : 0);
            spin->setKeyboardTracking(false);
            spin->setObjectName("param-" + p.key);
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this] {
                if (!updating)
                    emit edited();
            });
            editors[p.key] = w = spin;
            break;
        }
        case ops::ParamType::Bool: {
            auto check = new QCheckBox;
            check->setObjectName("param-" + p.key);
            connect(check, &QCheckBox::toggled, this, [this] {
                if (!updating)
                    emit edited();
            });
            editors[p.key] = w = check;
            break;
        }
        case ops::ParamType::Color: {
            auto button = new QToolButton;
            button->setObjectName("param-" + p.key);
            button->setIconSize({28, 16});
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            connect(button, &QToolButton::clicked, this, [this, button] {
                auto c = QColorDialog::getColor(QColor::fromRgba(button->property("rgba").toUInt()),
                                                this, "Colour", QColorDialog::ShowAlphaChannel);
                if (c.isValid()) {
                    button->setProperty("rgba", c.rgba());
                    button->setIcon(colorIcon(c.rgba()));
                    button->setText(ops::colorName(c.rgba()));
                    emit edited();
                }
            });
            editors[p.key] = w = button;
            break;
        }
        case ops::ParamType::Choice: {
            auto combo = new QComboBox;
            combo->setObjectName("param-" + p.key);
            combo->addItems(p.choices);
            connect(combo, &QComboBox::currentIndexChanged, this, [this] {
                if (!updating)
                    emit edited();
            });
            editors[p.key] = w = combo;
            break;
        }
        case ops::ParamType::Palette: {
            auto box = new QWidget;
            auto h = new QHBoxLayout(box);
            h->setContentsMargins(0, 0, 0, 0);
            auto line = new QLineEdit;
            line->setObjectName("param-" + p.key);
            line->setPlaceholderText("(active palette)");
            auto use = new QPushButton("Use active");
            connect(use, &QPushButton::clicked, this, [this, line] {
                if (paletteText)
                    line->setText(paletteText());
                emit edited();
            });
            connect(line, &QLineEdit::editingFinished, this, [this] { emit edited(); });
            h->addWidget(line, 1);
            h->addWidget(use);
            editors[p.key] = line;
            w = box;
            break;
        }
        }
        w->setToolTip(p.tooltip);
        form->addRow(p.label, w);
    }
    setParams(params.isEmpty() ? ops::defaults(id) : params);
}
QJsonObject ParamEditor::params() const {
    QJsonObject out;
    const auto info = ops::find(id);
    if (!info)
        return out;
    for (const auto &p : info->params) {
        auto w = editors.value(p.key);
        if (auto s = qobject_cast<QSpinBox *>(w))
            out[p.key] = s->value();
        else if (auto d = qobject_cast<QDoubleSpinBox *>(w))
            out[p.key] = d->value();
        else if (auto c = qobject_cast<QCheckBox *>(w))
            out[p.key] = c->isChecked();
        else if (auto b = qobject_cast<QToolButton *>(w))
            out[p.key] = ops::colorName(b->property("rgba").toUInt());
        else if (auto combo = qobject_cast<QComboBox *>(w))
            out[p.key] = combo->currentText();
        else if (auto line = qobject_cast<QLineEdit *>(w))
            out[p.key] = line->text().trimmed();
    }
    return out;
}
void ParamEditor::setParams(const QJsonObject &input) {
    updating = true;
    auto values = ops::defaults(id);
    for (auto it = input.begin(); it != input.end(); ++it)
        values[it.key()] = it.value();
    for (auto it = values.begin(); it != values.end(); ++it) {
        auto w = editors.value(it.key());
        if (auto s = qobject_cast<QSpinBox *>(w))
            s->setValue(it.value().toInt());
        else if (auto d = qobject_cast<QDoubleSpinBox *>(w))
            d->setValue(it.value().toDouble());
        else if (auto c = qobject_cast<QCheckBox *>(w))
            c->setChecked(it.value().toBool());
        else if (auto b = qobject_cast<QToolButton *>(w)) {
            const QRgb rgba = ops::parseColor(it.value().toString());
            b->setProperty("rgba", uint(rgba));
            b->setIcon(colorIcon(rgba));
            b->setText(ops::colorName(rgba));
        } else if (auto combo = qobject_cast<QComboBox *>(w))
            combo->setCurrentText(it.value().toString());
        else if (auto line = qobject_cast<QLineEdit *>(w))
            line->setText(it.value().toString());
    }
    updating = false;
}

OperationDialog::OperationDialog(DocumentSession *s, std::shared_ptr<Document> doc, Canvas *c,
                                 PresetStore *p, std::vector<QRgb> pal, const QStringList &ids,
                                 std::function<QString()> paletteText, QWidget *parent)
    : QDialog(parent), session(s), document(std::move(doc)), canvas(c), presets(p),
      palette(std::move(pal)) {
    setObjectName("operationDialog");
    layer = document->activeLayer();
    auto first = ops::find(ids.value(0));
    setWindowTitle(ids.size() == 1 && first ? first->name : QString("Image operation"));
    auto root = new QVBoxLayout(this);
    auto top = new QHBoxLayout;
    operations = new QComboBox;
    operations->setObjectName("operationSelector");
    for (const auto &id : ids)
        if (auto op = ops::find(id))
            operations->addItem(op->name, id);
    operations->setVisible(ids.size() > 1);
    top->addWidget(operations, 1);
    presetBox = new QComboBox;
    presetBox->setObjectName("operationPresets");
    presetBox->setToolTip("Presets: built-in (◆) and saved");
    auto savePreset = new QPushButton("Save preset…");
    auto deletePreset = new QPushButton("Delete");
    top->addWidget(new QLabel("Preset"));
    top->addWidget(presetBox, 1);
    top->addWidget(savePreset);
    top->addWidget(deletePreset);
    root->addLayout(top);
    auto body = new QHBoxLayout;
    editor = new ParamEditor;
    editor->paletteText = std::move(paletteText);
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(editor);
    scroll->setMinimumWidth(380);
    auto left = new QVBoxLayout;
    left->addWidget(scroll, 1);
    auto extra = new QFormLayout;
    extraForm = extra;
    heightLayer = new QComboBox;
    heightLayer->setObjectName("heightmapLayer");
    for (const auto &l : document->layers())
        heightLayer->addItem(l.name, l.id);
    heightLayer->setToolTip("Layer used when the source is a heightmap or normal-map layer");
    output = new QComboBox;
    output->setObjectName("generatorOutput");
    output->addItems({"New layer", "Replace active layer"});
    extra->addRow("Map layer", heightLayer);
    extra->addRow("Output", output);
    left->addLayout(extra);
    body->addLayout(left, 1);
    auto right = new QVBoxLayout;
    before = new QLabel;
    after = new QLabel;
    for (auto l : {before, after}) {
        l->setMinimumSize(170, 170);
        l->setAlignment(Qt::AlignCenter);
        l->setFrameShape(QFrame::StyledPanel);
    }
    after->setObjectName("operationAfter");
    right->addWidget(new QLabel("Before"));
    right->addWidget(before);
    right->addWidget(new QLabel("After"));
    right->addWidget(after);
    info = new QLabel;
    info->setObjectName("operationInfo");
    info->setWordWrap(true);
    info->setMaximumWidth(200);
    right->addWidget(info);
    busy = new QProgressBar;
    busy->setRange(0, 100);
    busy->setTextVisible(false);
    busy->setMaximumHeight(6);
    right->addWidget(busy);
    right->addStretch();
    body->addLayout(right);
    root->addLayout(body, 1);
    scope = new QLabel;
    scope->setObjectName("operationScope");
    scope->setEnabled(false);
    root->addWidget(scope);
    layerScope = new QComboBox;
    layerScope->setObjectName("operationLayerScope");
    layerScope->addItems({"Active layer", "Selected layers", "All layers"});
    layerScope->setToolTip("Explicit target scope. Hidden layers are included when selected or All "
                           "is chosen. Canvas-size changes require All.");
    root->addWidget(layerScope);
    connect(layerScope, &QComboBox::currentIndexChanged, this, [this] {
        if (layerScope->currentIndex() > 0 && canvas)
            canvas->clearExternalPreview();
        previewToggle->setChecked(layerScope->currentIndex() == 0);
        previewToggle->setEnabled(layerScope->currentIndex() == 0);
        if (layerScope->currentIndex() > 0)
            scope->setText(
                QString(
                    "Scope: %1 layers · thumbnails show the active layer · one undo transaction")
                    .arg(layerScope->currentIndex() == 2 ? qsizetype(document->layers().size())
                                                         : selectedLayers.size()));
        schedule();
    });
    auto buttons = new QHBoxLayout;
    previewToggle = new QCheckBox("Preview on canvas");
    previewToggle->setChecked(true);
    auto reset = new QPushButton("Reset");
    auto cancelButton = new QPushButton("Cancel");
    applyButton = new QPushButton("Apply");
    applyButton->setObjectName("operationApply");
    applyButton->setDefault(true);
    buttons->addWidget(previewToggle);
    buttons->addStretch();
    buttons->addWidget(reset);
    buttons->addWidget(cancelButton);
    buttons->addWidget(applyButton);
    root->addLayout(buttons);
    debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(70);
    connect(debounce, &QTimer::timeout, this, &OperationDialog::compute);
    connect(editor, &ParamEditor::edited, this, &OperationDialog::schedule);
    connect(heightLayer, &QComboBox::currentIndexChanged, this, &OperationDialog::schedule);
    connect(output, &QComboBox::currentIndexChanged, this, &OperationDialog::schedule);
    connect(previewToggle, &QCheckBox::toggled, this, [this](bool on) {
        if (!on && canvas)
            canvas->clearExternalPreview();
        schedule();
    });
    connect(operations, &QComboBox::currentIndexChanged, this, [this] {
        editor->setOperation(operations->currentData().toString());
        const auto op = ops::find(editor->operation());
        if (op && op->changesSize)
            layerScope->setCurrentIndex(2);
        if (op && op->generator)
            layerScope->setCurrentIndex(0);
        const bool maps = op && (op->id == "normal_map" || op->id == "directional_shading");
        heightLayer->setEnabled(maps);
        output->setEnabled(op && op->generator);
        extraForm->setRowVisible(heightLayer, maps);
        extraForm->setRowVisible(output, op && op->generator);
        if (op && op->generator) {
            // Generators default to the document's size.
            auto params = editor->params();
            params["width"] = document->image.width();
            params["height"] = document->image.height();
            editor->setParams(params);
        }
        if (op && op->changesSize && (op->id == "resize_image" || op->id == "resize_canvas")) {
            auto params = editor->params();
            params["width"] = document->image.width();
            params["height"] = document->image.height();
            editor->setParams(params);
            // Keep aspect ratio: width/height follow each other while checked.
            auto w = editor->findChild<QSpinBox *>("param-width"),
                 h = editor->findChild<QSpinBox *>("param-height");
            if (w && h && !aspect) {
                aspect = new QCheckBox("Keep aspect ratio");
                aspect->setObjectName("keepAspect");
                aspect->setChecked(op->id == "resize_image");
                extraForm->addRow(aspect);
            }
            if (w && h) {
                const double ratio =
                    double(document->image.width()) / std::max(1, document->image.height());
                connect(w, &QSpinBox::valueChanged, this, [this, h, ratio](int v) {
                    if (aspect && aspect->isChecked() && !syncingAspect) {
                        syncingAspect = true;
                        h->setValue(std::clamp(int(std::lround(v / ratio)), 1, 4096));
                        syncingAspect = false;
                    }
                });
                connect(h, &QSpinBox::valueChanged, this, [this, w, ratio](int v) {
                    if (aspect && aspect->isChecked() && !syncingAspect) {
                        syncingAspect = true;
                        w->setValue(std::clamp(int(std::lround(v * ratio)), 1, 4096));
                        syncingAspect = false;
                    }
                });
            }
        }
        if (op && op->id == "crop" && document->hasSelection()) {
            const auto b = ops::maskBounds(document->selection());
            editor->setParams(
                {{"x", b.x()}, {"y", b.y()}, {"width", b.width()}, {"height", b.height()}});
        }
        loadPresets();
        schedule();
    });
    connect(presetBox, &QComboBox::activated, this, [this](int i) {
        const auto key = presetBox->itemData(i).toString();
        if (key.startsWith("builtin:")) {
            for (const auto &pr : ops::builtInPresets())
                if (pr.operation == editor->operation() && pr.name == key.mid(8))
                    editor->setParams(pr.params);
        } else if (key.startsWith("user:")) {
            if (auto pr = presets->find("operation", key.mid(5), editor->operation()))
                editor->setParams(pr->data);
        }
        schedule();
    });
    connect(savePreset, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const auto name =
            QInputDialog::getText(this, "Save preset", "Preset name", QLineEdit::Normal, {}, &ok)
                .trimmed();
        if (!ok || name.isEmpty())
            return;
        presets->put({"operation", name, editor->operation(), editor->params()});
        QString error;
        if (!presets->save(&error))
            QMessageBox::warning(this, "Presets", error);
        loadPresets();
        presetBox->setCurrentIndex(presetBox->findData("user:" + name));
    });
    connect(deletePreset, &QPushButton::clicked, this, [this] {
        const auto key = presetBox->currentData().toString();
        if (!key.startsWith("user:"))
            return;
        presets->remove("operation", key.mid(5), editor->operation());
        presets->save();
        loadPresets();
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        editor->setParams(ops::defaults(editor->operation()));
        emit operations->currentIndexChanged(operations->currentIndex());
    });
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(applyButton, &QPushButton::clicked, this, [this] {
        QString error;
        if (applyOperation(&error))
            accept();
        else if (!error.isEmpty() && error != "Operation canceled")
            QMessageBox::warning(this, windowTitle(), error);
    });
    const auto selected = document->layer(layer);
    scope->setText(QString("Scope: %1 · %2")
                       .arg(selected ? "layer “" + selected->name + "”" : QString("composite"))
                       .arg(document->hasSelection()
                                ? QString("inside the %1×%2 selection")
                                      .arg(ops::maskBounds(document->selection()).width())
                                      .arg(ops::maskBounds(document->selection()).height())
                                : QString("whole layer")));
    resize(760, 520);
    emit operations->currentIndexChanged(0);
}
OperationDialog::~OperationDialog() {
    if (cancel)
        cancel->store(true);
    if (canvas)
        canvas->clearExternalPreview();
}
void OperationDialog::loadPresets() {
    presetBox->clear();
    presetBox->addItem("—", QString());
    for (const auto &pr : ops::builtInPresets())
        if (pr.operation == editor->operation())
            presetBox->addItem("◆ " + pr.name, "builtin:" + pr.name);
    for (const auto &name : presets->names("operation", editor->operation()))
        presetBox->addItem(name, "user:" + name);
}
ops::Context OperationDialog::context() const {
    ops::Context ctx;
    ctx.palette = palette;
    return ctx;
}
void OperationDialog::schedule() {
    ++generation; // stale previews are rejected by generation
    applyButton->setEnabled(false);
    if (cancel)
        cancel->store(true);
    debounce->start();
}
void OperationDialog::showThumbs(const QImage &out) {
    after->setPixmap(checkerPixmap(out, after->minimumSize()));
}
void OperationDialog::compute() {
    const auto op = ops::find(editor->operation());
    if (!op)
        return;
    const quint64 gen = generation;
    cancel = std::make_shared<std::atomic_bool>(false);
    const bool docWide = op->changesSize;
    const auto l = document->layer(layer);
    const QImage source = docWide || !l ? document->image : l->pixels;
    before->setPixmap(checkerPixmap(docWide ? document->image : source, before->minimumSize()));
    QImage mask = document->hasSelection() && !docWide ? document->selection() : QImage();
    QImage height;
    if (heightLayer->isEnabled())
        if (auto h = document->layer(QUuid(heightLayer->currentData().toUuid())))
            height = h->pixels;
    ops::Spec spec{op->id, editor->params()};
    const bool shadingLayer =
        op->id == "directional_shading" && spec.params["output"].toString() == "Shading layer";
    auto ctx = context();
    auto token = cancel;
    busy->setRange(0, 0);
    auto watcher = new QFutureWatcher<ops::Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, gen, spec, docWide, shadingLayer, mask] {
                watcher->deleteLater();
                if (gen != generation)
                    return; // a newer parameter set superseded this result
                busy->setRange(0, 100);
                busy->setValue(100);
                const auto r = watcher->result();
                const auto opInfo = ops::find(spec.id);
                if (!r.ok()) {
                    result = {};
                    info->setText("<span style='color:#e57373'>" +
                                  (r.canceled ? QString("Canceled") : r.error.toHtmlEscaped()) +
                                  "</span>");
                    applyButton->setEnabled(false);
                    if (canvas)
                        canvas->clearExternalPreview();
                    return;
                }
                applyButton->setEnabled(true);
                result = r.image;
                resultParams = spec.params;
                readyGeneration = gen;
                QImage shown = result;
                if (!mask.isNull() &&
                    (shadingLayer || (opInfo->generator && output->currentIndex() == 0))) {
                    shown = ops::maskAlpha(shown, mask);
                    result = shown;
                } else if (!mask.isNull() && opInfo->generator) {
                    if (const auto l = document->layer(layer))
                        shown = ops::applyMask(l->pixels, shown, &mask);
                }
                showThumbs(shadingLayer ? document->compositeWithInserted(layer, shown) : shown);
                info->setText(QString("%1 × %2 → %3 × %4%5")
                                  .arg(document->image.width())
                                  .arg(document->image.height())
                                  .arg(r.image.width())
                                  .arg(r.image.height())
                                  .arg(docWide ? "\nApplies to every layer"
                                       : opInfo->generator && output->currentIndex() == 0
                                           ? "\nAdds an editable layer"
                                       : shadingLayer ? "\nAdds a non-destructive shading layer"
                                                      : ""));
                if (!canvas || !previewToggle->isChecked() || docWide)
                    return;
                if (shadingLayer || (opInfo->generator && output->currentIndex() == 0))
                    canvas->setExternalPreview(document->compositeWithInserted(layer, shown));
                else if (shown.size() == document->image.size())
                    canvas->setExternalPreview(document->compositeWith(layer, shown));
            });
    watcher->setFuture(
        QtConcurrent::run([spec, source, mask, height, ctx, token, shadingLayer]() mutable {
            ctx.cancel = token.get();
            ctx.mask = mask.isNull() || shadingLayer ? nullptr : &mask;
            ctx.heightmap = height.isNull() ? nullptr : &height;
            return ops::run(spec, source, ctx);
        }));
}
bool OperationDialog::applyOperation(QString *error) {
    const auto op = ops::find(editor->operation());
    if (!op)
        return false;
    ops::Spec spec{op->id, editor->params()};
    if (!op->changesSize && document->hasSelection() && ops::maskEmpty(document->selection()))
        return true;
    auto ctx = context();
    if (layerScope->currentIndex() > 0 && !op->generator) {
        QVector<QUuid> ids = selectedLayers;
        if (layerScope->currentIndex() == 2) {
            ids.clear();
            for (const auto &l : document->layers())
                ids << l.id;
        }
        return session->applyLayers(document, ids, spec, error, &ctx);
    }
    QImage height;
    if (heightLayer->isEnabled())
        if (auto h = document->layer(QUuid(heightLayer->currentData().toUuid()))) {
            height = h->pixels;
            ctx.heightmap = &height;
        }
    if (op->generator && output->currentIndex() == 0) {
        // Generation already ran on a worker. Reuse that preview rather than
        // recomputing a potentially large texture on the UI thread.
        if (readyGeneration != generation || result.isNull() || resultParams != spec.params) {
            if (error)
                *error = "Wait for the current generator preview to finish";
            return false;
        }
        if (result.size() != document->image.size()) {
            if (error)
                *error = "Set the generator size to the document size to add it as a layer";
            return false;
        }
        return session->addLayer(document, op->name, result, {{"nova.generator", spec.toJson()}},
                                 error);
    }
    const bool shadingLayer =
        op->id == "directional_shading" && spec.params["output"].toString() == "Shading layer";
    if (op->changesSize || op->generator || shadingLayer)
        return session->apply(document, layer, spec, error, &ctx);
    // Same-size layer operation: reuse the finished preview when it is current.
    if (readyGeneration == generation && !result.isNull() && resultParams == spec.params)
        return session->replacePixels(document, layer, result, op->name, error);
    const auto l = document->layer(layer);
    if (!l) {
        if (error)
            *error = "Layer identity is not present";
        return false;
    }
    auto token = std::make_shared<std::atomic_bool>(false);
    QImage mask = document->hasSelection() ? document->selection() : QImage();
    QProgressDialog progress("Applying " + op->name + "…", "Cancel", 0, 100, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(150);
    QFutureWatcher<ops::Result> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    connect(&progress, &QProgressDialog::canceled, this, [token] { token->store(true); });
    const QImage source = l->pixels;
    watcher.setFuture(QtConcurrent::run([spec, source, mask, height, ctx, token]() mutable {
        ctx.cancel = token.get();
        ctx.mask = mask.isNull() ? nullptr : &mask;
        ctx.heightmap = height.isNull() ? nullptr : &height;
        return ops::run(spec, source, ctx);
    }));
    if (!watcher.isFinished())
        loop.exec();
    progress.close();
    const auto r = watcher.result();
    if (!r.ok()) {
        if (error)
            *error = r.canceled ? QString("Operation canceled") : r.error;
        return false;
    }
    return session->replacePixels(document, layer, r.image, op->name, error);
}
void OperationDialog::setSelectedLayers(const QVector<QUuid> &ids, bool selectScope) {
    selectedLayers = ids;
    layerScope->setItemText(1, QString("Selected layers (%1)").arg(ids.size()));
    const auto op = ops::find(editor->operation());
    if (selectScope && op && !op->changesSize && !op->generator)
        layerScope->setCurrentIndex(1);
}

void showTilePreview(QWidget *parent, const QImage &image) {
    QDialog dialog(parent);
    dialog.setObjectName("tilePreviewDialog");
    dialog.setWindowTitle("Tile Preview");
    auto v = new QVBoxLayout(&dialog);
    auto row = new QHBoxLayout;
    auto tiled = new QLabel, offset = new QLabel;
    tiled->setPixmap(checkerPixmap(ops::tile(image, 3, 3), {360, 360}));
    offset->setPixmap(
        checkerPixmap(ops::wrapOffset(image, image.width() / 2, image.height() / 2), {240, 240}));
    auto left = new QVBoxLayout, right = new QVBoxLayout;
    left->addWidget(new QLabel("3 × 3 tiling"));
    left->addWidget(tiled);
    right->addWidget(new QLabel("Half-offset (seams meet in the centre)"));
    right->addWidget(offset);
    right->addStretch();
    row->addLayout(left);
    row->addLayout(right);
    v->addLayout(row);
    const auto report = ops::seamReport(image);
    auto text =
        new QLabel(QString("Edge mismatch (mean absolute RGBA difference, 0 = identical):\n"
                           "left ↔ right %1 (worst row %2) · top ↔ bottom %3 (worst column %4)\n"
                           "Mathematically tileable generators report seams comparable to interior "
                           "neighbours; the Seamless Tile Assist filter is a heuristic repair.")
                       .arg(report.horizontal, 0, 'f', 2)
                       .arg(report.worstRow)
                       .arg(report.vertical, 0, 'f', 2)
                       .arg(report.worstColumn));
    text->setObjectName("seamReport");
    text->setWordWrap(true);
    v->addWidget(text);
    auto close = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    v->addWidget(close);
    if (parent && parent->property("nonModalTesting").toBool()) {
        dialog.show();
        QTimer::singleShot(400, &dialog, &QDialog::reject);
    }
    dialog.exec();
}
} // namespace nova
