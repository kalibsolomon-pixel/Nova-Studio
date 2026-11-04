// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_batch.h"
#include "minecraft_animation.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QImageWriter>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QThread>
#include <mutex>
#include <thread>

namespace nova::batch {
namespace {
constexpr qint64 NativeLimit = 128LL * 1024 * 1024;
bool fail(QString *error, const QString &text) {
    if (error)
        *error = text;
    return false;
}
QByteArray encodePng(const QImage &image) {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    return QImageWriter(&buffer, "png").write(image) ? png : QByteArray();
}
QString hex(const QByteArray &bytes) { return QString::fromLatin1(bytes.toHex()); }
QByteArray sha(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
bool inside(const QString &root, const QString &path) {
    const auto r = QDir::cleanPath(QFileInfo(root).absoluteFilePath()) + '/';
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).startsWith(r, Qt::CaseInsensitive);
}
QByteArray readLimited(const QString &path, bool *ok) {
    QFile f(path);
    *ok = f.open(QIODevice::ReadOnly) && f.size() <= NativeLimit;
    return *ok ? f.readAll() : QByteArray();
}
// Operations whose per-layer and per-frame behaviour is defined and covered by tests.
const QSet<QString> &nativeOperations() {
    static const QSet<QString> ids{"brightness_contrast", "hue_saturation", "exposure", "gamma",
                                   "temperature_tint", "levels", "curves", "channels",
                                   "replace_color", "grayscale", "invert", "colorize", "posterize",
                                   "quantize", "palette_map", "alpha_threshold",
                                   "directional_shading", "flip", "rotate", "image_flip",
                                   "image_rotate", "resize_image"};
    return ids;
}
bool nativeSupported(const ops::Spec &step, QString *error) {
    const auto info = ops::find(step.id);
    if (!info || !nativeOperations().contains(step.id))
        return fail(error, QString("%1 is not available for native layer batches yet")
                               .arg(info ? info->name : step.id));
    if (step.id == "directional_shading" && step.params["output"].toString() == "Shading layer")
        return fail(error, "Directional shading that creates a layer is not available in batches");
    return true;
}
struct Resolved {
    QList<int> frames;
    QStringList labels;
    QString error;
};
bool layerSelected(const RasterLayer &layer, bool active, const NativeOptions &o) {
    switch (o.layers) {
    case LayerScope::Active: return active;
    case LayerScope::All: return true;
    case LayerScope::Visible: return layer.visible;
    case LayerScope::Selected:
        return o.layerNames.contains(layer.name) ||
               o.layerNames.contains(layer.id.toString(QUuid::WithoutBraces)) ||
               o.layerNames.contains(layer.id.toString());
    case LayerScope::CompositeOnly: return false;
    }
    return false;
}
// Resolves the frames and layers a document will touch. Repeated playback references
// collapse to one source frame so it is processed exactly once.
Resolved resolve(const Document &doc, const NativeOptions &o) {
    Resolved s;
    if (!doc.animated()) {
        for (const auto &l : doc.layers())
            if (layerSelected(l, l.id == doc.activeLayer(), o))
                s.labels << l.name;
        if (o.layers != LayerScope::CompositeOnly && s.labels.isEmpty())
            s.error = "No layer matches the layer scope";
        return s;
    }
    const auto state = doc.animationState();
    const int count = int(state.frames.size());
    auto frameIndex = [&](QUuid id) {
        for (int i = 0; i < count; ++i)
            if (state.frames[size_t(i)].id == id)
                return i;
        return -1;
    };
    switch (o.frames) {
    case FrameScope::Current: s.frames << std::max(0, frameIndex(doc.activeFrame())); break;
    case FrameScope::All:
        for (int i = 0; i < count; ++i)
            s.frames << i;
        break;
    case FrameScope::Selected:
        for (int i : o.frameIndices)
            if (i >= 0 && i < count && !s.frames.contains(i))
                s.frames << i;
        break;
    case FrameScope::PlaybackEntries:
        for (int e : o.playbackIndices)
            if (e >= 0 && e < int(state.entries.size())) {
                const int f = frameIndex(state.entries[size_t(e)].frame);
                if (f >= 0 && !s.frames.contains(f))
                    s.frames << f;
            }
        break;
    }
    std::sort(s.frames.begin(), s.frames.end());
    if (s.frames.isEmpty()) {
        s.error = "No source frame matches the frame scope";
        return s;
    }
    if (o.layers == LayerScope::CompositeOnly) {
        s.error = "Composite-only export of animations: use Export Minecraft Animation instead";
        return s;
    }
    for (int f : s.frames) {
        const auto &frame = state.frames[size_t(f)];
        for (const auto &l : frame.layers)
            if (layerSelected(l, l.id == frame.activeLayer, o))
                s.labels << QString("%1 / %2").arg(frame.name, l.name);
    }
    if (s.labels.isEmpty())
        s.error = "No layer in the selected frames matches the layer scope";
    return s;
}
std::shared_ptr<Document> openNative(const QString &root, const QString &png, QString *error) {
    auto doc = Document::open(png, error);
    if (!doc)
        return {};
    if (!doc->loadNative(root, error))
        return {};
    return doc;
}
// Applies every enabled step to the scoped layers/frames through Document commands
// (the shared operation engine); never flattens.
bool process(Document &doc, const Pipeline &pipeline, const NativeOptions &o, const Resolved &scope,
             const ops::Context &context, QString *error) {
    for (const auto &input : pipeline.steps) {
        if (!input.enabled)
            continue;
        auto step = input;
        if (!ops::validate(step, error) || !nativeSupported(step, error))
            return false;
        const auto info = ops::find(step.id);
        if (!doc.animated()) {
            QVector<QUuid> ids;
            for (const auto &l : doc.layers())
                if (layerSelected(l, l.id == doc.activeLayer(), o))
                    ids << l.id;
            if (!doc.applyLayers(ids, step, error, &context))
                return false;
            continue;
        }
        const auto state = doc.animationState();
        if (info->changesSize) {
            std::vector<QUuid> all;
            for (const auto &f : state.frames)
                all.push_back(f.id);
            if (scope.frames.size() != int(state.frames.size()) || o.layers != LayerScope::All)
                return fail(error, info->name + " changes the canvas: choose All frames and All layers");
            if (!doc.applyFrames(all, step, error, &context))
                return false;
            continue;
        }
        auto next = state;
        int changed = 0;
        for (int f : scope.frames) {
            auto &frame = next.frames[size_t(f)];
            for (auto &layer : frame.layers) {
                if (!layerSelected(layer, layer.id == frame.activeLayer, o))
                    continue;
                if (context.canceled())
                    return fail(error, "Operation canceled");
                auto result = ops::run(step, layer.pixels, context);
                if (!result.ok())
                    return fail(error, result.canceled ? QString("Operation canceled") : result.error);
                if (result.image.size() != layer.pixels.size())
                    return fail(error, "Layer operation changed the frame size");
                layer.pixels = result.image.convertToFormat(layer.pixels.format());
                ++layer.revision;
                ++changed;
            }
        }
        if (changed && !doc.replaceAnimation(next, info->name + " (native batch)", error))
            return false;
    }
    return true;
}
QJsonObject optionsJson(const NativeOptions &o) {
    QJsonArray names, frames, entries;
    for (const auto &n : o.layerNames)
        names.append(n);
    for (int f : o.frameIndices)
        frames.append(f);
    for (int e : o.playbackIndices)
        entries.append(e);
    return {{"mode", sourceModeNames()[int(o.mode)]},
            {"layers", layerScopeNames()[int(o.layers)]},
            {"layer_names", names},
            {"frames", frameScopeNames()[int(o.frames)]},
            {"frame_indices", frames},
            {"playback_indices", entries},
            {"export_directory", o.exportOutput.directory},
            {"memory_budget", double(o.memoryBudget)}};
}
// Checksummed journal; every state change rewrites it atomically.
class Journal {
    QString file;
    QJsonObject data;
    Revision revision;
    std::mutex lock;

  public:
    static QByteArray checksum(QJsonObject data) {
        data.remove("checksum");
        return sha(QJsonDocument(data).toJson(QJsonDocument::Compact));
    }
    bool create(const QString &path, QJsonObject initial, QString *error) {
        file = path;
        data = std::move(initial);
        return flush(error);
    }
    bool flush(QString *error) {
        data["checksum"] = hex(checksum(data));
        const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Indented);
        if (!atomicWrite(file, bytes, revision, error))
            return false;
        revision = Revision::read(file);
        return true;
    }
    bool update(int index, const QJsonObject &fields, QString *error = nullptr) {
        std::lock_guard guard(lock);
        auto items = data["items"].toArray();
        auto item = items[index].toObject();
        for (auto it = fields.begin(); it != fields.end(); ++it)
            item[it.key()] = it.value();
        item["updated"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        items[index] = item;
        data["items"] = items;
        return flush(error);
    }
    bool finish(const QJsonObject &fields, QString *error) {
        std::lock_guard guard(lock);
        for (auto it = fields.begin(); it != fields.end(); ++it)
            data[it.key()] = it.value();
        return flush(error);
    }
};
} // namespace

QStringList sourceModeNames() {
    return {"Exported PNG", "Native document", "Native document + PNG export"};
}
QStringList layerScopeNames() {
    return {"Active layer", "Selected layers", "All layers", "Visible layers", "Composite only (export)"};
}
QStringList frameScopeNames() {
    return {"Current source frame", "Selected source frames", "All source frames",
            "Playback entries (unique frames)"};
}
QString nativeStatusName(NativeItem::Status s) {
    static const char *names[] = {"Ready",  "Conflict", "Invalid",  "Skipped",
                                  "Done",   "Failed",   "Canceled", "Rolled back"};
    return names[int(s)];
}
QString NativePlan::summary() const {
    return QString("%1 native documents · %2 ready · %3 conflicts · %4 invalid · ~%5 MiB peak · "
                   "~%6 MiB total work · %7 worker(s)")
        .arg(items.size())
        .arg(ready)
        .arg(conflicts)
        .arg(invalid)
        .arg(double(peakBytes) / (1024 * 1024), 0, 'f', 1)
        .arg(double(totalBytes) / (1024 * 1024), 0, 'f', 1)
        .arg(workers);
}
NativePlan planNative(Pipeline pipeline, const QString &root, const QStringList &files,
                      const NativeOptions &o) {
    NativePlan plan;
    QString pipelineError, optionError;
    bool pipelineOk = pipeline.validate(&pipelineError);
    for (const auto &step : pipeline.steps)
        if (pipelineOk && step.enabled && !nativeSupported(step, &pipelineError))
            pipelineOk = false;
    const bool exporting = o.mode == SourceMode::NativeWithExport || o.layers == LayerScope::CompositeOnly;
    if (o.mode == SourceMode::ExportedPng)
        optionError = "Exported PNG mode uses the standard batch planner";
    else if (o.transactionRoot.trimmed().isEmpty() || inside(root, o.transactionRoot) ||
             QDir::cleanPath(QFileInfo(o.transactionRoot).absoluteFilePath())
                     .compare(QDir::cleanPath(QFileInfo(root).absoluteFilePath()), Qt::CaseInsensitive) == 0)
        optionError = "Choose a transaction directory outside the resource pack";
    else if (exporting && (o.exportOutput.directory.trimmed().isEmpty() ||
                           inside(root, o.exportOutput.directory)))
        optionError = "PNG export needs an output directory outside the resource pack";
    else if (files.size() > 4096)
        optionError = "Native batches are limited to 4096 documents";
    QSet<QString> seen, outputs, blocked;
    for (const auto &b : o.blocked)
        blocked.insert(QFileInfo(b).absoluteFilePath().toCaseFolded());
    int index = 0;
    for (const auto &relative : files) {
        NativeItem item;
        item.relative = relative;
        item.png = QDir::cleanPath(root + '/' + relative);
        auto invalid = [&](const QString &why) {
            if (item.status == NativeItem::Status::Ready) {
                item.status = NativeItem::Status::Invalid;
                item.message = why;
            }
        };
        const auto key = QFileInfo(item.png).absoluteFilePath().toCaseFolded();
        if (!pipelineOk)
            invalid(pipelineError);
        else if (!optionError.isEmpty())
            invalid(optionError);
        else if (seen.contains(key)) {
            item.status = NativeItem::Status::Conflict;
            item.message = "Duplicate source path in this batch";
        } else if (!relative.endsWith(".png", Qt::CaseInsensitive) || !QFileInfo(item.png).isFile() ||
                   QFileInfo(item.png).isSymLink() || !inside(root, item.png))
            invalid("Missing, linked or non-PNG target");
        seen.insert(key);
        if (item.status == NativeItem::Status::Ready) {
            QString error;
            item.nvdoc = nativeDocumentPath(root, item.png, &error);
            if (item.nvdoc.isEmpty() || !QFileInfo(item.nvdoc).isFile())
                invalid("No editable native document exists for this PNG");
            else if (blocked.contains(key)) {
                item.status = NativeItem::Status::Conflict;
                item.message = "Open with unsaved or export-pending edits; save or close it first";
            }
        }
        std::shared_ptr<Document> doc;
        if (item.status == NativeItem::Status::Ready) {
            item.sourceRevision = Revision::read(item.nvdoc, NativeLimit);
            QString error;
            doc = openNative(root, item.png, &error);
            if (!doc)
                invalid("Invalid native document: " + error);
        }
        if (doc) {
            item.animated = doc->animated();
            item.size = doc->image.size();
            const auto state = doc->animationState();
            item.frameCount = item.animated ? int(state.frames.size()) : 1;
            item.playbackCount = item.animated ? int(state.entries.size()) : 0;
            item.layerCount = int(doc->layers().size());
            const auto scope = resolve(*doc, o);
            if (!scope.error.isEmpty())
                invalid(scope.error);
            item.targetLayers = scope.labels;
            item.targetFrames = scope.frames;
            for (const auto &step : pipeline.steps) {
                const auto info = step.enabled ? ops::find(step.id) : nullptr;
                if (info && info->changesSize &&
                    (o.layers != LayerScope::All ||
                     (item.animated && item.targetFrames.size() != item.frameCount)))
                    invalid(info->name + " changes the canvas: choose All layers" +
                            (item.animated ? QString(" and All source frames") : QString()));
            }
            if (item.animated && o.frames == FrameScope::PlaybackEntries)
                item.warnings << QString("%1 playback entries resolve to %2 unique source frame(s)")
                                     .arg(o.playbackIndices.size())
                                     .arg(scope.frames.size());
            if (o.mode == SourceMode::Native)
                item.warnings << "Pack PNG is not re-exported; the document becomes export pending";
            QSize size = item.size;
            const qint64 layers = std::max<qint64>(1, item.targetLayers.size());
            for (const auto &step : pipeline.steps)
                if (step.enabled) {
                    item.estimatedBytes = std::max(item.estimatedBytes, ops::estimateBytes(step, size));
                    size = ops::outputSize(step, size);
                }
            item.outputSize = size;
            // Whole document is held twice (original state + result) plus working buffers.
            item.estimatedBytes += 2 * doc->pixelBytes() + layers * 4LL * size.width() * size.height();
            if (item.estimatedBytes > o.memoryBudget)
                invalid(QString("Estimated %1 MiB exceeds the %2 MiB memory budget")
                            .arg(item.estimatedBytes / (1024 * 1024))
                            .arg(o.memoryBudget / (1024 * 1024)));
            if (size.width() > Document::MaxDimension || size.height() > Document::MaxDimension)
                invalid("Pipeline output would exceed 4096 px");
        }
        if (item.status == NativeItem::Status::Ready && exporting) {
            QString error;
            if (!safeOutputPath(o.exportOutput, item.relative, index, &item.exportPath, &error))
                invalid(error);
            else if (outputs.contains(item.exportPath.toCaseFolded())) {
                item.status = NativeItem::Status::Conflict;
                item.message = "Another target exports to the same file";
            } else {
                outputs.insert(item.exportPath.toCaseFolded());
                item.exportRevision = Revision::read(item.exportPath);
                if (item.exportRevision.exists && !o.exportOutput.overwrite) {
                    item.status = NativeItem::Status::Conflict;
                    item.message = "The export destination exists (enable overwrite to replace it)";
                }
            }
        }
        plan.ready += item.status == NativeItem::Status::Ready;
        plan.conflicts += item.status == NativeItem::Status::Conflict;
        plan.invalid += item.status == NativeItem::Status::Invalid;
        if (item.status == NativeItem::Status::Ready) {
            plan.peakBytes = std::max(plan.peakBytes, item.estimatedBytes);
            plan.totalBytes += item.estimatedBytes;
        }
        plan.items.push_back(std::move(item));
        ++index;
    }
    const int cores = std::max(1, QThread::idealThreadCount());
    plan.workers = std::clamp(cores, 1, 4);
    while (plan.workers > 1 && qint64(plan.workers) * plan.peakBytes > o.memoryBudget)
        --plan.workers;
    return plan;
}
bool previewNative(const Pipeline &pipeline, const NativeItem &item, const QString &root,
                   const NativeOptions &o, QImage *before, QImage *after, QString *error) {
    auto doc = openNative(root, item.png, error);
    if (!doc)
        return false;
    const auto frame = doc->animated() && !item.targetFrames.isEmpty()
                           ? doc->animationState().frames[size_t(item.targetFrames.first())].id
                           : QUuid();
    auto composite = [&] { return frame.isNull() ? doc->image : doc->frameComposite(frame); };
    *before = composite();
    if (o.layers == LayerScope::CompositeOnly) {
        auto result = ops::runPipeline(pipeline.steps, *before, {});
        *after = result.image;
        return result.ok() || fail(error, result.error);
    }
    const auto scope = resolve(*doc, o);
    if (!scope.error.isEmpty())
        return fail(error, scope.error);
    if (!process(*doc, pipeline, o, scope, {}, error))
        return false;
    *after = composite();
    return true;
}
NativeReport executeNative(const Pipeline &pipeline, NativePlan &plan, const QString &root,
                           const NativeOptions &o, const ops::Context &context,
                           const std::atomic_bool *cancel, const std::function<void(int)> &progress) {
    NativeReport report;
    QElapsedTimer timer;
    timer.start();
    const auto stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz");
    const auto directory = QDir::cleanPath(o.transactionRoot + "/nova-native-batch-" + stamp);
    if (o.transactionRoot.isEmpty() || inside(root, directory) || !QDir().mkpath(directory + "/originals")) {
        report.error = "Could not create the transaction directory outside the pack";
        return report;
    }
    QJsonArray items;
    for (size_t i = 0; i < plan.items.size(); ++i) {
        const auto &item = plan.items[i];
        items.append(QJsonObject{{"png", item.relative},
                                 {"nvdoc", item.nvdoc},
                                 {"backup", QString("originals/%1.nvdoc").arg(i, 4, 10, QChar('0'))},
                                 {"original_sha256", hex(item.sourceRevision.hash)},
                                 {"state", item.status == NativeItem::Status::Ready ? "planned" : "skipped"},
                                 {"message", item.message}});
    }
    Journal journal;
    QString journalError;
    report.journal = directory + "/journal.json";
    if (!journal.create(report.journal,
                        {{"format", "nova-native-batch-journal"},
                         {"schema", 1},
                         {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                         {"root", QFileInfo(root).canonicalFilePath()},
                         {"pipeline", pipeline.toJson()},
                         {"options", optionsJson(o)},
                         {"transaction",
                          "recoverable per-document replacements; not an all-or-nothing commit"},
                         {"complete", false},
                         {"items", items}},
                        &journalError)) {
        report.error = "Journal could not be written: " + journalError;
        return report;
    }
    std::atomic_int next = 0, done = 0, failed = 0, skipped = 0, canceled = 0, rolledBack = 0;
    std::atomic_bool interrupted = false;
    std::mutex directories;
    auto work = [&] {
        ops::Context ctx = context;
        ctx.cancel = cancel;
        for (;;) {
            if (interrupted)
                return;
            const int i = next++;
            if (i >= int(plan.items.size()))
                return;
            auto &item = plan.items[size_t(i)];
            auto finish = [&](NativeItem::Status status, const QString &message, const QString &state) {
                item.status = status;
                if (!message.isEmpty())
                    item.message = message;
                (status == NativeItem::Status::Done         ? done
                 : status == NativeItem::Status::Failed     ? failed
                 : status == NativeItem::Status::Canceled   ? canceled
                 : status == NativeItem::Status::RolledBack ? rolledBack
                                                            : skipped)++;
                journal.update(i, {{"state", state}, {"message", item.message}});
                if (progress)
                    progress(i);
            };
            auto injected = [&](const char *stage) { return o.fault && o.fault(stage, i); };
            if (item.status != NativeItem::Status::Ready) {
                finish(NativeItem::Status::Skipped, item.message.isEmpty() ? "Not ready" : item.message,
                       "skipped");
                continue;
            }
            if (cancel && cancel->load()) {
                finish(NativeItem::Status::Canceled, "Canceled before processing", "canceled");
                continue;
            }
            // 1. Original bytes must still match the dry run.
            bool readable = false;
            const auto original = readLimited(item.nvdoc, &readable);
            if (!readable || sha(original) != item.sourceRevision.hash) {
                finish(NativeItem::Status::Failed, "Native document changed after the dry run; untouched",
                       "failed");
                continue;
            }
            // 2. Recoverable copy of the original, verified, before anything else.
            const auto backup = directory + QString("/originals/%1.nvdoc").arg(i, 4, 10, QChar('0'));
            QString error;
            if (injected("backup") || !atomicWrite(backup, original, Revision{}, &error) ||
                Revision::read(backup, NativeLimit).hash != item.sourceRevision.hash) {
                finish(NativeItem::Status::Failed,
                       "Original copy failed" + (error.isEmpty() ? QString() : ": " + error), "failed");
                continue;
            }
            journal.update(i, {{"state", "backed_up"}});
            // 3. Process in memory through Document commands; encode; decode-verify.
            auto doc = openNative(root, item.png, &error);
            Resolved scope;
            if (doc)
                scope = resolve(*doc, o);
            QByteArray encoded, exported, exportedMeta;
            const auto asset = QDir(QFileInfo(root).canonicalFilePath())
                                   .relativeFilePath(QFileInfo(item.png).canonicalFilePath());
            if (!doc || !scope.error.isEmpty()) {
                finish(NativeItem::Status::Failed, doc ? scope.error : error, "failed");
                continue;
            }
            if (o.layers == LayerScope::CompositeOnly) {
                auto result = ops::runPipeline(pipeline.steps, doc->image, ctx);
                if (!result.ok()) {
                    finish(result.canceled ? NativeItem::Status::Canceled : NativeItem::Status::Failed,
                           result.canceled ? "Canceled" : result.error,
                           result.canceled ? "canceled" : "failed");
                    continue;
                }
                QString writeError;
                exported = encodePng(result.image);
                bool directory = false;
                {
                    std::lock_guard guard(directories);
                    directory = QDir().mkpath(QFileInfo(item.exportPath).absolutePath());
                }
                if (!directory || exported.isEmpty()) {
                    finish(NativeItem::Status::Failed, "PNG encoding or output directory failed", "failed");
                    continue;
                }
                if (injected("export") ||
                    !atomicWrite(item.exportPath, exported, item.exportRevision, &writeError)) {
                    finish(NativeItem::Status::Failed, "Export failed: " + writeError, "failed");
                    continue;
                }
                item.exportHash = sha(exported);
                journal.update(i, {{"export", item.exportPath}, {"export_sha256", hex(item.exportHash)}});
                finish(NativeItem::Status::Done, "Composite exported; native document unchanged", "done");
                continue;
            }
            if (!process(*doc, pipeline, o, scope, ctx, &error)) {
                const bool wasCanceled = cancel && cancel->load();
                finish(wasCanceled ? NativeItem::Status::Canceled : NativeItem::Status::Failed,
                       wasCanceled ? "Canceled; original untouched" : error + "; original untouched",
                       wasCanceled ? "canceled" : "failed");
                continue;
            }
            encoded = doc->encodeNative(asset, &error);
            {
                Document check(item.png, doc->image);
                if (encoded.isEmpty() || injected("encode") || !check.decodeNative(encoded, asset, &error)) {
                    finish(NativeItem::Status::Failed,
                           "Encoded result failed verification; original untouched" +
                               (error.isEmpty() ? QString() : ": " + error),
                           "failed");
                    continue;
                }
            }
            item.resultHash = sha(encoded);
            journal.update(i, {{"state", "staged"}, {"result_sha256", hex(item.resultHash)}});
            if (injected("interrupt")) { // simulated process termination
                interrupted = true;
                return;
            }
            // 4. Guarded replacement (expected = dry-run revision), then verification.
            if (injected("commit") || !atomicWrite(item.nvdoc, encoded, item.sourceRevision, &error)) {
                finish(NativeItem::Status::Failed,
                       "Replacement failed; original untouched" +
                           (error.isEmpty() ? QString(" (simulated write failure)") : ": " + error),
                       "failed");
                continue;
            }
            journal.update(i, {{"state", "committed"}});
            if (injected("interrupt-after-commit")) {
                interrupted = true;
                return;
            }
            const auto written = Revision::read(item.nvdoc, NativeLimit);
            if (injected("verify") || written.hash != item.resultHash) {
                // Restore the verified original copy only if the bytes are still ours.
                QString restoreError;
                const bool ours = written.hash == item.resultHash;
                if (ours && atomicWrite(item.nvdoc, original, written, &restoreError))
                    finish(NativeItem::Status::RolledBack, "Verification failed; original restored",
                           "rolled_back");
                else
                    finish(NativeItem::Status::Failed,
                           "Verification failed; journal retains the original copy for recovery",
                           "failed");
                continue;
            }
            // 5. Optional separate PNG (or Minecraft animation pair) export.
            if (o.mode == SourceMode::NativeWithExport) {
                QString exportError;
                bool ok = false;
                {
                    std::lock_guard guard(directories);
                    ok = QDir().mkpath(QFileInfo(item.exportPath).absolutePath());
                }
                if (ok && doc->animated()) {
                    auto pair = planAnimationExport(*doc, 1, {}, &exportError);
                    ok = pair.ok() && !injected("export") &&
                         atomicWrite(item.exportPath, pair.png, item.exportRevision, &exportError) &&
                         atomicWrite(item.exportPath + ".mcmeta", pair.mcmeta,
                                     Revision::read(item.exportPath + ".mcmeta"), &exportError);
                    exported = pair.png;
                } else if (ok) {
                    exported = encodePng(doc->image);
                    if (exported.isEmpty())
                        exportError = "PNG encoding failed";
                    ok = !exported.isEmpty() && !injected("export") &&
                         atomicWrite(item.exportPath, exported, item.exportRevision, &exportError);
                }
                if (!ok) {
                    // The native result is valid and committed; report the export separately.
                    item.warnings << "PNG export failed: " + exportError;
                    journal.update(i, {{"export_error", exportError}});
                } else {
                    item.exportHash = sha(exported);
                    journal.update(i, {{"export", item.exportPath}, {"export_sha256", hex(item.exportHash)}});
                }
            }
            finish(NativeItem::Status::Done,
                   item.warnings.isEmpty() ? QString("Native document updated and verified")
                                           : item.warnings.join("; "),
                   "done");
        }
    };
    std::vector<std::thread> threads;
    for (int w = 1; w < plan.workers; ++w)
        threads.emplace_back(work);
    work();
    for (auto &t : threads)
        t.join();
    report.done = done;
    report.failed = failed;
    report.skipped = skipped;
    report.canceled = canceled;
    report.rolledBack = rolledBack;
    report.interrupted = interrupted;
    report.milliseconds = timer.elapsed();
    if (!interrupted)
        journal.finish({{"complete", true},
                        {"done", report.done},
                        {"failed", report.failed},
                        {"skipped", report.skipped},
                        {"canceled", report.canceled},
                        {"rolled_back", report.rolledBack},
                        {"milliseconds", double(report.milliseconds)}},
                       &journalError);
    return report;
}
NativeRecovery recoverNative(const QString &path, bool rollback, bool apply) {
    NativeRecovery r;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) {
        r.error = "Journal is missing or unreadable";
        return r;
    }
    QJsonParseError parse;
    auto json = QJsonDocument::fromJson(file.readAll(), &parse);
    file.close();
    if (parse.error != QJsonParseError::NoError || !json.isObject()) {
        r.error = "Journal is corrupted (invalid JSON); nothing was changed";
        return r;
    }
    auto data = json.object();
    if (data["format"].toString() != "nova-native-batch-journal" || data["schema"].toInt() != 1) {
        r.error = "Not a supported native batch journal; nothing was changed";
        return r;
    }
    if (data["checksum"].toString() != hex(Journal::checksum(data))) {
        r.error = "Journal checksum mismatch (corrupted or edited); nothing was changed";
        return r;
    }
    const auto directory = QFileInfo(path).absolutePath();
    const auto root = data["root"].toString();
    auto items = data["items"].toArray();
    bool changed = false;
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject();
        const auto state = item["state"].toString();
        const auto target = item["nvdoc"].toString();
        if (state == "skipped" || state == "planned" || state == "rolled_back" || state == "canceled" ||
            state == "failed" || state == "recovered") {
            // "failed" items never replaced their target (or already restored it).
            ++r.unchanged;
            continue;
        }
        if (root.isEmpty() || !inside(root, target)) {
            ++r.conflicts;
            r.messages << item["png"].toString() + ": target outside the recorded pack; skipped";
            continue;
        }
        const auto original = QByteArray::fromHex(item["original_sha256"].toString().toLatin1());
        const auto result = QByteArray::fromHex(item["result_sha256"].toString().toLatin1());
        const auto current = Revision::read(target, NativeLimit);
        const auto backupPath = QDir::cleanPath(directory + '/' + item["backup"].toString());
        if (current.hash == original) {
            ++r.unchanged;
            r.messages << item["png"].toString() + ": original intact";
            if (apply) {
                item["state"] = state == "backed_up" || state == "staged" ? "recovered" : "rolled_back";
                changed = true;
            }
        } else if (!result.isEmpty() && current.hash == result) {
            if (!rollback) {
                ++r.completed;
                r.messages << item["png"].toString() + ": batch result present and verified";
                if (apply && state != "done") {
                    item["state"] = "done";
                    changed = true;
                }
                items[i] = item;
                continue;
            }
            bool ok = false;
            QFile backup(backupPath);
            const auto bytes = backup.open(QIODevice::ReadOnly) ? backup.readAll() : QByteArray();
            if (sha(bytes) != original) {
                ++r.conflicts;
                r.messages << item["png"].toString() + ": original copy missing or damaged; not restored";
                continue;
            }
            QString error;
            if (apply)
                ok = atomicWrite(target, bytes, current, &error);
            if (apply && !ok) {
                ++r.conflicts;
                r.messages << item["png"].toString() + ": restore failed: " + error;
                continue;
            }
            ++r.restored;
            r.messages << item["png"].toString() + (apply ? ": original restored" : ": would restore original");
            if (apply) {
                item["state"] = "rolled_back";
                changed = true;
            }
        } else {
            ++r.conflicts;
            r.messages << item["png"].toString() +
                              ": changed outside the batch since it ran; left untouched";
        }
        items[i] = item;
    }
    if (apply && changed) {
        data["items"] = items;
        data["recovered"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        data["checksum"] = hex(Journal::checksum(data));
        QString error;
        if (!atomicWrite(path, QJsonDocument(data).toJson(QJsonDocument::Indented), Revision::read(path),
                         &error)) {
            r.error = "Recovery applied but the journal update failed: " + error;
            return r;
        }
    }
    r.ok = true;
    return r;
}
QStringList pendingNativeJournals(const QString &transactionRoot) {
    QStringList out;
    QDirIterator it(transactionRoot, {"journal.json"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && out.size() < 256) {
        const auto path = it.next();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024 * 1024)
            continue;
        const auto data = QJsonDocument::fromJson(f.readAll()).object();
        if (data["format"].toString() == "nova-native-batch-journal" && !data["complete"].toBool())
            out << path;
    }
    return out;
}
} // namespace nova::batch
