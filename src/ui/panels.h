// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "palette.h"
#include "session.h"
#include "tools.h"
#include <QWidget>
#include <functional>

class QComboBox;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QSlider;
class QToolButton;
namespace nova {
class SwatchGrid;
// Unified colour panel: FG/BG, HSV/HSL/RGB/hex/alpha, recent colours and the
// persistent palette library (create/rename/delete/import/export/extract/sort).
class ColorPanel : public QWidget {
    Q_OBJECT
    ToolState *state;
    PaletteStore *store;
    QToolButton *fg, *bg;
    QComboBox *model, *paletteBox;
    QSpinBox *channel[3], *rgb[3], *alpha;
    QSlider *slider[3];
    QLineEdit *hex;
    QWidget *recentRow;
    SwatchGrid *grid;
    bool updating = false;
    void pull();
    void push(QRgb c);
    void rebuildPalettes(const QString &select = {});
    void rebuildRecent();
    Palette *editable();
    void persist();

  public:
    ColorPanel(ToolState *state, PaletteStore *store, QWidget *parent = nullptr);
    std::function<QImage()> currentImage;                  // for "Extract from image"
    std::function<void(const QString &palette)> mapImage;  // "Map image to palette"
    std::function<void(QRgb from, QRgb to)> replaceInImage; // "Replace colour in image"
    QString activePaletteName() const;
    std::vector<QRgb> activePalette() const;
    QString activePaletteText() const;
    void selectPalette(const QString &name);
    bool createPalette(const QString &name, const std::vector<QRgb> &colors);
};
class HistoryPanel : public QWidget {
    DocumentSession *session;
    std::shared_ptr<Document> document;
    QListWidget *list;
    bool updating = false;

  public:
    explicit HistoryPanel(DocumentSession *session, QWidget *parent = nullptr);
    void setDocument(const std::shared_ptr<Document> &doc);
    void refresh();
};
} // namespace nova
