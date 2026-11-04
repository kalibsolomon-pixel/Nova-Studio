// SPDX-License-Identifier: GPL-3.0-or-later
#include "batch.h"
#include "core.h"
#include <QFile>
#include "index.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QThread>
#include <mutex>
#include <thread>

namespace nova::batch {
namespace {
bool fail(QString *error, const QString &text) {
    if (error)
        *error = text;
    return false;
}
bool insideDirectory(const QString &root, const QString &path) {
    const auto r = QDir::cleanPath(QFileInfo(root).absoluteFilePath()) + '/';
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).startsWith(r, Qt::CaseInsensitive);
}
} // namespace

QJsonObject Pipeline::toJson() const {
    QJsonArray list;
    for (const auto &s : steps)
        list.append(s.toJson());
    return {{"format", "nova-batch-pipeline"}, {"schema", Schema}, {"name", name}, {"steps", list}};
}
bool Pipeline::fromJson(const QJsonObject &json, Pipeline *out, QString *error) {
    if (json.value("format").toString() != "nova-batch-pipeline")
        return fail(error, "Not a Nova batch pipeline");
    if (json.value("schema").toInt() != Schema)
        return fail(error, QString("Unsupported pipeline schema %1 (this build reads schema %2)")
                               .arg(json.value("schema").toInt())
                               .arg(Schema));
    Pipeline p;
    p.name = json.value("name").toString().left(128);
    const auto steps = json.value("steps").toArray();
    if (steps.size() > 64)
        return fail(error, "Pipelines are limited to 64 steps");
    for (const auto &s : steps)
        p.steps.push_back(ops::Spec::fromJson(s.toObject()));
    if (!p.validate(error))
        return false;
    *out = std::move(p);
    return true;
}
bool Pipeline::validate(QString *error) {
    if (steps.empty())
        return fail(error, "Add at least one operation to the pipeline");
    if (steps.size() > 64)
        return fail(error, "Pipelines are limited to 64 steps");
    for (size_t i = 0; i < steps.size(); ++i) {
        QString e;
        if (!ops::validate(steps[i], &e))
            return fail(error, QString("Step %1: %2").arg(i + 1).arg(e));
    }
    if (!enabledSteps())
        return fail(error, "Enable at least one pipeline step");
    return true;
}
int Pipeline::enabledSteps() const {
    return int(std::count_if(steps.begin(), steps.end(), [](const ops::Spec &s) { return s.enabled; }));
}
QStringList scopeNames() {
    return {"Selected file", "Current folder", "Folder and descendants", "Asset category", "Namespace",
            "Search results", "Entire resource pack"};
}
QStringList resolveTargets(const QStringList &files, Scope scope, const QString &value) {
    QStringList out;
    const auto v = value.trimmed();
    for (const auto &f : files) {
        if (!f.endsWith(".png", Qt::CaseInsensitive) || f.startsWith(".nova/", Qt::CaseInsensitive))
            continue;
        const auto dir = QFileInfo(f).path() == "." ? QString() : QFileInfo(f).path();
        const auto parts = f.split('/');
        bool take = false;
        switch (scope) {
        case Scope::Selected: take = f == v; break;
        case Scope::Folder: take = dir == v; break;
        case Scope::FolderTree: take = v.isEmpty() || f.startsWith(v + '/'); break;
        case Scope::Category: take = assetCategory(f) == v; break;
        case Scope::Namespace: take = parts.size() > 2 && parts[0] == "assets" && parts[1] == v; break;
        case Scope::Search: take = !v.isEmpty() && f.contains(v, Qt::CaseInsensitive); break;
        case Scope::Pack: take = true; break;
        }
        if (take)
            out.append(f);
    }
    return out;
}
bool safeOutputPath(const Output &output, const QString &relative, int index, QString *path, QString *error) {
    QString stem = relative.left(relative.size() - 4); // strip ".png"
    const auto dir = QFileInfo(relative).path() == "." ? QString() : QFileInfo(relative).path();
    QString expanded = output.pattern;
    expanded.replace("{path}", stem)
        .replace("{dir}", dir)
        .replace("{name}", QFileInfo(relative).completeBaseName())
        .replace("{index}", QString("%1").arg(index, 4, 10, QChar('0')));
    expanded = expanded.trimmed();
    while (expanded.startsWith('/'))
        expanded.remove(0, 1);
    if (expanded.isEmpty() || expanded.size() > 1024 || expanded.contains('\\') || expanded.contains(':') ||
        QDir::isAbsolutePath(expanded) || QDir::cleanPath(expanded) != expanded)
        return fail(error, "Output pattern produced an unsafe path: " + expanded);
    static const QString reserved = "<>\"|?*";
    for (const auto &part : expanded.split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.size() > 200 || part.endsWith('.') ||
            part.endsWith(' ') || part.compare(".nova", Qt::CaseInsensitive) == 0)
            return fail(error, "Output pattern produced an unsafe path component: " + part);
        for (auto ch : part)
            if (ch.unicode() < 32 || reserved.contains(ch))
                return fail(error, "Output names may not contain control characters or <>\"|?*");
    }
    if (output.directory.trimmed().isEmpty())
        return fail(error, "Choose an output directory");
    *path = QDir::cleanPath(QFileInfo(output.directory).absoluteFilePath() + '/' + expanded + ".png");
    if (!insideDirectory(output.directory, *path))
        return fail(error, "Output escapes the destination directory");
    return true;
}
QString statusName(Item::Status s) {
    static const char *names[] = {"Ready", "Conflict", "Invalid", "Skipped", "Done", "Failed", "Canceled"};
    return names[int(s)];
}
QString Plan::summary() const {
    return QString("%1 targets · %2 ready · %3 conflicts · %4 invalid · up to %5 MiB per image · %6 worker(s)")
        .arg(items.size())
        .arg(ready)
        .arg(conflicts)
        .arg(invalid)
        .arg(double(peakItemBytes) / (1024 * 1024), 0, 'f', 1)
        .arg(workers);
}
Plan plan(Pipeline pipeline, const QString &root, const QStringList &files, const Output &output, int maxWorkers) {
    Plan result;
    QString pipelineError;
    const bool pipelineOk = pipeline.validate(&pipelineError);
    QString outputError;
    bool outputOk = true;
    if (!output.inPlace) {
        if (output.directory.trimmed().isEmpty())
            outputOk = false, outputError = "Choose an output directory";
        else if (QDir::cleanPath(QFileInfo(output.directory).absoluteFilePath())
                         .compare(QDir::cleanPath(QFileInfo(root).absoluteFilePath()), Qt::CaseInsensitive) == 0 ||
                 insideDirectory(root, output.directory))
            outputOk = false, outputError = "Use an output directory outside the resource pack (originals stay untouched)";
    } else if (output.backupDirectory.isEmpty() || insideDirectory(root, output.backupDirectory)) {
        outputOk = false, outputError = "In-place updates need a backup directory outside the resource pack";
    }
    QSet<QString> outputs;
    int index = 0;
    for (const auto &relative : files) {
        Item item;
        item.relative = relative;
        item.source = QDir::cleanPath(root + '/' + relative);
        auto invalid = [&](const QString &why) {
            item.status = Item::Status::Invalid;
            item.message = why;
        };
        const QFileInfo info(item.source);
        if (!pipelineOk)
            invalid(pipelineError);
        else if (!outputOk)
            invalid(outputError);
        else if (!relative.endsWith(".png", Qt::CaseInsensitive) || !info.isFile() || info.isSymLink() ||
                 !insideDirectory(root, item.source))
            invalid("Missing, linked or non-PNG source");
        if(item.status==Item::Status::Ready && output.inPlace && QFileInfo::exists(nativeDocumentPath(root,item.source)))
            invalid("Editable native source exists; use its editor/export or choose directory output");
        if (item.status == Item::Status::Ready) {
            QImageReader reader(item.source, "png");
            item.size = reader.size();
            if (!item.size.isValid() || item.size.width() > 4096 || item.size.height() > 4096)
                invalid("Unreadable PNG header or image exceeds 4096 px");
        }
        if (item.status == Item::Status::Ready) {
            QSize size = item.size;
            for (const auto &step : pipeline.steps) {
                if (!step.enabled)
                    continue;
                item.estimatedBytes = std::max(item.estimatedBytes, ops::estimateBytes(step, size));
                size = ops::outputSize(step, size);
            }
            item.outputSize = size;
            if (size.width() > 4096 || size.height() > 4096 || size.isEmpty())
                invalid("Pipeline output would exceed 4096 px");
        }
        if (item.status == Item::Status::Ready) {
            QString error;
            if (output.inPlace) {
                item.output = item.source;
                item.backup = QDir::cleanPath(output.backupDirectory + '/' + relative);
                item.sourceRevision = Revision::read(item.source);
                if (QFileInfo::exists(item.backup)) {
                    item.status = Item::Status::Conflict;
                    item.message = "A backup already exists at " + item.backup;
                }
            } else if (!safeOutputPath(output, relative, index, &item.output, &error)) {
                invalid(error);
            } else {
                const auto key = item.output.toCaseFolded();
                item.outputRevision = Revision::read(item.output);
                if (outputs.contains(key)) {
                    item.status = Item::Status::Conflict;
                    item.message = "Another target writes the same output file";
                } else if (item.outputRevision.exists && !output.overwrite) {
                    item.status = Item::Status::Conflict;
                    item.message = "The destination already exists (enable overwrite to replace it)";
                } else if (QFileInfo(item.output).isSymLink()) {
                    invalid("Linked destinations are not supported");
                }
                outputs.insert(key);
            }
        }
        result.peakItemBytes = std::max(result.peakItemBytes, item.estimatedBytes);
        result.ready += item.status == Item::Status::Ready;
        result.conflicts += item.status == Item::Status::Conflict;
        result.invalid += item.status == Item::Status::Invalid;
        result.items.push_back(std::move(item));
        ++index;
    }
    // Bounded concurrency: keep total working memory under ~1 GiB.
    const int cores = std::max(1, QThread::idealThreadCount());
    int workers = std::clamp(maxWorkers, 1, std::min(cores, 8));
    while (workers > 1 && qint64(workers) * result.peakItemBytes > 1024LL * 1024 * 1024)
        --workers;
    result.workers = workers;
    return result;
}
QJsonObject manifestJson(const Pipeline &pipeline, const Plan &plan, const Output &output, const Report &report) {
    QJsonArray items;
    for (const auto &i : plan.items)
        items.append(QJsonObject{{"source", i.relative},
                                 {"output", i.output},
                                 {"backup", i.backup},
                                 {"status", statusName(i.status)},
                                 {"message", i.message},
                                 {"source_sha256", QString::fromLatin1(i.sourceRevision.hash.toHex())},
                                 {"result_sha256", QString::fromLatin1(i.resultHash.toHex())},
                                 {"width", i.outputSize.width()},
                                 {"height", i.outputSize.height()}});
    return {{"format", "nova-batch-manifest"},
            {"schema", 1},
            {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"pipeline", pipeline.toJson()},
            {"mode", output.inPlace ? "in-place" : "directory"},
            {"directory", output.directory},
            {"backup_directory", output.backupDirectory},
            {"pattern", output.pattern},
            {"transaction", "independent per-file operations; no all-or-nothing atomicity"},
            {"done", report.done},
            {"failed", report.failed},
            {"skipped", report.skipped},
            {"canceled", report.canceled},
            {"milliseconds", double(report.milliseconds)},
            {"items", items}};
}
Report execute(const Pipeline &pipeline, Plan &plan, const Output &output, const ops::Context &context,
               const std::atomic_bool *cancel, const std::function<void(int)> &progress) {
    Report report;
    QElapsedTimer timer;
    timer.start();
    std::atomic_int next = 0, done = 0, failed = 0, skipped = 0, canceled = 0;
    std::mutex directories;
    auto makeDirectory = [&](const QString &path) {
        // Serialised: concurrent mkpath calls on Windows can report a spurious failure.
        std::lock_guard lock(directories);
        return QDir().mkpath(path) || QFileInfo(path).isDir();
    };
    auto work = [&] {
        ops::Context ctx = context;
        ctx.cancel = cancel;
        for (;;) {
            const int i = next++;
            if (i >= int(plan.items.size()))
                return;
            auto &item = plan.items[size_t(i)];
            auto finish = [&](Item::Status status, const QString &message = {}) {
                item.status = status;
                if (!message.isEmpty())
                    item.message = message;
                (status == Item::Status::Done       ? done
                 : status == Item::Status::Failed   ? failed
                 : status == Item::Status::Canceled ? canceled
                                                    : skipped)++;
                if (progress)
                    progress(i);
            };
            if (item.status != Item::Status::Ready) {
                finish(Item::Status::Skipped, item.message.isEmpty() ? QString("Not ready") : item.message);
                continue;
            }
            if (cancel && cancel->load()) {
                finish(Item::Status::Canceled);
                continue;
            }
            // Failure isolation: each item is validated and written on its own.
            const auto before = Revision::read(item.source);
            if (!before.exists || !before.readable ||
                (output.inPlace && !before.sameContent(item.sourceRevision))) {
                finish(Item::Status::Failed, "Source changed or became unreadable after the dry run");
                continue;
            }
            item.sourceRevision = before;
            QString error;
            const QImage source = loadPng(item.source, &error);
            if (source.isNull()) {
                finish(Item::Status::Failed, error.isEmpty() ? QString("PNG decode failed") : error);
                continue;
            }
            const auto result = ops::runPipeline(pipeline.steps, source, ctx);
            if (result.canceled) {
                finish(Item::Status::Canceled);
                continue;
            }
            if (!result.ok()) {
                finish(Item::Status::Failed, result.error);
                continue;
            }
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            if (!QImageWriter(&buffer, "png").write(result.image)) {
                finish(Item::Status::Failed, "PNG encoding failed");
                continue;
            }
            if (!before.sameContent(Revision::read(item.source))) {
                finish(Item::Status::Failed, "Source changed during processing; nothing was written");
                continue;
            }
            item.resultHash = QCryptographicHash::hash(png, QCryptographicHash::Sha256);
            item.outputSize = result.image.size();
            if (output.inPlace) {
                QByteArray bytes;
                {
                    // Closed before the replacement: Windows cannot rename over an open file.
                    QFile original(item.source);
                    if (original.open(QIODevice::ReadOnly))
                        bytes = original.readAll();
                }
                // Recoverable originals: the backup is written before the source is replaced.
                if (bytes.isEmpty() || !makeDirectory(QFileInfo(item.backup).absolutePath()) ||
                    !atomicWrite(item.backup, bytes, Revision::read(item.backup), &error) ||
                    !atomicWrite(item.source, png, before, &error)) {
                    finish(Item::Status::Failed, error.isEmpty() ? QString("Backup or replacement failed") : error);
                    continue;
                }
            } else {
                if (!makeDirectory(QFileInfo(item.output).absolutePath()) ||
                    !atomicWrite(item.output, png, item.outputRevision, &error)) {
                    finish(Item::Status::Failed, error.isEmpty() ? QString("Could not write output") : error);
                    continue;
                }
            }
            finish(Item::Status::Done);
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
    report.milliseconds = timer.elapsed();
    const auto manifestDir = output.inPlace ? output.backupDirectory : output.directory;
    if (!manifestDir.isEmpty() && QDir().mkpath(manifestDir)) {
        const auto path = manifestDir + "/nova-batch-" +
                          QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + ".json";
        QString error;
        if (atomicWrite(path, QJsonDocument(manifestJson(pipeline, plan, output, report)).toJson(),
                        Revision::read(path), &error))
            report.manifest = path;
        else
            report.error = "Manifest could not be written: " + error;
    }
    return report;
}
} // namespace nova::batch
