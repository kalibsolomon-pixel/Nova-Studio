// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance-only instrumentation. These checks are evidence class A, never manual GUI evidence.
#include "animation_export.h"
#include "minecraft_animation.h"
#include "project.h"
#include "timeline.h"
#include "window.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSurfaceFormat>
#include <QtTest>
#include <QtWidgets>
#include <cstdio>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif
using namespace nova;
namespace nova {
class EditorTestAccess {
  public:
    static void open(Window &window, const QString &path) { window.openTexture(path); }
    static void frameOperation(Window &window) { window.processAnimationFrames("gaussian_blur"); }
    static bool indexing(const Window &window) { return window.indexBusy; }
    static bool serializationBusy(const Window &window) { return window.ioPool.activeThreadCount() > 0; }
};
}
namespace {
QJsonArray checks;
void require(bool ok, const QString &name, const QString &detail = {}) {
    checks.append(QJsonObject{{"name", name}, {"passed", ok}, {"detail", detail}});
    if (!ok) throw QString(name + ": " + detail);
}
QByteArray read(const QString &path) {
    QFile input(path); if (!input.open(QIODevice::ReadOnly)) return {}; return input.readAll();
}
void write(const QString &path, const QByteArray &bytes) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create output parent", path);
    require(atomicWrite(path, bytes, {}, nullptr), "write new acceptance artifact", path);
}
QByteArray pixelHash(const QImage &image) {
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()), QCryptographicHash::Sha256).toHex();
}
QImage originalPixels(const QString &fixture, int index, QSize size) {
    static const QColor colors[] = {QColor(240, 40, 30), QColor(30, 210, 65), QColor(35, 65, 240), QColor(245, 205, 25)};
    QImage image(size, QImage::Format_RGBA8888); image.fill(colors[index % 4]);
    for (int y = 0; y < size.height(); ++y) for (int x = 0; x < size.width(); ++x) {
        if (fixture == "E") {
            auto color = colors[index % 4]; color.setAlpha(x < 5 ? 0 : x < 11 ? 96 : 255); image.setPixelColor(x, y, color);
        } else if ((x == index + 1 || y == index + 1) && fixture != "D") image.setPixelColor(x, y, QColor(255, 255, 255));
    }
    // Small asymmetric identity marker: distinguishes cell identity and orientation.
    image.setPixelColor(0, 0, QColor(10 + index * 40, 20, 30, fixture == "E" ? 0 : 255));
    return image;
}
std::shared_ptr<Document> fixtureDocument(const QString &path, const QString &name, int count = 4, QSize size = {16, 16}) {
    auto doc = std::make_shared<Document>(path, originalPixels(name, 0, size));
    auto state = doc->animationState(); state.enabled = true; state.frames.clear(); state.entries.clear();
    state.defaultTicks = name == "D" ? 12 : 10; state.interpolate = name == "D";
    state.target = "1.21.11"; state.metadata = {{"texture", QJsonObject{{"blur", false}}}, {"nova_acceptance", QJsonObject{{"fixture", name}}}};
    for (int i = 0; i < count; ++i) {
        SourceFrame frame; frame.name = QString("%1 source %2").arg(name).arg(i);
        RasterLayer layer; layer.id = QUuid::createUuid(); layer.name = "Original fixture pixels";
        layer.pixels = originalPixels(name, i, size); frame.activeLayer = layer.id; frame.layers.push_back(layer);
        state.frames.push_back(frame); state.entries.push_back({frame.id, 0});
    }
    if (name == "B") state.entries = {{state.frames[2].id, 4}, {state.frames[0].id, 12}, {state.frames[2].id, 8}, {state.frames[3].id, 20}, {state.frames[1].id, 6}};
    state.active = state.frames[0].id; QString error;
    require(doc->replaceAnimation(state, "Acceptance fixture", &error), "construct " + name, error); return doc;
}
void generateFixtures(const QString &root) {
    require(!QFileInfo::exists(root), "fixture destination is new", root); require(QDir().mkpath(root), "create fixture destination");
    QJsonArray manifests;
    const QStringList names{"A", "B", "C", "D", "E"};
    const QStringList blocks{"stone", "oak_planks", "bricks", "gold_block", "white_stained_glass"};
    for (int fixture = 0; fixture < names.size(); ++fixture) {
        const auto name = names[fixture]; const QSize size = name == "C" ? QSize(16, 8) : QSize(16, 16);
        const int count = name == "D" || name == "E" ? 2 : 4, columns = name == "B" || name == "C" ? 2 : 1;
        const auto path = root + "/sources/" + name + "/texture.png";
        require(QDir().mkpath(QFileInfo(path).absolutePath()), "create source folder"); auto doc = fixtureDocument(path, name, count, size);
        QString error; auto planned = planAnimationExport(*doc, columns, "1.21.11", &error); require(planned.ok(), "plan " + name, error);
        require(doc->saveNative(QFileInfo(path).absolutePath(), &error), "save native " + name, error);
        const auto journal = stageAnimationPair(path, planned.png, planned.mcmeta, {}, {}, &error);
        require(!journal.isEmpty() && finishAnimationPair(journal, PairAction::Retry, &error), "export " + name, error);
        doc->acceptAnimationExport(Revision::read(path), Revision::read(path + ".mcmeta"));
        require(doc->saveNative(QFileInfo(path).absolutePath(), &error), "persist export baseline " + name, error);
        QJsonArray pixels, entries; const auto state = doc->animationState();
        for (int i = 0; i < count; ++i) pixels.append(QJsonObject{{"index", i}, {"rgba_sha256", QString::fromLatin1(pixelHash(originalPixels(name, i, size)))}});
        int totalTicks = 0;
        for (const auto &entry : state.entries) {
            int source = 0; while (state.frames[size_t(source)].id != entry.frame) ++source;
            const int ticks = entry.ticks ? entry.ticks : state.defaultTicks; totalTicks += ticks;
            entries.append(QJsonObject{{"index", source}, {"ticks", ticks}, {"milliseconds", ticks * 50}});
        }
        for (auto profile : {QString("1.21.11"), QString("26.1"), QString("26.3")}) {
            auto imported = openMinecraftAnimation(path, profile, &error); require(bool(imported), "import " + name + " / " + profile, error);
            const auto importedState = imported->animationState(); require(importedState.frames.size() == size_t(count), "source count " + name + " / " + profile);
            for (int i = 0; i < count; ++i) require(imported->frameComposite(importedState.frames[size_t(i)].id) == originalPixels(name, i, size), "exact RGBA " + name + " / " + profile + " / " + QString::number(i));
            require(importedState.entries.size() == state.entries.size() && importedState.defaultTicks == state.defaultTicks && importedState.interpolate == state.interpolate,
                    "timing settings " + name + " / " + profile);
            for (size_t i = 0; i < state.entries.size(); ++i) {
                const int index = entries[int(i)].toObject()["index"].toInt();
                require(importedState.entries[i].frame == importedState.frames[size_t(index)].id && importedState.entries[i].ticks == state.entries[i].ticks,
                        "playback references " + name + " / " + profile + " / " + QString::number(i));
            }
            require(importedState.metadata["nova_acceptance"] == state.metadata["nova_acceptance"], "unknown metadata " + name + " / " + profile);
        }
        manifests.append(QJsonObject{{"fixture", name}, {"frame_width", size.width()}, {"frame_height", size.height()}, {"source_count", count}, {"columns", columns},
                                     {"default_ticks", state.defaultTicks}, {"interpolate", state.interpolate}, {"entries", entries}, {"cycle_ticks", totalTicks},
                                     {"source_pixels", pixels}, {"png_sha256", QString::fromLatin1(Revision::read(path).hash.toHex())},
                                     {"metadata", planned.metadata}, {"runtime_asset", "assets/minecraft/textures/block/" + blocks[fixture] + ".png"}});
        for (auto profile : {QString("1.21.11"), QString("26.1"), QString("26.3")}) {
            const auto destination = root + "/packs/" + profile + "/assets/minecraft/textures/block/" + blocks[fixture] + ".png";
            write(destination, planned.png); write(destination + ".mcmeta", planned.mcmeta);
        }
    }
    for (const auto &profile : {QString("1.21.11"), QString("26.1"), QString("26.3")}) {
        const QJsonArray format = profile == "1.21.11" ? QJsonArray{75, 0} : profile == "26.1" ? QJsonArray{84, 0} : QJsonArray{97, 1};
        write(root + "/packs/" + profile + "/pack.mcmeta", QJsonDocument(QJsonObject{{"pack", QJsonObject{{"description", "Nova C4 original A-E acceptance fixtures"}, {"min_format", format}, {"max_format", format}}}}).toJson());
    }
    // An additional large native source supports scrolling/shutdown acceptance without touching 50k fixtures.
    const auto largePath = root + "/large/texture.png"; QDir().mkpath(QFileInfo(largePath).absolutePath());
    auto large = fixtureDocument(largePath, "Large", 300, {64, 64}); QString error;
    require(large->saveNative(QFileInfo(largePath).absolutePath(), &error), "large native fixture", error);
    write(root + "/expected.json", QJsonDocument(QJsonObject{{"evidence_class", "A: exported fixtures, not Minecraft runtime observations"}, {"fixtures", manifests}, {"checks", checks}}).toJson(QJsonDocument::Indented));
}
void integrity(const QString &root) {
    require(!QFileInfo::exists(root), "integrity output is new"); QDir().mkpath(root); QString error;
    const auto path = root + "/missing/texture.png"; QDir().mkpath(QFileInfo(path).absolutePath());
    auto doc = fixtureDocument(path, "A"); auto plan = planAnimationExport(*doc); write(path, plan.png); write(path + ".mcmeta", plan.mcmeta);
    auto beforePng = Revision::read(path), beforeMeta = Revision::read(path + ".mcmeta");
    doc->draw({1, 1}, {1, 1}, qRgb(12, 34, 56)); doc->finish(); plan = planAnimationExport(*doc);
    auto journal = stageAnimationPair(path, plan.png, plan.mcmeta, beforePng, beforeMeta, &error); require(!journal.isEmpty(), "stage missing-backup case", error);
    require(!finishAnimationPair(journal, PairAction::Retry, &error, [] { return false; }), "inject interruption after first replacement", error);
    const auto backup = QFileInfo(journal).absolutePath() + "/png.before", retained = backup + ".retained";
    require(QFile::rename(backup, retained), "retain backup outside expected path"); const auto partial = Revision::read(path);
    require(!finishAnimationPair(journal, PairAction::Retry, &error), "missing backup blocks retry", error);
    require(!finishAnimationPair(journal, PairAction::Rollback, &error), "missing backup blocks rollback", error);
    require(partial.sameContent(Revision::read(path)) && beforeMeta.sameContent(Revision::read(path + ".mcmeta")), "missing backup preserves valid external files");
    require(QFile::rename(retained, backup) && finishAnimationPair(journal, PairAction::Rollback, &error), "restore backup then roll back", error);
    require(beforePng.sameContent(Revision::read(path)) && beforeMeta.sameContent(Revision::read(path + ".mcmeta")), "original pair restored exactly");
#ifdef Q_OS_WIN
    const auto lockedPath = root + "/readonly/texture.png"; QDir().mkpath(QFileInfo(lockedPath).absolutePath());
    auto lockedDoc = fixtureDocument(lockedPath, "A"); auto lockedPlan = planAnimationExport(*lockedDoc); write(lockedPath, lockedPlan.png); write(lockedPath + ".mcmeta", lockedPlan.mcmeta);
    const auto originalMeta = Revision::read(lockedPath + ".mcmeta"); lockedDoc->draw({1, 1}, {1, 1}, qRgb(90, 80, 70)); lockedDoc->finish();
    auto changedTiming = lockedDoc->animationState(); changedTiming.defaultTicks = 11;
    require(lockedDoc->replaceAnimation(changedTiming, "Change exported metadata"), "prepare changed metadata for read-only test");
    lockedPlan = planAnimationExport(*lockedDoc); error.clear();
    auto readOnlyJournal = stageAnimationPair(lockedPath, lockedPlan.png, lockedPlan.mcmeta, Revision::read(lockedPath), originalMeta, &error);
    require(!readOnlyJournal.isEmpty(), "stage literal read-only case", error);
    const auto nativeMeta = QDir::toNativeSeparators(lockedPath + ".mcmeta");
    const bool changed = SetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeMeta.utf16()), FILE_ATTRIBUTE_READONLY) != 0;
    require(changed, "set read-only attribute on owned test metadata");
    const bool committed = finishAnimationPair(readOnlyJournal, PairAction::Retry, &error);
    SetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeMeta.utf16()), FILE_ATTRIBUTE_NORMAL);
    require(!committed, "read-only metadata rejects second replacement", error);
    require(originalMeta.sameContent(Revision::read(lockedPath + ".mcmeta")), "read-only original remains exact");
    require(!pendingAnimationPairs(lockedPath).isEmpty(), "partial read-only export is detected");
    require(finishAnimationPair(readOnlyJournal, PairAction::Retry, &error), "retry after read-only attribute removed", error);
#endif
    write(root + "/results.json", QJsonDocument(QJsonObject{{"evidence_class", "A: direct filesystem fault checks"}, {"checks", checks}}).toJson(QJsonDocument::Indented));
}
void shutdownScenario(const QString &scenario, const QString &fixtures, const QString &settings, const QString &output) {
    auto checkpoint = [&](const char *stage) { QFile trace(output + ".stages"); if (trace.open(QIODevice::Append)) { trace.write(stage); trace.write("\n"); trace.flush(); } };
    checkpoint("start");
    qInfo() << "shutdown probe start" << scenario;
    qApp->setProperty("settingsFile", settings); QElapsedTimer started; started.start();
    const auto project = scenario == "empty" ? QString{} : scenario == "indexing" ? QDir::currentPath() + "/fixtures/generated/pack-50000" :
        fixtures + (scenario == "static" ? "/static" : scenario == "scrolling" || scenario == "native-save" || scenario == "frame-operation" ? "/large" : "/sources/A");
    Window window(project, output + ".telemetry.json", started); window.show();
    checkpoint("window constructed");
    qInfo() << "window constructed";
    if (scenario == "indexing") {
        require(EditorTestAccess::indexing(window), "indexing active at project close");
        window.findChild<QAction *>("closeProjectCommand")->trigger();
    } else if (scenario != "empty") {
        auto path = project + "/texture.png"; EditorTestAccess::open(window, path);
        checkpoint("document opened");
        qInfo() << "document opened";
        require(window.activeCanvas() != nullptr, "open document for shutdown", scenario);
        require(window.activeCanvas()->document->animated() == (scenario != "static"), "expected animation mode", scenario);
    }
    QTimer responder; responder.setInterval(10);
    QObject::connect(&responder, &QTimer::timeout, &window, [] {
        if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            if (auto discard = box->button(QMessageBox::Discard)) discard->click();
        }
    }); responder.start();
    checkpoint("responder started");
    qInfo() << "responder started";
    QTest::qWait(150);
    auto timeline = window.findChild<TimelinePanel *>();
    if (scenario == "playback" && timeline) timeline->playback.play();
    if (scenario == "playback") require(timeline && timeline->playback.playing(), "playback active before close");
    if (scenario == "onion") if (auto on = window.findChild<QCheckBox *>("onionEnabled")) on->setChecked(true);
    if (scenario == "onion") require(window.findChild<QCheckBox *>("onionEnabled")->isChecked(), "onion enabled before close");
    if (scenario == "scrolling" && timeline) for (int index = 0; index < 300; index += 17) { timeline->timelineView()->scrollTo(timeline->timelineView()->model()->index(index, 0)); QApplication::processEvents(); }
    if (scenario == "scrolling") require(timeline && timeline->timelineView()->horizontalScrollBar()->value() > 0, "timeline scrolled before close");
    if (scenario == "floating") { auto dock = window.findChild<QDockWidget *>("preview"); dock->setFloating(true); dock->resize(350, 260); QTest::qWait(100);
        require(dock->isFloating() && dock->findChild<QOpenGLWidget *>()->isValid(), "floating preview has valid OpenGL context"); }
    if (scenario == "native-save") {
        bool workerActiveAtClose = false;
        window.activeCanvas()->document->draw({1, 1}, {1, 1}, qRgb(30, 80, 120)); window.activeCanvas()->document->finish();
        QTimer::singleShot(5, &window, [&] { workerActiveAtClose = EditorTestAccess::serializationBusy(window); window.close(); });
        window.findChild<QAction *>("saveNativeCommand")->trigger();
        require(workerActiveAtClose, "native serialization worker active at close request");
    }
    if (scenario == "frame-operation") {
        bool workerActiveAtClose = false, cancelClicked = false;
        QTimer::singleShot(80, &window, [&] {
            if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                auto scope = dialog->findChild<QComboBox *>("frameOperationScope"); if (scope) scope->setCurrentIndex(2);
                for (auto button : dialog->findChildren<QPushButton *>()) if (button->text() == "Preview") {
                    QTimer::singleShot(10, &window, [&] {
                        workerActiveAtClose = QThreadPool::globalInstance()->activeThreadCount() > 0;
                        for (auto widget : QApplication::topLevelWidgets()) if (auto progress = qobject_cast<QProgressDialog *>(widget))
                            for (auto cancel : progress->findChildren<QPushButton *>()) if (cancel->text() == "Cancel") { cancelClicked = true; cancel->click(); }
                        window.close();
                    });
                    button->click(); dialog->reject(); break;
                }
            }
        }); EditorTestAccess::frameOperation(window);
        require(cancelClicked && workerActiveAtClose, "frame worker active at cancellation and close request");
    }
    if (scenario == "export-cancel") {
        QTimer::singleShot(100, &window, [] { if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        window.findChild<QAction *>("exportAnimation")->trigger();
    }
    QElapsedTimer closing; closing.start();
    checkpoint("closing");
    qInfo() << "closing";
    if (scenario != "native-save") require(window.close(), "close accepted", scenario);
    require(!window.isVisible(), "window hidden after close", scenario);
    checkpoint("close returned");
    qInfo() << "close returned";
    QTest::qWait(50);
    write(output, QJsonDocument(QJsonObject{{"scenario", scenario}, {"post_operation_close_ms", double(closing.elapsed())}, {"lifetime_before_destructor_ms", double(started.elapsed())}, {"checks", checks}}).toJson(QJsonDocument::Indented));
}
void supplement(const QString &root) {
    QString error; const auto path = root + "/large/texture.png";
    auto large = std::make_shared<Document>(path, QImage{});
    require(large->loadNative(QFileInfo(path).absolutePath(), &error), "load large native fixture", error);
    auto plan = planAnimationExport(*large, 10, "1.21.11", &error); require(plan.ok(), "plan large sheet", error);
    const auto journal = stageAnimationPair(path, plan.png, plan.mcmeta, large->revision(), large->mcmetaRevision(), &error);
    require(!journal.isEmpty() && finishAnimationPair(journal, PairAction::Retry, &error), "export large fixture", error);
    large->acceptAnimationExport(Revision::read(path), Revision::read(path + ".mcmeta"));
    require(large->saveNative(QFileInfo(path).absolutePath(), &error), "save large baseline", error);
    QDir().mkpath(root + "/static"); require(savePng(root + "/static/texture.png", originalPixels("Static", 0, {32, 32})), "static fixture");
}
void compareNative(const QString &root, const QString &path, const QString &output) {
    QString error; auto native = std::make_shared<Document>(path, QImage{});
    require(native->loadNative(root, &error), "load native for comparison", error);
    auto imported = openMinecraftAnimation(path, "26.3", &error); require(bool(imported), "reimport native UI export", error);
    const auto a = native->animationState(), b = imported->animationState();
    require(a.frames.size() == b.frames.size() && a.entries.size() == b.entries.size(), "source and entry counts match");
    QJsonArray frames, entries;
    for (size_t i = 0; i < a.frames.size(); ++i) {
        const auto image = native->frameComposite(a.frames[i].id);
        require(image == imported->frameComposite(b.frames[i].id), "exact composite RGBA " + QString::number(i));
        frames.append(QJsonObject{{"index", int(i)}, {"layers", int(a.frames[i].layers.size())}, {"rgba_sha256", QString::fromLatin1(pixelHash(image))}});
    }
    require(a.defaultTicks == b.defaultTicks && a.interpolate == b.interpolate, "timing settings match");
    for (size_t i = 0; i < a.entries.size(); ++i) {
        int ai = 0, bi = 0;
        while (a.frames[size_t(ai)].id != a.entries[i].frame) ++ai;
        while (b.frames[size_t(bi)].id != b.entries[i].frame) ++bi;
        require(ai == bi && a.entries[i].ticks == b.entries[i].ticks, "playback index and duration " + QString::number(i));
        entries.append(QJsonObject{{"source", ai}, {"ticks", a.entries[i].ticks ? a.entries[i].ticks : a.defaultTicks}});
    }
    write(output, QJsonDocument(QJsonObject{{"evidence_class", "A: exact comparison of B-native-created export"}, {"frames", frames}, {"entries", entries}, {"checks", checks}}).toJson());
}
void verifyProfiles(const QString &root, const QString &output) {
    const auto fixtures = QJsonDocument::fromJson(read(root + "/expected.json")).object()["fixtures"].toArray();
    require(fixtures.size() == 5, "five original fixture manifests");
    QString error;
    for (const auto &value : fixtures) {
        const auto fixture = value.toObject(); const auto name = fixture["fixture"].toString();
        const auto path = root + "/sources/" + name + "/texture.png";
        auto doc = std::make_shared<Document>(path, QImage{});
        require(doc->loadNative(QFileInfo(path).absolutePath(), &error), "load source " + name, error);
        for (const auto &profile : {QString("1.21.11"), QString("26.1"), QString("26.3")}) {
            const auto plan = planAnimationExport(*doc, fixture["columns"].toInt(), profile, &error);
            require(plan.ok(), "plan " + name + " / " + profile, error);
            require(plan.png == read(path) && plan.mcmeta == read(path + ".mcmeta"), "exact planned pair " + name + " / " + profile);
        }
    }
    write(output, QJsonDocument(QJsonObject{{"evidence_class", "A: independent export planning for all 15 fixture/profile combinations"}, {"checks", checks}}).toJson());
}
}
int main(int argc, char **argv) {
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &message) {
        const auto bytes = message.toLocal8Bit(); std::fprintf(stderr, "Qt[%d] %s\n", int(type), bytes.constData()); std::fflush(stderr);
    });
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format; format.setDepthBufferSize(24); format.setVersion(2, 1); format.setProfile(QSurfaceFormat::CompatibilityProfile); QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv); app.setStyle("Fusion"); const auto args = app.arguments();
    try {
        if (args.size() == 3 && args[1] == "--fixtures") generateFixtures(args[2]);
        else if (args.size() == 3 && args[1] == "--integrity") integrity(args[2]);
        else if (args.size() == 3 && args[1] == "--supplement") supplement(args[2]);
        else if (args.size() == 5 && args[1] == "--compare") compareNative(args[2], args[3], args[4]);
        else if (args.size() == 4 && args[1] == "--profiles") verifyProfiles(args[2], args[3]);
        else if (args.size() == 6 && args[1] == "--shutdown") shutdownScenario(args[2], args[3], args[4], args[5]);
        else return 2;
    } catch (const QString &error) { qCritical().noquote() << error; return 1; }
    return 0;
}
