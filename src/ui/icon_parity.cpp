// SPDX-License-Identifier: GPL-3.0-or-later
#include "icon_parity.h"
#include "assets.h"
#include "companion.h"
#include "models.h"
#include "theme.h"
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace nova {
namespace {
constexpr int Shown = 64; // icons are shown at least this size, nearest-neighbour

QLabel *picture(const QImage &image) {
    auto label = new QLabel;
    label->setAlignment(Qt::AlignCenter);
    if (!image.isNull()) {
        const int side = qMax(Shown, image.width());
        label->setPixmap(QPixmap::fromImage(image.scaled(side, side, Qt::KeepAspectRatio, Qt::FastTransformation)));
    }
    return label;
}
} // namespace

IconComparison compareIcons(const QImage &nova, const QImage &minecraft, const QColor &background, int tolerance) {
    IconComparison result;
    result.minecraft = minecraft.convertToFormat(QImage::Format_ARGB32);
    if (!nova.isNull()) {
        result.nova = QImage(nova.size(), QImage::Format_ARGB32);
        result.nova.fill(background);
        QPainter painter(&result.nova);
        painter.drawImage(0, 0, nova.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }
    if (result.nova.isNull() || result.minecraft.isNull() || result.nova.size() != result.minecraft.size())
        return result;
    result.difference = QImage(result.nova.size(), QImage::Format_ARGB32);
    for (int y = 0; y < result.nova.height(); ++y)
        for (int x = 0; x < result.nova.width(); ++x) {
            const QColor a = result.nova.pixelColor(x, y), b = result.minecraft.pixelColor(x, y);
            const int delta = qMax(qMax(qAbs(a.red() - b.red()), qAbs(a.green() - b.green())), qAbs(a.blue() - b.blue()));
            result.maxDelta = qMax(result.maxDelta, delta);
            ++result.total;
            // Matching pixels fade to grey; differing ones turn red by how much they differ.
            const int grey = qGray(b.rgb()) / 3 + 40;
            if (delta > tolerance) {
                ++result.differing;
                result.difference.setPixelColor(x, y, QColor(qMin(255, 120 + delta), 24, 24));
            } else {
                result.difference.setPixelColor(x, y, QColor(grey, grey, grey));
            }
        }
    return result;
}

QStringList defaultParityItems() {
    return {"minecraft:stone",          "minecraft:grass_block",  "minecraft:oak_leaves",
            "minecraft:glass",          "minecraft:red_stained_glass", "minecraft:oak_log",
            "minecraft:oak_stairs",     "minecraft:torch",        "minecraft:apple",
            "minecraft:diamond_sword",  "minecraft:leather_helmet", "minecraft:potion",
            "minecraft:chest",          "minecraft:shield",       "minecraft:player_head",
            "minecraft:red_bed"};
}

IconParityDialog::IconParityDialog(Context ctx, QWidget *parent) : QDialog(parent), context(std::move(ctx)) {
    setObjectName("iconParityDialog");
    setWindowTitle("Compare Icons with Minecraft");
    resize(760, 720);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::PanelPadding, ui::PanelPadding, ui::PanelPadding, ui::PanelPadding);
    layout->setSpacing(ui::SpaceXL);

    auto itemsBox = new QGroupBox("Items");
    itemsBox->setProperty("role", "section");
    auto itemsLayout = new QVBoxLayout(itemsBox);
    itemsLayout->addWidget(ui::hint("One item ID per line, up to 64. Minecraft draws them in inventory slots at "
                                    "its current GUI scale with the packs it has loaded; Nova draws the same "
                                    "items from this pack and its resource stack."));
    itemList = new QPlainTextEdit;
    itemList->setObjectName("parityItems");
    itemList->setPlainText(defaultParityItems().join('\n'));
    itemList->setMaximumHeight(120);
    itemsLayout->addWidget(itemList);
    auto choices = new QHBoxLayout;
    auto standard = new QPushButton("Reference Set");
    standard->setObjectName("parityDefaultItems");
    auto pack = new QPushButton("This Pack's Items");
    pack->setObjectName("parityPackItems");
    choices->addWidget(standard);
    choices->addWidget(pack);
    choices->addStretch();
    itemsLayout->addLayout(choices);
    layout->addWidget(itemsBox);

    status = ui::hint("Minecraft must be running from Nova's test environment.");
    status->setObjectName("parityStatus");
    layout->addWidget(status);
    results = new QTableWidget(0, 5);
    results->setObjectName("parityResults");
    results->setHorizontalHeaderLabels({"Item", "Nova", "Minecraft", "Difference", "Result"});
    results->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    results->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    results->verticalHeader()->hide();
    results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(results, 1);

    auto buttons = new QHBoxLayout;
    auto folder = new QPushButton("Open Folder");
    folder->setObjectName("parityOpenFolder");
    compareButton = new QPushButton("Compare with Minecraft");
    compareButton->setObjectName("parityCompare");
    ui::setPrimary(compareButton);
    auto close = new QPushButton("Close");
    buttons->addWidget(folder);
    buttons->addStretch();
    buttons->addWidget(compareButton);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(standard, &QPushButton::clicked, this, [this] { itemList->setPlainText(defaultParityItems().join('\n')); });
    connect(pack, &QPushButton::clicked, this, &IconParityDialog::usePackItems);
    connect(compareButton, &QPushButton::clicked, this, &IconParityDialog::compare);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(folder, &QPushButton::clicked, this, [this] {
        const auto path = context.folder ? context.folder() : QString();
        QDir().mkpath(path);
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    if (context.companion)
        connect(context.companion, &testing::CompanionLink::iconsFinished, this, &IconParityDialog::finished);
}

QStringList IconParityDialog::items() const {
    QStringList list;
    for (auto line : itemList->toPlainText().split('\n')) {
        line = line.trimmed().toLower();
        if (line.isEmpty())
            continue;
        if (!line.contains(':'))
            line.prepend("minecraft:");
        if (!list.contains(line))
            list << line;
    }
    return list;
}

void IconParityDialog::usePackItems() {
    const auto root = context.projectRoot ? context.projectRoot() : QString();
    QStringList found;
    QDirIterator it(root + "/assets", {"*.json"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && found.size() < 64) {
        const auto relative = QDir(root + "/assets").relativeFilePath(it.next());
        const auto parts = relative.split('/');
        if (parts.size() >= 3 && parts[1] == "items")
            found << parts[0] + ':' + parts.mid(2).join('/').chopped(5);
    }
    found.sort();
    if (found.isEmpty()) {
        status->setText("This pack has no item definitions (assets/<namespace>/items/*.json).");
        return;
    }
    itemList->setPlainText(found.join('\n'));
}

void IconParityDialog::compare() {
    requested = items();
    QString error;
    if (!context.companion) {
        error = "Minecraft is not running from Nova's test environment.";
    } else if (context.companion->requestIcons(requested, &error, 0, false)) {
        compareButton->setEnabled(false);
        status->setText(QString("Minecraft is drawing %1 icons…").arg(requested.size()));
        return;
    }
    status->setText(error);
}

void IconParityDialog::finished(bool ok, const QJsonObject &result, const QString &error) {
    compareButton->setEnabled(true);
    if (!ok) {
        status->setText("Minecraft could not draw the icons: " + error);
        return;
    }
    const int scale = result.value("guiScale").toInt(1);
    const QColor background(result.value("background").toString("#8b8b8b"));
    const auto files = result.value("files").toArray();
    const auto library = context.library ? context.library() : nullptr;
    const auto &t = ui::current();
    results->setRowCount(0);
    QJsonArray report;
    int matching = 0, differing = 0, unavailable = 0;
    for (int i = 0; i < requested.size(); ++i) {
        const auto id = requested[i];
        const auto file = files.at(i).toString();
        const QImage game(file);
        QImage icon;
        if (library) {
            const auto ns = id.section(':', 0, 0), path = id.section(':', 1);
            icon = renderInventoryIcon(*library, "assets/" + ns + "/items/" + path + ".json", scale);
        }
        const auto comparison = compareIcons(icon, game, background);
        QString verdict;
        QColor ink;
        if (game.isNull()) {
            verdict = "Not an item in this Minecraft version";
            ink = t.muted;
            ++unavailable;
        } else if (icon.isNull()) {
            verdict = "Nova could not draw it (no model in the pack, resource stack or vanilla baseline)";
            ink = t.warning;
            ++unavailable;
        } else if (comparison.matches()) {
            verdict = "Match";
            ink = t.success;
            ++matching;
        } else {
            verdict = QString("Differs · %1% of pixels, largest channel difference %2")
                          .arg(QString::number(100.0 * comparison.differing / qMax(1, comparison.total), 'f', 1))
                          .arg(comparison.maxDelta);
            ink = t.danger;
            ++differing;
        }
        if (!file.isEmpty()) {
            const QFileInfo info(file);
            if (!comparison.nova.isNull())
                comparison.nova.save(info.dir().filePath(QString("nova-%1.png").arg(i)));
            if (!comparison.difference.isNull())
                comparison.difference.save(info.dir().filePath(QString("difference-%1.png").arg(i)));
        }
        report.append(QJsonObject{{"item", id}, {"result", verdict}, {"differingPixels", comparison.differing},
                                  {"pixels", comparison.total}, {"maxDelta", comparison.maxDelta}});
        const int row = results->rowCount();
        results->insertRow(row);
        results->setItem(row, 0, new QTableWidgetItem(id));
        results->setCellWidget(row, 1, picture(comparison.nova));
        results->setCellWidget(row, 2, picture(comparison.minecraft));
        results->setCellWidget(row, 3, picture(comparison.difference));
        auto verdictItem = new QTableWidgetItem(verdict);
        verdictItem->setForeground(ink);
        results->setItem(row, 4, verdictItem);
        results->setRowHeight(row, qMax(Shown, 16 * scale) + ui::SpaceL);
    }
    if (!files.isEmpty() && !files.at(0).toString().isEmpty()) {
        const QFileInfo first(files.at(0).toString());
        QFile out(first.dir().filePath("report.json"));
        if (out.open(QIODevice::WriteOnly))
            out.write(QJsonDocument(QJsonObject{{"guiScale", scale}, {"items", report}}).toJson());
    }
    status->setText(QString("GUI scale %1 · %2 match · %3 differ · %4 not compared")
                        .arg(scale).arg(matching).arg(differing).arg(unavailable));
}

} // namespace nova
