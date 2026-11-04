// SPDX-License-Identifier: GPL-3.0-or-later
#include "session.h"
#include <QFileInfo>
#include <algorithm>

namespace nova {
bool DocumentSession::canRestoreDocument(const std::shared_ptr<Document> &doc,
                                         const Document &replacement) const {
    qint64 bytes = replacement.pixelBytes();
    for (const auto &other : documents)
        if (other != doc) {
            if (other->id() == replacement.id())
                return false;
            bytes += other->pixelBytes();
        }
    return bytes <= 512LL * 1024 * 1024;
}
bool DocumentSession::canRestorePixels(const std::shared_ptr<Document> &doc,
                                       const QImage &pixels) const {
    qint64 bytes = pixels.sizeInBytes();
    for (const auto &other : documents)
        if (other != doc)
            bytes += other->pixelBytes();
    return bytes <= 512LL * 1024 * 1024;
}
std::shared_ptr<Document> DocumentSession::newRaster(const QString &path, const QImage &pixels,
                                                     const QString &layerName, int frames, int ticks,
                                                     bool interpolate, QString *error) {
    auto doc = std::make_shared<Document>(QString{}, pixels);
    if (doc->image.isNull() || path.isEmpty() || QFileInfo::exists(path) || frames < 0 || frames > 15 ||
        layerName.trimmed().isEmpty() || layerName.size() > 128 || ticks < 1) {
        if (error)
            *error = "The raster initialization parameters or destination are invalid.";
        return {};
    }
    doc->path = path;
    doc->stack.front().name = layerName;
    if (frames > 0) {
        auto state = doc->animationState();
        state.enabled = true;
        state.defaultTicks = ticks;
        state.interpolate = interpolate;
        for (int i = 1; i < frames; ++i) {
            auto frame = state.frames.front();
            frame.id = QUuid::createUuid();
            frame.name = QString("Frame %1").arg(i + 1);
            frame.activeLayer = frame.layers.front().id = QUuid::createUuid();
            state.entries.push_back({frame.id, 0});
            state.frames.push_back(std::move(frame));
        }
        if (!doc->validateAnimation(state, error))
            return {};
        doc->installAnimation(state);
    }
    doc->native = true;
    // The native format requires a SHA-256-sized value even before the first export.
    doc->exportedPixels = QByteArray(32, '\0');
    doc->markRecovered();
    return doc;
}

bool DocumentSession::adopt(const std::shared_ptr<Document> &doc) {
    if (!doc || documents.size() >= 32)
        return false;
    qint64 bytes = doc->pixelBytes();
    for (const auto &other : documents) {
        if (pathIdentity(other->path) == pathIdentity(doc->path) || other->id() == doc->id())
            return false;
        bytes += other->pixelBytes();
    }
    if (bytes > 512LL * 1024 * 1024)
        return false;
    documents.push_back(doc);
    return true;
}
static QString normalized(const QString &path) {
    auto info = QFileInfo(path);
    return info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
}
std::shared_ptr<Document> DocumentSession::open(const QString &path, QString *error) {
    auto key = normalized(path);
    for (auto &doc : documents)
        if (normalized(doc->path).compare(key, Qt::CaseInsensitive) == 0)
            return doc;
    qint64 bytes = 0;
    for (auto &doc : documents)
        bytes += doc->pixelBytes();
    if (documents.size() >= 32 || bytes >= 448LL * 1024 * 1024) {
        if (error)
            *error = "Close a texture before opening more documents (512 MiB / 32 document limit)";
        return {};
    }
    const auto root = projectRoot.isEmpty() ? QFileInfo(key).absolutePath() : projectRoot;
    auto sidecar = nativeDocumentPath(root, key);
    std::shared_ptr<Document> doc;
    if (!sidecar.isEmpty() && QFileInfo::exists(sidecar)) {
        doc = std::make_shared<Document>(key, QImage{});
        if (!doc->loadNative(root, error))
            return {};
    } else
        doc = Document::open(key, error);
    if (doc && bytes + doc->pixelBytes() > 512LL * 1024 * 1024) {
        if (error)
            *error = "Close a document before loading more layer pixels (512 MiB limit)";
        return {};
    }
    if (doc)
        for (const auto &other : documents)
            if (other->id() == doc->id()) {
                if (error)
                    *error = "An editable document with this identity is already open";
                return {};
            }
    if (doc)
        documents.push_back(doc);
    return doc;
}
bool DocumentSession::dispatch(const std::shared_ptr<Document> &doc, Command command,
                               const QString &target, bool overwrite, QString *error) {
    if (!doc || std::find(documents.begin(), documents.end(), doc) == documents.end()) {
        if (error)
            *error = "The document is not open";
        return false;
    }
    bool ok = true;
    switch (command) {
    case Command::Undo:
        doc->undo();
        break;
    case Command::Redo:
        doc->redo();
        break;
    case Command::Save:
        ok = doc->nativeDocument()
                 ? doc->saveNative(projectRoot.isEmpty() ? QFileInfo(doc->path).absolutePath()
                                                         : projectRoot,
                                   error)
                 : doc->save(error);
        break;
    case Command::SaveNative:
        ok = doc->saveNative(
            projectRoot.isEmpty() ? QFileInfo(doc->path).absolutePath() : projectRoot, error);
        break;
    case Command::ExportPng:
        ok = doc->saveAsRevision(doc->path, doc->revision(), error);
        break;
    case Command::SaveAs:
        for (const auto &other : documents)
            if (other != doc &&
                normalized(other->path).compare(normalized(target), Qt::CaseInsensitive) == 0) {
                if (error)
                    *error = "That destination is already open in another document";
                return false;
            }
        ok = doc->saveAs(target, overwrite, error);
        break;
    }
    emit changed(doc->id());
    return ok;
}
bool DocumentSession::edit(const std::shared_ptr<Document> &doc, const LayerEdit &edit,
                           QString *error) {
    if (!doc || std::find(documents.begin(), documents.end(), doc) == documents.end()) {
        if (error)
            *error = "The target document is not open";
        return false;
    }
    if (edit.command == LayerCommand::Add || edit.command == LayerCommand::Duplicate) {
        qint64 bytes = 2 * doc->image.sizeInBytes();
        for (const auto &d : documents)
            bytes += d->pixelBytes();
        if (bytes > 512LL * 1024 * 1024) {
            if (error)
                *error = "Close another document before adding layer pixels (512 MiB limit)";
            return false;
        }
    }
    const bool ok = doc->editLayer(edit, error);
    if (ok)
        emit changed(doc->id());
    return ok;
}
bool DocumentSession::paint(const std::shared_ptr<Document> &doc, QUuid layer, QPoint a, QPoint b,
                            QRgb color, bool finishStroke, QString *error) {
    if (!doc || std::find(documents.begin(), documents.end(), doc) == documents.end()) {
        if (error)
            *error = "The target document is not open";
        return false;
    }
    const bool ok = doc->paint(layer, a, b, color, error);
    if (finishStroke)
        doc->finish();
    if (ok)
        emit changed(doc->id());
    return ok;
}
void DocumentSession::close(const std::shared_ptr<Document> &doc) {
    if (!doc)
        return;
    doc->close();
    std::erase(documents, doc);
    emit closed(doc->id());
}
} // namespace nova
