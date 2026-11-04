// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "session.h"
#include <QWidget>
class QListWidget;
class QSpinBox;
class QSlider;
class QPushButton;
class QLabel;
namespace nova {
class LayersPanel : public QWidget {
    DocumentSession *session;
    std::shared_ptr<Document> document;
    QListWidget *list;
    QSpinBox *opacity;
    QSlider *opacitySlider;
    QLabel *empty;
    QPushButton *add, *remove, *duplicate, *rename, *up, *down, *transform;
    void run(LayerCommand command, int value = 0, const QString &name = {});

  public:
    std::function<void()> transformSelected;
    QVector<QUuid> selectedLayers() const;
    explicit LayersPanel(DocumentSession *session, QWidget *parent = nullptr);
    void setDocument(const std::shared_ptr<Document> &doc);
    void refresh();
    // Layer commands shared by the panel buttons and the Layer menu.
    void addLayer();
    void duplicateLayer();
    void deleteLayer();
    void renameLayer();
    void raiseLayer();
    void lowerLayer();
    void refreshIcons();
};
} // namespace nova
