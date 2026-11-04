// SPDX-License-Identifier: GPL-3.0-or-later
#include "layers.h"
#include "theme.h"
#include <QtWidgets>
namespace nova {
LayersPanel::LayersPanel(DocumentSession *s, QWidget *parent) : QWidget(parent), session(s) {
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::SpaceL, ui::SpaceM, ui::SpaceL, ui::SpaceM);
    layout->setSpacing(ui::SpaceM);
    // Opacity first, as in most layer panels: it applies to the active layer.
    auto row = new QHBoxLayout;
    row->setSpacing(ui::SpaceM);
    auto opacityLabel = new QLabel("Opacity");
    opacityLabel->setProperty("role", "hint");
    row->addWidget(opacityLabel);
    opacitySlider = new QSlider(Qt::Horizontal);
    opacitySlider->setObjectName("layerOpacitySlider");
    opacitySlider->setRange(0, 255);
    opacitySlider->setAccessibleName("Layer opacity");
    row->addWidget(opacitySlider, 1);
    opacity = new QSpinBox;
    opacity->setObjectName("layerOpacity");
    opacity->setRange(0, 255);
    opacity->setAccessibleName("Layer opacity value");
    opacity->setToolTip("Active layer opacity (0–255)");
    opacity->setKeyboardTracking(false);
    opacity->setFixedWidth(64);
    row->addWidget(opacity);
    opacityLabel->setBuddy(opacity);
    layout->addLayout(row);
    list = new QListWidget;
    list->setObjectName("layerList");
    list->setAccessibleName("Layers");
    list->setMinimumHeight(90);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setEditTriggers(QAbstractItemView::EditKeyPressed);
    list->setToolTip("Top row paints over lower rows. Check a layer to show it. F2 renames.\n"
                     "Ctrl/Shift-click selects several layers for Transform.");
    layout->addWidget(list, 1);
    empty = ui::hint("Open a texture to see its layers.");
    empty->setObjectName("layersEmpty");
    layout->addWidget(empty);
    // Compact command strip under the list.
    auto buttons = new QHBoxLayout;
    buttons->setSpacing(ui::SpaceXS);
    auto button = [&](const QString &icon, const QString &tip, const QString &name) {
        auto b = new QPushButton;
        b->setObjectName(name);
        b->setProperty("role", "icon");
        b->setProperty("novaIcon", icon);
        b->setIcon(ui::lineIcon(icon));
        b->setIconSize({ui::IconSize, ui::IconSize});
        b->setToolTip(tip);
        b->setAccessibleName(tip.section(" (", 0, 0));
        buttons->addWidget(b);
        return b;
    };
    add = button("add", "New layer (Ctrl+Shift+N)", "addLayer");
    duplicate = button("duplicate", "Duplicate layer", "duplicateLayer");
    remove = button("delete", "Delete layer", "deleteLayer");
    buttons->addSpacing(ui::SpaceS);
    up = button("up", "Raise layer (Ctrl+])", "raiseLayer");
    down = button("down", "Lower layer (Ctrl+[)", "lowerLayer");
    rename = button("rename", "Rename layer (F2 in the list)", "renameLayer");
    buttons->addStretch();
    transform = button("transform", "Transform selected layers… (move, flip, rotate, scale or recolour as one step)",
                       "transformSelectedLayers");
    layout->addLayout(buttons);
    connect(transform, &QPushButton::clicked, this, [this] { if (transformSelected) transformSelected(); });
    connect(add, &QPushButton::clicked, this, &LayersPanel::addLayer);
    connect(duplicate, &QPushButton::clicked, this, &LayersPanel::duplicateLayer);
    connect(remove, &QPushButton::clicked, this, &LayersPanel::deleteLayer);
    connect(rename, &QPushButton::clicked, this, &LayersPanel::renameLayer);
    connect(up, &QPushButton::clicked, this, &LayersPanel::raiseLayer);
    connect(down, &QPushButton::clicked, this, &LayersPanel::lowerLayer);
    connect(opacity, &QSpinBox::valueChanged, this, [this](int value) {
        { QSignalBlocker block(opacitySlider); opacitySlider->setValue(value); }
        run(LayerCommand::Opacity, value);
    });
    // The slider previews its position and commits one opacity edit on release.
    connect(opacitySlider, &QSlider::sliderMoved, this, [this](int value) {
        QSignalBlocker block(opacity);
        opacity->setValue(value);
    });
    auto commitSlider = [this] {
        if (!document || opacitySlider->isSliderDown())
            return;
        for (const auto &layer : document->layers())
            if (layer.id == document->activeLayer() && layer.opacity != opacitySlider->value())
                run(LayerCommand::Opacity, opacitySlider->value());
    };
    connect(opacitySlider, &QSlider::sliderReleased, this, commitSlider);
    connect(opacitySlider, &QSlider::actionTriggered, this, [this, commitSlider](int action) {
        if (action != QAbstractSlider::SliderMove) // keyboard / page steps commit immediately
            QTimer::singleShot(0, this, commitSlider);
    });
    connect(list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (!item || !document)
            return;
        QString error;
        session->edit(document, {LayerCommand::Select, QUuid(item->data(Qt::UserRole).toString())},
                      &error);
    });
    connect(list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (!document || !item)
            return;
        const auto id = QUuid(item->data(Qt::UserRole).toString());
        for (const auto &layer : document->layers())
            if (layer.id == id) {
                LayerEdit edit{LayerCommand::Visibility, id, {}, item->checkState() == Qt::Checked};
                if (layer.name != item->text())
                    edit = {LayerCommand::Rename, id, item->text()};
                QString error;
                if (!session->edit(document, edit, &error)) {
                    setToolTip(error);
                    refresh();
                }
                break;
            }
    });
    connect(session, &DocumentSession::changed, this, [this](QUuid id) {
        if (document && id == document->id())
            refresh();
    });
    setDocument({});
}
void LayersPanel::addLayer() {
    if (document)
        run(LayerCommand::Add, 0, "Layer " + QString::number(document->layers().size() + 1));
}
void LayersPanel::duplicateLayer() { run(LayerCommand::Duplicate); }
void LayersPanel::deleteLayer() { run(LayerCommand::Delete); }
void LayersPanel::renameLayer() {
    if (document && list->currentItem()) {
        list->setFocus();
        list->editItem(list->currentItem());
    }
}
void LayersPanel::raiseLayer() {
    if (document && list->currentRow() > 0)
        run(LayerCommand::Move, int(document->layers().size()) - list->currentRow());
}
void LayersPanel::lowerLayer() {
    if (document && list->currentRow() >= 0 && list->currentRow() < list->count() - 1)
        run(LayerCommand::Move, int(document->layers().size()) - list->currentRow() - 2);
}
void LayersPanel::refreshIcons() {
    for (auto b : findChildren<QPushButton *>())
        if (b->property("novaIcon").isValid())
            b->setIcon(ui::lineIcon(b->property("novaIcon").toString()));
}
void LayersPanel::run(LayerCommand command, int value, const QString &name) {
    if (!document)
        return;
    QString error;
    if (!session->edit(document, {command, document->activeLayer(), name, value}, &error)) {
        setToolTip(error);
        QToolTip::showText(mapToGlobal(rect().center()), error, this);
    }
}
void LayersPanel::setDocument(const std::shared_ptr<Document> &doc) {
    if(document!=doc) { QSignalBlocker blocked(list); list->clear(); }
    document = doc;
    refresh();
}
void LayersPanel::refresh() {
    QSignalBlocker blocked(list), blockedOpacity(opacity), blockedSlider(opacitySlider);
    const bool open = bool(document);
    for (QWidget *w : std::initializer_list<QWidget *>{list, opacity, opacitySlider, add, remove, duplicate, rename, up, down, transform})
        w->setEnabled(open);
    list->setVisible(open);
    empty->setVisible(!open);
    if (!document) {
        list->clear();
        return;
    }
    const auto &layers = document->layers();
    const auto selectedIds = selectedLayers();
    bool rebuild = list->count() != int(layers.size());
    if (!rebuild)
        for (int row = 0; row < list->count(); ++row)
            if (QUuid(list->item(row)->data(Qt::UserRole).toString()) !=
                layers[layers.size() - 1 - row].id)
                rebuild = true;
    if (rebuild)
        list->clear();
    int selected = -1;
    for (int row = 0; row < int(layers.size()); ++row) {
        const auto &layer = layers[layers.size() - 1 - row];
        auto item = rebuild ? new QListWidgetItem(list) : list->item(row);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable |
                       Qt::ItemIsEditable);
        item->setData(Qt::UserRole, layer.id.toString());
        if (item->text() != layer.name)
            item->setText(layer.name);
        item->setCheckState(layer.visible ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(layer.visible ? "Visible" : "Hidden — show this layer before drawing");
        if (layer.id == document->activeLayer()) {
            selected = row;
            opacity->setValue(layer.opacity);
            opacitySlider->setValue(layer.opacity);
        }
        item->setSelected(selectedIds.contains(layer.id) || (selectedIds.isEmpty() && layer.id==document->activeLayer()));
    }
    list->setCurrentRow(selected, QItemSelectionModel::NoUpdate);
    remove->setEnabled(layers.size() > 1);
    add->setEnabled(layers.size() < Document::LayerLimit);
    duplicate->setEnabled(layers.size() < Document::LayerLimit);
    up->setEnabled(selected > 0);
    down->setEnabled(selected >= 0 && selected < list->count() - 1);
}
QVector<QUuid> LayersPanel::selectedLayers() const {
    QVector<QUuid> out;
    for(auto item:list->selectedItems())out<<QUuid(item->data(Qt::UserRole).toString());
    return out;
}
} // namespace nova
