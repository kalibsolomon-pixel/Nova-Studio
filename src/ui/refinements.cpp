// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include <QtWidgets>
#include <cmath>
namespace nova {
void Window::shapeParameters() {
    auto canvas = current();
    if (!canvas)
        return;
    canvas->commitFloating();
    auto doc = canvas->document;
    QDialog dialog(this);
    dialog.setObjectName("shapeParameterDialog");
    dialog.setWindowTitle("Shape parameters / presets");
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    layout->addLayout(form);
    auto names = new QComboBox;
    for (int i = 0; i < 6; ++i)
        if (i != int(ops::ShapeKind::Polygon))
            names->addItem(ops::shapeNames()[i], i);
    names->setCurrentIndex(std::max(0, names->findData(int(toolState.shape))));
    names->setObjectName("numericShapeKind");
    form->addRow("Shape", names);
    auto integer = [&](const QString &name, int low, int high, int value) {
        auto s = new QSpinBox;
        s->setObjectName("numericShape" + name);
        s->setRange(low, high);
        s->setValue(value);
        form->addRow(name, s);
        return s;
    };
    auto x = integer("X", -4096, 4096, 0), y = integer("Y", -4096, 4096, 0),
         w = integer("Width", 1, 4096, std::min(16, doc->image.width())),
         h = integer("Height", 1, 4096, std::min(16, doc->image.height()));
    auto thickness = integer("Thickness", 1, 64, toolState.thickness),
         points = integer("Sides / points", 3, 64, toolState.sides);
    auto real = [&](const QString &name, double low, double high, double value) {
        auto s = new QDoubleSpinBox;
        s->setRange(low, high);
        s->setValue(value);
        s->setObjectName("numericShape" + name);
        form->addRow(name, s);
        return s;
    };
    auto rotation = real("Rotation", -360, 360, toolState.rotation),
         radius = real("Corner radius", 0, 512, toolState.radius),
         inner = real("Star inner ratio", .05, .95, toolState.innerRatio);
    auto filled = new QCheckBox("Filled"), smooth = new QCheckBox("Smooth edges"),
         lock = new QCheckBox("Lock aspect ratio");
    filled->setChecked(toolState.filled);
    smooth->setChecked(toolState.smoothShape);
    lock->setObjectName("shapeAspectLock");
    form->addRow(filled);
    form->addRow(smooth);
    form->addRow(lock);
    auto preset = new QComboBox;
    preset->addItem("Choose saved shape…");
    preset->addItems(presets.names("shape"));
    preset->setObjectName("namedShapePresets");
    form->addRow("Preset", preset);
    auto save = new QPushButton("Save named preset…");
    form->addRow(save);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    auto spec = [=] {
        ops::ShapeSpec s;
        s.kind = ops::ShapeKind(names->currentData().toInt());
        s.bounds = {double(x->value()), double(y->value()), double(w->value()), double(h->value())};
        s.thickness = thickness->value();
        s.sides = points->value();
        s.rotation = rotation->value();
        s.radius = radius->value();
        s.innerRatio = inner->value();
        s.filled = filled->isChecked();
        s.smooth = smooth->isChecked();
        return s;
    };
    auto preview = [=, this] {
        const auto l = doc->layer(doc->activeLayer());
        if (!l)
            return;
        auto mask = ops::rasterizeShape(doc->image.size(), spec());
        auto image =
            ops::paintCoverage(l->pixels, mask, toolState.foreground, toolState.shapeOpacity, false,
                               doc->hasSelection() ? &doc->selection() : nullptr);
        canvas->setExternalPreview(doc->compositeWith(l->id, image));
    };
    bool updating = false;
    double aspect = double(w->value()) / h->value();
    connect(lock, &QCheckBox::toggled, &dialog, [&](bool on) {
        if (on)
            aspect = double(w->value()) / h->value();
    });
    connect(w, &QSpinBox::valueChanged, &dialog, [&](int n) {
        if (lock->isChecked() && !updating) {
            updating = true;
            h->setValue(std::max(1, int(std::lround(n / aspect))));
            updating = false;
        }
    });
    connect(h, &QSpinBox::valueChanged, &dialog, [&](int n) {
        if (lock->isChecked() && !updating) {
            updating = true;
            w->setValue(std::max(1, int(std::lround(n * aspect))));
            updating = false;
        }
    });
    for (auto s : dialog.findChildren<QSpinBox *>())
        connect(s, &QSpinBox::valueChanged, &dialog, [&] { preview(); });
    for (auto s : dialog.findChildren<QDoubleSpinBox *>())
        connect(s, &QDoubleSpinBox::valueChanged, &dialog, [&] { preview(); });
    connect(names, &QComboBox::currentIndexChanged, &dialog, [&] { preview(); });
    connect(filled, &QCheckBox::toggled, &dialog, [&] { preview(); });
    connect(smooth, &QCheckBox::toggled, &dialog, [&] { preview(); });
    connect(save, &QPushButton::clicked, &dialog, [&] {
        bool ok = false;
        auto name =
            QInputDialog::getText(&dialog, "Shape preset", "Name", QLineEdit::Normal, {}, &ok)
                .trimmed();
        if (!ok || name.isEmpty())
            return;
        auto s = spec();
        presets.put({"shape",
                     name,
                     {},
                     {{"kind", int(s.kind)},
                      {"x", s.bounds.x()},
                      {"y", s.bounds.y()},
                      {"width", s.bounds.width()},
                      {"height", s.bounds.height()},
                      {"thickness", s.thickness},
                      {"points", s.sides},
                      {"rotation", s.rotation},
                      {"radius", s.radius},
                      {"inner", s.innerRatio},
                      {"filled", s.filled},
                      {"smooth", s.smooth}}});
        QString error;
        if (!presets.save(&error))
            statusBar()->showMessage(error, 6000);
        else {
            QSignalBlocker b(preset);
            preset->clear();
            preset->addItem("Choose saved shape…");
            preset->addItems(presets.names("shape"));
        }
    });
    connect(preset, &QComboBox::activated, &dialog, [&](int index) {
        if (index < 1)
            return;
        auto p = presets.find("shape", preset->currentText());
        if (!p)
            return;
        auto o = p->data;
        names->setCurrentIndex(std::max(0, names->findData(o["kind"].toInt())));
        x->setValue(o["x"].toInt());
        y->setValue(o["y"].toInt());
        w->setValue(o["width"].toInt(16));
        h->setValue(o["height"].toInt(16));
        thickness->setValue(o["thickness"].toInt(1));
        points->setValue(o["points"].toInt(5));
        rotation->setValue(o["rotation"].toDouble());
        radius->setValue(o["radius"].toDouble());
        inner->setValue(o["inner"].toDouble(.5));
        filled->setChecked(o["filled"].toBool());
        smooth->setChecked(o["smooth"].toBool());
        preview();
    });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
        QString error;
        auto s = spec();
        if (session.shape(doc, doc->activeLayer(), s, toolState.foreground, toolState.shapeOpacity,
                          false, &error)) {
            toolState.shape = s.kind;
            toolState.filled = s.filled;
            toolState.smoothShape = s.smooth;
            toolState.thickness = s.thickness;
            toolState.sides = s.sides;
            toolState.rotation = s.rotation;
            toolState.radius = s.radius;
            toolState.innerRatio = s.innerRatio;
            toolState.touch();
            dialog.accept();
        } else
            statusBar()->showMessage(error, 6000);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    preview();
    dialog.exec();
    canvas->clearExternalPreview();
    refresh();
}
void Window::stampLibrary() {
    QDialog dialog(this);
    dialog.setObjectName("stampLibraryDialog");
    dialog.setWindowTitle("Original geometric stamps");
    auto layout = new QVBoxLayout(&dialog);
    auto list = new QListWidget;
    list->setIconSize({64, 64});
    layout->addWidget(list);
    QVector<QImage> stamps;
    for (int i = 0; i < 4; ++i) {
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                if (i == 0 && (std::abs(x - 7) + std::abs(y - 7) < 7))
                    image.setPixelColor(x, y, QColor(80 + x * 10, 90 + y * 9, 210, 255));
                if (i == 1 && (x % 8 != 0 && y % 5 != 0))
                    image.setPixelColor(x, y, QColor(170 + y * 3, 75 + x * 3, 65, 255));
                if (i == 2 && ((x / 2 + y / 2) % 2 == 0))
                    image.setPixelColor(x, y, QColor(110, 190, 140, 200));
                if (i == 3 && ((x - 7) * (x - 7) + (y - 7) * (y - 7) < 48) &&
                    ((x - 7) * (x - 7) + (y - 7) * (y - 7) > 24))
                    image.setPixelColor(x, y, QColor(245, 205, 85, 255));
            }
        stamps << image;
        auto item = new QListWidgetItem(
            QIcon(QPixmap::fromImage(
                image.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::FastTransformation))),
            QStringList{"Prism diamond", "Brickwork", "Soft checker", "Golden ring"}[i], list);
        item->setToolTip("Original procedural artwork · CC0-1.0");
    }
    list->setCurrentRow(0);
    auto use = new QPushButton("Use stamp");
    layout->addWidget(use);
    connect(use, &QPushButton::clicked, &dialog, [&] {
        if (list->currentRow() < 0)
            return;
        auto &b = toolState.brush(Tool::Brush);
        b.stamp = stamps[list->currentRow()];
        b.sourceColors = true;
        b.height = 0;
        b.shape = ops::BrushShape::Custom;
        toolState.setTool(Tool::Brush);
        toolState.touch();
        dialog.accept();
    });
    dialog.resize(340, 410);
    dialog.exec();
}
} // namespace nova
