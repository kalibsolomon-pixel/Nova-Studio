// SPDX-License-Identifier: GPL-3.0-or-later
#include "panels.h"
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace nova {
// Compact palette swatches; click = foreground, right-click = background,
// double-click = edit (user palettes), context menu for removal.
class SwatchGrid : public QWidget {
  public:
    std::vector<QRgb> colors;
    std::function<void(int, bool)> picked;
    std::function<void(int)> edit, removeAt;
    bool editable = false;
    int cell = 16;
    using QWidget::QWidget;
    int columns() const { return std::max(1, (width() - 2) / cell); }
    QSize sizeHint() const override {
        const int rows = int((colors.size() + 9) / 10);
        return {10 * cell + 2, std::max(1, rows) * cell + 2};
    }
    int heightForWidth(int w) const override {
        const int cols = std::max(1, (w - 2) / cell);
        return int((colors.size() + cols - 1) / cols) * cell + 2;
    }
    bool hasHeightForWidth() const override { return true; }
    int indexAt(QPoint p) const {
        const int i = (p.y() / cell) * columns() + p.x() / cell;
        return p.x() / cell < columns() && i >= 0 && i < int(colors.size()) ? i : -1;
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const int cols = columns();
        for (int i = 0; i < int(colors.size()); ++i) {
            const QRect r((i % cols) * cell, (i / cols) * cell, cell - 1, cell - 1);
            p.fillRect(r, QColor(60, 60, 60));
            p.fillRect(r.adjusted(0, 0, -r.width() / 2, -r.height() / 2), QColor(90, 90, 90));
            p.fillRect(r.adjusted(r.width() / 2, r.height() / 2, 0, 0), QColor(90, 90, 90));
            p.fillRect(r, QColor::fromRgba(colors[size_t(i)]));
        }
    }
    void mousePressEvent(QMouseEvent *e) override {
        const int i = indexAt(e->position().toPoint());
        if (i < 0)
            return;
        if (e->button() == Qt::RightButton && editable && (e->modifiers() & Qt::ControlModifier)) {
            if (removeAt)
                removeAt(i);
        } else if (picked)
            picked(i, e->button() == Qt::RightButton);
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override {
        const int i = indexAt(e->position().toPoint());
        if (i >= 0 && editable && edit)
            edit(i);
    }
    bool event(QEvent *e) override {
        if (e->type() == QEvent::ToolTip) {
            auto he = static_cast<QHelpEvent *>(e);
            const int i = indexAt(he->pos());
            setToolTip(i < 0 ? QString()
                             : ops::colorName(colors[size_t(i)]) +
                                   "\nClick: foreground · Right-click: background" +
                                   (editable ? "\nDouble-click: edit · Ctrl+right-click: remove" : ""));
        }
        return QWidget::event(e);
    }
};

namespace {
QIcon swatchIcon(QRgb c, int size = 30) {
    QPixmap pm(size, size);
    QPainter p(&pm);
    p.fillRect(pm.rect(), QColor(70, 70, 70));
    p.fillRect(QRect(0, 0, size / 2, size / 2), QColor(110, 110, 110));
    p.fillRect(QRect(size / 2, size / 2, size / 2, size / 2), QColor(110, 110, 110));
    p.fillRect(pm.rect(), QColor::fromRgba(c));
    p.setPen(Qt::black);
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    return QIcon(pm);
}
} // namespace

ColorPanel::ColorPanel(ToolState *s, PaletteStore *p, QWidget *parent) : QWidget(parent), state(s), store(p) {
    setObjectName("colorPanel");
    auto v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(6);
    auto top = new QHBoxLayout;
    fg = new QToolButton;
    fg->setObjectName("foregroundSwatch");
    fg->setIconSize({30, 30});
    fg->setToolTip("Foreground colour (click to choose)");
    bg = new QToolButton;
    bg->setObjectName("backgroundSwatch");
    bg->setIconSize({22, 22});
    bg->setToolTip("Background colour (click to choose)");
    auto swap = new QToolButton;
    swap->setText("⇄");
    swap->setToolTip("Swap colours (X)");
    auto reset = new QToolButton;
    reset->setText("◩");
    reset->setToolTip("Default black/white (D)");
    top->addWidget(fg);
    top->addWidget(bg);
    top->addWidget(swap);
    top->addWidget(reset);
    top->addStretch();
    hex = new QLineEdit;
    hex->setObjectName("colorHex");
    hex->setToolTip("Hexadecimal colour: #RRGGBB or #AARRGGBB");
    hex->setMaximumWidth(110);
    top->addWidget(hex);
    v->addLayout(top);
    auto grid2 = new QGridLayout;
    grid2->setHorizontalSpacing(4);
    grid2->setVerticalSpacing(2);
    model = new QComboBox;
    model->addItems({"HSV", "HSL"});
    model->setToolTip("Hue/saturation model for the sliders (encoded sRGB)");
    grid2->addWidget(model, 0, 0, 1, 3);
    const char *names[3] = {"H", "S", "V"};
    for (int i = 0; i < 3; ++i) {
        auto l = new QLabel(names[i]);
        l->setObjectName(QString("channelLabel%1").arg(i));
        slider[i] = new QSlider(Qt::Horizontal);
        channel[i] = new QSpinBox;
        slider[i]->setRange(0, i == 0 ? 359 : 100);
        channel[i]->setRange(0, i == 0 ? 359 : 100);
        grid2->addWidget(l, i + 1, 0);
        grid2->addWidget(slider[i], i + 1, 1);
        grid2->addWidget(channel[i], i + 1, 2);
        connect(slider[i], &QSlider::valueChanged, channel[i], &QSpinBox::setValue);
        connect(channel[i], &QSpinBox::valueChanged, slider[i], &QSlider::setValue);
        connect(channel[i], &QSpinBox::valueChanged, this, [this] {
            if (updating)
                return;
            const double h = channel[0]->value(), a = channel[1]->value() / 100.0, b = channel[2]->value() / 100.0;
            push(model->currentIndex() == 0 ? ops::fromHsv({h, a, b}, alpha->value())
                                            : ops::fromHsl({h, a, b}, alpha->value()));
        });
    }
    auto rgbRow = new QHBoxLayout;
    const char *rgbNames[3] = {"R", "G", "B"};
    for (int i = 0; i < 3; ++i) {
        rgb[i] = new QSpinBox;
        rgb[i]->setRange(0, 255);
        rgb[i]->setObjectName(QString("rgb%1").arg(rgbNames[i]));
        rgbRow->addWidget(new QLabel(rgbNames[i]));
        rgbRow->addWidget(rgb[i]);
        connect(rgb[i], &QSpinBox::valueChanged, this, [this] {
            if (!updating)
                push(qRgba(rgb[0]->value(), rgb[1]->value(), rgb[2]->value(), alpha->value()));
        });
    }
    alpha = new QSpinBox;
    alpha->setRange(0, 255);
    alpha->setObjectName("rgbA");
    rgbRow->addWidget(new QLabel("A"));
    rgbRow->addWidget(alpha);
    connect(alpha, &QSpinBox::valueChanged, this, [this](int a) {
        if (!updating)
            push(qRgba(qRed(state->foreground), qGreen(state->foreground), qBlue(state->foreground), a));
    });
    grid2->addLayout(rgbRow, 4, 0, 1, 3);
    v->addLayout(grid2);
    v->addWidget(new QLabel("Recent"));
    recentRow = new QWidget;
    auto rl = new QHBoxLayout(recentRow);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(2);
    v->addWidget(recentRow);
    auto pRow = new QHBoxLayout;
    paletteBox = new QComboBox;
    paletteBox->setObjectName("paletteSelector");
    paletteBox->setToolTip("Active palette (also used by palette lock and Map to Palette)");
    auto menuButton = new QToolButton;
    menuButton->setText("☰");
    menuButton->setObjectName("paletteMenu");
    menuButton->setToolTip("Palette commands");
    menuButton->setPopupMode(QToolButton::InstantPopup);
    auto menu = new QMenu(menuButton);
    menuButton->setMenu(menu);
    pRow->addWidget(paletteBox, 1);
    pRow->addWidget(menuButton);
    v->addLayout(pRow);
    grid = new SwatchGrid;
    grid->setObjectName("paletteSwatches");
    v->addWidget(grid);
    v->addStretch();

    connect(fg, &QToolButton::clicked, this, [this] {
        auto c = QColorDialog::getColor(QColor::fromRgba(state->foreground), this, "Foreground", QColorDialog::ShowAlphaChannel);
        if (c.isValid())
            state->setForeground(c.rgba());
    });
    connect(bg, &QToolButton::clicked, this, [this] {
        auto c = QColorDialog::getColor(QColor::fromRgba(state->background), this, "Background", QColorDialog::ShowAlphaChannel);
        if (c.isValid())
            state->setBackground(c.rgba());
    });
    connect(swap, &QToolButton::clicked, state, &ToolState::swapColors);
    connect(reset, &QToolButton::clicked, state, &ToolState::resetColors);
    connect(model, &QComboBox::currentIndexChanged, this, [this] {
        findChild<QLabel *>("channelLabel2")->setText(model->currentIndex() == 0 ? "V" : "L");
        pull();
    });
    connect(hex, &QLineEdit::editingFinished, this, [this] {
        bool ok = false;
        auto text = hex->text().trimmed();
        if (!text.startsWith('#'))
            text.prepend('#');
        const QRgb c = ops::parseColor(text, &ok);
        if (ok)
            state->setForeground(c);
        else
            pull();
    });
    connect(state, &ToolState::colorsChanged, this, [this] {
        pull();
        rebuildRecent();
    });
    connect(paletteBox, &QComboBox::currentIndexChanged, this, [this] {
        const auto colors = activePalette();
        grid->colors = colors;
        grid->editable = editable() != nullptr;
        grid->updateGeometry();
        grid->update();
        state->palette = colors;
        state->touch();
    });
    grid->picked = [this](int i, bool background) {
        const QRgb c = grid->colors[size_t(i)];
        if (background)
            state->setBackground(c);
        else
            state->setForeground(c);
    };
    grid->edit = [this](int i) {
        auto p = editable();
        if (!p)
            return;
        auto c = QColorDialog::getColor(QColor::fromRgba(p->colors[size_t(i)]), this, "Edit palette colour",
                                        QColorDialog::ShowAlphaChannel);
        if (c.isValid()) {
            p->colors[size_t(i)] = c.rgba();
            persist();
            rebuildPalettes(p->name);
        }
    };
    grid->removeAt = [this](int i) {
        if (auto p = editable()) {
            p->colors.erase(p->colors.begin() + i);
            persist();
            rebuildPalettes(p->name);
        }
    };
    auto requireEditable = [this]() -> Palette * {
        auto p = editable();
        if (!p)
            QMessageBox::information(this, "Built-in palette",
                                     "Built-in palettes are read-only. Duplicate it with “New palette from current” first.");
        return p;
    };
    menu->addAction("New empty palette…", this, [this] {
        bool ok = false;
        auto name = QInputDialog::getText(this, "New palette", "Name", QLineEdit::Normal, "My palette", &ok);
        if (ok)
            createPalette(name, {});
    });
    menu->addAction("New palette from current", this, [this] {
        createPalette(activePaletteName() + " copy", activePalette());
    });
    menu->addAction("Rename…", this, [this, requireEditable] {
        if (auto p = requireEditable()) {
            bool ok = false;
            auto name = QInputDialog::getText(this, "Rename palette", "Name", QLineEdit::Normal, p->name, &ok).trimmed();
            if (ok && !name.isEmpty() && name != p->name) {
                p->name = store->uniqueName(name);
                persist();
                rebuildPalettes(p->name);
            }
        }
    });
    menu->addAction("Delete", this, [this, requireEditable] {
        if (auto p = requireEditable())
            if (QMessageBox::question(this, "Delete palette", "Delete “" + p->name + "”?") == QMessageBox::Yes) {
                const auto name = p->name;
                std::erase_if(store->palettes, [&](const Palette &x) { return x.name == name; });
                persist();
                rebuildPalettes();
            }
    });
    menu->addSeparator();
    menu->addAction("Add foreground colour", this, [this, requireEditable] {
        if (auto p = requireEditable(); p && int(p->colors.size()) < Palette::MaxColors) {
            p->colors.push_back(state->foreground);
            persist();
            rebuildPalettes(p->name);
        }
    });
    menu->addAction("Extract from image…", this, [this] {
        if (!currentImage)
            return;
        const auto image = currentImage();
        if (image.isNull())
            return;
        bool ok = false;
        const int n = QInputDialog::getInt(this, "Extract palette", "Maximum colours", 16, 2, 256, 1, &ok);
        if (ok)
            createPalette("Extracted", ops::extractPalette(image, n));
    });
    auto sort = menu->addMenu("Sort");
    for (auto [label, mode] : {std::pair{"By hue", PaletteSort::Hue}, {"By luminance", PaletteSort::Luminance},
                               {"By saturation", PaletteSort::Saturation}})
        sort->addAction(label, this, [this, requireEditable, mode] {
            if (auto p = requireEditable()) {
                sortPalette(*p, mode);
                persist();
                rebuildPalettes(p->name);
            }
        });
    menu->addAction("Remove duplicates", this, [this, requireEditable] {
        if (auto p = requireEditable()) {
            removeDuplicateColors(*p);
            persist();
            rebuildPalettes(p->name);
        }
    });
    menu->addSeparator();
    menu->addAction("Import…", this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "Import palette", {}, paletteFileFilter());
        if (path.isEmpty())
            return;
        Palette imported;
        QString error;
        if (!importPalette(path, &imported, &error)) {
            QMessageBox::warning(this, "Import failed", error);
            return;
        }
        createPalette(imported.name, imported.colors);
    });
    menu->addAction("Export…", this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "Export palette", activePaletteName() + ".gpl", paletteFileFilter());
        if (path.isEmpty())
            return;
        Palette out{activePaletteName(), activePalette()};
        QString error;
        if (!exportPalette(path, out, &error))
            QMessageBox::warning(this, "Export failed", error);
    });
    menu->addSeparator();
    menu->addAction("Map image to palette…", this, [this] {
        if (mapImage)
            mapImage(activePaletteText());
    });
    menu->addAction("Replace background colour with foreground in image", this, [this] {
        if (replaceInImage)
            replaceInImage(state->background, state->foreground);
    });
    rebuildPalettes();
    pull();
    rebuildRecent();
}
Palette *ColorPanel::editable() { return store->find(paletteBox->currentData().toString()); }
void ColorPanel::persist() {
    QString error;
    if (!store->save(&error))
        QMessageBox::warning(this, "Palette library", "Palettes could not be saved: " + error);
}
QString ColorPanel::activePaletteName() const { return paletteBox->currentData().toString(); }
std::vector<QRgb> ColorPanel::activePalette() const {
    for (const auto &p : store->all())
        if (p.name == activePaletteName())
            return p.colors;
    return {};
}
QString ColorPanel::activePaletteText() const {
    QStringList parts;
    for (auto c : activePalette())
        parts.append(ops::colorName(c));
    return parts.join(',');
}
void ColorPanel::selectPalette(const QString &name) {
    const int i = paletteBox->findData(name);
    if (i >= 0)
        paletteBox->setCurrentIndex(i);
}
bool ColorPanel::createPalette(const QString &name, const std::vector<QRgb> &colors) {
    Palette p;
    p.name = store->uniqueName(name);
    p.colors = colors;
    if (int(p.colors.size()) > Palette::MaxColors)
        p.colors.resize(Palette::MaxColors);
    store->palettes.push_back(p);
    persist();
    rebuildPalettes(p.name);
    return true;
}
void ColorPanel::rebuildPalettes(const QString &select) {
    const auto keep = select.isEmpty() ? activePaletteName() : select;
    QSignalBlocker block(paletteBox);
    paletteBox->clear();
    for (const auto &p : store->all())
        paletteBox->addItem(QString("%1%2 (%3)").arg(p.builtIn ? "◆ " : "").arg(p.name).arg(p.colors.size()), p.name);
    const int i = paletteBox->findData(keep);
    paletteBox->setCurrentIndex(i >= 0 ? i : 0);
    block.unblock();
    emit paletteBox->currentIndexChanged(paletteBox->currentIndex());
}
void ColorPanel::rebuildRecent() {
    auto layout = recentRow->layout();
    while (auto item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const auto &name : state->recent.mid(0, 12)) {
        auto b = new QToolButton;
        b->setIconSize({14, 14});
        b->setAutoRaise(true);
        const QRgb c = ops::parseColor(name);
        b->setIcon(swatchIcon(c, 14));
        b->setToolTip(name);
        connect(b, &QToolButton::clicked, this, [this, c] { state->setForeground(c); });
        layout->addWidget(b);
    }
    static_cast<QHBoxLayout *>(layout)->addStretch();
}
void ColorPanel::pull() {
    updating = true;
    const QRgb c = state->foreground;
    fg->setIcon(swatchIcon(c, 30));
    bg->setIcon(swatchIcon(state->background, 22));
    if (!hex->hasFocus())
        hex->setText(ops::colorName(c));
    // Keep the hue when the colour is grey so dragging saturation is stable.
    if (model->currentIndex() == 0) {
        const auto h = ops::toHsv(c);
        if (h.s > 0)
            channel[0]->setValue(int(std::lround(h.h)) % 360);
        channel[1]->setValue(int(std::lround(h.s * 100)));
        channel[2]->setValue(int(std::lround(h.v * 100)));
    } else {
        const auto h = ops::toHsl(c);
        if (h.s > 0)
            channel[0]->setValue(int(std::lround(h.h)) % 360);
        channel[1]->setValue(int(std::lround(h.s * 100)));
        channel[2]->setValue(int(std::lround(h.l * 100)));
    }
    rgb[0]->setValue(qRed(c));
    rgb[1]->setValue(qGreen(c));
    rgb[2]->setValue(qBlue(c));
    alpha->setValue(qAlpha(c));
    updating = false;
}
void ColorPanel::push(QRgb c) {
    state->foreground = c;
    emit state->colorsChanged();
    state->touch();
}

HistoryPanel::HistoryPanel(DocumentSession *s, QWidget *parent) : QWidget(parent), session(s) {
    auto v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    list = new QListWidget;
    list->setObjectName("historyList");
    list->setToolTip("Click a step to undo or redo to it");
    v->addWidget(list);
    connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (updating || !document || row < 0)
            return;
        session->jump(document, row); // row 0 = original, row n = after n steps
    });
}
void HistoryPanel::setDocument(const std::shared_ptr<Document> &doc) {
    document = doc;
    refresh();
}
void HistoryPanel::refresh() {
    updating = true;
    QStringList labels;
    if (document) {
        labels.append("◇ Start of history");
        labels += document->historyLabels();
    }
    bool same = list->count() == labels.size();
    for (int i = 0; same && i < labels.size(); ++i)
        same = list->item(i)->text() == labels[i];
    if (!same) { // rebuild only when entries changed (never inside our own signal)
        list->clear();
        list->addItems(labels);
    }
    if (document) {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setForeground(i > document->historyCursor() ? QColor(130, 130, 140)
                                                                       : palette().color(QPalette::Text));
        list->setCurrentRow(document->historyCursor());
    }
    updating = false;
}
} // namespace nova
