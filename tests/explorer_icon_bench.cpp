// SPDX-License-Identifier: GPL-3.0-or-lateral
// Explorer thumbnail benchmark. Builds a generated pack (~500 item/block models with
// parent chains, shared textures and item definitions), views it through AssetModel
// and measures the icon pipeline for (a) a whole-library change, as produced by a
// resource-stack reload or version switch, and (b) one texture edit used by a handful
// of models. Records icons invalidated, icons rebuilt, total GUI-thread capture time
// and the longest single GUI-thread stall. Writes one JSON object to argv[1] or stdout.
#include "assets.h"
#include "models.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <algorithm>
#include <functional>

using namespace nova;

namespace {
QString zero3(int i) { return QString("%1").arg(i, 3, 10, QChar('0')); }

void writeJson(const QString &path, const QJsonObject &value) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        qWarning() << "failed to write" << path;
    else
        file.write(QJsonDocument(value).toJson(QJsonDocument::Compact));
    file.close();
}

QJsonObject cubeElements() {
    QJsonObject faces;
    for (const char *side : {"north", "south", "east", "west", "up", "down"})
        faces[side] = QJsonObject{{"texture", "#layer0"}, {"uv", QJsonArray{0, 0, 16, 16}}};
    return QJsonObject{{"from", QJsonArray{0, 0, 0}},
                       {"to", QJsonArray{16, 16, 16}},
                       {"faces", faces}};
}

// 4 shared block bases (elements + one base texture each), 500 item models: the first
// five reference nova:item/shared directly, the rest parent a base (or the previous
// model every 7th for short parent chains) with per-model textures, and 20 item
// definitions dispatch into the model range.
void makePack(const QString &root) {
    const QString itemModels = root + "/assets/nova/models/item";
    const QString blockModels = root + "/assets/nova/models/block";
    const QString items = root + "/assets/nova/items";
    const QString itemTextures = root + "/assets/nova/textures/item";
    const QString blockTextures = root + "/assets/nova/textures/block";
    for (const QString &dir : {itemModels, blockModels, items, itemTextures, blockTextures})
        QDir().mkpath(dir);
    for (int t = 0; t < 100; ++t) {
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(QColor((40 + t * 2) % 216, 80, 160));
        image.save(itemTextures + "/t" + zero3(t) + ".png");
    }
    for (int b = 0; b < 4; ++b) {
        const QString name = QString("base_") + QChar('a' + b);
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(QColor(90, 30 + b * 40, 60));
        image.save(blockTextures + '/' + name + ".png");
        QJsonObject faces;
        for (const char *side : {"north", "south", "east", "west", "up", "down"})
            faces[side] = QJsonObject{{"texture", "#all"}, {"uv", QJsonArray{0, 0, 16, 16}}};
        writeJson(blockModels + '/' + name + ".json",
                  QJsonObject{{"textures", QJsonObject{{"all", "nova:block/" + name}}},
                              {"elements", QJsonArray{QJsonObject{
                                               {"from", QJsonArray{0, 0, 0}},
                                               {"to", QJsonArray{16, 16, 16}},
                                               {"faces", faces}}}}});
    }
    QImage shared(16, 16, QImage::Format_RGBA8888);
    shared.fill(QColor(10, 200, 30));
    shared.save(itemTextures + "/shared.png");
    for (int i = 0; i < 500; ++i) {
        QJsonObject source;
        if (i < 5) {
            source["textures"] = QJsonObject{{"layer0", "nova:item/shared"}};
            source["elements"] = QJsonArray{cubeElements()};
        } else {
            source["parent"] = (i % 7 == 0) ? "nova:item/m" + zero3(i - 1)
                                            : QString("nova:block/base_") + QChar('a' + i % 4);
            source["textures"] = QJsonObject{
                {"layer0", "nova:item/t" + zero3((i * 37) % 100)}};
        }
        writeJson(itemModels + "/m" + zero3(i) + ".json", source);
    }
    for (int d = 0; d < 20; ++d)
        writeJson(items + "/d" + zero3(d) + ".json",
                  QJsonObject{{"model", QJsonObject{{"type", "minecraft:model"},
                                                    {"model", "nova:item/m" + zero3(d * 25)}}}});
}

QStringList modelRows(const AssetModel &model) {
    QStringList rows;
    std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
        for (int r = 0; r < model.rowCount(parent); ++r) {
            const auto index = model.index(r, 0, parent);
            if (model.isDirectory(index))
                walk(index);
            else {
                const QString path = model.pathFor(index);
                if (path.endsWith(".json") &&
                    (!mc::Location::fromAsset(path, "models").isEmpty() ||
                     !mc::Location::fromAsset(path, "items").isEmpty() ||
                     !mc::Location::fromAsset(path, "blockstates").isEmpty()))
                    rows << path;
            }
        }
    };
    walk({});
    std::sort(rows.begin(), rows.end());
    return rows;
}

// Ask a page of rows for their decoration, as the visible part of Explorer does, and
// keep the event loop running until every started thumbnail finished (pending drains
// to 0 and the capture count stops moving).
bool warmIcons(AssetModel &model, const QStringList &rows, double *wallMs) {
    QElapsedTimer wall;
    wall.start();
    int stableCaptured = -1, stablePending = -1;
    QElapsedTimer guard;
    guard.start();
    while (guard.elapsed() < 120000) {
        for (const QString &path : rows)
            model.data(model.indexFor(path), Qt::DecorationRole);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 5);
        const auto stats = model.iconStats();
        if (stableCaptured == stats.captured && stablePending == model.pendingCount() &&
            model.pendingCount() == 0)
            break;
        stableCaptured = stats.captured;
        stablePending = model.pendingCount();
    }
    if (wallMs)
        *wallMs = wall.nsecsElapsed() / 1e6;
    return model.pendingCount() == 0;
}

QString iconChecksum(AssetModel &model, const QStringList &rows) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &path : rows) {
        const auto icon = qvariant_cast<QIcon>(model.data(model.indexFor(path), Qt::DecorationRole));
        const auto image = icon.pixmap(64, 64).toImage();
        hash.addData(path.toUtf8());
        hash.addData(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
    }
    return QString::fromLatin1(hash.result().toHex());
}

QJsonObject phaseStats(const char *name, const AssetModel &model, double wallMs,
                       const QString &checksum) {
    const auto stats = model.iconStats();
    return QJsonObject{{"phase", name},
                       {"icons_invalidated", stats.invalidated},
                       {"icons_rebuilt", stats.captured},
                       {"gui_capture_ms", double(stats.captureNs) / 1e6},
                       {"longest_capture_ms", double(stats.longestCaptureNs) / 1e6},
                       {"longest_stall_ms", double(stats.longestStallNs) / 1e6},
                       {"rebuild_wall_ms", wallMs},
                       {"icon_sha256", checksum}};
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    QTemporaryDir scratch;
    if (!scratch.isValid())
        return 2;
    const QString pack = scratch.filePath("pack");
    makePack(pack);
    const QString sharedTexture = "assets/nova/textures/item/shared.png";

    QElapsedTimer t;
    t.start();
    auto index = Index::scan(pack);
    const double indexMs = t.nsecsElapsed() / 1e6;
    auto catalog = std::make_shared<mc::Library>("1.21.11");
    t.restart();
    catalog->update(pack, index);
    const double catalogMs = t.nsecsElapsed() / 1e6;

    AssetModel explorer;
    explorer.setIndex(index, pack);
    explorer.setModelSources(catalog);
    const QStringList rows = modelRows(explorer);
    if (rows.size() < 500)
        return 3;
    // Explorer's icon cache (4 MiB at 16 KiB per icon) holds ~256 icons, so a view
    // only keeps the visible page materialized. Drive one stable 64-row page - the
    // five shared-texture models and the definition dispatching the first among them.
    const QStringList page = rows.mid(0, 64);

    // (a) whole-library change: a resource-stack reload or version switch reparses
    // everything and flushes every model icon.
    explorer.resetIconStats();
    double stackWall = 0;
    if (!warmIcons(explorer, page, &stackWall))
        return 4;
    const auto initial = phaseStats("initial_fill", explorer, stackWall, iconChecksum(explorer, page));

    explorer.resetIconStats();
    t.restart();
    auto reloaded = std::make_shared<mc::Library>(*catalog);
    reloaded->update(pack, Index::scan(pack), {""});
    const double stackUpdateMs = t.nsecsElapsed() / 1e6;
    explorer.setModelSources(reloaded);
    double rebuildWall = 0;
    if (!warmIcons(explorer, page, &rebuildWall))
        return 5;
    const auto stackPhase = phaseStats("stack_reload", explorer, rebuildWall,
                                       iconChecksum(explorer, page));

    // (b) one texture edit used by five models (plus the item definition dispatching
    // the first of them).
    QImage edited(16, 16, QImage::Format_RGBA8888);
    edited.fill(QColor(220, 20, 200));
    edited.save(pack + '/' + sharedTexture);
    explorer.resetIconStats();
    t.restart();
    auto refreshed = std::make_shared<mc::Library>(*reloaded);
    refreshed->update(pack, Index::refresh(pack, index, {sharedTexture}), {sharedTexture});
    const double textureUpdateMs = t.nsecsElapsed() / 1e6;
    explorer.setModelSources(refreshed);
    double editWall = 0;
    if (!warmIcons(explorer, page, &editWall))
        return 6;
    const auto editPhase = phaseStats("texture_edit", explorer, editWall,
                                      iconChecksum(explorer, page));

    const QJsonObject output{{"models", rows.size()},
                              {"index_ms", indexMs},
                              {"catalog_ms", catalogMs},
                              {"stack_library_update_ms", stackUpdateMs},
                              {"texture_library_update_ms", textureUpdateMs},
                              {"phases", QJsonArray{initial, stackPhase, editPhase}}};
    const QByteArray json = QJsonDocument(output).toJson(QJsonDocument::Compact);
    if (argc > 1) {
        QFile out(QString::fromLocal8Bit(argv[1]));
        if (!out.open(QIODevice::WriteOnly))
            return 7;
        out.write(json);
    } else
        fwrite(json.constData(), 1, json.size(), stdout);
    return 0;
}
