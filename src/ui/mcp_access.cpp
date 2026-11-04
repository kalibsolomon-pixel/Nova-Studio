// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "modelpanel.h"
#include "timeline.h"
#include <QListWidget>
#include <QPlainTextEdit>
#include "service.h"
#include "mcp_setup.h"
#include <QTabWidget>
#include <QScreen>
#include "scope.h"
#include <QMessageAuthenticationCode>
#include <QJsonArray>
#include <QMessageBox>
#include <QClipboard>
#include <QDialog>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QApplication>
#include <QDir>
#include <QBuffer>
#include <QRegularExpression>
#include <future>
#include <type_traits>

namespace nova {
namespace {
constexpr int MaxEditDimension = 256;
constexpr int MaxEditPixelBytes = 4 * 1024 * 1024;
constexpr int MaxReportedFrames = 128;

bool rasterDestination(mcp::Scope &scope, const QString &path) {
    return scope.vacant(path) && scope.vacant(path + ".mcmeta");
}

QString creationAssetPath(const QJsonObject &args, bool model) {
    const auto nameSpace = args["namespace"].toString();
    const auto logical = args["path"].toString();
    const auto parts = QStringList{nameSpace} + logical.split('/');
    static const QRegularExpression device("^(con|prn|aux|nul|com[0-9]|lpt[0-9])($|\\.)");
    for (const auto &part : parts)
        if (part.isEmpty() || part.endsWith('.') || device.match(part).hasMatch() ||
            part == ".nova")
            return {};
    if (logical.endsWith(".png") || logical.endsWith(".mcmeta"))
        return {};
    if (model && logical.endsWith(".json"))
        return {};
    const auto path = "assets/" + nameSpace + (model ? "/models/" : "/textures/") +
                      logical + (model ? ".json" : ".png");
    return mcp::logicalAsset(path) ? path : QString{};
}

template <typename DocumentType>
QString revisionToken(const mcp::Snapshot &scope, const DocumentType &doc) {
    auto payload = doc.instanceId().toByteArray() + ':' + QByteArray::number(doc.changeSerial());
    if constexpr (std::is_same_v<DocumentType, mc::ModelEdit>)
        payload += ':' + scope.version.toUtf8();
    return QString::fromLatin1(QMessageAuthenticationCode::hash(
        payload, scope.idKey, QCryptographicHash::Sha256).toHex());
}
QJsonObject documentState(const mcp::Snapshot &scope, const Document &doc) {
    QJsonArray layers, frames;
    for (const auto &layer : doc.layers())
        layers.append(QJsonObject{{"id", layer.id.toString(QUuid::WithoutBraces)}, {"name", layer.name},
                                  {"visible", layer.visible}, {"opacity", layer.opacity}});
    const auto animation = doc.animationState();
    for (const auto &frame : animation.frames) {
        if (frames.size() >= MaxReportedFrames)
            break;
        frames.append(QJsonObject{{"id", frame.id.toString(QUuid::WithoutBraces)}, {"name", frame.name}});
    }
    return {{"revision", revisionToken(scope, doc)}, {"dirty", doc.modified()},
            {"undoAvailable", doc.ownsHistoryHead(scope.generation, false)},
            {"redoAvailable", doc.ownsHistoryHead(scope.generation, true)},
            {"width", doc.image.width()}, {"height", doc.image.height()},
            {"animated", doc.animated()}, {"layers", layers}, {"frames", frames},
            {"frameCount", qint64(animation.frames.size())}, {"playbackCount", qint64(animation.entries.size())},
            {"defaultTicks", animation.defaultTicks}, {"interpolate", animation.interpolate},
            {"activeFrame", doc.activeFrame().toString(QUuid::WithoutBraces)},
            {"activeLayer", doc.activeLayer().toString(QUuid::WithoutBraces)}};
}
QJsonObject documentState(const mcp::Snapshot &scope, const mc::ModelEdit &doc) {
    return {{"revision", revisionToken(scope, doc)}, {"dirty", doc.modified()},
            {"undoAvailable", doc.ownsHistoryHead(scope.generation, false)},
            {"redoAvailable", doc.ownsHistoryHead(scope.generation, true)},
            {"elementCount", doc.authoritative()["elements"].toArray().size()}};
}
QJsonArray describeOperations(const QJsonArray &operations) {
    QJsonArray description;
    for (const auto &operation : operations) {
        const auto command = operation.toObject();
        QJsonObject item{{"command", command["command"]}};
        for (const auto &key : {"element", "face", "layer", "frame", "alias"})
            if (command.contains(key))
                item[key] = command[key];
        if (command["command"] == "fill_rect")
            item["maximumPixels"] = command["width"].toInt() * command["height"].toInt();
        description.append(item);
    }
    return description;
}

QJsonObject encodePreview(const QImage &image, const QString &asset, const QString &revision) {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG") || png.size() > 320 * 1024)
        return mcp::toolError("preview_limit", "The PNG exceeds the response budget.");
    return {{"assetId", asset}, {"revision", revision}, {"width", image.width()},
            {"height", image.height()}, {"_pngImage", QString::fromLatin1(png.toBase64())}};
}

void completeEdit(QJsonObject &result, const QString &id, qsizetype count, bool changed) {
    result["transactionId"] = id;
    result["appliedOperations"] = count;
    result["changed"] = changed;
    result["warnings"] = QJsonArray{};
}
}
Index Window::workspaceIndex(QSet<QString> *provisional) const {
    auto view = collection;
    std::unique_ptr<mcp::Scope> scope;
    for (const auto &doc : session.all()) {
        if (doc->revision().exists)
            continue;
        const auto path = QDir(project).relativeFilePath(doc->path);
        const auto directory = QFileInfo(path).path();
        if (view.records.contains(path) || !view.directories.contains(directory) ||
            !path.startsWith("assets/") || !path.contains("/textures/") || !path.endsWith(".png"))
            continue;
        if (!scope)
            scope = std::make_unique<mcp::Scope>(project);
        if (!rasterDestination(*scope, path))
            continue;
        if (provisional)
            provisional->insert(path);
        view.files.append(path);
        view.directories[directory].append(path);
        view.records.insert(path, AssetIdentity::fromPath(project, path));
    }
    for (const auto &path : modelPanel->openModelAssets()) {
        if (view.records.contains(path) ||
            !view.directories.contains(QFileInfo(path).path()))
            continue;
        if (!scope)
            scope = std::make_unique<mcp::Scope>(project);
        const auto edit = modelPanel->openModelDocument(path);
        if (!edit || (edit->provisional() ? !scope->vacant(path) : !scope->open(path)))
            continue;
        if (provisional && edit->provisional())
            provisional->insert(path);
        view.files.append(path);
        view.directories[QFileInfo(path).path()].append(path);
        view.records.insert(path, AssetIdentity::fromPath(project, path));
    }
    return view;
}

void Window::showMcpAccess(int page) {
    if (auto existing = findChild<QDialog *>("mcpAccessDialog")) {
        if (auto pages = existing->findChild<QTabWidget *>("mcpTabs"))
            pages->setCurrentIndex(page);
        existing->raise();
        existing->activateWindow();
        return;
    }
    if (!mcpService) {
        mcpService = new mcp::Service([this](mcp::Snapshot &snapshot, const QString &tool) {
            if (snapshot.root != project || indexBusy)
                return false;
            snapshot.index = workspaceIndex(&snapshot.provisional);
            snapshot.diskRecords = collection.records;
            auto provisional = snapshot.provisional.values();
            provisional.sort();
            const auto indexState = QByteArray::number(indexGeneration) + ':' +
                provisional.join('\n').toUtf8();
            snapshot.indexRevision = QString::fromLatin1(QMessageAuthenticationCode::hash(
                indexState, snapshot.idKey, QCryptographicHash::Sha256).toHex());
            snapshot.version = modelPanel->catalog()->version();
            if (tool == "nova.workflow.execute" || tool == "nova.workflow.plan" ||
                tool == "nova.asset.create" || tool.endsWith(".edit") || tool.startsWith("nova.edit.")) {
                auto addOpen = [&](const QString &path) {
                    if (snapshot.index.records.contains(path))
                        snapshot.openAssetPaths.insert(mcp::assetIdentifier(snapshot, path), path);
                };
                for (const auto &path : modelPanel->openModelAssets())
                    addOpen(path);
                for (const auto &doc : session.all())
                    addOpen(QDir(project).relativeFilePath(doc->path));
            }
            const bool batch = tool == "nova.assets.inspect_batch";
            const bool context = tool == "nova.assets.context";
            const bool revisions = tool == "nova.workflow.prepare" || tool == "nova.workflow.validate_changes" ||
                tool == "validation_revision_check";
            if (tool == "nova.model.inspect" || tool == "nova.asset.inspect" || batch || context ||
                tool == "nova.pack.validate" || tool == "nova.workflow.validate_changes")
                snapshot.models = modelPanel->documentSources();
            if (revisions)
                for (const auto &path : modelPanel->openModelAssets())
                    if (const auto doc = modelPanel->openModelDocument(path))
                        snapshot.revisions.insert(path, revisionToken(snapshot, *doc));
            const bool rasterMetadata = tool == "nova.texture.inspect" || tool == "nova.animation.inspect" ||
                tool == "nova.asset.inspect" || batch || context;
            if (rasterMetadata || revisions) {
                for (const auto &doc : session.all()) {
                    const auto relative = QDir(project).relativeFilePath(doc->path);
                    if (!snapshot.index.records.contains(relative))
                        continue;
                    if (revisions)
                        snapshot.revisions.insert(relative, revisionToken(snapshot, *doc));
                    if (tool == "nova.workflow.validate_changes" && (doc->animated() || snapshot.provisional.contains(relative)))
                        snapshot.animationDocuments.insert(relative, doc->recoverySnapshot());
                    if (!rasterMetadata)
                        continue;
                    snapshot.textures.insert(relative, {
                        {"source", "live"}, {"width", doc->image.width()}, {"height", doc->image.height()},
                        {"qtImageFormat", int(doc->image.format())}, {"alphaChannel", doc->image.hasAlphaChannel()},
                        {"layerCount", qint64(doc->layers().size())}});
                    if (doc->animated()) {
                        auto animation = mcp::summarizeAnimation(doc->animationState());
                        animation["source"] = "live";
                        snapshot.animations.insert(relative, animation);
                    }
                }
            }
            return true;
        }, this);
        connect(mcpService, &mcp::Service::requestParsed, this,
                [this](const QElapsedTimer &timer) { metrics.record("mcp_request_parse", timer); });
        connect(mcpService, &mcp::Service::responseQueued, this,
                [this](const QElapsedTimer &timer) { metrics.record("mcp_response_json_queue", timer); });
        connect(mcpService, &mcp::Service::editSnapshotTaken, this,
                [this](const QElapsedTimer &timer) { metrics.record("mcp_edit_dispatch_snapshot", timer); });
        mcpService->setEditHandler([this](const mcp::Snapshot &snapshot, const mcp::Request &request,
                                          bool editing, mcp::Service::Commit &commit) -> QJsonObject {
            using mcp::toolError;
            const bool create = request.tool == "nova.asset.create";
            if (create || request.tool == "nova.asset.create_preview") {
                const auto args = request.arguments;
                if (args["indexRevision"] != snapshot.indexRevision)
                    return toolError("stale_index", "Inspect the current project index before creating an asset.");
                const auto parameters = args["parameters"].toObject();
                const bool modelCreation = parameters["command"] == "model";
                const auto relative = creationAssetPath(args, modelCreation);
                if (relative.isEmpty())
                    return toolError("invalid_asset_path", "Use a relative Minecraft resource path without a file extension.");
                const auto absolute = snapshot.root + '/' + relative;
                auto check = [this, relative, absolute, snapshot, modelCreation](mcp::Scope &scope) -> QJsonObject {
                    if (modelCreation) {
                        for (const auto &path : modelPanel->openModelAssets())
                            if (path.compare(relative, Qt::CaseInsensitive) == 0)
                                return toolError("asset_conflict", "A model already uses this resource path.");
                        if (collection.records.contains(relative) || scope.open(relative))
                            return toolError("asset_conflict", "A resource already uses this path.");
                        if (!scope.vacant(relative))
                            return toolError("unsafe_asset", "The model needs a vacant path in the authorized pack.");
                        return {};
                    }
                    for (const auto &doc : session.all())
                        if (doc->path.compare(absolute, Qt::CaseInsensitive) == 0)
                            return toolError("asset_conflict", "A document already uses this resource path.");
                    if (collection.records.contains(relative) || scope.open(relative) ||
                        scope.open(relative + ".mcmeta"))
                        return toolError("asset_conflict", "A resource already uses this path.");
                    if (!rasterDestination(scope, relative))
                        return toolError("unsafe_asset", "The resource directory must already exist inside the authorized pack.");
                    const auto editable = nativeDocumentPath(snapshot.root, absolute);
                    if (editable.isEmpty())
                        return toolError("unsafe_asset", "The editable document destination is unavailable.");
                    if (QFileInfo::exists(editable))
                        return toolError("asset_conflict", "An editable document already uses this resource path.");
                    return {};
                };
                mcp::Scope scope(snapshot.root, snapshot.rootIdentity);
                if (const auto error = check(scope); !error.isEmpty())
                    return error;
                if (modelCreation) {
                    QElapsedTimer prepareCreation;
                    prepareCreation.start();
                    const auto parent = parameters["parent"].toString();
                    const auto texture = parameters["cubeTexture"].toString();
                    const auto parentId = mc::Location::parse(parent);
                    if ((!parent.isEmpty() && (!parentId.valid() ||
                        parentId.asset("models") == relative ||
                        (parentId.id() != "minecraft:builtin/generated" && !scope.open(parentId.asset("models"))))) ||
                        (parent.isEmpty() && texture.isEmpty()))
                        return toolError("invalid_model", "Specify a valid project-local parent or a cube texture.");
                    auto draft = std::make_shared<mc::ModelEdit>();
                    QString error;
                    QJsonObject source;
                    if (!parent.isEmpty())
                        source["parent"] = parent;
                    if (!draft->create(source, &error) ||
                        (!texture.isEmpty() && (!draft->addElement(texture, &error) ||
                         // Like the native cube templates, define the particle without a parent.
                         (parent.isEmpty() && !draft->textureBinding("particle", texture, &error)) ||
                         !draft->commitPreview("Initialize model"))) ||
                        !draft->create(draft->authoritative(), &error) ||
                        !draft->bindProvisional(snapshot.root, relative, &error))
                        return toolError("invalid_model", error);
                    auto catalog = *modelPanel->catalog();
                    catalog.setProvisionalSource(relative, draft->authoritative());
                    const auto resolved = catalog.model(mc::Location::fromAsset(relative, "models"));
                    if (!resolved.valid)
                        return toolError("invalid_model", "The parent chain cannot be resolved by the native editor.");
                    QJsonObject response{{"assetId", mcp::assetIdentifier(snapshot, relative)},
                        {"path", relative}, {"type", "model"}, {"supported", true}, {"conflictPolicy", "reject"},
                        {"elementCount", draft->authoritative()["elements"].toArray().size()},
                        {"warnings", QJsonArray{}}, {"references", parent.isEmpty() ? QJsonArray{} : QJsonArray{parent}}};
                    metrics.record("mcp_model_create_prepare", prepareCreation);
                    if (!create)
                        return response;
                    const auto expectedIndex = indexGeneration;
                    commit = [this, draft, snapshot, relative, check, expectedIndex, response]() mutable -> QJsonObject {
                        if (indexBusy || indexGeneration != expectedIndex ||
                            !collection.directories.contains(QFileInfo(relative).path()))
                            return toolError("stale_index", "The project index changed before publication.");
                        if (QApplication::activeModalWidget())
                            return toolError("document_busy", "Finish the current editor dialog first.");
                        mcp::Scope scope(snapshot.root, snapshot.rootIdentity);
                        if (const auto error = check(scope); !error.isEmpty())
                            return error;
                        QElapsedTimer publication;
                        publication.start();
                        QString error;
                        if (!modelPanel->registerProvisionalModel(draft, &error))
                            return toolError("creation_failed", error);
                        metrics.record("mcp_model_create_publication", publication);
                        response["document"] = documentState(snapshot, *draft);
                        response["provisional"] = true;
                        response["changed"] = true;
                        response["changeSummary"] = QJsonObject{{"assetPath", relative}};
                        return response;
                    };
                    return {};
                }
                const auto width = parameters["width"].toInt(), height = parameters["height"].toInt();
                const auto frames = parameters["frames"].toInt();
                QJsonObject response{{"assetId", mcp::assetIdentifier(snapshot, relative)}, {"path", relative},
                    {"type", parameters["command"]}, {"width", width}, {"height", height},
                    {"frameCount", qMax(frames, 1)}, {"estimatedPixelBytes", 4 * width * height * (qMax(frames, 1) + 1)},
                    {"supported", true}, {"warnings", QJsonArray{}},
                    {"references", QJsonArray{}}, {"conflictPolicy", "reject"}};
                if (!create)
                    return response;
                if (QApplication::activeModalWidget())
                    return toolError("document_busy", "Finish the current editor dialog first.");
                const auto expectedIndex = indexGeneration;
                if (!collection.directories.contains(QFileInfo(relative).path()))
                    return toolError("stale_index", "Wait for the resource directory to be indexed.");
                commit = [this, snapshot, check, relative, absolute, expectedIndex, parameters,
                          width, height, frames, response]() mutable -> QJsonObject {
                    if (indexBusy || indexGeneration != expectedIndex)
                        return toolError("stale_index", "The project index changed before publication.");
                    if (QApplication::activeModalWidget())
                        return toolError("document_busy", "Finish the current editor dialog first.");
                    mcp::Scope scope(snapshot.root, snapshot.rootIdentity);
                    if (const auto error = check(scope); !error.isEmpty())
                        return error;
                    QElapsedTimer publication;
                    publication.start();
                    QImage pixels(width, height, QImage::Format_RGBA8888);
                    const auto fill = parameters["fill"].toString();
                    pixels.fill(fill == "transparent" ? QColor(Qt::transparent) : QColor(fill));
                    QString error;
                    const auto doc = DocumentSession::newRaster(absolute, pixels,
                        parameters["layerName"].toString(), frames, parameters["ticks"].toInt(1),
                        parameters["interpolate"].toBool(), &error);
                    if (!doc || !session.adopt(doc))
                        return toolError("creation_failed", "The editor could not allocate or register the document.");
                    QElapsedTimer checkpoint;
                    checkpoint.start();
                    if (!recovery->writeDocument(*doc, project, &error)) {
                        session.close(doc);
                        return toolError("recovery_failed", "Nova could not checkpoint the new document.");
                    }
                    metrics.record("mcp_raster_creation_recovery", checkpoint);
                    recoveredKeys[doc->id()] = qint64(doc->changeSerial());
                    model->applyIndex(workspaceIndex(), {relative});
                    showTextureDocument(doc);
                    metrics.record("mcp_raster_create_publication", publication);
                    response["document"] = documentState(snapshot, *doc);
                    response["provisional"] = true;
                    response["changed"] = true;
                    response["changeSummary"] = QJsonObject{{"assetPath", relative}};
                    return response;
                };
                return {};
            }
            const bool capabilities = request.tool == "nova.edit.capabilities";
            const bool preview = request.tool == "nova.edit.preview";
            const bool imagePreview = request.tool == "nova.asset.preview_image";
            const auto asset = request.arguments["assetId"].toString();
            QJsonObject result{{"schemaVersion", 1}, {"projectId", snapshot.projectId},
                {"generation", snapshot.generation}, {"permission", editing ? "editing" : "read_only"},
                {"assetTypes", QJsonArray{"texture", "model"}}, {"maxOperations", 8},
                {"maxDimension", MaxEditDimension}, {"maxPixelBytes", MaxEditPixelBytes},
                {"tools", QJsonArray{"nova.texture.edit", "nova.layer.edit", "nova.animation.edit",
                                     "nova.model.edit", "nova.uv.edit", "nova.edit.batch", "nova.edit.undo", "nova.edit.redo"}}};
            if (capabilities && asset.isEmpty())
                return result;
            if (!capabilities && QApplication::activeModalWidget())
                return toolError("document_busy", "Finish the current editor dialog first.");

            const bool redo = request.tool == "nova.edit.redo";
            const bool history = redo || request.tool == "nova.edit.undo";
            const auto operations = history || capabilities ? QJsonArray{} :
                (request.tool == "nova.edit.batch" || preview) ? request.arguments["operations"].toArray() :
                                                    QJsonArray{request.arguments["operation"]};
            const auto modelOperation = [](const QJsonValue &value) {
                const auto name = value.toObject()["command"].toString();
                return name.startsWith("model_") || name.startsWith("uv_");
            };
            auto previewResult = [&](const auto &doc) -> QJsonObject {
                return {{"assetId", asset}, {"revision", revisionToken(snapshot, doc)},
                        {"permission", editing ? "editing" : "read_only"},
                        {"operations", describeOperations(operations)}, {"validationRequired", true},
                        {"undoAvailable", doc.ownsHistoryHead(snapshot.generation, false)}};
            };
            auto summary = [&](const QString &path, const QString &revision) -> QJsonObject {
                return {{"assetId", asset}, {"assetPath", path}, {"previousRevision", revision},
                        {"tool", request.tool}, {"operations", describeOperations(operations)}};
            };
            mcp::Scope scope(snapshot.root, snapshot.rootIdentity);
            if (scope.identity().isEmpty())
                return toolError("asset_unavailable", "The authorized project root has changed.");
            for (const auto &path : modelPanel->openModelAssets()) {
                if (!snapshot.index.records.contains(path) || !mcp::logicalAsset(path) ||
                    mcp::assetIdentifier(snapshot, path) != asset)
                    continue;
                const auto model = modelPanel->openModelDocument(path);
                if (!model)
                    return toolError("document_not_open", "The model is unavailable or open in the authoring window.");
                auto source = scope.open(path);
                if (model->provisional() ? !scope.vacant(path) : !source)
                    return toolError("asset_unavailable", "The model failed project containment.");
                if (capabilities) {
                    result["document"] = documentState(snapshot, *model);
                    return result;
                }
                if (request.arguments["expectedRevision"] != revisionToken(snapshot, *model))
                    return toolError("revision_conflict", "The model changed. Obtain its current revision.");
                if (model->previewing() || modelPanel->uvInteractionActive())
                    return toolError("document_busy", "Finish the current model or UV preview first.");
                if (imagePreview) {
                    const auto view = request.arguments["view"].toString();
                    if (view.isEmpty() || request.arguments.contains("frameId") ||
                        (view != "uv" && request.arguments.contains("faceIndex")))
                        return toolError("invalid_arguments", "Select a model or UV view with compatible arguments.");
                    if (modelPanel->catalog()->source(path) != model->authoritative())
                        return toolError("revision_conflict", "The native model source changed.");
                    if (modelPanel->modelAsset() != path)
                        return toolError("preview_unavailable", "Select this model in the native preview.");
                    for (const auto &parent : modelPanel->previewParents())
                        if (parent != "minecraft:builtin/generated" &&
                            !scope.open(mc::Location::parse(parent).asset("models")))
                            return toolError("asset_unavailable", "A preview parent is outside the authorized pack.");
                    for (const auto &material : modelPanel->previewMaterials())
                        if (!scope.open(material))
                            return toolError("asset_unavailable", "A preview material is outside the authorized pack.");
                    QString error;
                    const int face = request.arguments["faceIndex"].toInt();
                    QElapsedTimer render;
                    render.start();
                    const auto image = modelPanel->modelPreviewImage(path, view == "uv", face, &error);
                    metrics.record(view == "uv" ? "mcp_uv_render" : "mcp_model_render", render);
                    if (image.isNull())
                        return toolError("preview_unavailable", error);
                    QElapsedTimer encode;
                    encode.start();
                    auto response = encodePreview(image, asset, revisionToken(snapshot, *model));
                    metrics.record("mcp_model_preview_encode", encode);
                    if (!response.contains("error")) {
                        response["view"] = view;
                        if (view == "uv")
                            response["faceIndex"] = face;
                    }
                    return response;
                }
                if (!history && ((source && source->size() > 256 * 1024) || model->authoritative()["elements"].toArray().size() > 256))
                    return toolError("edit_limit", "The model exceeds the editing budget.");
                for (const auto &operation : operations)
                    if (!modelOperation(operation))
                        return toolError("invalid_asset_type", "This command requires a texture document.");
                std::function<bool()> prepared;
                QString error;
                if (!history) {
                    QElapsedTimer staging;
                    staging.start();
                    prepared = model->prepareBatch(operations, snapshot.generation, snapshot.version, &error,
                                                   request.continuePreparation);
                    metrics.record("mcp_model_prepare", staging);
                    if (!prepared)
                        return toolError("edit_rejected", error.isEmpty() ? QString("The native model editor rejected the transaction.") : error);
                }
                if (preview)
                    return previewResult(*model);
                const auto revision = revisionToken(snapshot, *model);
                commit = [this, model, path, snapshot, revision, history, redo, prepared,
                          changes = summary(path, revision),
                          operations, count = operations.size()]() -> QJsonObject {
                    if (modelPanel->openModelDocument(path) != model ||
                        modelPanel->catalog()->version() != snapshot.version ||
                        revisionToken(snapshot, *model) != revision)
                        return toolError("revision_conflict", "The model changed before commit.");
                    if (QApplication::activeModalWidget() || model->previewing() ||
                        modelPanel->uvInteractionActive())
                        return toolError("document_busy", "Finish the current model or UV preview first.");
                    mcp::Scope currentScope(snapshot.root, snapshot.rootIdentity);
                    auto verified = currentScope.open(path);
                    if (model->provisional() ? !currentScope.vacant(path) :
                        !verified)
                        return toolError("asset_unavailable", "The model failed project containment.");
                    QElapsedTimer publication;
                    publication.start();
                    const auto transaction = mcp::randomToken();
                    Comparison comparison;
                    comparison.path = path;
                    comparison.instance = model->instanceId();
                    comparison.beforeRevision = revision;
                    comparison.owner = snapshot.generation;
                    const auto first = operations.isEmpty() ? QJsonObject{} : operations.first().toObject();
                    const int element = first["element"].toInt(-1);
                    const auto faceName = first["face"].toString();
                    QString texture;
                    auto capture = [&](QImage &modelImage, QImage &uvImage, bool before) {
                        if (modelPanel->modelAsset() != path ||
                            modelPanel->catalog()->source(path) != model->authoritative())
                            return;
                        for (const auto &parent : modelPanel->previewParents())
                            if (parent != "minecraft:builtin/generated" &&
                                !currentScope.open(mc::Location::parse(parent).asset("models")))
                                return;
                        for (const auto &material : modelPanel->previewMaterials())
                            if (!currentScope.open(material))
                                return;
                        QString error;
                        modelImage = modelPanel->modelPreviewImage(path, false, 0, &error, true);
                        if (!first["command"].toString().startsWith("uv_"))
                            return;
                        const auto &faces = cube->meshData().faces;
                        for (int i = 0; i < faces.size(); ++i) {
                            const auto &face = faces[i];
                            if (face.elementIndex != element || face.name != faceName)
                                continue;
                            if (before)
                                texture = face.texture;
                            if (face.texture == texture)
                                uvImage = modelPanel->modelPreviewImage(path, true, i, &error, true);
                            break;
                        }
                    };
                    capture(comparison.before, comparison.uvBefore, true);
                    if (!faceName.isEmpty())
                        comparison.detail = QString("UV face %1 of element %2 · %3").arg(faceName).arg(element).arg(texture);
                    if (history) {
                        if (!model->ownsHistoryHead(snapshot.generation, redo))
                            return toolError("history_conflict", "The next history entry belongs to another editor or session.");
                        if (!(redo ? model->redo() : model->undo()))
                            return toolError("edit_rejected", "The model history could not be moved.");
                    } else if (!prepared()) {
                        return toolError("revision_conflict", "The model changed before commit.");
                    }
                    metrics.record("mcp_model_native_mutation", publication);
                    const bool changed = revisionToken(snapshot, *model) != revision;
                    auto response = documentState(snapshot, *model);
                    completeEdit(response, transaction, history ? 1 : count, changed);
                    response["changeSummary"] = changes;
                    if (changed) {
                        const auto warning = modelPanel->publishModelEdit(model);
                        if (!warning.isEmpty())
                            response["warnings"] = QJsonArray{"Model recovery checkpoint failed. The edit is still in memory."};
                    }
                    if (changed) {
                        capture(comparison.after, comparison.uvAfter, false);
                        comparison.serial = model->changeSerial();
                        comparison.afterRevision = response["revision"].toString();
                        rememberComparison(std::move(comparison));
                    }
                    metrics.record("mcp_model_native_publication", publication);
                    return response;
                };
                return {};
            }

            std::shared_ptr<Document> doc;
            QString relative;
            for (const auto &open : session.all()) {
                const auto path = QDir(snapshot.root).relativeFilePath(open->path);
                if (path.endsWith(".png") && mcp::logicalAsset(path) &&
                    snapshot.index.records.contains(path) && mcp::assetIdentifier(snapshot, path) == asset) {
                    doc = open;
                    relative = path;
                    break;
                }
            }
            if (!doc)
                return toolError("document_not_open", "The identifier must refer to an indexed open document.");
            auto source = scope.open(relative);
            const bool provisional = snapshot.provisional.contains(relative);
            if (provisional ? !rasterDestination(scope, relative) : (!source || !doc->revision().exists))
                return toolError("asset_unavailable", "The source asset failed project containment.");
            if (capabilities) {
                result["document"] = documentState(snapshot, *doc);
                return result;
            }
            if (request.arguments["expectedRevision"] != revisionToken(snapshot, *doc))
                return toolError("revision_conflict", "The document changed. Obtain its current revision.");
            auto interactionActive = [this, doc] {
                for (int i = 0; i < tabs->count(); ++i) {
                    const auto canvas = dynamic_cast<Canvas *>(tabs->widget(i));
                    if (!canvas)
                        continue;
                    if (canvas->document == doc && canvas->interactionActive())
                        return true;
                }
                return doc->strokeOpen();
            };
            if (interactionActive())
                return toolError("document_busy", "Finish the current canvas interaction first.");

            if (imagePreview) {
                if (request.arguments.contains("view") || request.arguments.contains("faceIndex"))
                    return toolError("invalid_asset_type", "Model and UV views require a model document.");
                if (doc->pixelBytes() > MaxEditPixelBytes ||
                    doc->image.width() > MaxEditDimension || doc->image.height() > MaxEditDimension)
                    return toolError("preview_limit", "The document exceeds the visual preview budget.");
                auto frame = doc->activeFrame();
                if (request.arguments.contains("frameId")) {
                    if (!doc->animated())
                        return toolError("invalid_asset_type", "A frame identifier requires an animation.");
                    frame = QUuid(request.arguments["frameId"].toString());
                }
                QElapsedTimer render;
                render.start();
                const auto image = doc->animated() ? doc->frameComposite(frame) : doc->image;
                metrics.record("mcp_raster_preview_pixels", render);
                if (image.isNull())
                    return toolError("invalid_arguments", "The source frame does not exist.");
                QElapsedTimer encode;
                encode.start();
                auto response = encodePreview(image, asset, revisionToken(snapshot, *doc));
                metrics.record("mcp_raster_preview_encode", encode);
                if (!response.contains("error"))
                    response["frameId"] = doc->animated() ? frame.toString(QUuid::WithoutBraces) : QString{};
                return response;
            }

            std::function<bool(QString *)> prepared;
            if (!history) {
                if (doc->image.width() > MaxEditDimension || doc->image.height() > MaxEditDimension ||
                    doc->pixelBytes() > MaxEditPixelBytes)
                    return toolError("edit_limit", "The document exceeds the editing budget.");
                for (const auto &value : operations) {
                    if (modelOperation(value))
                        return toolError("invalid_asset_type", "This command requires a model document.");
                    const auto op = value.toObject();
                    const auto name = op["command"].toString();
                    auto inside = [&](int x, int y) { return doc->image.rect().contains(x, y); };
                    if (name == "fill" && !inside(op["x"].toInt(), op["y"].toInt()))
                        return toolError("invalid_arguments", "The seed lies outside the canvas.");
                    if (name == "fill_rect" && !doc->image.rect().contains(
                            QRect(op["x"].toInt(), op["y"].toInt(), op["width"].toInt(), op["height"].toInt())))
                        return toolError("invalid_arguments", "The rectangle lies outside the canvas.");
                    for (const auto &point : op["points"].toArray()) {
                        const auto xy = point.toArray();
                        if (!inside(xy[0].toInt(), xy[1].toInt()))
                            return toolError("invalid_arguments", "A stroke point lies outside the canvas.");
                    }
                    for (const auto &key : {"layer", "frame"})
                        if (op.contains(key) && QUuid(op[key].toString()).isNull())
                            return toolError("invalid_arguments", "The layer or frame identifier is invalid.");
                }
                QString error;
                QElapsedTimer staging;
                staging.start();
                prepared = session.prepareBatch(doc, operations, &error, snapshot.generation, MaxEditPixelBytes,
                                                request.continuePreparation);
                metrics.record(doc->animated() ? "mcp_animation_prepare" : "mcp_texture_prepare", staging);
                if (!prepared)
                    return toolError("edit_rejected", error.isEmpty() ? QString("The native editor rejected the transaction.") : error);
            }
            if (preview)
                return previewResult(*doc);
            const auto revision = revisionToken(snapshot, *doc);
            commit = [this, doc, relative, snapshot, revision, interactionActive, prepared, history, provisional,
                      changes = summary(relative, revision),
                      operations, redo, workflowStage = request.workflowStage,
                      count = operations.size()]() -> QJsonObject {
                if (!session.owns(doc) || revisionToken(snapshot, *doc) != revision)
                    return toolError("revision_conflict", "The texture changed before commit.");
                if (QApplication::activeModalWidget() || interactionActive())
                    return toolError("document_busy", "Finish the current canvas interaction first.");
                mcp::Scope currentScope(snapshot.root, snapshot.rootIdentity);
                auto verified = currentScope.open(relative);
                if ((provisional ? !rasterDestination(currentScope, relative) :
                                   (!verified || !collection.records.contains(relative))) ||
                    QDir(snapshot.root).relativeFilePath(doc->path) != relative)
                    return toolError("asset_unavailable", "The source asset failed project containment.");
                QElapsedTimer publication;
                publication.start();
                const auto transaction = mcp::randomToken();
                QString error;
                Comparison comparison;
                comparison.path = relative;
                comparison.instance = doc->instanceId();
                comparison.beforeRevision = revision;
                comparison.owner = snapshot.generation;
                auto frame = doc->activeFrame();
                if (doc->animated()) {
                    for (const auto &operation : operations) {
                        const QUuid target(operation.toObject()["frame"].toString());
                        if (!target.isNull()) {
                            frame = target;
                            break;
                        }
                    }
                }
                // Describe frame position and playback so reordered, added or removed
                // frames are reported rather than hidden behind a single image pair.
                auto playback = [&] {
                    if (!doc->animated())
                        return QString{};
                    const auto state = doc->animationState();
                    int position = 0;
                    for (size_t i = 0; i < state.frames.size(); ++i)
                        if (state.frames[i].id == frame)
                            position = int(i) + 1;
                    return (position ? QString("frame %1 of %2").arg(position).arg(state.frames.size())
                                     : QString("frame absent of %1").arg(state.frames.size())) +
                           QString(" · %1 entries · %2 ticks%3").arg(state.entries.size())
                               .arg(state.defaultTicks).arg(state.interpolate ? " · interpolated" : "");
                };
                const auto playbackBefore = playback();
                auto pixels = [&] {
                    if (doc->image.width() > MaxEditDimension || doc->image.height() > MaxEditDimension)
                        return QImage{};
                    return doc->animated() ? doc->frameComposite(frame) : doc->image;
                };
                const auto command = operations.isEmpty() ? QString{} :
                    operations.first().toObject()["command"].toString();
                const bool addedFrame = operations.size() == 1 &&
                    (command == "frame_add" || command == "frame_duplicate");
                if (!addedFrame)
                    comparison.before = pixels();
                if (history) {
                    if (!doc->ownsHistoryHead(snapshot.generation, redo))
                        return toolError("history_conflict", "The next history entry belongs to another editor or session.");
                    if (!session.dispatch(doc, redo ? Command::Redo : Command::Undo, {}, false, &error))
                        return toolError("edit_rejected", error);
                } else if (!prepared(&error)) {
                    return toolError("edit_rejected", error.isEmpty() ? QString("The commit budget changed.") : error);
                }
                metrics.record(doc->animated() ? "mcp_animation_native_publication" : "mcp_texture_native_publication", publication);
                auto response = documentState(snapshot, *doc);
                completeEdit(response, transaction, history ? 1 : count, revisionToken(snapshot, *doc) != revision);
                response["changeSummary"] = changes;
                if (response["changed"].toBool()) {
                    if (addedFrame)
                        frame = doc->activeFrame();
                    if (doc->animated())
                        comparison.detail = QString("%1 %2\nBefore %3\nAfter %4")
                            .arg(addedFrame ? "Added frame" : "Frame", frame.toString(QUuid::WithoutBraces),
                                 playbackBefore.isEmpty() ? QString("not animated") : playbackBefore, playback());
                    comparison.after = pixels();
                    comparison.serial = doc->changeSerial();
                    comparison.afterRevision = response["revision"].toString();
                    rememberComparison(std::move(comparison));
                }
                if (workflowStage && response["changed"].toBool() && doc->modified()) {
                    // Serialize with the periodic checkpoint writer so an older snapshot
                    // cannot replace the stage we are about to report as committed.
                    const auto store = *recovery;
                    QElapsedTimer copy;
                    copy.start();
                    const auto checkpoint = doc->recoverySnapshot();
                    metrics.record("mcp_workflow_recovery_snapshot", copy);
                    std::packaged_task<bool()> write([store, checkpoint, snapshotRoot = snapshot.root] {
                        return store.writeDocument(*checkpoint, snapshotRoot);
                    });
                    auto completion = write.get_future();
                    QElapsedTimer wait;
                    wait.start();
                    ioPool.start([write = std::move(write)]() mutable { write(); });
                    bool written = false;
                    try {
                        written = completion.get();
                    } catch (...) {
                        // Publication already committed. Report a recovery warning.
                    }
                    metrics.record("mcp_workflow_recovery_wait", wait);
                    if (written)
                        recoveredKeys[doc->id()] = qint64(doc->changeSerial());
                    else
                        response["warnings"] = QJsonArray{"Recovery checkpoint failed. The edit is still in memory."};
                }
                return response;
            };
            return {};
        });
    }

    auto dialog = new QDialog(this);
    dialog->setObjectName("mcpAccessDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("MCP Integration");
    auto outer = new QVBoxLayout(dialog);
    outer->setSizeConstraint(QLayout::SetMinimumSize);
    // Phase 4-G: one status line for the whole integration, then Access and Agent Setup.
    auto overview = new QLabel(dialog);
    overview->setObjectName("mcpOverview");
    overview->setTextFormat(Qt::PlainText);
    overview->setWordWrap(true);
    overview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(overview);
    auto mcpTabs = new QTabWidget(dialog);
    mcpTabs->setObjectName("mcpTabs");
    outer->addWidget(mcpTabs);
    auto accessPage = new QWidget(mcpTabs);
    accessPage->setObjectName("mcpAccessPage");
    auto layout = new QVBoxLayout(accessPage);
    mcpTabs->addTab(accessPage, "Access");
    mcpTabs->addTab(createMcpSetupPage(mcpTabs, [this] {
        return mcpService && mcpService->status() == "Connected"
            ? mcpService->clientName() + " · " + mcpService->permission() : QString{};
    }), "Agent Setup");
    mcpTabs->setCurrentIndex(page);
    auto description = new QLabel("MCP lets an AI agent on this PC inspect the open resource pack and, only if you "
                                  "allow editing, change it through Nova Studio. Access is off until you choose "
                                  "Allow. It covers the current project only and ends when the agent disconnects, "
                                  "the project changes or you revoke it. An unused authorization expires after five "
                                  "minutes.", dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto projectLabel = new QLabel(dialog);
    projectLabel->setTextFormat(Qt::PlainText);
    projectLabel->setWordWrap(true);
    layout->addWidget(projectLabel);
    auto status = new QLabel(dialog);
    status->setObjectName("mcpAccessStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto permissionLabel = new QLabel(dialog);
    permissionLabel->setObjectName("mcpAccessPermission");
    permissionLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(permissionLabel);
    auto workflowActivity = new QLabel(dialog);
    workflowActivity->setObjectName("mcpWorkflowActivity");
    workflowActivity->setTextFormat(Qt::PlainText);
    workflowActivity->setWordWrap(true);
    layout->addWidget(workflowActivity);
    auto row = new QHBoxLayout;
    layout->addLayout(row);
    auto authorize = new QPushButton("Allow read-only", dialog);
    authorize->setObjectName("mcpAuthorize");
    row->addWidget(authorize);
    auto write = new QPushButton("Allow editing…", dialog);
    write->setObjectName("mcpAuthorizeEditing");
    row->addWidget(write);
    auto copy = new QPushButton("Copy connection code", dialog);
    copy->setObjectName("mcpCopyCode");
    row->addWidget(copy);
    auto revoke = new QPushButton("Revoke access", dialog);
    revoke->setObjectName("mcpRevoke");
    row->addWidget(revoke);
    auto reviewToggle = new QToolButton(dialog);
    reviewToggle->setText("Review changes");
    reviewToggle->setCheckable(true);
    reviewToggle->setObjectName("mcpReviewToggle");
    layout->addWidget(reviewToggle, 0, Qt::AlignLeft);
    auto reviewPane = new QWidget(dialog);
    auto reviewLayout = new QVBoxLayout(reviewPane);
    reviewLayout->setContentsMargins(0, 0, 0, 0);
    auto reviewTitle = new QLabel(reviewPane);
    reviewTitle->setObjectName("mcpReviewState");
    reviewTitle->setTextFormat(Qt::PlainText);
    reviewTitle->setWordWrap(true);
    reviewTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    reviewLayout->addWidget(reviewTitle);
    auto stages = new QListWidget(reviewPane);
    stages->setObjectName("mcpReviewStages");
    stages->setAccessibleName("Workflow stages");
    stages->setMinimumHeight(64);
    stages->setMaximumHeight(128);
    stages->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    reviewLayout->addWidget(stages);
    auto stageDetail = new QLabel(reviewPane);
    stageDetail->setTextFormat(Qt::PlainText);
    stageDetail->setWordWrap(true);
    stageDetail->setObjectName("mcpReviewStageDetail");
    reviewLayout->addWidget(stageDetail);
    auto stageActions = new QHBoxLayout;
    reviewLayout->addLayout(stageActions);
    auto open = new QPushButton("Open asset", reviewPane);
    open->setObjectName("mcpReviewOpen");
    stageActions->addWidget(open);
    auto inspect = new QPushButton("Compare…", reviewPane);
    inspect->setObjectName("mcpReviewCompare");
    stageActions->addWidget(inspect);
    // A push button reserves room for its menu indicator at every scale.
    auto more = new QPushButton("Actions", reviewPane);
    more->setObjectName("mcpReviewActions");
    auto actions = new QMenu(more);
    more->setMenu(actions);
    auto validation = actions->addAction("Last validation results…");
    auto problems = actions->addAction("Validate in Problems");
    auto historyAction = actions->addAction("Show asset history");
    auto undo = actions->addAction("Undo this edit");
    undo->setObjectName("mcpReviewUndo");
    stageActions->addWidget(more);
    stageActions->addStretch();
    layout->addWidget(reviewPane);
    reviewPane->hide();
    connect(reviewToggle, &QToolButton::toggled, reviewPane, &QWidget::setVisible);
    const auto selectedStage = [this, stages]() -> mcp::ReviewStage {
        return mcpService->workflowReview().stages.value(stages->currentRow());
    };
    const auto canUndo = [this](const mcp::ReviewStage &stage) {
        for (const auto &comparison : comparisons) {
            if (comparison.path != stage.path || comparison.afterRevision != stage.revision ||
                !comparisonCurrent(comparison))
                continue;
            if (const auto model = modelPanel->openModelDocument(stage.path))
                return !model->previewing() && !modelPanel->uvInteractionActive() &&
                       model->ownsHistoryHead(comparison.owner, false);
            if (const auto doc = reviewRaster(stage.path))
                return !doc->strokeOpen() && doc->ownsHistoryHead(comparison.owner, false);
        }
        return false;
    };
    const auto refreshStage = [this, selectedStage, canUndo, open, inspect, historyAction, undo, stageDetail] {
        const auto stage = selectedStage();
        const auto model = modelPanel->openModelDocument(stage.path);
        const auto raster = reviewRaster(stage.path);
        const bool available = model || raster || collection.records.contains(stage.path);
        const bool undoable = canUndo(stage) && mcpService->workflowReview().tool != "nova.edit.undo";
        open->setEnabled(available);
        inspect->setEnabled(stage.state == "Committed" && !stage.path.isEmpty());
        historyAction->setEnabled(bool(raster));
        undo->setEnabled(undoable);
        QStringList details;
        if (!stage.operations.isEmpty())
            details << stage.operations.join(", ");
        if (stage.state == "Committed" && !stage.changed)
            details << "No document change";
        if (model || raster)
            details << ((model ? model->modified() : raster->modified()) ? "Unsaved changes" : "Clean")
                    << (undoable ? "Undo available" : "Undo unavailable");
        stageDetail->setText(details.join(" · "));
    };
    connect(stages, &QListWidget::currentRowChanged, dialog, refreshStage);
    connect(open, &QPushButton::clicked, dialog, [this, selectedStage] { openRelatedAsset(selectedStage().path); });
    connect(stages, &QListWidget::itemActivated, dialog, [this, selectedStage] { openRelatedAsset(selectedStage().path); });
    connect(inspect, &QPushButton::clicked, dialog, [this, selectedStage] {
        const auto stage = selectedStage();
        showComparison(stage.path, stage.revision);
    });
    connect(validation, &QAction::triggered, dialog, [this, dialog] {
        auto findings = new QDialog(dialog);
        findings->setAttribute(Qt::WA_DeleteOnClose);
        findings->setWindowTitle("Last MCP Validation");
        auto layout = new QVBoxLayout(findings);
        auto text = new QPlainTextEdit(findings);
        text->setReadOnly(true);
        auto summary = mcpService->validationActivity();
        if (summary.isEmpty())
            summary = "No validation results for this project";
        text->setPlainText(summary + "\n" + mcpService->reviewValidationFindings().join('\n'));
        layout->addWidget(text);
        findings->resize(490, 260);
        findings->show();
    });
    connect(problems, &QAction::triggered, dialog, [this, selectedStage] {
        const auto stage = selectedStage();
        if (!stage.path.isEmpty())
            openRelatedAsset(stage.path);
        showProblems(true);
    });
    connect(historyAction, &QAction::triggered, dialog, [this, selectedStage] {
        if (openRelatedAsset(selectedStage().path)) {
            historyDock->show();
            historyDock->raise();
        }
    });
    connect(undo, &QAction::triggered, dialog, [this, selectedStage, canUndo, refreshStage] {
        const auto stage = selectedStage();
        if (!canUndo(stage) || !isLocalAsset(stage.path))
            return;
        if (const auto model = modelPanel->openModelDocument(stage.path))
            modelPanel->modelHistory(false, model);
        else if (const auto doc = reviewRaster(stage.path)) {
            for (int i = 0; i < tabs->count(); ++i)
                if (auto canvas = dynamic_cast<Canvas *>(tabs->widget(i)); canvas &&
                    canvas->document == doc && canvas->interactionActive())
                    return;
            session.dispatch(doc, Command::Undo);
        }
        refreshStage();
    });
    auto reviewTimer = new QTimer(reviewPane);
    reviewTimer->setInterval(250);
    connect(reviewTimer, &QTimer::timeout, reviewPane, refreshStage);
    connect(reviewToggle, &QToolButton::toggled, dialog, [reviewTimer, refreshStage](bool expanded) {
        if (expanded) {
            refreshStage();
            reviewTimer->start();
        } else
            reviewTimer->stop();
    });
    const auto refreshReview = [this, reviewTitle, stages, refreshStage, reviewToggle] {
        if (!reviewToggle->isChecked())
            return;
        const auto &review = mcpService->workflowReview();
        reviewTitle->setText(review.id.isEmpty() ? "No workflow activity yet" :
            QString("%1 · %2\n%3 · %4").arg(review.state, review.id.left(12), review.client,
                                            QFileInfo(review.project).fileName()) +
            (review.stages.size() > 1 ? QString("\nEach stage commits on its own. There is no workflow rollback.")
                                      : QString{}) +
            (review.error.isEmpty() ? QString{} : "\n" + review.error));
        const auto selected = stages->currentRow();
        const QSignalBlocker block(stages);
        stages->clear();
        for (const auto &stage : review.stages) {
            const auto asset = stage.path.isEmpty() ? stage.assetId.left(12) : stage.path;
            auto item = new QListWidgetItem(stage.state + " · " +
                (asset.isEmpty() ? QString("New asset") : asset), stages);
            item->setToolTip(item->text());
        }
        if (stages->count())
            stages->setCurrentRow(std::clamp(selected, 0, stages->count() - 1));
        refreshStage();
    };
    connect(mcpService, &mcp::Service::changed, dialog, refreshReview);
    connect(reviewToggle, &QToolButton::toggled, dialog, refreshReview);
    auto refresh = [this, overview, projectLabel, status, permissionLabel, workflowActivity, authorize, write, copy, revoke] {
        const auto state = mcpService->status();
        const auto permission = mcpService->permission();
        const auto error = mcpService->error();
        QStringList summary{mcpBridgeSummary()};
        if (state == "Connected")
            summary << "Client connected · " + mcpService->clientName() + " · " + permission;
        else if (state == "Waiting for client")
            summary << (mcpService->offeredToAgents()
                ? "Authorized · waiting for an agent. Start or reconnect your agent within five minutes."
                : "Authorized · waiting for a client that supplies the connection code. " + mcpService->offerProblem());
        else if (state == "Project changed")
            summary << "Authorization required · the project changed. Allow access again for this project.";
        else
            summary << "Access off · choose Allow read-only or Allow editing to let one agent connect.";
        if (!error.isEmpty())
            summary << "Connection or configuration error · " + error;
        overview->setText(summary.join(QChar(0x0A)));
        const bool canAuthorize = state == "Disabled" || state == "Project changed";
        const QString prefix = canAuthorize ? "Current project: " : "Authorized project: ";
        projectLabel->setText(project.isEmpty() ? "No project open" : prefix + QFileInfo(project).fileName());
        status->setText(error.isEmpty() ? state : state + "\nLast error: " + error);
        permissionLabel->setText(permission == "Disabled" ? "Permission · None" : "Permission · " + permission);
        QStringList details;
        if (!mcpService->clientName().isEmpty())
            details << "Client · " + mcpService->clientName();
        if (!mcpService->editActivity().isEmpty())
            details << mcpService->editActivity();
        if (!mcpService->validationActivity().isEmpty())
            details << mcpService->validationActivity();
        workflowActivity->setText(details.join('\n'));
        workflowActivity->setVisible(!details.isEmpty());
        authorize->setEnabled(!project.isEmpty() && canAuthorize);
        write->setEnabled(!project.isEmpty() && canAuthorize);
        copy->setEnabled(!mcpService->connectionCode().isEmpty());
        revoke->setEnabled(state != "Disabled");
    };
    connect(mcpService, &mcp::Service::changed, dialog, refresh);
    connect(authorize, &QPushButton::clicked, dialog, [this] { mcpService->authorize(project); });
    connect(write, &QPushButton::clicked, dialog, [this, dialog] {
        const auto selected = project;
        if (QMessageBox::question(dialog, "Allow MCP editing?",
                "This connection can create unsaved textures and animations, modify open documents and remove layers, frames, and model elements. "
                "Edits are undoable and follow Nova Studio’s normal save and recovery rules.",
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes &&
            project == selected)
            mcpService->authorize(selected, true);
    });
    connect(copy, &QPushButton::clicked, dialog, [this] {
        QApplication::clipboard()->setText(mcpService->connectionCode());
    });
    connect(revoke, &QPushButton::clicked, mcpService, &mcp::Service::revoke);
    refresh();
    // A comfortable default that still fits the screen at every display scale; pages scroll.
    const QSize available = screen() ? screen()->availableGeometry().size() * 0.9 : QSize(1280, 800);
    dialog->resize(QSize(600, 620).boundedTo(available).expandedTo(dialog->minimumSizeHint()));
    dialog->show();
}
} // namespace nova
