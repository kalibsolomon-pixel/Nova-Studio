// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "animation_export.h"
#include "modelpanel.h"
#include "scope.h"
#include "timeline.h"
#include <QDialog>
#include <QDir>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPainter>
#include <QScrollArea>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace nova {
namespace {
class ComparisonPixels : public QWidget {
    QImage pixels;

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter paint(this);
        constexpr int cell = 8;
        paint.fillRect(rect(), QColor(174, 174, 174));
        for (int y = 0; y < height(); y += cell)
            for (int x = ((y / cell) % 2) * cell; x < width(); x += 2 * cell)
                paint.fillRect(x, y, cell, cell, QColor(126, 126, 126));
        paint.drawImage(rect(), pixels);
    }

  public:
    void setPixels(const QImage &image, int scale) {
        pixels = image;
        setFixedSize(pixels.size() * scale);
        update();
    }
};
}

std::shared_ptr<Document> Window::reviewRaster(const QString &path) const {
    if (!path.isEmpty())
        for (const auto &doc : session.all())
            if (doc->path == project + '/' + path)
                return doc;
    return {};
}

bool Window::isLocalAsset(const QString &path) const {
    if (project.isEmpty() || !mcp::logicalAsset(path))
        return false;
    mcp::Scope scope(project);
    if (auto doc = reviewRaster(path); doc && !doc->revision().exists)
        return scope.vacant(path) && scope.vacant(path + ".mcmeta");
    if (auto edit = modelPanel->openModelDocument(path); edit && edit->provisional())
        return scope.vacant(path);
    return bool(scope.open(path));
}

bool Window::openRelatedAsset(const QString &path) {
    const bool animation = path.endsWith(".png.mcmeta");
    const auto asset = animation ? path.chopped(7) : path;
    if (!isLocalAsset(asset) || (animation && !isLocalAsset(path) &&
                                 !(reviewRaster(asset) && reviewRaster(asset)->animated()))) {
        statusBar()->showMessage("This reference has no available local asset", 5000);
        return false;
    }
    if (asset.endsWith(".png")) {
        if (!reviewRaster(asset) && !pendingAnimationPairs(project + '/' + asset).isEmpty()) {
            statusBar()->showMessage("Open the texture from the project tree to review pending export recovery", 5000);
            return false;
        }
        // Switch directly when possible so a navigation click never triggers export recovery.
        if (auto doc = reviewRaster(asset))
            showTextureDocument(doc);
        else {
            mcp::Scope scope(project);
            if (!scope.open(asset + ".mcmeta") && !scope.vacant(asset + ".mcmeta")) {
                statusBar()->showMessage("The texture metadata failed project containment", 5000);
                return false;
            }
            openTexture(project + '/' + asset);
        }
        if (animation && current() && current()->document->path == project + '/' + asset) {
            ensureTimeline();
            animationDock->show();
            animationDock->raise();
        }
    } else if (modelPanel->catalog()->editable(asset) &&
               !mc::Location::fromAsset(asset, "models").isEmpty()) {
        openModelAsset(asset);
    } else {
        statusBar()->showMessage("This resource can be inspected from the project tree", 5000);
        return false;
    }
    return tabPath(tabs->currentWidget()) == project + '/' + asset;
}

void Window::showRelatedAssets(QString path) {
    if (path.isEmpty())
        path = tabs->currentWidget() ? QDir(project).relativeFilePath(tabPath(tabs->currentWidget())) : nav.selected;
    if (!mcp::logicalAsset(path) || project.isEmpty())
        return;
    if (auto previous = findChild<QDialog *>("relatedAssetsDialog"))
        previous->close();
    auto dialog = new QDialog(this);
    dialog->setObjectName("relatedAssetsDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Related Assets");
    auto layout = new QVBoxLayout(dialog);
    auto asset = new QLabel(dialog);
    asset->setTextFormat(Qt::PlainText);
    asset->setWordWrap(true);
    asset->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(asset);
    auto list = new QListWidget(dialog);
    list->setObjectName("relatedAssetsList");
    list->setAccessibleName("Related assets and references");
    layout->addWidget(list);
    auto detail = new QLabel(dialog);
    detail->setTextFormat(Qt::PlainText);
    detail->setWordWrap(true);
    detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detail);
    auto actions = new QHBoxLayout;
    layout->addLayout(actions);
    auto open = new QPushButton("Open asset", dialog);
    open->setObjectName("relatedAssetsOpen");
    actions->addWidget(open);
    auto back = new QPushButton("Back to origin", dialog);
    back->setObjectName("relatedAssetsBack");
    actions->addWidget(back);
    actions->addStretch();
    const auto origin = QPointer(tabs->currentWidget());
    const auto root = project;
    const auto populate = [this, asset, list](const QString &selected) {
        asset->setText(selected);
        list->clear();
        const auto catalog = modelPanel->catalog();
        constexpr int limit = 64;
        auto add = [&](const QString &type, const QString &target, bool openable, const QString &message = {}) {
            if (list->count() >= limit)
                return;
            auto row = new QListWidgetItem(type + " · " + target, list);
            if (openable)
                row->setData(Qt::UserRole, target);
            row->setData(Qt::UserRole + 1, message.isEmpty() ? row->text() : message);
        };
        const auto texture = selected.endsWith(".png.mcmeta") ? selected.chopped(7) : selected;
        if (selected.endsWith(".png.mcmeta"))
            add("Spritesheet", texture, isLocalAsset(texture));
        else if (texture.endsWith(".png")) {
            const auto doc = reviewRaster(texture);
            if ((doc && doc->animated()) || collection.records.contains(texture + ".mcmeta"))
                add("Animation metadata", texture + ".mcmeta", true);
        }
        auto addAsset = [&](const QString &target, const QString &type) {
            const bool local = isLocalAsset(target);
            const bool navigable = target.endsWith(".png") ||
                (!mc::Location::fromAsset(target, "models").isEmpty() && catalog->editable(target));
            const bool builtIn = !local && !catalog->providers(target).isEmpty();
            add(local ? type : builtIn ? QString("Base resource") : QString("Missing or unsafe reference"),
                target, local && navigable);
        };
        for (const auto &target : catalog->dependencies(selected)) {
            if (list->count() >= limit)
                break;
            addAsset(target, target.endsWith(".png") ? "Referenced texture" :
                mc::Location::fromAsset(selected, "models").isEmpty() ? "Referenced model" : "Parent model");
        }
        for (const auto &target : catalog->dependents(selected, false)) {
            if (list->count() >= limit)
                break;
            addAsset(target, texture.endsWith(".png") ? "Uses this texture" : "Dependent asset");
        }
        const auto id = mc::Location::fromAsset(selected, "models");
        if (!id.isEmpty() && list->count() < limit)
            for (const auto &message : catalog->model(id).diagnostics)
                add("Model reference", message, false);
        if (!list->count())
            add("No related local assets", selected, false);
        else if (list->count() == limit)
            new QListWidgetItem("Additional references may exist", list);
        list->setCurrentRow(0);
    };
    connect(list, &QListWidget::currentItemChanged, dialog, [open, detail](QListWidgetItem *item) {
        open->setEnabled(item && item->data(Qt::UserRole).isValid());
        detail->setText(item ? item->data(Qt::UserRole + 1).toString() : QString{});
    });
    const auto follow = [this, root, list, populate] {
        if (project != root || !list->currentItem())
            return;
        const auto target = list->currentItem()->data(Qt::UserRole).toString();
        if (!target.isEmpty() && openRelatedAsset(target))
            populate(target);
    };
    connect(open, &QPushButton::clicked, dialog, follow);
    connect(list, &QListWidget::itemActivated, dialog, follow);
    back->setEnabled(bool(origin));
    if (origin)
        connect(origin, &QObject::destroyed, back, [back] { back->setEnabled(false); });
    connect(back, &QPushButton::clicked, dialog, [this, root, origin, dialog] {
        if (project == root && origin && tabs->indexOf(origin) >= 0)
            tabs->setCurrentWidget(origin);
        dialog->close();
    });
    populate(path);
    dialog->resize(550, 290);
    dialog->show();
}

void Window::rememberComparison(Comparison comparison) {
    comparisons.removeIf([&](const auto &item) { return item.path == comparison.path; });
    constexpr int limit = 4;
    if (comparisons.size() >= limit)
        comparisons.removeFirst();
    comparisons.append(std::move(comparison));
}

void Window::clearComparisons(const QString &path) {
    if (comparisonDialog && (path.isEmpty() || comparisonDialog->property("asset").toString() == path)) {
        comparisonDialog->close();
        comparisonDialog = nullptr;
    }
    if (path.isEmpty())
        comparisons.clear();
    else
        comparisons.removeIf([&](const auto &item) { return item.path == path; });
}

bool Window::comparisonCurrent(const Comparison &comparison) const {
    if (auto edit = modelPanel->openModelDocument(comparison.path))
        return edit->instanceId() == comparison.instance && edit->changeSerial() == comparison.serial;
    if (auto doc = reviewRaster(comparison.path))
        return doc->instanceId() == comparison.instance && doc->changeSerial() == comparison.serial;
    return false;
}

void Window::showComparison(const QString &path, const QString &revision) {
    if (comparisonDialog)
        comparisonDialog->close();
    auto dialog = new QDialog(this);
    comparisonDialog = dialog;
    dialog->setObjectName("revisionComparison");
    dialog->setProperty("asset", path);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Compare Revisions");
    auto layout = new QVBoxLayout(dialog);
    auto identity = new QLabel(dialog);
    identity->setObjectName("comparisonIdentity");
    identity->setTextFormat(Qt::PlainText);
    identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    identity->setWordWrap(true);
    layout->addWidget(identity);
    const auto it = std::find_if(comparisons.cbegin(), comparisons.cend(), [&](const auto &capture) {
        return capture.path == path && capture.afterRevision == revision;
    });
    if (it == comparisons.cend() || !isLocalAsset(path)) {
        identity->setText(path + "\nThe original revision is unavailable for this change.");
        dialog->resize(440, 100);
        dialog->show();
        return;
    }
    const auto capture = *it;
    const auto updateIdentity = [this, identity, capture] {
        identity->setText(capture.path + "\nBefore " + capture.beforeRevision.left(12) +
                          "  ·  After " + capture.afterRevision.left(12) + "  ·  " +
                          (comparisonCurrent(capture) ? "Current" : "Stale snapshot") +
                          (capture.detail.isEmpty() ? QString{} : "\n" + capture.detail));
        identity->setToolTip("Before " + capture.beforeRevision + "\nAfter " + capture.afterRevision);
    };
    updateIdentity();
    auto timer = new QTimer(dialog);
    connect(timer, &QTimer::timeout, dialog, updateIdentity);
    timer->start(250);
    auto toolbar = new QHBoxLayout;
    layout->addLayout(toolbar);
    auto view = new QComboBox(dialog);
    view->setAccessibleName("Preview type");
    view->addItem("Model");
    view->addItem("UV layout");
    view->setVisible(!capture.uvBefore.isNull() || !capture.uvAfter.isNull() ||
                     capture.detail.startsWith("UV face"));
    toolbar->addWidget(view);
    auto mode = new QComboBox(dialog);
    mode->setAccessibleName("Comparison view");
    mode->addItems({"Side by side", "Before only", "After only"});
    toolbar->addWidget(mode);
    auto zoom = new QComboBox(dialog);
    zoom->setAccessibleName("Pixel zoom");
    zoom->addItems({"100%", "200%", "400%", "800%"});
    // Open small textures at a readable integer scale; the view remains pixel-exact.
    const int extent = std::max({capture.before.width(), capture.before.height(),
                                 capture.after.width(), capture.after.height(), 1});
    int fit = 0;
    while (fit < 3 && extent << (fit + 1) <= 256)
        ++fit;
    zoom->setCurrentIndex(fit);
    toolbar->addWidget(zoom);
    toolbar->addStretch();
    auto images = new QHBoxLayout;
    layout->addLayout(images, 1);
    auto addPane = [&](const QString &title) {
        auto pane = new QWidget(dialog);
        auto box = new QVBoxLayout(pane);
        box->setContentsMargins(0, 0, 0, 0);
        auto heading = new QLabel(title, pane);
        heading->setWordWrap(true);
        box->addWidget(heading);
        auto scroll = new QScrollArea(pane);
        scroll->setAccessibleName(title + " preview");
        scroll->setAlignment(Qt::AlignCenter);
        auto pixels = new ComparisonPixels;
        scroll->setWidget(pixels);
        box->addWidget(scroll, 1);
        images->addWidget(pane, 1);
        return [pane, heading, scroll, pixels, title](const QImage &image, int factor, bool visible) {
            pane->setVisible(visible);
            heading->setText(image.isNull() ? title + " · Snapshot unavailable" :
                QString("%1 · %2 × %3 px").arg(title).arg(image.width()).arg(image.height()));
            scroll->setVisible(!image.isNull());
            if (!image.isNull())
                pixels->setPixels(image, factor);
        };
    };
    const auto before = addPane("Before");
    const auto after = addPane("After");
    const auto redraw = [capture, before, after, mode, view, zoom] {
        const auto uv = view->currentIndex() == 1;
        const auto factor = 1 << zoom->currentIndex();
        before(uv ? capture.uvBefore : capture.before, factor, mode->currentIndex() != 2);
        after(uv ? capture.uvAfter : capture.after, factor, mode->currentIndex() != 1);
    };
    for (auto control : {view, mode, zoom})
        connect(control, &QComboBox::currentIndexChanged, dialog, redraw);
    redraw();
    dialog->resize(640, 385);
    dialog->show();
}
} // namespace nova
