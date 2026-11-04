// SPDX-License-Identifier: GPL-3.0-or-later
#include "timeline.h"
#include "metrics.h"
#include <QtWidgets>
#include <algorithm>
#include <array>
namespace nova {
int TimelineModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(entries.size()); }
QUuid TimelineModel::frameAt(int row) const { return row >= 0 && row < int(entries.size()) ? entries[size_t(row)].frame : QUuid{}; }
QVariant TimelineModel::data(const QModelIndex &index, int role) const {
    if (!document || !index.isValid() || index.row() >= rowCount()) return {};
    auto entry = entries[size_t(index.row())];
    auto frame = std::find_if(frames.begin(), frames.end(), [&](const auto &f) { return f.first == entry.frame; });
    if (frame == frames.end()) return {};
    const int ticks = entry.ticks ? entry.ticks : defaultTicks;
    if (role == Qt::DisplayRole) return QString("%1%2 · %3t\n%4")
        .arg(cursor == index.row() ? "▶ " : "").arg(index.row() + 1).arg(ticks).arg(frame->second);
    if (role == Qt::ToolTipRole) return QString("Entry %1 → %2\n%3 ticks = %4 ms%5\nDrag to reorder playback references")
        .arg(index.row() + 1).arg(frame->second).arg(ticks).arg(qint64(ticks) * 50).arg(entry.ticks ? "" : " (default)");
    if (role == Qt::UserRole) return entry.frame;
    if (role == Qt::BackgroundRole && entry.frame == document->activeFrame()) return QBrush(QColor(90, 70, 150, 95));
    if (role == Qt::DecorationRole) {
        QElapsedTimer timer;
        timer.start();
        auto image = document->frameComposite(entry.frame);
        const QString key = QString::number(image.cacheKey()) + ':' + QString::number(thumbnailSize);
        if (auto cached = thumbnails.object(key)) return *cached;
        QPixmap pixmap(thumbnailSize, thumbnailSize);
        pixmap.fill(QColor(48, 48, 52));
        QPainter painter(&pixmap);
        const auto size = image.size().scaled(thumbnailSize, thumbnailSize, Qt::KeepAspectRatio);
        const QRect target((thumbnailSize - size.width()) / 2, (thumbnailSize - size.height()) / 2, size.width(), size.height());
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(target, image);
        painter.end();
        thumbnails.insert(key, new QPixmap(pixmap), thumbnailSize * thumbnailSize * 4);
        if (metrics)
            metrics->record("animation_thumbnail_miss", timer);
        return pixmap;
    }
    return {};
}
bool TimelineModel::sync(const std::shared_ptr<Document> &doc) {
    auto state = doc ? doc->animationState() : AnimationState{};
    std::vector<std::pair<QUuid, QString>> nextFrames;
    for (const auto &frame : state.frames) nextFrames.push_back({frame.id, frame.name});
    const bool documentChanged = doc != document;
    bool structure = documentChanged || entries.size() != state.entries.size() || nextFrames != frames;
    if (!structure) for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].frame != state.entries[i].frame) { structure = true; break; }
    if (structure) beginResetModel();
    document = doc; entries = std::move(state.entries); frames = std::move(nextFrames); defaultTicks = state.defaultTicks;
    // Pixel identity and thumbnail size already key the bounded cache.
    // Playback order changes can reuse thumbnails for unchanged pixels.
    if (documentChanged) thumbnails.clear();
    if (structure) endResetModel();
    else if (!entries.empty()) emit dataChanged(index(0), index(rowCount() - 1));
    return structure;
}
Qt::ItemFlags TimelineModel::flags(const QModelIndex &index) const {
    return index.isValid() ? QAbstractListModel::flags(index) | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled : Qt::ItemIsDropEnabled;
}
QStringList TimelineModel::mimeTypes() const { return {"application/x-nova-playback-entry"}; }
QMimeData *TimelineModel::mimeData(const QModelIndexList &indices) const {
    auto data = new QMimeData;
    if (document && indices.size() == 1) data->setData(mimeTypes()[0],
        document->id().toByteArray() + '|' + QByteArray::number(document->changeSerial()) + '|' + QByteArray::number(indices[0].row()));
    return data;
}
bool TimelineModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int, const QModelIndex &parent) {
    if (action == Qt::IgnoreAction) return true;
    if (!document || action != Qt::MoveAction) return false;
    auto parts = data->data(mimeTypes()[0]).split('|');
    if (parts.size() != 3 || parts[0] != document->id().toByteArray() || parts[1].toULongLong() != document->changeSerial()) return false;
    const int from = parts[2].toInt();
    int to = row >= 0 ? row : parent.isValid() ? parent.row() : rowCount();
    if (to > from) --to;
    if (from < 0 || from >= rowCount() || to < 0 || to >= rowCount()) return false;
    emit moveRequested(from, to); return true;
}
void TimelineModel::setThumbnailSize(int size) {
    thumbnailSize = std::clamp(size, 32, 112); thumbnails.clear();
    if (rowCount()) emit dataChanged(index(0), index(rowCount() - 1));
}
void TimelineModel::setCursor(int value) {
    const int old = cursor; cursor = value;
    if (old >= 0 && old < rowCount()) emit dataChanged(index(old), index(old));
    if (cursor >= 0 && cursor < rowCount()) emit dataChanged(index(cursor), index(cursor));
}
TimelinePanel::TimelinePanel(DocumentSession *s, QWidget *parent) : QWidget(parent), session(s), playback(this) {
    setObjectName("animationTimeline");
    setMinimumWidth(240);
    auto layout = new QVBoxLayout(this); layout->setContentsMargins(6, 4, 6, 4); layout->setSpacing(4);
    auto scrollRow = [this, layout](QHBoxLayout *row) {
        auto content = new QWidget; content->setLayout(row); row->setContentsMargins(0, 0, 0, 0);
        auto scroll = new QScrollArea; scroll->setFrameShape(QFrame::NoFrame); scroll->setWidget(content);
        scroll->setWidgetResizable(false); scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded); scroll->setMinimumWidth(0);
        content->adjustSize(); scroll->setFixedHeight(content->sizeHint().height() + 17);
        layout->addWidget(scroll);
    };
    auto controls = new QHBoxLayout;
    auto button = [&](const QString &text, const QString &name, std::function<void()> callback) {
        auto b = new QToolButton; b->setText(text); b->setObjectName(name); b->setToolTip(text);
        controls->addWidget(b); connect(b, &QToolButton::clicked, this, callback); return b;
    };
    button("|◀", "animationFirst", [this] { navigate(0, true); });
    button("◀", "animationPrevious", [this] { navigate(-1); });
    auto play = button("Play", "animationPlay", [this] {
        if (playback.playing()) playback.pause(); else { emit beforeContextChange(); playback.play(); }
    });
    button("Stop", "animationStop", [this] { playback.stop(); });
    button("▶", "animationNext", [this] { navigate(1); });
    button("▶|", "animationLast", [this] { navigate(-1, true); });
    auto loop = new QCheckBox("Loop"); loop->setChecked(true); controls->addWidget(loop);
    loop->setObjectName("animationLoop");
    connect(loop, &QCheckBox::toggled, &playback, &PlaybackController::setLoop);
    auto speed = new QComboBox; speed->addItems({"0.25×", "0.5×", "1×", "2×", "4×"}); speed->setCurrentIndex(2);
    speed->setObjectName("animationSpeed");
    speed->setToolTip("Preview speed only; exported ticks are unchanged"); controls->addWidget(speed);
    connect(speed, &QComboBox::currentIndexChanged, this, [this](int i) { playback.setSpeed(std::array{.25, .5, 1., 2., 4.}[size_t(i)]); });
    controls->addSpacing(6);
    button("+ Frame", "addFrame", [this] { frameCommand(FrameCommand::Add); });
    button("Duplicate", "duplicateFrame", [this] { frameCommand(FrameCommand::Duplicate); });
    button("Delete", "deleteFrame", [this] { frameCommand(FrameCommand::Delete); });
    controls->addStretch();
    button("Properties…", "animationProperties", [this] { emit propertiesRequested(); });
    scrollRow(controls);
    model = new TimelineModel(this);
    view = new QListView; view->setObjectName("frameTimeline"); view->setModel(model);
    view->setViewMode(QListView::IconMode); view->setFlow(QListView::LeftToRight); view->setWrapping(false);
    view->setResizeMode(QListView::Adjust); view->setUniformItemSizes(true); view->setGridSize({102, 98});
    view->setIconSize({56, 56}); view->setMinimumHeight(105); view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setDragDropMode(QAbstractItemView::InternalMove); view->setDefaultDropAction(Qt::MoveAction);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel); view->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(view, 1);
    connect(view, &QListView::clicked, this, &TimelinePanel::selected);
    connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &index) { if (!syncing) selected(index); });
    connect(view, &QListView::customContextMenuRequested, this, &TimelinePanel::contextMenu);
    connect(model, &TimelineModel::moveRequested, this, &TimelinePanel::moveEntry);
    auto timingRow = new QHBoxLayout;
    sources = new QComboBox; sources->setObjectName("sourceFrameSelector"); sources->setMinimumContentsLength(8);
    sources->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    timingRow->addWidget(new QLabel("Source")); timingRow->addWidget(sources);
    duration = new QSpinBox; duration->setObjectName("entryDuration"); duration->setRange(0, INT_MAX);
    duration->setSpecialValueText("Default"); duration->setSuffix(" t"); duration->setKeyboardTracking(false);
    timingRow->addWidget(new QLabel("Entry")); timingRow->addWidget(duration);
    defaultTime = new QSpinBox; defaultTime->setObjectName("defaultDuration"); defaultTime->setRange(1, INT_MAX);
    defaultTime->setSuffix(" t"); defaultTime->setKeyboardTracking(false);
    timingRow->addWidget(new QLabel("Default")); timingRow->addWidget(defaultTime);
    timing = new QLabel("1 tick = 50 ms"); timingRow->addWidget(timing);
    interpolate = new QCheckBox("Interpolate"); interpolate->setObjectName("animationInterpolate"); timingRow->addWidget(interpolate);
    timingRow->addStretch();
    auto zoom = new QSlider(Qt::Horizontal); zoom->setRange(32, 112); zoom->setValue(56); zoom->setMaximumWidth(100);
    zoom->setObjectName("timelineZoom");
    zoom->setToolTip("Timeline thumbnail size"); timingRow->addWidget(zoom); scrollRow(timingRow);
    connect(zoom, &QSlider::valueChanged, this, [this](int size) {
        model->setThumbnailSize(size); view->setIconSize({size, size}); view->setGridSize({std::max(102, size + 16), size + 42});
    });
    auto onionRow = new QHBoxLayout;
    auto onion = new QCheckBox("Onion skin"); onion->setObjectName("onionEnabled"); onionRow->addWidget(onion);
    auto previous = new QSpinBox; previous->setRange(0, 8); previous->setValue(1); previous->setPrefix("Previous "); previous->setObjectName("onionPrevious"); onionRow->addWidget(previous);
    auto next = new QSpinBox; next->setRange(0, 8); next->setValue(1); next->setPrefix("Next "); next->setObjectName("onionNext"); onionRow->addWidget(next);
    auto opacity = new QSpinBox; opacity->setRange(1, 100); opacity->setValue(25); opacity->setSuffix("%"); opacity->setObjectName("onionOpacity"); onionRow->addWidget(opacity);
    auto tint = new QCheckBox("Tint"); tint->setChecked(true); onionRow->addWidget(tint);
    tint->setObjectName("onionTint");
    auto updateOnion = [=, this] { emit onionChanged(onion->isChecked(), previous->value(), next->value(), opacity->value(), tint->isChecked()); };
    connect(onion, &QCheckBox::toggled, this, updateOnion); connect(previous, &QSpinBox::valueChanged, this, updateOnion);
    connect(next, &QSpinBox::valueChanged, this, updateOnion); connect(opacity, &QSpinBox::valueChanged, this, updateOnion); connect(tint, &QCheckBox::toggled, this, updateOnion);
    onionRow->addStretch(); auto scope = new QPushButton("Process frames…"); scope->setObjectName("processAnimationFrames"); onionRow->addWidget(scope);
    connect(scope, &QPushButton::clicked, this, [this] { emit scopeOperationRequested(); }); scrollRow(onionRow);
    connect(defaultTime, &QSpinBox::valueChanged, this, [this](int ticks) { if (!syncing && document) { auto state = document->animationState(); state.defaultTicks = ticks; apply(state, "Default Frame Timing"); } });
    connect(interpolate, &QCheckBox::toggled, this, [this](bool on) { if (!syncing && document) { auto state = document->animationState(); state.interpolate = on; apply(state, "Animation Interpolation"); } });
    connect(duration, &QSpinBox::valueChanged, this, [this](int ticks) {
        if (syncing || !document) return;
        auto state = document->animationState();
        for (const auto &index : view->selectionModel()->selectedIndexes()) state.entries[size_t(index.row())].ticks = ticks;
        apply(state, "Playback Entry Timing");
    });
    connect(sources, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (syncing || !document || index < 0) return;
        emit beforeContextChange(); playback.stop(); QString error;
        if (!session->frameSelect(document, sources->itemData(index).toUuid(), &error)) emit failure(error);
    });
    connect(&playback, &PlaybackController::displayed, this, [this](const QImage &, int entry) { model->setCursor(entry); view->scrollTo(model->index(entry)); });
    connect(&playback, &PlaybackController::stopped, this, [this] { model->setCursor(-1); });
    connect(&playback, &PlaybackController::statusChanged, this, [this, play] { play->setText(playback.playing() ? "Pause" : "Play"); });
}
void TimelinePanel::setDocument(std::shared_ptr<Document> doc) {
    if (document == doc && (!doc || syncedSerial == doc->changeSerial())) return;
    syncing = true;
    document = std::move(doc); playback.setDocument(document);
    const auto selectedRows = view->selectionModel()->selectedIndexes();
    const int current = view->currentIndex().row();
    const bool structure = model->sync(document); sources->clear();
    if (document) {
        const auto state = document->animationState();
        for (const auto &frame : state.frames) sources->addItem(frame.name, frame.id);
        sources->setCurrentIndex(sources->findData(document->activeFrame()));
        defaultTime->setValue(state.defaultTicks); interpolate->setChecked(state.interpolate);
        int selectedRow = current >= 0 && current < model->rowCount() && model->frameAt(current) == document->activeFrame() ? current : -1;
        if (selectedRow < 0) for (int i = 0; i < model->rowCount(); ++i) if (model->frameAt(i) == document->activeFrame()) { selectedRow = i; break; }
        if (selectedRow >= 0) {
            view->setCurrentIndex(model->index(selectedRow));
            if (!structure) for (const auto &index : selectedRows) if (index.row() < model->rowCount()) view->selectionModel()->select(model->index(index.row()), QItemSelectionModel::Select);
            duration->setValue(state.entries[size_t(selectedRow)].ticks);
            const int ticks = state.entries[size_t(selectedRow)].ticks ? state.entries[size_t(selectedRow)].ticks : state.defaultTicks;
            timing->setText(QString("%1 ms · 20 t/s").arg(qint64(ticks) * 50));
        }
        syncedSerial = document->changeSerial();
    }
    setEnabled(document && document->animated()); syncing = false;
}
void TimelinePanel::apply(AnimationState state, const QString &label) {
    emit beforeContextChange(); playback.stop(); QString error;
    if (!session->animationEdit(document, state, label, &error)) emit failure(error);
}
void TimelinePanel::frameCommand(FrameCommand command) {
    if (!document) return;
    emit beforeContextChange(); playback.stop(); QString error;
    if (!session->frameEdit(document, command, {}, -1, {}, &error)) emit failure(error);
}
void TimelinePanel::selected(const QModelIndex &index) {
    if (syncing || !document || !index.isValid()) return;
    const int row = index.row();
    emit beforeContextChange(); playback.stop(); QString error;
    if (!session->frameSelect(document, model->frameAt(row), &error)) emit failure(error);
    syncing = true;
    const auto state = document->animationState();
    duration->setValue(state.entries[size_t(row)].ticks);
    const int ticks = state.entries[size_t(row)].ticks ? state.entries[size_t(row)].ticks : state.defaultTicks;
    timing->setText(QString("%1 ms · 20 t/s").arg(qint64(ticks) * 50)); syncing = false;
}
void TimelinePanel::navigate(int delta, bool absolute) {
    if (!document || !model->rowCount()) return;
    int index = absolute ? (delta < 0 ? model->rowCount() - 1 : 0) : std::clamp(view->currentIndex().row() + delta, 0, model->rowCount() - 1);
    view->setCurrentIndex(model->index(index)); selected(model->index(index)); view->scrollTo(model->index(index));
}
void TimelinePanel::moveEntry(int from, int to) {
    if (!document) return;
    auto state = document->animationState();
    if (from < 0 || to < 0 || from >= int(state.entries.size()) || to >= int(state.entries.size()) || from == to) return;
    auto entry = state.entries[size_t(from)]; state.entries.erase(state.entries.begin() + from);
    state.entries.insert(state.entries.begin() + to, entry); apply(state, "Reorder Playback Entries");
    view->setCurrentIndex(model->index(to));
}
void TimelinePanel::duplicateEntry() {
    if (!document || !view->currentIndex().isValid()) return;
    auto state = document->animationState(); const int index = view->currentIndex().row();
    state.entries.insert(state.entries.begin() + index + 1, state.entries[size_t(index)]); apply(state, "Repeat Playback Entry");
}
void TimelinePanel::removeEntries() {
    if (!document) return;
    auto state = document->animationState(); auto indices = view->selectionModel()->selectedIndexes();
    std::sort(indices.begin(), indices.end(), [](const auto &a, const auto &b) { return a.row() > b.row(); });
    for (const auto &index : indices) state.entries.erase(state.entries.begin() + index.row());
    apply(state, "Remove Playback Entries");
}
std::vector<QUuid> TimelinePanel::selectedFrames() const {
    std::vector<QUuid> result;
    for (const auto &index : view->selectionModel()->selectedIndexes()) {
        auto id = model->frameAt(index.row());
        if (std::find(result.begin(), result.end(), id) == result.end()) result.push_back(id);
    }
    return result;
}
void TimelinePanel::contextMenu(QPoint point) {
    if (!document) return;
    QMenu menu;
    menu.addAction("Add blank source", this, [this] { frameCommand(FrameCommand::Add); });
    menu.addAction("Duplicate source", this, [this] { frameCommand(FrameCommand::Duplicate); });
    menu.addAction("Delete source and its references", this, [this] { frameCommand(FrameCommand::Delete); });
    menu.addAction("Rename source…", this, [this] {
        bool ok; auto text = QInputDialog::getText(this, "Frame label", "Name", QLineEdit::Normal, sources->currentText(), &ok);
        if (ok) { QString error; if (!session->frameEdit(document, FrameCommand::Rename, {}, -1, text, &error)) emit failure(error); }
    });
    for (int delta : {-1, 1}) menu.addAction(delta < 0 ? "Move source left" : "Move source right", this, [this, delta] {
        QString error; const int position = sources->currentIndex() + delta;
        if (!session->frameEdit(document, FrameCommand::Move, {}, position, {}, &error)) emit failure(error);
    });
    menu.addAction("Copy source frame", this, [this] {
        for (const auto &frame : document->animationState().frames) if (frame.id == document->activeFrame()) clipboard = frame;
    });
    auto paste = menu.addAction("Paste independent source frame", this, [this] {
        auto state = document->animationState(); auto frame = *clipboard; frame.id = QUuid::createUuid();
        const auto active = frame.activeLayer;
        for (auto &layer : frame.layers) { auto previous = layer.id; layer.id = QUuid::createUuid(); if (previous == active) frame.activeLayer = layer.id; }
        state.active = frame.id; state.entries.push_back({frame.id, 0}); state.frames.push_back(frame);
        apply(state, "Paste Source Frame");
    }); paste->setEnabled(clipboard.has_value());
    menu.addSeparator(); menu.addAction("Repeat entry", this, &TimelinePanel::duplicateEntry);
    menu.addAction("Remove selected entries", this, &TimelinePanel::removeEntries);
    menu.addAction("Move entry left", this, [this] { int row = view->currentIndex().row(); moveEntry(row, row - 1); });
    menu.addAction("Move entry right", this, [this] { int row = view->currentIndex().row(); moveEntry(row, row + 1); });
    menu.addAction("Append reverse sequence (ping-pong)", this, [this] {
        auto state = document->animationState(); const auto count = state.entries.size();
        if (count > 2) for (qsizetype i = qsizetype(count) - 2; i > 0; --i) state.entries.push_back(state.entries[size_t(i)]);
        apply(state, "Build Explicit Ping-Pong Sequence");
    });
    menu.exec(view->viewport()->mapToGlobal(point));
}
void TimelinePanel::hideEvent(QHideEvent *event) { playback.stop(); QWidget::hideEvent(event); }
QJsonObject TimelinePanel::viewSettings() const {
    QJsonObject result;
    for (auto name : {"onionEnabled", "onionTint", "animationLoop"}) result[name] = findChild<QCheckBox *>(name)->isChecked();
    for (auto name : {"onionPrevious", "onionNext", "onionOpacity"}) result[name] = findChild<QSpinBox *>(name)->value();
    result["timelineZoom"] = findChild<QSlider *>("timelineZoom")->value();
    result["animationSpeed"] = findChild<QComboBox *>("animationSpeed")->currentIndex();
    return result;
}
void TimelinePanel::setViewSettings(const QJsonObject &settings) {
    for (auto name : {"onionEnabled", "onionTint", "animationLoop"}) if (settings[name].isBool()) findChild<QCheckBox *>(name)->setChecked(settings[name].toBool());
    for (auto name : {"onionPrevious", "onionNext", "onionOpacity"}) if (settings[name].isDouble()) findChild<QSpinBox *>(name)->setValue(settings[name].toInt());
    if (settings["timelineZoom"].isDouble()) findChild<QSlider *>("timelineZoom")->setValue(settings["timelineZoom"].toInt());
    if (settings["animationSpeed"].isDouble()) findChild<QComboBox *>("animationSpeed")->setCurrentIndex(qBound(0, settings["animationSpeed"].toInt(), 4));
}
} // namespace nova
