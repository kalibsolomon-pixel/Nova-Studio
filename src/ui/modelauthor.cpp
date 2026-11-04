// SPDX-License-Identifier: GPL-3.0-or-later
#include "modelauthor.h"
#include "creation.h"
#include <QCryptographicHash>
#include <QBuffer>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtWidgets>
#include <cmath>
namespace nova {
namespace {
QVector3D vector(const QJsonValue &v, QVector3D fallback = {}) {
    auto a = v.toArray();
    return a.size() == 3
               ? QVector3D(float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble()))
               : fallback;
}
mc::ElementTransform readTransform(const QJsonObject &e) {
    mc::ElementTransform t;
    t.from = vector(e["from"]);
    t.to = vector(e["to"], {16, 16, 16});
    auto r = e["rotation"].toObject();
    t.rotation = e.contains("rotation");
    t.origin = vector(r["origin"], {8, 8, 8});
    t.rescale = r["rescale"].toBool();
    if (r.contains("axis")) {
        int axis = QStringList{"x", "y", "z"}.indexOf(r["axis"].toString());
        if (axis >= 0)
            t.angles[axis] = float(r["angle"].toDouble());
    } else
        t.angles = {float(r["x"].toDouble()), float(r["y"].toDouble()), float(r["z"].toDouble())};
    return t;
}
} // namespace
ElementHandles::ElementHandles(QWidget *p) : QWidget(p) {
    setObjectName("elementHandles");
    setMinimumSize(230, 200);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName("Element transform handles");
    setToolTip("Drag box to move; use Resize or Rotate for corner/rotation handles. Arrows nudge; "
               "Escape cancels drag.");
}
double ElementHandles::unit() const { return std::min(width() - 32, height() - 32) / 48.; }
QPointF ElementHandles::project(QVector3D p) const {
    const int a = plane == 2 ? 1 : 0, b = plane == 0 ? 1 : 2;
    return {16 + (p[a] + 16) * unit(), height() - 16 - (p[b] + 16) * unit()};
}
QRectF ElementHandles::box() const {
    return QRectF(project(value.from), project(value.to)).normalized();
}
void ElementHandles::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), palette().base());
    p.setPen(palette().mid().color());
    for (int n = -16; n <= 32; n += 4) {
        auto x = 16 + (n + 16) * unit(), y = height() - 16 - (n + 16) * unit();
        p.drawLine(QPointF(x, 16), QPointF(x, height() - 16));
        p.drawLine(QPointF(16, y), QPointF(width() - 16, y));
    }
    p.setPen(QPen(palette().highlight().color(), 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(box());
    auto c = box().center();
    p.drawLine(c + QPointF(-10, 0), c + QPointF(10, 0));
    p.drawLine(c + QPointF(0, -10), c + QPointF(0, 10));
    p.setBrush(palette().highlight());
    p.drawRect(QRectF(box().topRight() - QPointF(5, 5), QSizeF(10, 10)));
    p.drawEllipse(c + QPointF(0, -35), 5, 5);
    p.setPen(palette().text().color());
    p.drawText(QRect(8, 3, width() - 16, 20), Qt::AlignLeft,
               QStringList{"XY", "XZ", "YZ"}[plane] + " · " +
                   QStringList{"Move", "Resize corner", "Rotate handle"}[mode]);
}
void ElementHandles::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    const auto point = e->position();
    if ((mode == 0 && !box().adjusted(-8, -8, 8, 8).contains(point)) ||
        (mode == 1 && QLineF(point, box().topRight()).length() > 14) ||
        (mode == 2 && QLineF(point, box().center() + QPointF(0, -35)).length() > 14))
        return;
    setFocus();
    pressed = e->position();
    initial = value;
    dragging = true;
    grabMouse();
}
void ElementHandles::mouseMoveEvent(QMouseEvent *e) {
    if (!dragging)
        return;
    const int a = plane == 2 ? 1 : 0, b = plane == 0 ? 1 : 2;
    auto t = initial;
    auto round = [&](double x) { return snap > 0 ? std::round(x / snap) * snap : x; };
    auto delta = e->position() - pressed;
    const auto dx = float(round(delta.x() / unit())), dy = float(round(-delta.y() / unit()));
    if (mode == 0) {
        t.from[a] += dx;
        t.to[a] += dx;
        t.from[b] += dy;
        t.to[b] += dy;
    } else if (mode == 1) {
        t.to[a] += dx;
        t.to[b] += dy;
    } else {
        const auto c = box().center();
        auto start = pressed - c, now = e->position() - c;
        auto angle = (std::atan2(now.y(), now.x()) - std::atan2(start.y(), start.x())) * 180 /
                     3.141592653589793;
        t.rotation = true;
        t.angles[3 - a - b] -= float(snap > 0 ? std::round(angle / 5) * 5 : angle);
    }
    if (changed)
        changed(t);
}
void ElementHandles::mouseReleaseEvent(QMouseEvent *) {
    if (dragging) {
        dragging = false;
        releaseMouse();
    }
}
void ElementHandles::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape && dragging) {
        dragging = false;
        releaseMouse();
        if (changed)
            changed(initial);
        return;
    }
    int a = plane == 2 ? 1 : 0, b = plane == 0 ? 1 : 2;
    auto t = value;
    float step = float(snap > 0 ? snap : 0.1);
    int axis = -1;
    float d = 0;
    if (e->key() == Qt::Key_Left) {
        axis = a;
        d = -step;
    }
    if (e->key() == Qt::Key_Right) {
        axis = a;
        d = step;
    }
    if (e->key() == Qt::Key_Up) {
        axis = b;
        d = step;
    }
    if (e->key() == Qt::Key_Down) {
        axis = b;
        d = -step;
    }
    if (axis >= 0) {
        t.from[axis] += d;
        t.to[axis] += d;
        if (changed)
            changed(t);
        e->accept();
    } else
        QWidget::keyPressEvent(e);
}
ModelAuthorDialog::ModelAuthorDialog(std::shared_ptr<mc::ModelEdit> document,
                                     std::shared_ptr<mc::Library> catalog, const QString &project,
                                     QWidget *p)
    : QDialog(p), edit(std::move(document)), library(std::move(catalog)), root(project),
      asset(edit->assetPath()) {
    setObjectName("modelAuthorDialog");
    setWindowTitle("Edit Minecraft model · " + asset);
    resize(1040, 720);
    auto outer = new QVBoxLayout(this);
    auto split = new QSplitter;
    outer->addWidget(split, 1);
    auto left = new QWidget;
    auto lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->addWidget(new QLabel("Elements"));
    elements = new QListWidget;
    elements->setObjectName("authorElements");
    elements->setSelectionMode(QAbstractItemView::ExtendedSelection); // bulk UV across elements
    elements->setAccessibleName("Model elements");
    lv->addWidget(elements, 1);
    auto addButton = [&](QLayout *layout, const QString &text, const QString &name, auto callback) {
        auto b = new QPushButton(text);
        b->setAutoDefault(false);
        b->setObjectName(name);
        layout->addWidget(b);
        connect(b, &QPushButton::clicked, this, callback);
        return b;
    };
    auto row = new QHBoxLayout;
    lv->addLayout(row);
    addButton(row, "Add", "authorAdd", [this] {
        QString e;
        if (result(edit->addElement("#all", &e), e))
            refresh(edit->source()["elements"].toArray().size() - 1);
    });
    addButton(row, "Duplicate", "authorDuplicate", [this] {
        QString e;
        if (result(edit->duplicateElement(elements->currentRow(), &e), e))
            refresh(edit->source()["elements"].toArray().size() - 1);
    });
    addButton(lv, "Delete selected element", "authorDelete", [this] {
        QString e;
        if (result(edit->deleteElement(elements->currentRow(), &e), e))
            refresh(0);
    });
    isolate = new QCheckBox("Isolate selection in preview");
    lv->addWidget(isolate);
    connect(isolate, &QCheckBox::toggled, this, [this] { showPreview(); });
    addButton(lv, "Hide / show selected", "authorHide", [this] {
        int i = elements->currentRow();
        if (hidden.contains(i))
            hidden.remove(i);
        else
            hidden.insert(i);
        showPreview();
    });
    auto middle = new QWidget;
    auto mv = new QVBoxLayout(middle);
    mv->setContentsMargins(0, 0, 0, 0);
    preview = new Cube(&metrics);
    preview->setObjectName("authorPreview");
    preview->setMinimumSize(260, 180);
    mv->addWidget(preview, 1);
    handles = new ElementHandles;
    mv->addWidget(handles, 1);
    auto controls = new QHBoxLayout;
    mv->addLayout(controls);
    auto plane = new QComboBox;
    plane->addItems({"XY", "XZ", "YZ"});
    controls->addWidget(plane);
    connect(plane, &QComboBox::currentIndexChanged, this, [this](int i) {
        handles->plane = i;
        handles->update();
    });
    auto mode = new QComboBox;
    mode->setObjectName("authorHandleMode");
    mode->addItems({"Move", "Resize", "Rotate"});
    controls->addWidget(mode);
    connect(mode, &QComboBox::currentIndexChanged, this, [this](int i) {
        handles->mode = i;
        handles->update();
    });
    auto snap = new QDoubleSpinBox;
    snap->setPrefix("Snap ");
    snap->setDecimals(3);
    snap->setRange(0, 16);
    snap->setValue(1);
    controls->addWidget(snap);
    connect(snap, &QDoubleSpinBox::valueChanged, this, [this](double v) { handles->snap = v; });
    auto tabs = new QTabWidget;
    tabs->setMinimumWidth(300);
    split->addWidget(left);
    split->addWidget(middle);
    split->addWidget(tabs);
    split->setSizes({180, 420, 340});
    auto geometry = new QWidget;
    auto form = new QFormLayout(geometry);
    tabs->addTab(geometry, "Geometry");
    label = new QLineEdit;
    label->setObjectName("authorLabel");
    form->addRow("Label", label);
    connect(label, &QLineEdit::editingFinished, this, [this] {
        if (syncing)
            return;
        QString e;
        if (result(edit->labelElement(elements->currentRow(), label->text(), &e), e))
            refresh();
    });
    const QStringList names{"From X",   "From Y",   "From Z",   "To X",     "To Y",     "To Z",
                            "Origin X", "Origin Y", "Origin Z", "Rotate X", "Rotate Y", "Rotate Z"};
    for (int i = 0; i < 12; ++i) {
        auto s = new QDoubleSpinBox;
        numbers[i] = s;
        s->setObjectName("authorNumber" + QString::number(i));
        s->setDecimals(4);
        s->setRange(i < 6 ? -16 : i < 9 ? -10000 : -36000, i < 6 ? 32 : i < 9 ? 10000 : 36000);
        s->setKeyboardTracking(false);
        form->addRow(names[i], s);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { transform(); });
    }
    rotate = new QCheckBox("Enable rotation");
    rescale = new QCheckBox("Rescale");
    form->addRow(rotate);
    form->addRow(rescale);
    connect(rotate, &QCheckBox::toggled, this, [this] { transform(); });
    connect(rescale, &QCheckBox::toggled, this, [this] { transform(); });
    addButton(form, "Reset transforms", "authorReset", [this] {
        QString e;
        auto t = mc::ElementTransform{};
        if (result(edit->previewElement(elements->currentRow(), t, &e), e))
            refresh();
    });
    auto facePage = new QWidget;
    auto ff = new QVBoxLayout(facePage);
    tabs->addTab(facePage, "Faces / UV");
    directions = new QListWidget;
    directions->setObjectName("authorDirections");
    directions->setAccessibleName("Element faces");
    directions->addItems({"down", "up", "north", "south", "west", "east"});
    directions->setSelectionMode(QAbstractItemView::ExtendedSelection);
    directions->setMaximumHeight(110);
    ff->addWidget(directions);
    texture = new QComboBox;
    texture->setEditable(true);
    texture->setObjectName("authorTexture");
    texture->setAccessibleName("Face texture or alias");
    texture->addItem("#all");
    for (const auto &f : library->texturePaths().mid(0, 4096))
        texture->addItem(mc::Location::fromAsset(f, "textures"));
    ff->addWidget(texture);
    textureInfo = new QLabel;
    textureInfo->setWordWrap(true);
    textureInfo->setTextFormat(Qt::PlainText);
    textureInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ff->addWidget(textureInfo);
    texturePreview = new QLabel;
    texturePreview->setMaximumHeight(64);
    ff->addWidget(texturePreview);
    connect(texture, &QComboBox::currentTextChanged, this, [this] { updateTextureInfo(); });
    auto faceForm = new QFormLayout;
    ff->addLayout(faceForm);
    cull = new QComboBox;
    cull->addItems({"", "down", "up", "north", "south", "west", "east"});
    tint = new QSpinBox;
    tint->setRange(-1, 65535);
    tint->setValue(-1);
    faceForm->addRow("Cull face", cull);
    faceForm->addRow("Tint index", tint);
    auto faceButtons = new QHBoxLayout;
    ff->addLayout(faceButtons);
    addButton(faceButtons, "Assign", "authorAssign", [this] {
        QString e;
        mc::FaceEdit f;
        f.directions = selectedFaces();
        f.texture = texture->currentText();
        f.cullface = cull->currentText();
        f.tint = tint->value();
        if (result(edit->previewFaces(elements->currentRow(), f, &e), e))
            showPreview();
    });
    addButton(faceButtons, "Disable", "authorDisable", [this] {
        QString e;
        mc::FaceEdit f;
        f.directions = selectedFaces();
        f.enabled = false;
        if (result(edit->previewFaces(elements->currentRow(), f, &e), e))
            showPreview();
    });
    auto copyRow = new QHBoxLayout;
    ff->addLayout(copyRow);
    addButton(copyRow, "Copy face", "authorCopyFace", [this] {
        auto ds = selectedFaces();
        auto a = edit->source()["elements"].toArray();
        int i = elements->currentRow();
        if (i >= 0 && i < a.size() && !ds.isEmpty())
            clipboard = a[i].toObject()["faces"].toObject()[ds.first()].toObject();
    });
    addButton(copyRow, "Paste face", "authorPasteFace", [this] {
        QString e;
        if (result(edit->pasteFaces(elements->currentRow(), selectedFaces(), clipboard, &e), e))
            showPreview();
    });
    auto uvForm = new QFormLayout;
    ff->addLayout(uvForm);
    for (int i = 0; i < 4; ++i) {
        uv[i] = new QDoubleSpinBox;
        uv[i]->setDecimals(4);
        uv[i]->setRange(-4096, 4096);
        uv[i]->setValue(i < 2 ? 0 : 16);
        uvForm->addRow(QStringList{"U1", "V1", "U2", "V2"}[i], uv[i]);
    }
    rotation = new QComboBox;
    rotation->addItems({"0°", "90°", "180°", "270°"});
    uvForm->addRow("UV rotation", rotation);
    auto uvRow = new QHBoxLayout;
    ff->addLayout(uvRow);
    addButton(uvRow, "Map selected", "authorMapUv", [this] {
        QString e;
        QVector<mc::UvTarget> targets;
        for (const auto &d : selectedFaces())
            targets.append({elements->currentRow(), d});
        if (result(edit->previewUvs(targets,
                                    {float(uv[0]->value()), float(uv[1]->value()),
                                     float(uv[2]->value()), float(uv[3]->value())},
                                    rotation->currentIndex() * 90, &e),
                   e))
            showPreview();
    });
    addButton(uvRow, "Mirror U", "authorMirrorU", [this] {
        auto a = uv[0]->value();
        uv[0]->setValue(uv[2]->value());
        uv[2]->setValue(a);
    });
    addButton(uvRow, "Mirror V", "authorMirrorV", [this] {
        auto a = uv[1]->value();
        uv[1]->setValue(uv[3]->value());
        uv[3]->setValue(a);
    });
    addButton(ff, "Reset inferred UVs on selected faces", "authorResetUv", [this] {
        QString e;
        QVector<mc::UvTarget> targets;
        for (const auto &d : selectedFaces())
            targets.append({elements->currentRow(), d});
        if (result(edit->transformUvs(targets, mc::UvOperation::Reset, {}, {16, 16},
                                      library->version(), &e),
                   e))
            showPreview();
    });
    auto uvAction = new QComboBox;
    uvAction->setObjectName("authorUvAction");
    uvAction->addItems({"Move by U1 / V1", "Mirror selected U", "Mirror selected V",
                        "Rotate selected 90°", "Align top-left to U1 / V1",
                        "Snap selected to texels", "Reset inferred defaults"});
    ff->addWidget(uvAction);
    addButton(ff, "Preview UV action", "authorUvActionPreview", [this, uvAction] {
        QVector<mc::UvTarget> targets;
        for (const auto &d : selectedFaces())
            targets.append({elements->currentRow(), d});
        auto path = mc::Location::parse(texture->currentText()).asset("textures");
        if (texture->currentText().startsWith('#'))
            path = library->model(mc::Location::fromAsset(asset, "models"))
                       .textures.value(texture->currentText().mid(1));
        QImageReader reader(library->assetPath(path), "PNG");
        auto size = reader.size();
        if (!size.isValid())
            size = {16, 16};
        QString error;
        if (result(edit->transformUvs(targets, mc::UvOperation(uvAction->currentIndex()),
                                      {uv[0]->value(), uv[1]->value()}, size, library->version(),
                                      &error),
                   error))
            showPreview();
    });
    // ---- C8 bulk numeric UV: faces across all selected elements ----
    auto bulkToggle = new QToolButton;
    bulkToggle->setObjectName("authorBulkToggle");
    bulkToggle->setText("Bulk numeric UV (selected elements × faces)");
    bulkToggle->setCheckable(true);
    bulkToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bulkToggle->setArrowType(Qt::RightArrow);
    ff->addWidget(bulkToggle);
    auto bulkGroup = new QWidget;
    bulkGroup->setObjectName("authorBulkUv");
    bulkGroup->setAccessibleName("Bulk numeric UV editor");
    bulkGroup->hide();
    ff->addWidget(bulkGroup);
    connect(bulkToggle, &QToolButton::toggled, this, [this, bulkToggle, bulkGroup](bool open) {
        bulkToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        bulkGroup->setVisible(open);
        updateBulkSummary();
    });
    auto bv = new QVBoxLayout(bulkGroup);
    bv->setContentsMargins(0, 0, 0, 0);
    bulkSummary = new QLabel;
    bulkSummary->setObjectName("authorBulkSummary");
    bulkSummary->setWordWrap(true);
    bulkSummary->setTextFormat(Qt::PlainText);
    bulkSummary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    bv->addWidget(bulkSummary);
    auto bulkGrid = new QGridLayout;
    const QStringList bulkNames{"U", "V", "Width", "Height", "Offset U", "Offset V"};
    const QStringList bulkIds{"U", "V", "Width", "Height", "OffsetU", "OffsetV"};
    for (int i = 0; i < 6; ++i) {
        auto spin = new QDoubleSpinBox;
        bulkValues[i] = spin;
        spin->setObjectName("authorBulk" + bulkIds[i]);
        spin->setAccessibleName("Bulk UV " + bulkNames[i]);
        spin->setDecimals(4);
        // One step below the valid range is the "mixed" sentinel for absolute fields.
        spin->setRange(i < 4 ? MixedValue : -4096, 4096);
        if (i < 4)
            spin->setSpecialValueText("mixed");
        spin->setKeyboardTracking(false);
        spin->setSingleStep(1);
        auto label = new QLabel(bulkNames[i]);
        label->setBuddy(spin);
        bulkGrid->addWidget(label, i / 2, (i % 2) * 2);
        bulkGrid->addWidget(spin, i / 2, (i % 2) * 2 + 1);
    }
    bulkValues[4]->setValue(0);
    bulkValues[5]->setValue(0);
    bv->addLayout(bulkGrid);
    bulkAspect = new QCheckBox("Lock aspect ratio");
    bulkAspect->setObjectName("authorBulkAspect");
    bv->addWidget(bulkAspect);
    connect(bulkValues[2], &QDoubleSpinBox::valueChanged, this, [this](double w) {
        if (syncing || !bulkAspect->isChecked() || bulkAspectRatio <= 0 || w == MixedValue)
            return;
        QSignalBlocker block(bulkValues[3]);
        bulkValues[3]->setValue(w / bulkAspectRatio);
    });
    connect(bulkValues[3], &QDoubleSpinBox::valueChanged, this, [this](double h) {
        if (syncing || !bulkAspect->isChecked() || bulkAspectRatio <= 0 || h == MixedValue)
            return;
        QSignalBlocker block(bulkValues[2]);
        bulkValues[2]->setValue(h * bulkAspectRatio);
    });
    auto bulkButtons = new QGridLayout;
    int bulkIndex = 0;
    auto bulkButton = [&](const QString &text, const QString &name, const QString &tip, auto build) {
        auto b = new QPushButton(text);
        b->setObjectName(name);
        b->setToolTip(tip);
        b->setAutoDefault(false);
        bulkButtons->addWidget(b, bulkIndex / 3, bulkIndex % 3);
        ++bulkIndex;
        connect(b, &QPushButton::clicked, this, [this, build] {
            mc::UvBulk op;
            if (!build(op))
                return;
            applyBulk(op);
        });
    };
    bulkButton("Set U/V", "authorBulkPosition", "Move each rectangle's top-left; mixed axes stay",
               [this](mc::UvBulk &op) {
                   op.kind = mc::UvBulk::Position;
                   op.first = bulkValues[0]->value() != MixedValue;
                   op.second = bulkValues[1]->value() != MixedValue;
                   op.value = {bulkValues[0]->value(), bulkValues[1]->value()};
                   return op.first || op.second;
               });
    bulkButton("Set size", "authorBulkSize", "Shared width/height, keeping top-left and mirroring",
               [this](mc::UvBulk &op) {
                   op.kind = mc::UvBulk::Size;
                   op.first = bulkValues[2]->value() != MixedValue;
                   op.second = bulkValues[3]->value() != MixedValue;
                   op.value = {bulkValues[2]->value(), bulkValues[3]->value()};
                   return op.first || op.second;
               });
    bulkButton("Move by offset", "authorBulkOffset", "Relative move in model UV units",
               [this](mc::UvBulk &op) {
                   op.kind = mc::UvBulk::Offset;
                   op.value = {bulkValues[4]->value(), bulkValues[5]->value()};
                   return true;
               });
    bulkButton("Mirror U", "authorBulkMirrorU", {}, [](mc::UvBulk &op) {
        op.kind = mc::UvBulk::MirrorU;
        return true;
    });
    bulkButton("Mirror V", "authorBulkMirrorV", {}, [](mc::UvBulk &op) {
        op.kind = mc::UvBulk::MirrorV;
        return true;
    });
    bulkButton("Rotate 90°", "authorBulkRotate", "Face UV rotation +90°", [](mc::UvBulk &op) {
        op.kind = mc::UvBulk::Rotate;
        return true;
    });
    bulkButton("Snap texels", "authorBulkSnap", "Each face snaps to its own texture's pixel grid",
               [](mc::UvBulk &op) {
                   op.kind = mc::UvBulk::Snap;
                   return true;
               });
    bulkButton("Distribute U", "authorBulkDistributeU", "Side by side from the leftmost; Offset U is the gap",
               [this](mc::UvBulk &op) {
                   op.kind = mc::UvBulk::Distribute;
                   op.horizontal = true;
                   op.gap = bulkValues[4]->value();
                   return true;
               });
    bulkButton("Distribute V", "authorBulkDistributeV", "Stacked from the topmost; Offset V is the gap",
               [this](mc::UvBulk &op) {
                   op.kind = mc::UvBulk::Distribute;
                   op.horizontal = false;
                   op.gap = bulkValues[5]->value();
                   return true;
               });
    bulkButton("Make explicit", "authorBulkInfer", "Write inferred default UVs explicitly (same rendering)",
               [](mc::UvBulk &op) {
                   op.kind = mc::UvBulk::Infer;
                   return true;
               });
    bulkButton("Reset inferred", "authorBulkReset", "Remove explicit UVs so Minecraft infers them",
               [](mc::UvBulk &op) {
                   op.kind = mc::UvBulk::Reset;
                   return true;
               });
    bulkButton("Copy UV", "authorBulkCopy", "Copy the first selected face's UV and rotation",
               [this](mc::UvBulk &) {
                   const auto states = mc::uvStates(edit->source(), bulkTargets(), library->version());
                   if (states.isEmpty())
                       return false;
                   uvClipboard = {{"uv", QJsonArray{states[0].uv.x(), states[0].uv.y(), states[0].uv.z(),
                                                    states[0].uv.w()}},
                                  {"rotation", states[0].rotation}};
                   status->setText("Copied UV of the first selected face");
                   return false;
               });
    bulkButton("Paste UV", "authorBulkPaste", "Apply the copied UV/rotation to every selected face",
               [this](mc::UvBulk &op) {
                   op.kind = mc::UvBulk::Paste;
                   op.clipboard = uvClipboard;
                   return !uvClipboard.isEmpty();
               });
    bv->addLayout(bulkButtons);
    connect(directions, &QListWidget::itemSelectionChanged, this, [this] { updateBulkSummary(); });
    connect(elements, &QListWidget::itemSelectionChanged, this, [this] { updateBulkSummary(); });
    ff->addStretch();
    connect(directions, &QListWidget::currentRowChanged, this, [this](int row) {
        auto a = edit->source()["elements"].toArray();
        int i = elements->currentRow();
        if (row < 0 || i < 0 || i >= a.size())
            return;
        auto f = a[i].toObject()["faces"].toObject()[directions->item(row)->text()].toObject();
        texture->setCurrentText(f["texture"].toString("#all"));
        cull->setCurrentText(f["cullface"].toString());
        tint->setValue(f["tintindex"].toInt(-1));
        auto coords = f["uv"].toArray();
        if (coords.size() != 4) {
            const auto geometry =
                mc::mesh(library->model(mc::Location::fromAsset(asset, "models")));
            for (const auto &face : geometry.faces)
                if (face.elementIndex == i && face.name == directions->item(row)->text()) {
                    int start = (4 - face.rotation / 90) % 4;
                    auto a = face.uv[start] * 16, b = face.uv[(start + 2) % 4] * 16;
                    coords = {a.x(), a.y(), b.x(), b.y()};
                    break;
                }
        }
        if (coords.size() == 4)
            for (int n = 0; n < 4; ++n)
                uv[n]->setValue(coords[n].toDouble());
        rotation->setCurrentIndex(f["rotation"].toInt() / 90);
        updateTextureInfo();
    });
    auto props = new QWidget;
    auto pf = new QFormLayout(props);
    tabs->addTab(props, "Model");
    parentId = new QLineEdit(edit->source()["parent"].toString());
    pf->addRow("Parent", parentId);
    light = new QComboBox;
    light->addItems({"side", "front"});
    light->setCurrentText(edit->source()["gui_light"].toString("side"));
    pf->addRow("GUI light", light);
    ao = new QCheckBox("Ambient occlusion");
    ao->setChecked(edit->source()["ambientocclusion"].toBool(true));
    pf->addRow(ao);
    addButton(pf, "Preview properties", "authorProperties", [this] {
        QString e;
        if (result(edit->properties(parentId->text(), light->currentText(), ao->isChecked(), &e),
                   e))
            showPreview();
    });
    bindingAlias = new QLineEdit("all");
    bindingResource = new QLineEdit;
    pf->addRow("Texture alias", bindingAlias);
    pf->addRow("Resource / #alias", bindingResource);
    addButton(pf, "Preview binding", "authorBinding", [this] {
        QString e;
        if (result(edit->textureBinding(bindingAlias->text(), bindingResource->text(), &e), e))
            showPreview();
    });
    addButton(pf, "Inspect raw JSON…", "authorRaw", [this] {
        // Read-only snapshot window; non-modal so editing can continue beside it (C8).
        auto d = new QDialog(this);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setObjectName("authorRawJson");
        d->setWindowTitle("Raw model JSON · read-only snapshot");
        auto v = new QVBoxLayout(d);
        auto text = new QPlainTextEdit;
        text->setReadOnly(true);
        text->setPlainText(QString::fromUtf8(QJsonDocument(edit->source()).toJson()));
        v->addWidget(text);
        d->resize(650, 500);
        d->show();
    });
    status = new QLabel;
    status->setObjectName("authorStatus");
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    outer->addWidget(status);
    auto buttons = new QHBoxLayout;
    outer->addLayout(buttons);
    addButton(buttons, "Undo model", "authorUndo", [this] {
        if (edit->previewing())
            edit->cancelPreview();
        else
            edit->undo();
        refresh();
        checkpoint();
    });
    addButton(buttons, "Redo model", "authorRedo", [this] {
        edit->redo();
        refresh();
        checkpoint();
    });
    addButton(buttons, "Apply preview", "authorApply", [this] {
        if (edit->commitPreview("Edit model")) {
            refresh();
            checkpoint();
        }
    });
    addButton(buttons, "Cancel preview", "authorCancel", [this] {
        edit->cancelPreview();
        refresh();
    });
    addButton(buttons, "Save JSON", "authorSave", [this] {
        QString e;
        const bool provisional = edit->provisional();
        if (result(edit->save(&e), e)) {
            refresh();
            if (!provisional)
                checkpoint();
        }
    });
    addButton(buttons, "Save As…", "authorSaveAs", [this] { saveAs(); });
    addButton(buttons, "Rename…", "authorRename", [this] {
        bool ok = false;
        const auto id = QInputDialog::getText(
            this, "Rename model with forwarding alias",
            "New namespace:path. The old path stays as a parent alias so references keep working.",
            QLineEdit::Normal, mc::Location::fromAsset(asset, "models") + "_renamed", &ok);
        if (!ok)
            return;
        const auto old = asset;
        QString error;
        if (edit->renameKeepingAlias(root, mc::Location::parse(id).asset("models"), &error)) {
            asset = edit->assetPath();
            library->refreshLocal({old, asset});
            setWindowTitle("Edit Minecraft model · " + asset);
            refresh();
        } else
            result(false, error);
    });
    addButton(buttons, "Close", "authorClose", [this] { reject(); });
    connect(this, &QDialog::finished, this, [this] {
        edit->cancelPreview();
        library->previewSource(asset, edit->source());
    });
    connect(elements, &QListWidget::currentRowChanged, this, [this] { selection(); });
    connect(preview, &Cube::faceSelected, this, [this, tabs](int f) {
        const auto &m = preview->meshData();
        if (f < 0 || f >= m.faces.size())
            return;
        tabs->setCurrentIndex(1);
        elements->setCurrentRow(m.faces[f].elementIndex);
        for (int i = 0; i < directions->count(); ++i)
            if (directions->item(i)->text() == m.faces[f].name)
                directions->setCurrentRow(i);
    });
    handles->changed = [this](const mc::ElementTransform &t) {
        QString e;
        if (result(edit->previewElement(elements->currentRow(), t, &e), e)) {
            selection();
            showPreview();
        }
    };
    // Scroll contextual properties at compact sizes/high DPI instead of clipping controls.
    for (int i = 0; i < tabs->count(); ++i) {
        auto page = tabs->widget(i);
        auto title = tabs->tabText(i);
        tabs->removeTab(i);
        auto scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        tabs->insertTab(i, scroll, title);
    }
    tabs->setCurrentIndex(0);
    refresh(0);
    preview->fitModel();
}
ModelAuthorDialog::~ModelAuthorDialog() {
    delete preview;
    preview = nullptr;
}
bool ModelAuthorDialog::result(bool ok, const QString &error) {
    if (!ok)
        status->setText(error.isEmpty() ? "Operation rejected" : error);
    return ok;
}
QStringList ModelAuthorDialog::selectedFaces() const {
    QStringList out;
    for (auto i : directions->selectedItems())
        out << i->text();
    return out;
}
void ModelAuthorDialog::refresh(int selected) {
    if (selected == -2)
        selected = elements->currentRow();
    syncing = true;
    elements->clear();
    auto a = edit->source()["elements"].toArray();
    for (int i = 0; i < a.size(); ++i)
        elements->addItem(a[i].toObject()["name"].toString("Element " + QString::number(i + 1)));
    elements->setCurrentRow(std::clamp(selected, 0, std::max(0, int(a.size()) - 1)));
    syncing = false;
    selection();
    showPreview();
}
void ModelAuthorDialog::selection() {
    if (syncing)
        return;
    auto a = edit->source()["elements"].toArray();
    int i = elements->currentRow();
    bool valid = i >= 0 && i < a.size();
    for (auto s : numbers)
        s->setEnabled(valid);
    handles->setEnabled(valid);
    if (!valid)
        return;
    syncing = true;
    auto e = a[i].toObject();
    auto t = readTransform(e);
    label->setText(e["name"].toString());
    for (int n = 0; n < 3; ++n) {
        numbers[n]->setValue(t.from[n]);
        numbers[n + 3]->setValue(t.to[n]);
        numbers[n + 6]->setValue(t.origin[n]);
        numbers[n + 9]->setValue(t.angles[n]);
    }
    rotate->setChecked(t.rotation);
    rescale->setChecked(t.rescale);
    handles->setTransform(t);
    syncing = false;
    showPreview();
}
void ModelAuthorDialog::transform() {
    if (syncing)
        return;
    mc::ElementTransform t;
    for (int n = 0; n < 3; ++n) {
        t.from[n] = float(numbers[n]->value());
        t.to[n] = float(numbers[n + 3]->value());
        t.origin[n] = float(numbers[n + 6]->value());
        t.angles[n] = float(numbers[n + 9]->value());
    }
    t.rotation = rotate->isChecked();
    t.rescale = rescale->isChecked();
    QString e;
    if (result(edit->previewElement(elements->currentRow(), t, &e), e)) {
        handles->setTransform(t);
        showPreview();
    }
}
void ModelAuthorDialog::showPreview() {
    library->previewSource(asset, edit->source());
    auto model = library->model(mc::Location::fromAsset(asset, "models"));
    auto baseMesh = mc::mesh(model);
    QSet<QString> texturePaths;
    for (const auto &t : model.textures)
        texturePaths.insert(t);
    for (const auto &face : baseMesh.faces)
        if (!face.texture.isEmpty())
            texturePaths.insert(face.texture);
    for (const auto &t : texturePaths)
        if (!images.contains(t)) {
            auto path = library->assetPath(t);
            if (path.isEmpty())
                continue;
            auto encoded = readFileBytes(path, 64LL * 1024 * 1024);
            QBuffer source(&encoded);
            source.open(QIODevice::ReadOnly);
            QImageReader r(&source, "PNG");
            auto size = r.size();
            const auto bytes = qint64(size.width()) * size.height() * 4;
            if (size.isValid() && bytes <= 16 * 1024 * 1024 &&
                imageBytes + bytes <= 64 * 1024 * 1024 && images.size() < 64) {
                auto image = r.read();
                if (!image.isNull()) {
                    imageBytes += image.sizeInBytes();
                    images[t] = std::move(image);
                }
            }
        }
    auto mesh = std::make_shared<mc::Mesh>(mc::generatedMesh(model, images));
    if (!hidden.isEmpty() || isolate->isChecked())
        mesh->faces.erase(std::remove_if(mesh->faces.begin(), mesh->faces.end(),
                                         [this](const auto &f) {
                                             return hidden.contains(f.elementIndex) ||
                                                    (isolate->isChecked() &&
                                                     f.elementIndex != elements->currentRow());
                                         }),
                          mesh->faces.end());
    preview->setModel(mesh, {}, images);
    if (changed)
        changed();
    status->setText(edit->externalChange() ? "External change detected · saving will be blocked"
                    : edit->previewing()
                        ? "Preview only · Apply creates one undo step; Cancel restores the model"
                    : edit->modified()
                        ? "Unsaved model changes · Save JSON writes the checked revision"
                        : "Model matches saved JSON");
}
void ModelAuthorDialog::saveAs() {
    bool ok = false;
    auto id = QInputDialog::getText(
        this, "Save model as", "New namespace:path (existing files preserved)", QLineEdit::Normal,
        mc::Location::fromAsset(asset, "models") + "_copy", &ok);
    if (!ok)
        return;
    QString error;
    auto copy = std::make_shared<mc::ModelEdit>();
    copy->create(edit->authoritative());
    if (copy->saveAs(root, mc::Location::parse(id).asset("models"), &error))
        status->setText("Saved copy; original document remains open");
    else
        result(false, error);
}
void ModelAuthorDialog::checkpoint() {
    QString path, error;
    const auto key = QString::fromLatin1(
        QCryptographicHash::hash(asset.toUtf8(), QCryptographicHash::Sha256).toHex());
    if (mc::safeDestination(root, ".nova/model-recovery/" + key + ".json", &path, &error)) {
        if (!edit->modified()) {
            QFile::remove(path);
            return;
        }
        if (edit->writeRecovery(path, &error))
            return;
    }
    status->setText("Model retained in memory; recovery snapshot failed: " + error);
}
void ModelAuthorDialog::updateTextureInfo() {
    const auto text = texture->currentText();
    auto path = mc::Location::parse(text).asset("textures");
    if (text.startsWith('#'))
        path = library->model(mc::Location::fromAsset(asset, "models")).textures.value(text.mid(1));
    const auto abs = library->assetPath(path);
    textureInfo->setText(path + "\n" + (library->editable(path) ? "Local: " : "Fallback: ") + abs);
    if (abs.isEmpty()) {
        texturePreview->clear();
        return;
    }
    auto encoded = readFileBytes(abs, 64LL * 1024 * 1024);
    QBuffer source(&encoded);
    source.open(QIODevice::ReadOnly);
    QImageReader reader(&source, "PNG");
    reader.setScaledSize({64, 64});
    texturePreview->setPixmap(QPixmap::fromImage(reader.read()));
}
QVector<mc::UvTarget> ModelAuthorDialog::bulkTargets() const {
    QVector<mc::UvTarget> out;
    QList<int> rows;
    for (auto item : elements->selectedItems())
        rows << elements->row(item);
    if (rows.isEmpty() && elements->currentRow() >= 0)
        rows << elements->currentRow();
    std::sort(rows.begin(), rows.end());
    const auto a = edit->source()["elements"].toArray();
    for (int row : rows)
        for (const auto &d : selectedFaces())
            if (row < a.size() && a[row].toObject()["faces"].toObject()[d].isObject())
                out.append({row, d});
    return out;
}
QSize ModelAuthorDialog::faceTextureSize(const mc::UvTarget &t) {
    const auto face =
        edit->source()["elements"].toArray()[t.element].toObject()["faces"].toObject()[t.face].toObject();
    const auto reference = face["texture"].toString();
    const auto path =
        reference.startsWith('#')
            ? library->model(mc::Location::fromAsset(asset, "models")).textures.value(reference.mid(1))
            : mc::Location::parse(reference).asset("textures");
    if (!sizeCache.contains(path)) {
        QImageReader reader(library->assetPath(path), "PNG");
        sizeCache[path] = reader.size();
    }
    return sizeCache.value(path);
}
void ModelAuthorDialog::updateBulkSummary() {
    if (!bulkSummary || bulkSummary->parentWidget()->isHidden() || syncing) // collapsed group
        return;
    const auto targets = bulkTargets();
    const auto states = mc::uvStates(edit->source(), targets, library->version());
    QSet<int> elementSet;
    QSet<QString> sizes;
    int inferred = 0;
    for (int i = 0; i < targets.size(); ++i) {
        elementSet.insert(targets[i].element);
        const auto size = faceTextureSize(targets[i]);
        sizes.insert(size.isValid() ? QString("%1x%2").arg(size.width()).arg(size.height()) : "unknown");
        if (i < states.size() && states[i].inferred)
            ++inferred;
    }
    QStringList sizeList(sizes.begin(), sizes.end());
    std::sort(sizeList.begin(), sizeList.end());
    bulkSummary->setText(
        targets.isEmpty()
            ? "Select elements (Ctrl/Shift-click) and faces above"
            : QString("%1 face(s) across %2 element(s) · %3 inferred · texture px %4%5\n"
                      "Values are model UV units (0-16 = whole texture); Snap uses each face's own texels.")
                  .arg(targets.size())
                  .arg(elementSet.size())
                  .arg(inferred)
                  .arg(sizeList.join(", "))
                  .arg(sizeList.size() > 1 ? " · mixed resolutions" : ""));
    syncing = true;
    double values[4]{};
    bool mixed[4]{};
    for (int i = 0; i < states.size(); ++i) {
        const auto &u = states[i].uv;
        const double v[4] = {std::min(u.x(), u.z()), std::min(u.y(), u.w()), std::abs(u.z() - u.x()),
                             std::abs(u.w() - u.y())};
        for (int k = 0; k < 4; ++k) {
            if (i == 0)
                values[k] = v[k];
            else if (std::abs(values[k] - v[k]) > 1e-6)
                mixed[k] = true;
        }
    }
    for (int k = 0; k < 4; ++k)
        bulkValues[k]->setValue(states.isEmpty() || mixed[k] ? MixedValue : values[k]);
    bulkAspectRatio =
        !states.isEmpty() && !mixed[2] && !mixed[3] && values[3] > 0 ? values[2] / values[3] : 0;
    syncing = false;
}
void ModelAuthorDialog::applyBulk(const mc::UvBulk &op) {
    QString error;
    if (result(edit->bulkUv(bulkTargets(), op, [this](const mc::UvTarget &t) { return faceTextureSize(t); },
                            library->version(), &error),
               error)) {
        showPreview();
        updateBulkSummary();
    }
}
} // namespace nova
