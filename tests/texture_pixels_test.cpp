// SPDX-License-Identifier: GPL-3.0-or-later
// MCP Phase 2: compact texture tools (nova.texture.put_pixels, palette_remap, gradient_map, erase).
// Exact pixel semantics on synthetic fixtures, selection and layer rules, atomicity and history,
// schema compatibility with the Phase 1 tool list, and the tools end to end through the real bridge:
// authorization, drafts, validation and saving. efficiency() compares them with fill_rect workflows.
#include "window.h"
#include "imageops.h"
#include "protocol.h"
#include "service.h"
#include "texture_edit.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QSysInfo>
#include <QThread>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonDocument>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>
#include <random>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX // windows.h must not define min/max macros over std::min/std::max
#endif
#include <windows.h>
#include <psapi.h>
#endif

using namespace nova::mcp;

namespace nova {
class EditorTestAccess {
  public:
    static void open(Window &window, const QString &file) { window.openTexture(file); }
    static std::shared_ptr<Document> active(Window &window) { return window.current() ? window.current()->document : nullptr; }
    static bool dirty(Window &window, const std::shared_ptr<Document> &doc) { return window.documentDirty(*doc); }
    static QString documentId(Window &window, const std::shared_ptr<Document> &doc) {
        return window.documentIdentifier(doc->instanceId());
    }
    static bool indexReady(Window &window) {
        return !window.indexBusy && !window.indexDebounce->isActive() && window.pendingChanges.isEmpty();
    }
    static double metric(Window &window, const QString &name) {
        double total = 0;
        for (const auto &value : window.metrics.samples.value(name))
            total += value.toDouble();
        return total;
    }
};
} // namespace nova

namespace {
QString bridge() { return QString::fromUtf8(NOVA_MCP_TEST_BRIDGE); }
QByteArray bytes(const QString &file) {
    QFile input(file);
    return input.open(QIODevice::ReadOnly) ? input.readAll() : QByteArray();
}
void write(const QString &file, const QByteArray &data) {
    QVERIFY(QDir().mkpath(QFileInfo(file).path()));
    QFile output(file);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write(data), data.size());
}
QMap<QString, QByteArray> fingerprint(const QString &root) {
    QMap<QString, QByteArray> hashes;
    QDirIterator files(root, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto file = files.next();
        if (!QDir(root).relativeFilePath(file).startsWith(".nova/"))
            hashes.insert(QDir(root).relativeFilePath(file), QCryptographicHash::hash(bytes(file), QCryptographicHash::Sha256));
    }
    return hashes;
}
QImage canvas(int width, int height, QRgb fill) {
    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(QColor::fromRgba(fill));
    return image;
}
// Raw RGBA8888 storage, including RGB under zero alpha (QImage::pixel keeps it for this format).
QRgb at(const QImage &image, int x, int y) { return image.convertToFormat(QImage::Format_RGBA8888).pixel(x, y); }
QString hex(QRgb c) {
    return QString("#%1%2%3%4").arg(qRed(c), 2, 16, QChar('0')).arg(qGreen(c), 2, 16, QChar('0'))
        .arg(qBlue(c), 2, 16, QChar('0')).arg(qAlpha(c), 2, 16, QChar('0'));
}
QImage run(const QString &command, const QJsonObject &args, const QImage &layer, const QImage &selection = {},
           nova::texture::Failure *failure = nullptr) {
    QImage out;
    const auto result = nova::texture::apply(command, args, layer, selection, &out);
    if (failure)
        *failure = result;
    return result.ok() ? out : QImage{};
}
nova::texture::Failure rejected(const QString &command, const QJsonObject &args, QSize size = {4, 4}) {
    nova::texture::Failure failure;
    run(command, args, canvas(size.width(), size.height(), qRgba(1, 2, 3, 255)), {}, &failure);
    return failure;
}
// Pixels that differ between two images of the same size.
int differences(const QImage &a, const QImage &b) {
    const auto x = a.convertToFormat(QImage::Format_RGBA8888), y = b.convertToFormat(QImage::Format_RGBA8888);
    int count = 0;
    for (int j = 0; j < x.height(); ++j)
        for (int i = 0; i < x.width(); ++i)
            count += x.pixel(i, j) != y.pixel(i, j);
    return count;
}

// ---- MCP client helpers (the same framing as an agent through nova-mcp.exe)
QByteArray request(int id, const QString &method, const QJsonObject &params = {}) {
    QJsonObject data{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (id >= 0)
        data["id"] = id;
    return QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n';
}
QByteArray call(int id, const QString &tool, const QJsonObject &args = {}) {
    return request(id, "tools/call", {{"name", tool}, {"arguments", args}});
}
void start(QProcess &process, const QString &code, const QString &program = bridge(), const QStringList &arguments = {}) {
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("NOVA_MCP_CODE", code);
    process.setProcessEnvironment(env);
    process.start(program, arguments);
    QVERIFY(process.waitForStarted());
}
QJsonObject receive(QProcess &process, int timeout = 15000) {
    if (!QTest::qWaitFor([&] { return process.canReadLine() || process.state() == QProcess::NotRunning; }, timeout))
        return {};
    return QJsonDocument::fromJson(process.readLine()).object();
}
QJsonObject payload(const QJsonObject &response) {
    const auto result = response["result"].toObject();
    if (!result["isError"].toBool())
        return result["structuredContent"].toObject();
    if (result.contains("structuredContent"))
        return {{"unexpected", "structured content on an error"}};
    return QJsonDocument::fromJson(result["content"].toArray().first().toObject()["text"].toString().toUtf8()).object();
}
QString code(const QJsonObject &response) { return payload(response)["error"].toObject()["code"].toString(); }
QString pointer(const QJsonObject &response) {
    return payload(response)["error"].toObject()["details"].toObject()["pointer"].toString();
}
QJsonObject initialize(QProcess &process) {
    process.write(request(1, "initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}},
        {"clientInfo", QJsonObject{{"name", "Nova phase 2 tests"}, {"version", "1"}}}}));
    const auto reply = receive(process);
    process.write(request(-1, "notifications/initialized"));
    return reply["result"].toObject();
}
QString authorize(QWidget &window, bool editing) {
    auto dialog = window.findChild<QDialog *>("mcpAccessDialog");
    if (!dialog) {
        window.findChild<QAction *>("mcpAccess")->trigger();
        dialog = window.findChild<QDialog *>("mcpAccessDialog");
    }
    if (editing)
        QTimer::singleShot(0, &window, [] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box && box->button(QMessageBox::Yes))
                box->button(QMessageBox::Yes)->click();
        });
    dialog->findChild<QPushButton *>(editing ? "mcpAuthorizeEditing" : "mcpAuthorize")->click();
    return window.findChild<Service *>()->connectionCode();
}
QJsonObject projectInfo(QProcess &client) {
    QJsonObject info;
    for (int attempt = 0; attempt < 100; ++attempt) {
        client.write(call(90, "nova.project.info"));
        info = payload(receive(client));
        if (!info.contains("error"))
            break;
        QTest::qWait(50);
    }
    return info;
}
QJsonObject parse(const QByteArray &json) { return QJsonDocument::fromJson(json).object(); }
// Round-trips through text so integer and floating-point JSON numbers compare alike.
QJsonObject canonical(const QJsonObject &object) { return parse(QJsonDocument(object).toJson(QJsonDocument::Compact)); }

// ---- Synthetic benchmark textures: opaque pixel-art-like patterns from a fixed seed.
QImage synthetic(int width, int height, int colors, quint32 seed) {
    std::mt19937 random(seed);
    std::vector<QRgb> palette;
    for (int i = 0; i < colors; ++i)
        palette.push_back(qRgba(int(random() % 256), int(random() % 256), int(random() % 256), 255));
    QImage image = canvas(width, height, palette[0]);
    for (int i = 0; i < width * height / 48; ++i) {
        const int w = 1 + int(random() % std::max(1u, quint32(width / 4))), h = 1 + int(random() % std::max(1u, quint32(height / 4)));
        const int x = int(random() % quint32(width)), y = int(random() % quint32(height));
        const QRgb c = palette[random() % palette.size()];
        for (int j = y; j < std::min(height, y + h); ++j)
            for (int k = x; k < std::min(width, x + w); ++k)
                image.setPixel(k, j, c);
    }
    for (int j = 0; j < height; ++j)
        for (int k = 0; k < width; ++k)
            if (random() % 8 == 0)
                image.setPixel(k, j, palette[random() % palette.size()]);
    return image;
}
// put_pixels arguments for rows [top, top + count) of an image (at most 62 colours).
QJsonObject putPixelsArguments(const QImage &image, int top, int count) {
    QList<QRgb> palette;
    QJsonArray rows;
    for (int y = top; y < top + count; ++y) {
        QString row;
        for (int x = 0; x < image.width(); ++x) {
            const QRgb c = image.pixel(x, y);
            if (!palette.contains(c))
                palette << c;
            row += QChar(nova::texture::Alphabet[palette.indexOf(c)]);
        }
        rows.append(row);
    }
    QJsonArray colours;
    for (const auto c : palette)
        colours.append(QString("#%1").arg(c & 0xffffff, 6, 16, QChar('0')));
    return {{"x", 0}, {"y", top}, {"width", image.width()}, {"height", count}, {"palette", colours}, {"rows", rows}};
}
// Greedy rectangle cover of equal colours: the fewest fill_rect calls a careful agent would make.
QList<QPair<QRect, QRgb>> rectangles(const QImage &image) {
    QList<QPair<QRect, QRgb>> result;
    std::vector<char> done(size_t(image.width() * image.height()), 0);
    auto used = [&](int x, int y) -> char & { return done[size_t(y * image.width() + x)]; };
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            if (used(x, y))
                continue;
            const QRgb c = image.pixel(x, y);
            int w = 1;
            while (x + w < image.width() && !used(x + w, y) && image.pixel(x + w, y) == c)
                ++w;
            int h = 1;
            for (bool grow = true; grow && y + h < image.height(); h += grow) {
                for (int i = 0; i < w && grow; ++i)
                    grow = !used(x + i, y + h) && image.pixel(x + i, y + h) == c;
            }
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i)
                    used(x + i, y + j) = 1;
            result.append({QRect(x, y, w, h), c});
        }
    return result;
}
qint64 privateBytes() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
        return qint64(counters.PrivateUsage);
#endif
    return -1;
}
} // namespace

class TexturePixelsTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString root, other;
    const QString plain = "assets/nova/textures/item/plain.png";
    const QString second = "assets/nova/textures/item/second.png";
    QTimer sentinel;
    QStringList unexpectedModals;

    void makePack(const QString &pack) {
        write(pack + "/pack.mcmeta", R"({"pack":{"min_format":[75,0],"max_format":[75,0],"description":"Phase 2"}})");
        for (const auto &file : {plain, second}) {
            QVERIFY(QDir().mkpath(QFileInfo(pack + '/' + file).path()));
            QVERIFY(canvas(16, 16, qRgba(128, 128, 128, 255)).save(pack + '/' + file));
        }
    }
    // One editing connection with the scope and the open document's handles.
    struct Agent {
        QProcess client;
        QJsonObject scope;
        QString asset, revision, layer;
        int id = 100;
        QJsonObject send(const QString &tool, QJsonObject args, int timeout = 15000) {
            for (auto it = scope.begin(); it != scope.end(); ++it)
                args.insert(it.key(), it.value());
            client.write(call(++id, tool, args));
            return receive(client, timeout);
        }
        QJsonObject edit(const QString &tool, QJsonObject args) {
            args["assetId"] = asset;
            args["expectedRevision"] = revision;
            const auto response = send(tool, args);
            if (const auto result = payload(response); result.contains("revision"))
                revision = result["revision"].toString();
            return response;
        }
        void refresh() {
            client.write(call(++id, "nova.edit.capabilities", {{"generation", scope["generation"]}, {"assetId", asset}}));
            const auto document = payload(receive(client))["document"].toObject();
            revision = document["revision"].toString();
            layer = document["activeLayer"].toString();
        }
    };
    void attach(nova::Window &window, Agent &agent, const QString &path, bool editing = true) {
        start(agent.client, authorize(window, editing));
        initialize(agent.client);
        const auto info = projectInfo(agent.client);
        agent.scope = {{"generation", info["generation"]}, {"projectId", info["projectId"]}};
        agent.client.write(call(++agent.id, "nova.drafts.list", agent.scope));
        for (const auto &value : payload(receive(agent.client))["drafts"].toArray())
            if (value.toObject()["path"] == path)
                agent.asset = value.toObject()["assetId"].toString();
        QVERIFY(!agent.asset.isEmpty());
        agent.refresh();
    }

  private slots:
    void initTestCase() { QVERIFY(dir.isValid()); }
    void init() {
        sentinel.setInterval(1500);
        QObject::disconnect(&sentinel, nullptr, nullptr, nullptr);
        QObject::connect(&sentinel, &QTimer::timeout, this, [this] {
            auto modal = QApplication::activeModalWidget();
            if (!modal)
                return;
            auto box = qobject_cast<QMessageBox *>(modal);
            unexpectedModals << modal->objectName() + ": " + modal->windowTitle() + (box ? " · " + box->text() : QString());
            modal->close();
        });
        sentinel.start();
        const auto base = dir.path() + '/' + QTest::currentTestFunction();
        root = base + "/phase2-pack";
        other = base + "/other-pack";
        makePack(root);
        makePack(other);
        qApp->setProperty("settingsFile", base + "/settings/studio.ini");
    }
    void cleanup() {
        sentinel.stop();
        QVERIFY2(unexpectedModals.isEmpty(), qPrintable(unexpectedModals.join(" | ")));
        unexpectedModals.clear();
    }

    void colours() {
        QRgb c = 0;
        bool alpha = true;
        QVERIFY(nova::texture::parseColor("#A0b1C2", &c, &alpha));
        QCOMPARE(c, qRgba(0xa0, 0xb1, 0xc2, 255));
        QVERIFY(!alpha);
        QVERIFY(nova::texture::parseColor("#ff000080", &c, &alpha));
        QCOMPARE(c, qRgba(255, 0, 0, 0x80)); // CSS order: alpha last
        QVERIFY(alpha);
        for (const auto bad : {"ff0000", "#ff00", "#ff00000", "#gg0000", "#ff0000801", ""})
            QVERIFY2(!nova::texture::parseColor(bad, &c), bad);
        QCOMPARE(QString(nova::texture::Alphabet).size(), nova::texture::MaxPalette);
    }

    // Exact replacement (including alpha), '.' keeps a pixel, over composites, and zero alpha is stored as #00000000.
    void putPixels() {
        const QRgb base = qRgba(0x10, 0x20, 0x30, 255);
        const QJsonObject args{{"x", 1}, {"y", 0}, {"width", 3}, {"height", 2},
            {"palette", QJsonArray{"#ff0000", "#00ff0080", "#0000ff00"}}, {"rows", QJsonArray{"01.", "2.0"}}};
        const auto out = run("put_pixels", args, canvas(4, 2, base));
        QVERIFY(!out.isNull());
        const QRgb expected[2][4] = {{base, qRgba(255, 0, 0, 255), qRgba(0, 255, 0, 0x80), base},
                                     {base, 0, base, qRgba(255, 0, 0, 255)}};
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 4; ++x)
                QVERIFY2(at(out, x, y) == expected[y][x], qPrintable(QString("%1,%2 %3").arg(x).arg(y).arg(hex(at(out, x, y)))));

        auto over = args;
        over["mode"] = "over";
        over["palette"] = QJsonArray{"#ff000080", "#00ff00", "#00000000"};
        over["rows"] = QJsonArray{"012", "..."};
        const auto blended = run("put_pixels", over, canvas(4, 2, qRgba(0, 0, 255, 255)));
        QCOMPARE(at(blended, 1, 0), qRgba(128, 0, 127, 255)); // straight-alpha source-over, rounded
        QCOMPARE(at(blended, 1, 0), nova::ops::blend(qRgba(0, 0, 255, 255), qRgba(255, 0, 0, 128), 255));
        QCOMPARE(at(blended, 2, 0), qRgba(0, 255, 0, 255));
        QCOMPARE(at(blended, 3, 0), qRgba(0, 0, 255, 255)); // a transparent colour composites to nothing

        // A complete texture with all 62 indices round-trips exactly.
        QImage target(16, 16, QImage::Format_RGBA8888);
        QJsonArray palette, rows;
        for (int i = 0; i < 62; ++i)
            palette.append(hex(qRgba(i * 4, 255 - i * 4, (i * 37) % 256, 255 - i)));
        std::mt19937 random(7);
        for (int y = 0; y < 16; ++y) {
            QString row;
            for (int x = 0; x < 16; ++x) {
                const int index = int(random() % 62);
                row += QChar(nova::texture::Alphabet[index]);
                target.setPixel(x, y, qRgba(index * 4, 255 - index * 4, (index * 37) % 256, 255 - index));
            }
            rows.append(row);
        }
        const auto full = run("put_pixels", {{"x", 0}, {"y", 0}, {"width", 16}, {"height", 16}, {"palette", palette},
                                             {"rows", rows}}, canvas(16, 16, 0));
        QCOMPARE(full, target);
        // A palette index outside the palette, a bad character, wrong row counts and widths, out-of-canvas regions.
        auto bad = [&](QJsonObject changed, const QString &expectedPointer, const QString &text) {
            auto a = args;
            for (auto it = changed.begin(); it != changed.end(); ++it)
                a[it.key()] = it.value();
            const auto failure = rejected("put_pixels", a);
            QCOMPARE(failure.pointer, expectedPointer);
            QVERIFY2(failure.message.contains(text), qPrintable(failure.message));
            QVERIFY(failure.message.startsWith("Invalid argument at " + expectedPointer + ": ") ||
                    failure.message.startsWith("Missing required argument"));
        };
        bad({{"rows", QJsonArray{"01.", "2.3"}}}, "/rows/1", "column 2 uses palette index 3, but the palette has 3 entries");
        bad({{"rows", QJsonArray{"0!.", "2.0"}}}, "/rows/0", "column 1 is not a palette index");
        bad({{"rows", QJsonArray{"01."}}}, "/rows", "expected 2 rows");
        bad({{"rows", QJsonArray{"01.", "2.00"}}}, "/rows/1", "expected 3 characters");
        bad({{"x", 2}}, "/width", "past the canvas width (4 pixels)");
        bad({{"y", 3}}, "/height", "past the canvas height (4 pixels)");
        bad({{"mode", "blend"}}, "/mode", "expected one of");
        bad({{"palette", QJsonArray{"#ff0000", "red"}}}, "/palette/1", "#RRGGBB or #RRGGBBAA");
        auto missing = args;
        missing.remove("x");
        QCOMPARE(rejected("put_pixels", missing).pointer, "/x");
        QCOMPARE(rejected("put_pixels", missing).message, "Missing required argument /x.");
        // Width and height are bounded before the 64-bit area check; x + width cannot overflow.
        QCOMPARE(rejected("put_pixels", {{"x", 0}, {"y", 0}, {"width", 2147483647}, {"height", 1}, {"palette", QJsonArray{"#000000"}},
                                         {"rows", QJsonArray{"0"}}}).pointer, "/width");
        QCOMPARE(rejected("put_pixels", {{"x", 255}, {"y", 0}, {"width", 256}, {"height", 1}, {"palette", QJsonArray{"#000000"}},
                                         {"rows", QJsonArray{"0"}}}, {256, 256}).pointer, "/width");
    }

    // Exact and tolerance matching, the alpha rules, swaps without chaining, first-match precedence.
    void paletteRemap() {
        const QRgb a = qRgba(0x11, 0x22, 0x33, 255), b = qRgba(0x44, 0x55, 0x66, 255), nearby = qRgba(0x11, 0x22, 0x34, 255);
        const QRgb half = qRgba(0x11, 0x22, 0x33, 0x80), hidden = qRgba(255, 0, 0, 0);
        QImage source(5, 1, QImage::Format_RGBA8888);
        const QRgb pixels[] = {a, b, nearby, half, hidden};
        for (int x = 0; x < 5; ++x)
            source.setPixel(x, 0, pixels[x]);
        QCOMPARE(at(source, 4, 0), hidden); // the fixture really stores RGB under zero alpha
        auto remap = [&](QJsonArray mappings, int tolerance = 0, QJsonObject extra = {}) {
            QJsonObject args{{"mappings", mappings}, {"tolerance", tolerance}};
            for (auto it = extra.begin(); it != extra.end(); ++it)
                args[it.key()] = it.value();
            return run("palette_remap", args, source);
        };
        auto map = [](const char *from, const char *to) { return QJsonObject{{"from", from}, {"to", to}}; };
        auto out = remap({map("#112233", "#445566"), map("#445566", "#112233")});
        QCOMPARE(at(out, 0, 0), b);
        QCOMPARE(at(out, 1, 0), a);                                // swapped, not chained
        QCOMPARE(at(out, 2, 0), nearby);                             // exact by default
        QCOMPARE(at(out, 3, 0), qRgba(0x44, 0x55, 0x66, 0x80));    // #RRGGBB keeps the pixel's alpha
        QCOMPARE(at(out, 4, 0), hidden);                           // invisible pixels never match #RRGGBB
        QCOMPARE(at(remap({map("#112233", "#445566")}, 1), 2, 0), b); // per-channel difference 1 <= tolerance 1
        QCOMPARE(at(remap({map("#112232", "#445566")}, 1), 2, 0), nearby); // difference 2 > 1
        // #RRGGBBAA matches alpha too, and an 8-digit target sets it; zero alpha is stored as #00000000.
        out = remap({map("#11223380", "#00000000")});
        QCOMPARE(at(out, 3, 0), qRgba(0, 0, 0, 0));
        QCOMPARE(at(out, 0, 0), a);
        out = remap({map("#00000000", "#ff00ffff")});              // transparent pixels compare as #00000000
        QCOMPARE(at(out, 4, 0), qRgba(255, 0, 255, 255));
        out = remap({map("#112234", "#000000"), map("#112233", "#ffffff")}, 1);
        QCOMPARE(at(out, 0, 0), qRgba(0, 0, 0, 255));              // the first matching mapping wins
        QCOMPARE(at(remap({map("#112233", "#ffffff")}, 0, {{"region", QJsonObject{{"x", 1}, {"y", 0}, {"width", 4}, {"height", 1}}}}), 0, 0), a);
        QCOMPARE(differences(remap({map("#abcdef", "#ffffff")}), source), 0);
        QCOMPARE(rejected("palette_remap", {{"mappings", QJsonArray{map("#112233", "#000000"), map("#112233", "#ffffff")}}}).pointer,
                 "/mappings/1/from");
        QCOMPARE(rejected("palette_remap", {{"mappings", QJsonArray{map("#00000000", "#ffffff")}}}).pointer, "/mappings/0/to");
        QCOMPARE(rejected("palette_remap", {{"mappings", QJsonArray{map("#112233", "#12")}}}).pointer, "/mappings/0/to");
        QCOMPARE(rejected("palette_remap", {{"mappings", QJsonArray{}}}).pointer, "/mappings");
        QCOMPARE(rejected("palette_remap", {{"mappings", QJsonArray{map("#112233", "#000000")}},
            {"region", QJsonObject{{"x", 0}, {"y", 0}, {"width", 5}, {"height", 1}}}}).pointer, "/region/width");
    }

    // Rec.709 luma, rounded integer interpolation, clamped ends, step mode and the two alpha modes.
    void gradientMap() {
        const QRgb pixels[] = {qRgba(0, 0, 0, 255), qRgba(255, 255, 255, 255), qRgba(128, 128, 128, 255),
                               qRgba(255, 0, 0, 255), qRgba(128, 128, 128, 0x80), qRgba(9, 9, 9, 0)};
        QImage source(6, 1, QImage::Format_RGBA8888);
        for (int x = 0; x < 6; ++x)
            source.setPixel(x, 0, pixels[x]);
        QCOMPARE(nova::ops::luma(pixels[3]), 54); // (255 * 2126 + 5000) / 10000
        auto stop = [](int position, const char *color) { return QJsonObject{{"position", position}, {"color", color}}; };
        auto gradient = [&](QJsonArray stops, QJsonObject extra = {}) {
            QJsonObject args{{"stops", stops}};
            for (auto it = extra.begin(); it != extra.end(); ++it)
                args[it.key()] = it.value();
            return run("gradient_map", args, source);
        };
        auto out = gradient({stop(0, "#000000"), stop(255, "#ffffff")});
        QCOMPARE(at(out, 2, 0), qRgba(128, 128, 128, 255));
        QCOMPARE(at(out, 3, 0), qRgba(54, 54, 54, 255));
        QCOMPARE(at(out, 4, 0), qRgba(128, 128, 128, 0x80)); // alpha preserved
        QCOMPARE(at(out, 5, 0), qRgba(9, 9, 9, 0));          // fully transparent: unchanged, hidden RGB kept
        out = gradient({stop(64, "#ff0000"), stop(192, "#0000ff")});
        QCOMPARE(at(out, 0, 0), qRgba(255, 0, 0, 255)); // below the first stop
        QCOMPARE(at(out, 1, 0), qRgba(0, 0, 255, 255)); // above the last stop
        QCOMPARE(at(out, 2, 0), qRgba(128, 0, 128, 255)); // (2 * 255 * 64 + 128) / 256
        QCOMPARE(at(out, 3, 0), qRgba(255, 0, 0, 255));
        out = gradient({stop(0, "#000000"), stop(54, "#ff0000"), stop(200, "#00ff00")}, {{"interpolation", "step"}});
        QCOMPARE(at(out, 3, 0), qRgba(255, 0, 0, 255)); // exactly on a stop
        QCOMPARE(at(out, 2, 0), qRgba(255, 0, 0, 255)); // 128: last stop at or below
        QCOMPARE(at(out, 1, 0), qRgba(0, 255, 0, 255));
        QCOMPARE(at(out, 0, 0), qRgba(0, 0, 0, 255));
        out = gradient({stop(0, "#000000ff"), stop(255, "#ffffff80")}, {{"alpha", "multiply"}});
        QCOMPARE(at(out, 1, 0), qRgba(255, 255, 255, 0x80));
        QCOMPARE(at(out, 4, 0), qRgba(128, 128, 128, (128 * ((2 * (255 * 127 + 128 * 128) + 255) / 510) + 127) / 255));
        QCOMPARE(at(out, 5, 0), qRgba(9, 9, 9, 0));
        QCOMPARE(rejected("gradient_map", {{"stops", QJsonArray{stop(0, "#000000"), stop(255, "#ffffff80")}}}).pointer,
                 "/stops/1/color");
        QCOMPARE(rejected("gradient_map", {{"stops", QJsonArray{stop(10, "#000000"), stop(10, "#ffffff")}}}).pointer,
                 "/stops/1/position");
        QCOMPARE(rejected("gradient_map", {{"stops", QJsonArray{stop(0, "#000000")}}}).pointer, "/stops");
        QCOMPARE(rejected("gradient_map", {{"stops", QJsonArray{stop(0, "#000000"), stop(9, "#ffffff")}}, {"source", "hsv"}}).pointer,
                 "/source");
    }

    // Nova's eraser rule: alpha reduced by coverage; zero alpha stores #00000000; nothing outside the region.
    void erase() {
        auto source = canvas(4, 2, qRgba(200, 100, 50, 255));
        source.setPixel(0, 1, qRgba(7, 7, 7, 0));
        const QJsonObject region{{"x", 1}, {"y", 0}, {"width", 2}, {"height", 2}};
        auto out = run("erase", {{"region", region}}, source);
        QCOMPARE(at(out, 1, 0), qRgba(0, 0, 0, 0));
        QCOMPARE(at(out, 2, 1), qRgba(0, 0, 0, 0));
        QCOMPARE(at(out, 0, 0), qRgba(200, 100, 50, 255));
        QCOMPARE(at(out, 3, 1), qRgba(200, 100, 50, 255));
        QCOMPARE(at(out, 0, 1), qRgba(7, 7, 7, 0)); // outside the region the stored bytes are untouched
        out = run("erase", {{"region", region}, {"strength", 128}}, source);
        QCOMPARE(at(out, 1, 0), qRgba(200, 100, 50, 127)); // 255 * (255 - 128) / 255; RGB kept
        QCOMPARE(out, nova::ops::paintCoverage(source, nova::ops::rectMask(source.size(), QRect(1, 0, 2, 2)), 0, 128, true, nullptr));
        QCOMPARE(rejected("erase", {}).pointer, "/region");
        QCOMPARE(rejected("erase", {{"region", region}, {"strength", 0}}).pointer, "/strength");
    }

    // Selection coverage: full, none, partial (per-channel mix as Nova's Adjust operations), and an empty mask.
    void selectionClipping() {
        const auto source = canvas(3, 1, qRgba(0, 0, 0, 255));
        QImage mask(3, 1, QImage::Format_Grayscale8);
        mask.scanLine(0)[0] = 255;
        mask.scanLine(0)[1] = 0;
        mask.scanLine(0)[2] = 128;
        const QJsonObject args{{"x", 0}, {"y", 0}, {"width", 3}, {"height", 1}, {"palette", QJsonArray{"#ffffff"}}, {"rows", QJsonArray{"000"}}};
        auto out = run("put_pixels", args, source, mask);
        QCOMPARE(at(out, 0, 0), qRgba(255, 255, 255, 255));
        QCOMPARE(at(out, 1, 0), qRgba(0, 0, 0, 255));
        QCOMPARE(at(out, 2, 0), qRgba(128, 128, 128, 255)); // (255 * 128 + 0 * 127 + 127) / 255
        QCOMPARE(out, nova::ops::applyMask(source, run("put_pixels", args, source), &mask));
        QCOMPARE(differences(run("put_pixels", args, source, nova::ops::emptyMask(source.size())), source), 0);
        out = run("erase", {{"region", QJsonObject{{"x", 0}, {"y", 0}, {"width", 3}, {"height", 1}}}}, source, mask);
        QCOMPARE(at(out, 0, 0), qRgba(0, 0, 0, 0));
        QCOMPARE(at(out, 1, 0), qRgba(0, 0, 0, 255));
        QCOMPARE(qAlpha(at(out, 2, 0)), 127);
        out = run("palette_remap", {{"mappings", QJsonArray{QJsonObject{{"from", "#000000"}, {"to", "#ffffff"}}}}}, source, mask);
        QCOMPARE(at(out, 1, 0), qRgba(0, 0, 0, 255));
        QCOMPARE(at(out, 2, 0), qRgba(128, 128, 128, 255));
    }

    // Native session: layer targeting, hidden layers, the selection, animation frames, undo and redo.
    void layersAndHistory() {
        nova::DocumentSession session;
        auto doc = std::make_shared<nova::Document>(root + '/' + plain, canvas(8, 8, qRgba(128, 128, 128, 255)));
        QVERIFY(session.adopt(doc));
        const auto bottom = doc->activeLayer();
        QVERIFY(session.addLayer(doc, "Top", canvas(8, 8, 0)));
        const auto top = doc->activeLayer();
        QVERIFY(top != bottom);
        auto put = [](const QString &layer = {}) {
            QJsonObject c{{"command", "put_pixels"}, {"x", 2}, {"y", 2}, {"width", 2}, {"height", 1},
                          {"palette", QJsonArray{"#ff0000"}}, {"rows", QJsonArray{"00"}}};
            if (!layer.isEmpty())
                c["layer"] = layer;
            return c;
        };
        const auto bottomBefore = doc->layer(bottom)->pixels;
        QString error;
        QVERIFY2(session.execute(doc, put(), &error), qPrintable(error)); // defaults to the active layer
        QCOMPARE(at(doc->layer(top)->pixels, 2, 2), qRgba(255, 0, 0, 255));
        QCOMPARE(doc->layer(bottom)->pixels, bottomBefore);
        QVERIFY(!session.execute(doc, put(bottom.toString(QUuid::WithoutBraces)), &error));
        QCOMPARE(error, "Select the target layer before drawing"); // like fill_rect, painting needs the active layer
        const auto remap = QJsonObject{{"command", "palette_remap"}, {"layer", bottom.toString(QUuid::WithoutBraces)},
            {"mappings", QJsonArray{QJsonObject{{"from", "#808080"}, {"to", "#000080"}}}}};
        QVERIFY2(session.execute(doc, remap, &error), qPrintable(error)); // adjustments may target any visible layer
        QCOMPARE(at(doc->layer(bottom)->pixels, 0, 0), qRgba(0, 0, 128, 255));
        QCOMPARE(doc->activeLayer(), bottom); // as with Nova's Adjust operations, the edited layer becomes active
        QVERIFY(doc->editLayer({nova::LayerCommand::Visibility, bottom, {}, 0}));
        QVERIFY(!session.execute(doc, remap, &error));
        QCOMPARE(error, "Show the selected layer before editing it");
        QVERIFY(!session.execute(doc, put(QUuid::createUuid().toString(QUuid::WithoutBraces)), &error));
        QCOMPARE(error, "Layer identity is not present");
        QVERIFY(doc->editLayer({nova::LayerCommand::Visibility, bottom, {}, 1}));

        // Selection clips every tool.
        QVERIFY(session.select(doc, nova::ops::rectMask(doc->image.size(), QRect(0, 0, 4, 8)), nova::ops::Combine::Replace, "Select"));
        QVERIFY(session.edit(doc, {nova::LayerCommand::Select, top}));
        QVERIFY2(session.execute(doc, {{"command", "erase"}, {"region", QJsonObject{{"x", 0}, {"y", 0}, {"width", 8}, {"height", 8}}}}, &error), qPrintable(error));
        QCOMPARE(qAlpha(at(doc->layer(top)->pixels, 2, 2)), 0);
        QVERIFY(session.execute(doc, {{"command", "gradient_map"}, {"layer", bottom.toString(QUuid::WithoutBraces)},
            {"stops", QJsonArray{QJsonObject{{"position", 0}, {"color", "#00ff00"}}, QJsonObject{{"position", 255}, {"color", "#00ff00"}}}}}, &error));
        QCOMPARE(at(doc->layer(bottom)->pixels, 3, 0), qRgba(0, 255, 0, 255));
        QCOMPARE(at(doc->layer(bottom)->pixels, 4, 0), qRgba(0, 0, 128, 255));

        // Undo restores the exact prior layers and composite; redo the exact result.
        struct State { QImage composite; QList<QImage> layers; };
        auto state = [&] {
            State s{doc->image.copy(), {}};
            for (const auto &layer : doc->layers())
                s.layers << layer.pixels.copy();
            return s;
        };
        const auto after = state();
        session.dispatch(doc, nova::Command::Undo);
        QCOMPARE(at(doc->layer(bottom)->pixels, 3, 0), qRgba(0, 0, 128, 255));
        session.dispatch(doc, nova::Command::Redo);
        QCOMPARE(state().composite, after.composite);
        QCOMPARE(state().layers, after.layers);

        // Animation: the edit changes the active frame only.
        auto animation = std::make_shared<nova::Document>(root + '/' + second, canvas(4, 4, qRgba(10, 10, 10, 255)));
        QVERIFY(session.adopt(animation));
        QVERIFY2(animation->convertToAnimation(&error), qPrintable(error));
        QVERIFY2(session.frameEdit(animation, nova::FrameCommand::Duplicate, animation->activeFrame(), -1, {}, &error), qPrintable(error));
        const auto frames = animation->animationState().frames;
        QCOMPARE(int(frames.size()), 2);
        QVERIFY(session.frameSelect(animation, frames[0].id));
        const auto otherFrame = animation->frameComposite(frames[1].id);
        QVERIFY(session.execute(animation, {{"command", "put_pixels"}, {"x", 0}, {"y", 0}, {"width", 1}, {"height", 1},
            {"palette", QJsonArray{"#ffffff"}}, {"rows", QJsonArray{"0"}}}, &error));
        QCOMPARE(at(animation->frameComposite(frames[0].id), 0, 0), qRgba(255, 255, 255, 255));
        QCOMPARE(animation->frameComposite(frames[1].id), otherFrame);
    }

    // The MCP transaction path: nothing changes unless the whole call commits; one history step.
    void atomicity() {
        nova::DocumentSession session;
        auto doc = std::make_shared<nova::Document>(root + '/' + plain, canvas(16, 16, qRgba(128, 128, 128, 255)));
        QVERIFY(session.adopt(doc));
        const auto before = doc->image.copy();
        const auto serial = doc->changeSerial();
        const auto cursor = doc->historyCursor();
        auto good = QJsonObject{{"command", "put_pixels"}, {"x", 0}, {"y", 0}, {"width", 16}, {"height", 1},
                                {"palette", QJsonArray{"#ff0000", "#00ff00"}}, {"rows", QJsonArray{"0101010101010101"}}};
        auto badRow = good;
        badRow["rows"] = QJsonArray{"010101010101010X"};
        QString error;
        // A failure in a later command discards the earlier one (the draft is never published).
        QVERIFY(!session.prepareBatch(doc, {good, badRow}, &error, "agent"));
        QVERIFY(error.contains("/rows/0"));
        QCOMPARE(doc->image, before);
        QCOMPARE(doc->changeSerial(), serial);
        // Cancellation during preparation.
        QVERIFY(!session.prepareBatch(doc, {good}, &error, "agent", nova::Document::PixelLimit, [] { return false; }));
        QCOMPARE(doc->image, before);
        // An injected history budget failure.
        doc->setHistoryLimits(64, 2048);
        QVERIFY(!session.prepareBatch(doc, {good}, &error, "agent"));
        QCOMPARE(error, "The transaction exceeds the available history or pixel budget");
        QCOMPARE(doc->image, before);
        doc->setHistoryLimits(16LL * 1024 * 1024, 2048);
        // A document change between preparation and commit rejects the commit.
        auto stale = session.prepareBatch(doc, {good}, &error, "agent");
        QVERIFY(stale);
        QVERIFY(session.paint(doc, doc->activeLayer(), {5, 5}, {5, 5}, qRgba(0, 0, 255, 255), true));
        const auto painted = doc->image.copy();
        QVERIFY(!stale(&error));
        QCOMPARE(doc->image, painted);
        QCOMPARE(at(doc->image, 0, 0), qRgba(128, 128, 128, 255));
        // Success: exactly one owned history step; undo and redo are exact.
        const auto prior = doc->image.copy();
        const auto steps = doc->historyCursor();
        auto commit = session.prepareBatch(doc, {good}, &error, "agent");
        QVERIFY(commit && commit(&error));
        QCOMPARE(doc->historyCursor(), steps + 1);
        QVERIFY(doc->ownsHistoryHead("agent", false));
        const auto result = doc->image.copy();
        QCOMPARE(at(result, 1, 0), qRgba(0, 255, 0, 255));
        session.dispatch(doc, nova::Command::Undo);
        QCOMPARE(doc->image, prior);
        session.dispatch(doc, nova::Command::Redo);
        QCOMPARE(doc->image, result);
        QVERIFY(cursor < doc->historyCursor());
    }

    // The 32 Phase 1 tools are byte-for-byte the same definitions, except MCP Phase 3's intended change to
    // nova.pack.validate (description and output schema; same input schema and annotations); the four new
    // ones match the published schemas.
    void schemas() {
        const auto definitions = toolDefinitions();
        QCOMPARE(definitions.size(), 39);
        const auto phase1 = parse(bytes(QString::fromUtf8(NOVA_PHASE1_TOOLS)));
        QCOMPARE(phase1.size(), 32);
        QMap<QString, QJsonObject> runtime;
        for (const auto &value : definitions)
            runtime.insert(value.toObject()["name"].toString(), canonical(value.toObject()));
        for (auto it = phase1.begin(); it != phase1.end(); ++it) {
            auto now = runtime.value(it.key()), then = canonical(it.value().toObject());
            if (it.key() == "nova.pack.validate") {
                QCOMPARE(now["inputSchema"], then["inputSchema"]);
                QCOMPARE(now["annotations"], then["annotations"]);
                for (auto *definition : {&now, &then}) {
                    definition->remove("description");
                    definition->remove("outputSchema");
                }
            }
            QVERIFY2(now == then, qPrintable(it.key()));
        }
        if (const auto dump = qEnvironmentVariable("NOVA_TEXTURE_SCHEMA_DUMP"); !dump.isEmpty()) {
            // Regenerates the reference blocks: one indented definition per tool, in tools/list order.
            QFile file(dump);
            QVERIFY(file.open(QIODevice::WriteOnly));
            for (const auto &value : definitions)
                if (!compactTextureCommand(value.toObject()["name"].toString()).isEmpty())
                    file.write(QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented) + "\n");
        }
        // The published reference (docs/release/MCP_TEXTURE_TOOLS.md) embeds each schema as a JSON block.
        const auto doc = QString::fromUtf8(bytes(QString::fromUtf8(NOVA_TEXTURE_TOOLS_DOC)));
        const QRegularExpression block("```json\\s*\\n(\\{.*?\\n)```", QRegularExpression::DotMatchesEverythingOption);
        QStringList documented;
        for (auto it = block.globalMatch(doc); it.hasNext();) {
            const auto published = parse(it.next().captured(1).toUtf8());
            if (!published.contains("inputSchema"))
                continue; // a request or result example
            const auto name = published["name"].toString();
            documented << name;
            QVERIFY2(runtime.contains(name) && runtime[name] == canonical(published), qPrintable(name));
        }
        QCOMPARE(documented, (QStringList{"nova.texture.put_pixels", "nova.texture.palette_remap",
                                          "nova.texture.gradient_map", "nova.texture.erase"}));
        for (const auto &name : documented) {
            QVERIFY(runtime.value(name)["inputSchema"].toObject().contains("properties"));
            QCOMPARE(compactTextureCommand(name), name.section('.', 2));
        }
        QVERIFY(compactTextureCommand("nova.texture.edit").isEmpty());
        QVERIFY(compactTextureCommand("nova.texture.inspect").isEmpty());

        // Schema-level argument errors name the field.
        Protocol protocol;
        protocol.receive(request(1, "initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "t"}, {"version", "1"}}}}));
        protocol.receive(request(-1, "notifications/initialized"));
        const auto token = QString(64, 'a');
        const QJsonObject target{{"generation", token}, {"projectId", token}, {"assetId", token}, {"expectedRevision", token}};
        auto argumentError = [&](const QString &tool, QJsonObject args) {
            for (auto it = target.begin(); it != target.end(); ++it)
                args.insert(it.key(), it.value());
            const auto response = protocol.receive(call(2, tool, args)).response;
            return std::make_pair(code(response), pointer(response));
        };
        const QJsonObject put{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}, {"palette", QJsonArray{"#000000"}}, {"rows", QJsonArray{"0"}}};
        QCOMPARE(argumentError("nova.texture.put_pixels", put).first, ""); // valid: passed on to the editor
        auto with = [](QJsonObject base, const QString &key, const QJsonValue &value) { base[key] = value; return base; };
        const QList<std::tuple<QString, QJsonObject, QString>> cases{
            {"nova.texture.put_pixels", with(put, "palette", QJsonArray{"#00000"}), "/palette/0"},
            {"nova.texture.put_pixels", with(put, "palette", QJsonArray{}), "/palette"},
            {"nova.texture.put_pixels", with(put, "rows", QJsonArray{"0 1"}), "/rows/0"},
            {"nova.texture.put_pixels", with(put, "rows", QJsonArray{QString(257, '0')}), "/rows/0"},
            {"nova.texture.put_pixels", with(put, "width", 257), "/width"},
            {"nova.texture.put_pixels", with(put, "x", -1), "/x"},
            {"nova.texture.put_pixels", with(put, "x", 1.5), "/x"},
            {"nova.texture.put_pixels", with(put, "layer", "not-a-layer"), "/layer"},
            {"nova.texture.put_pixels", with(put, "opacity", 255), "/opacity"},
            {"nova.texture.palette_remap", {{"mappings", QJsonArray{QJsonObject{{"from", "#000000"}}}}}, "/mappings/0/to"},
            {"nova.texture.palette_remap", {{"mappings", QJsonArray{QJsonObject{{"from", "#000000"}, {"to", "#fff"}}}}}, "/mappings/0/to"},
            {"nova.texture.palette_remap", {{"mappings", QJsonArray{QJsonObject{{"from", "#000000"}, {"to", "#ffffff"}}}}, {"tolerance", 256}}, "/tolerance"},
            {"nova.texture.gradient_map", {{"stops", QJsonArray{QJsonObject{{"position", 0}, {"color", "#000000"}}}}}, "/stops"},
            {"nova.texture.gradient_map", {{"stops", QJsonArray{QJsonObject{{"position", 0}, {"color", "#000000"}},
                QJsonObject{{"position", 300}, {"color", "#ffffff"}}}}}, "/stops/1/position"},
            {"nova.texture.erase", {}, "/region"},
            {"nova.texture.erase", {{"region", QJsonObject{{"x", 0}, {"y", 0}, {"width", 1}}}}, "/region/height"},
            {"nova.texture.erase", {{"region", QJsonObject{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}, {"z", 1}}}}, "/region/z"}};
        for (const auto &[tool, args, expected] : cases) {
            const auto [failure, where] = argumentError(tool, args);
            QVERIFY2(failure == "invalid_arguments" && where == expected,
                     qPrintable(tool + ' ' + QJsonDocument(args).toJson(QJsonDocument::Compact) + " -> " + failure + ' ' + where));
        }
        // The framing limit bounds every payload before it is parsed.
        QJsonArray rows;
        for (int i = 0; i < 256; ++i)
            rows.append(QString(256, '0'));
        const auto oversized = call(3, "nova.texture.put_pixels", with(with(with(put, "rows", rows), "width", 256), "height", 256));
        QVERIFY(oversized.size() > RequestLimit);
        QCOMPARE(protocol.receive(oversized).response["error"].toObject()["code"].toInt(), -32600);
    }

    // End to end through nova-mcp.exe with a real window: results, drafts, validation, history, errors, saving.
    void endToEnd() {
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        nova::EditorTestAccess::open(window, root + '/' + second);
        auto secondDoc = nova::EditorTestAccess::active(window);
        nova::EditorTestAccess::open(window, root + '/' + plain);
        auto doc = nova::EditorTestAccess::active(window);
        QVERIFY(doc && secondDoc && doc != secondDoc);
        const auto disk = fingerprint(root);
        auto service = [&] { return window.findChild<Service *>(); };

        // Read-only access cannot use the tools, and nothing changes.
        {
            Agent reader;
            attach(window, reader, plain, false);
            const auto refused = reader.edit("nova.texture.put_pixels", {{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1},
                                                                         {"palette", QJsonArray{"#ff0000"}}, {"rows", QJsonArray{"0"}}});
            QCOMPARE(code(refused), "permission_denied");
            for (const auto tool : {"nova.texture.palette_remap", "nova.texture.gradient_map", "nova.texture.erase"})
                QCOMPARE(code(reader.edit(tool, {})), "invalid_arguments"); // arguments are checked first
            QCOMPARE(code(reader.edit("nova.texture.erase", {{"region", QJsonObject{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}})),
                     "permission_denied");
            reader.client.closeWriteChannel();
            QTRY_COMPARE(reader.client.state(), QProcess::NotRunning);
        }
        QVERIFY(!nova::EditorTestAccess::dirty(window, doc));
        QTRY_COMPARE(service()->status(), "Disabled");

        Agent agent;
        attach(window, agent, plain);
        const auto original = doc->image.copy();
        const QJsonObject icon{{"x", 2}, {"y", 3}, {"width", 4}, {"height", 2}, {"palette", QJsonArray{"#ff0000", "#0000ff80"}},
                               {"rows", QJsonArray{"0..1", "1001"}}};
        auto response = agent.edit("nova.texture.put_pixels", icon);
        auto result = payload(response);
        QVERIFY2(!result.contains("error"), QJsonDocument(result).toJson().constData());
        QCOMPARE(result["documentId"].toString(), nova::EditorTestAccess::documentId(window, doc));
        QCOMPARE(result["path"].toString(), plain);
        QCOMPARE(result["assetId"].toString(), agent.asset);
        QCOMPARE(result["layer"].toString(), agent.layer);
        QCOMPARE(result["region"].toObject(), (QJsonObject{{"x", 2}, {"y", 3}, {"width", 4}, {"height", 2}}));
        QCOMPARE(result["changedBounds"].toObject(), (QJsonObject{{"x", 2}, {"y", 3}, {"width", 4}, {"height", 2}}));
        QCOMPARE(result["pixelsChanged"].toInt(), 6);
        QVERIFY(result["changed"].toBool() && result["dirty"].toBool() && result["undoAvailable"].toBool());
        QCOMPARE(result["appliedOperations"].toInt(), 1);
        QVERIFY(!result.contains("saved"));
        const auto text = QJsonDocument(result).toJson();
        QVERIFY2(!text.contains(QDir::toNativeSeparators(dir.path()).toUtf8()) && !text.contains(dir.path().toUtf8()), text.constData());
        QCOMPARE(at(doc->image, 2, 3), qRgba(255, 0, 0, 255));
        QCOMPARE(at(doc->image, 3, 3), qRgba(128, 128, 128, 255));
        QCOMPARE(at(doc->image, 5, 3), qRgba(0, 0, 255, 0x80));
        QCOMPARE(at(doc->image, 3, 4), qRgba(255, 0, 0, 255));
        QCOMPARE(fingerprint(root), disk); // an edit is not a save
        const auto edited = doc->image.copy();
        // The draft list and validation see the unsaved edit.
        agent.client.write(call(++agent.id, "nova.drafts.list", agent.scope));
        QJsonObject row;
        for (const auto &value : payload(receive(agent.client))["drafts"].toArray())
            if (value.toObject()["path"] == plain)
                row = value.toObject();
        QVERIFY(row["dirty"].toBool() && !row["packCurrent"].toBool());
        QCOMPARE(row["revision"].toString(), agent.revision);
        QCOMPARE(row["documentId"], result["documentId"]);
        agent.client.write(call(++agent.id, "nova.pack.validate", {{"generation", agent.scope["generation"]}}));
        const auto validation = payload(receive(agent.client, 60000));
        QVERIFY(validation["drafts"].toObject()["complete"].toBool());
        QCOMPARE(validation["drafts"].toObject()["included"].toArray().first().toObject()["path"].toString(), plain);

        // Undo and redo restore the exact images.
        QVERIFY(!payload(agent.edit("nova.edit.undo", {})).contains("error"));
        QCOMPARE(doc->image, original);
        QVERIFY(!payload(agent.edit("nova.edit.redo", {})).contains("error"));
        QCOMPARE(doc->image, edited);

        // Errors change nothing: argument pointers, stale revisions, the wrong project or generation.
        auto unchanged = [&](const QJsonObject &reply, const QString &expectedCode, const QString &expectedPointer = {}) {
            QCOMPARE(code(reply), expectedCode);
            if (!expectedPointer.isEmpty())
                QCOMPARE(pointer(reply), expectedPointer);
            QCOMPARE(doc->image, edited);
        };
        auto broken = icon;
        broken["rows"] = QJsonArray{"0..1", "100z"};
        unchanged(agent.edit("nova.texture.put_pixels", broken), "invalid_arguments", "/rows/1");
        broken = icon;
        broken["x"] = 13;
        unchanged(agent.edit("nova.texture.put_pixels", broken), "invalid_arguments", "/width");
        broken = icon;
        broken["layer"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        unchanged(agent.edit("nova.texture.put_pixels", broken), "invalid_arguments", "/layer");
        const auto revision = agent.revision;
        agent.revision = randomToken();
        unchanged(agent.edit("nova.texture.put_pixels", icon), "revision_conflict");
        agent.revision = revision;
        auto scope = agent.scope;
        agent.scope["projectId"] = randomToken();
        unchanged(agent.edit("nova.texture.erase", {{"region", QJsonObject{{"x", 0}, {"y", 0}, {"width", 16}, {"height", 16}}}}),
                  "stale_project");
        agent.scope = scope;
        agent.scope["generation"] = randomToken();
        unchanged(agent.edit("nova.texture.erase", {{"region", QJsonObject{{"x", 0}, {"y", 0}, {"width", 16}, {"height", 16}}}}),
                  "stale_project");
        agent.scope = scope;
        // Injected failure: the history budget rejects the transaction before anything is published.
        doc->setHistoryLimits(64, 2048);
        unchanged(agent.edit("nova.texture.gradient_map", {{"stops", QJsonArray{QJsonObject{{"position", 0}, {"color", "#000000"}},
                                                           QJsonObject{{"position", 255}, {"color", "#ffffff"}}}}}), "edit_rejected");
        doc->setHistoryLimits(16LL * 1024 * 1024, 2048);
        // Cancellation before publication.
        bool canceled = false;
        for (int attempt = 0; attempt < 5 && !canceled; ++attempt) {
            QJsonObject args{{"assetId", agent.asset}, {"expectedRevision", agent.revision}, {"region",
                QJsonObject{{"x", 0}, {"y", 0}, {"width", 16}, {"height", 16}}}};
            for (auto it = agent.scope.begin(); it != agent.scope.end(); ++it)
                args.insert(it.key(), it.value());
            const int id = ++agent.id;
            agent.client.write(call(id, "nova.texture.erase", args) +
                               request(-1, "notifications/cancelled", {{"requestId", id}}));
            const auto reply = receive(agent.client);
            if (code(reply) == "operation_canceled") {
                canceled = true;
                QCOMPARE(doc->image, edited);
            } else {
                QVERIFY2(!payload(reply).contains("error"), QJsonDocument(reply).toJson().constData());
                agent.revision = payload(reply)["revision"].toString();
                QVERIFY(!payload(agent.edit("nova.edit.undo", {})).contains("error"));
                QCOMPARE(doc->image, edited);
            }
        }
        QVERIFY(canceled);
        // An identifier of no open texture is refused.
        QCOMPARE(code(agent.send("nova.texture.erase", {{"assetId", QString(64, '0')}, {"expectedRevision", agent.revision},
            {"region", QJsonObject{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}})), "document_not_open");

        // The other tools, each one history step, exact.
        result = payload(agent.edit("nova.texture.palette_remap", {{"mappings", QJsonArray{
            QJsonObject{{"from", "#808080"}, {"to", "#204060"}}, QJsonObject{{"from", "#ff0000"}, {"to", "#00ff00"}}}}}));
        QCOMPARE(result["pixelsChanged"].toInt(), 16 * 16 - 3); // the three #0000ff80 pixels match no mapping
        QCOMPARE(at(doc->image, 2, 3), qRgba(0, 255, 0, 255));
        QCOMPARE(at(doc->image, 5, 3), qRgba(0, 0, 255, 0x80));
        QCOMPARE(at(doc->image, 0, 0), qRgba(0x20, 0x40, 0x60, 255));
        result = payload(agent.edit("nova.texture.gradient_map", {{"stops", QJsonArray{QJsonObject{{"position", 0}, {"color", "#000000"}},
            QJsonObject{{"position", 255}, {"color", "#ffffff"}}}}, {"region", QJsonObject{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}}));
        const int luma = nova::ops::luma(qRgba(0x20, 0x40, 0x60, 255));
        QCOMPARE(at(doc->image, 0, 0), qRgba(luma, luma, luma, 255));
        QCOMPARE(result["changedBounds"].toObject(), (QJsonObject{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}));
        result = payload(agent.edit("nova.texture.erase", {{"region", QJsonObject{{"x", 15}, {"y", 15}, {"width", 1}, {"height", 1}}}}));
        QCOMPARE(at(doc->image, 15, 15), qRgba(0, 0, 0, 0));
        QCOMPARE(result["pixelsChanged"].toInt(), 1);
        // An edit with no matching pixels succeeds without a history step.
        const auto cursor = doc->historyCursor();
        result = payload(agent.edit("nova.texture.palette_remap", {{"mappings", QJsonArray{QJsonObject{{"from", "#123456"}, {"to", "#000000"}}}}}));
        QVERIFY(!result["changed"].toBool());
        QVERIFY(result["changedBounds"].isNull());
        QCOMPARE(result["pixelsChanged"].toInt(), 0);
        QCOMPARE(doc->historyCursor(), cursor);
        // fill_rect is unchanged alongside.
        result = payload(agent.edit("nova.texture.edit", {{"operation", QJsonObject{{"command", "fill_rect"}, {"layer", agent.layer},
            {"x", 8}, {"y", 8}, {"width", 2}, {"height", 2}, {"color", "#ffff00"}, {"opacity", 255}}}}));
        QVERIFY(result["changed"].toBool() && !result.contains("pixelsChanged") && !result.contains("documentId"));
        QCOMPARE(at(doc->image, 9, 9), qRgba(255, 255, 0, 255));

        // Saving through the Phase 1 mechanism writes exactly the edited image.
        auto save = [&](const QString &documentId) {
            return agent.send("nova.drafts.save", {{"documentId", documentId}, {"expectedRevision", agent.revision}});
        };
        QCOMPARE(payload(save(row["documentId"].toString()))["status"].toString(), "saved");
        QCOMPARE(QImage(root + '/' + plain).convertToFormat(QImage::Format_RGBA8888), doc->image.convertToFormat(QImage::Format_RGBA8888));
        QVERIFY(!nova::EditorTestAccess::dirty(window, doc));
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        // A failed save keeps the draft and reports the failure.
        agent.refresh();
        QVERIFY(!payload(agent.edit("nova.texture.put_pixels", icon)).contains("error"));
        const auto draft = doc->image.copy();
        const auto external = canvas(16, 16, qRgba(1, 2, 3, 255));
        QVERIFY(external.save(root + '/' + plain));
        const auto externalBytes = bytes(root + '/' + plain);
        QTest::qWait(100);
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        const auto refused = payload(save(row["documentId"].toString()))["error"].toObject();
        QCOMPARE(refused["code"].toString(), "external_change");
        QVERIFY(refused["details"].toObject()["failed"].toArray().first().toObject()["draftPreserved"].toBool());
        QCOMPARE(bytes(root + '/' + plain), externalBytes);
        QCOMPARE(doc->image, draft);
        QVERIFY(nova::EditorTestAccess::dirty(window, doc));

        // Revocation closes the connection; a used code cannot reconnect; nothing more changes.
        const auto beforeRevoke = doc->image.copy();
        const auto usedCode = service()->connectionCode();
        window.findChild<QDialog *>("mcpAccessDialog")->findChild<QPushButton *>("mcpRevoke")->click();
        QTRY_COMPARE(agent.client.state(), QProcess::NotRunning);
        {
            QProcess expired;
            start(expired, usedCode);
            expired.write(request(1, "initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}},
                {"clientInfo", QJsonObject{{"name", "expired"}, {"version", "1"}}}}));
            const auto reply = receive(expired);
            QVERIFY(!reply.contains("result"));
            expired.write(call(2, "nova.texture.erase", {{"generation", agent.scope["generation"]}}));
            QTRY_COMPARE(expired.state(), QProcess::NotRunning);
        }
        QCOMPARE(doc->image, beforeRevoke);

        // Project switch: a stale connection cannot edit either project.
        QTRY_COMPARE(service()->status(), "Disabled");
        Agent switched;
        attach(window, switched, plain);
        // The refused draft is still unsaved, so switching asks first; answer as a user would: discard.
        QTimer discard;
        discard.setInterval(50);
        QObject::connect(&discard, &QTimer::timeout, &window, [] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || !box->text().startsWith("Save changes"))
                return;
            for (auto button : box->buttons())
                if (box->buttonRole(button) == QMessageBox::DestructiveRole)
                    return button->click();
        });
        discard.start();
        auto path = window.findChild<QLineEdit *>("projectPath");
        path->setText(other);
        QTest::keyClick(path, Qt::Key_Return);
        QTRY_COMPARE(service()->status(), "Project changed");
        discard.stop();
        const auto otherBefore = fingerprint(other);
        QCOMPARE(code(switched.edit("nova.texture.erase", {{"region", QJsonObject{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}})),
                 "stale_project");
        QCOMPARE(fingerprint(other), otherBefore);
        switched.client.closeWriteChannel();
        QTRY_COMPARE(switched.client.state(), QProcess::NotRunning);
    }

    // Payload size, call count and wall time against fill_rect workflows on the same synthetic images.
    // Full measurements (repetitions and all sizes) run only with NOVA_TEXTURE_BENCH set to an output folder.
    void efficiency() {
        const auto output = qEnvironmentVariable("NOVA_TEXTURE_BENCH");
        const int runs = qEnvironmentVariableIntValue("NOVA_TEXTURE_BENCH_RUNS");
        const int repetitions = output.isEmpty() ? 1 : runs > 0 ? runs : 5;
        struct Case { QString name; QSize size; int colors; quint32 seed; bool single; };
        QList<Case> cases{{"icon-16x16", {16, 16}, 6, 1601, true}};
        if (!output.isEmpty())
            cases << Case{"armor-64x32", {64, 32}, 8, 6432, true} << Case{"large-128x128", {128, 128}, 16, 12828, true}
                  << Case{"max-256x256", {256, 256}, 16, 25656, !qEnvironmentVariableIsSet("NOVA_TEXTURE_BENCH_SKIP_SINGLE_256")};
        const QStringList workflows{"put_pixels", "fill_rect_batch8", "fill_rect_single"};
        auto relativePath = [](const Case &c, const QString &w, int r) {
            return QString("assets/nova/textures/bench/%1-%2-%3.png").arg(c.name, w).arg(r);
        };
        for (const auto &c : cases)
            for (const auto &w : workflows)
                for (int r = 0; r < repetitions; ++r) {
                    const auto file = root + '/' + relativePath(c, w, r);
                    QVERIFY(QDir().mkpath(QFileInfo(file).path()));
                    QVERIFY(canvas(c.size.width(), c.size.height(), qRgba(128, 128, 128, 255)).save(file));
                }
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        QMap<QString, std::shared_ptr<nova::Document>> docs;
        for (const auto &c : cases)
            for (const auto &w : workflows)
                for (int r = 0; r < repetitions; ++r) {
                    nova::EditorTestAccess::open(window, root + '/' + relativePath(c, w, r));
                    docs.insert(relativePath(c, w, r), nova::EditorTestAccess::active(window));
                }
        Agent agent;
        attach(window, agent, docs.firstKey());
        agent.client.write(call(++agent.id, "nova.drafts.list", agent.scope));
        QMap<QString, QString> assets;
        for (const auto &value : payload(receive(agent.client))["drafts"].toArray())
            assets.insert(value.toObject()["path"].toString(), value.toObject()["assetId"].toString());
        const auto memoryBefore = privateBytes();
        QJsonArray samples;
        for (const auto &c : cases) {
            const auto target = synthetic(c.size.width(), c.size.height(), c.colors, c.seed);
            const auto cover = rectangles(target);
            for (const auto &w : workflows) {
                if (w == "fill_rect_single" && !c.single)
                    continue;
                for (int r = 0; r < repetitions; ++r) {
                    const auto relative = relativePath(c, w, r);
                    agent.asset = assets.value(relative);
                    QVERIFY(!agent.asset.isEmpty());
                    agent.refresh();
                    QList<QPair<QString, QJsonObject>> requests;
                    if (w == "put_pixels") {
                        // The 64 KiB request limit allows 128 rows of 256 pixels per call.
                        const int band = c.size.width() > 128 ? 128 : c.size.height();
                        for (int top = 0; top < c.size.height(); top += band)
                            requests.append({"nova.texture.put_pixels", putPixelsArguments(target, top, std::min(band, c.size.height() - top))});
                    } else {
                        QJsonArray operations;
                        for (const auto &[rect, color] : cover) {
                            const QJsonObject op{{"command", "fill_rect"}, {"layer", agent.layer}, {"x", rect.x()}, {"y", rect.y()},
                                {"width", rect.width()}, {"height", rect.height()},
                                {"color", QString("#%1").arg(color & 0xffffff, 6, 16, QChar('0'))}, {"opacity", 255}};
                            if (w == "fill_rect_single") {
                                requests.append({"nova.texture.edit", {{"operation", op}}});
                                continue;
                            }
                            operations.append(op);
                            if (operations.size() == 8) {
                                requests.append({"nova.edit.batch", {{"operations", operations}}});
                                operations = {};
                            }
                        }
                        if (!operations.isEmpty())
                            requests.append({"nova.edit.batch", {{"operations", operations}}});
                    }
                    qint64 sent = 0, received = 0, argumentBytes = 0;
                    const double prepareBefore = nova::EditorTestAccess::metric(window, "mcp_texture_prepare");
                    const double publishBefore = nova::EditorTestAccess::metric(window, "mcp_texture_native_publication");
                    QElapsedTimer wall;
                    wall.start();
                    for (const auto &[tool, arguments] : requests) {
                        auto args = arguments;
                        argumentBytes += QJsonDocument(args).toJson(QJsonDocument::Compact).size();
                        args["assetId"] = agent.asset;
                        args["expectedRevision"] = agent.revision;
                        for (auto it = agent.scope.begin(); it != agent.scope.end(); ++it)
                            args.insert(it.key(), it.value());
                        const auto line = call(++agent.id, tool, args);
                        sent += line.size();
                        agent.client.write(line);
                        if (!QTest::qWaitFor([&] { return agent.client.canReadLine(); }, 30000))
                            QFAIL("no reply");
                        const auto reply = agent.client.readLine();
                        received += reply.size();
                        const auto result = payload(QJsonDocument::fromJson(reply).object());
                        QVERIFY2(!result.contains("error"), reply.left(400).constData());
                        agent.revision = result["revision"].toString();
                    }
                    const double wallMs = wall.nsecsElapsed() / 1e6;
                    const auto doc = docs.value(relative);
                    const auto final = doc->image.convertToFormat(QImage::Format_RGBA8888);
                    const bool equal = final == target;
                    QVERIFY2(equal, qPrintable(relative));
                    samples.append(QJsonObject{{"case", c.name}, {"width", c.size.width()}, {"height", c.size.height()},
                        {"colors", c.colors}, {"workflow", w}, {"repetition", r}, {"calls", requests.size()},
                        {"fillRects", w == "put_pixels" ? 0 : cover.size()}, {"requestBytes", sent},
                        {"argumentBytes", argumentBytes}, {"responseBytes", received}, {"wallMs", wallMs},
                        {"novaPrepareMs", nova::EditorTestAccess::metric(window, "mcp_texture_prepare") - prepareBefore},
                        {"novaPublishMs", nova::EditorTestAccess::metric(window, "mcp_texture_native_publication") - publishBefore},
                        {"imageEqual", equal}, {"imageSha256", QString::fromLatin1(QCryptographicHash::hash(
                            QByteArray(reinterpret_cast<const char *>(final.constBits()), final.sizeInBytes()),
                            QCryptographicHash::Sha256).toHex())}});
                    qInfo().noquote() << QJsonDocument(samples.last().toObject()).toJson(QJsonDocument::Compact);
                }
            }
        }
        const auto memoryAfter = privateBytes();
        if (output.isEmpty())
            return;
        // The only work this phase adds to an ordinary edit (fill_rect, strokes, layers) is these name
        // checks: once per request in the service and the editor, once per command in the session.
        QElapsedTimer overhead;
        int matches = 0;
        overhead.start();
        for (int i = 0; i < 1000000; ++i)
            matches += !compactTextureCommand(i % 2 ? "nova.texture.edit" : "nova.edit.batch").isEmpty() +
                       nova::texture::compactCommand(i % 2 ? "fill_rect" : "stroke");
        const double checkNs = overhead.nsecsElapsed() / 1e6;
        QCOMPARE(matches, 0);
        QJsonObject environment{{"qt", qVersion()}, {"compiler",
#if defined(_MSC_VER)
            QString("MSVC %1").arg(_MSC_FULL_VER)
#elif defined(__GNUC__)
            QString("GCC %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__)
#else
            QString("unknown")
#endif
            }, {"buildType",
#ifdef NDEBUG
            "Release"
#else
            "Debug"
#endif
            }, {"cpuThreads", QThread::idealThreadCount()}, {"os", QSysInfo::prettyProductName()},
            {"privateBytesBefore", memoryBefore}, {"privateBytesAfter", memoryAfter},
            {"historyBudgetBytesPerDocument", 16 * 1024 * 1024}, {"repetitions", repetitions},
            {"addedNameCheckPairNs", checkNs},
            {"note", "Request bytes are a proxy for model-token usage only; no model was involved."}};
        QFile file(output + "/texture-efficiency-samples.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"environment", environment}, {"samples", samples}}).toJson());
    }

    // The official MCP Python SDK: listing, output schema validation of every result, error shape, saving.
    void officialSdk() {
        const auto python = qEnvironmentVariable("NOVA_MCP_SDK_PYTHON");
        if (python.isEmpty())
            QSKIP("NOVA_MCP_SDK_PYTHON is not set");
        QElapsedTimer timer;
        timer.start();
        nova::Window window(root, {}, timer);
        window.show();
        QTRY_VERIFY(nova::EditorTestAccess::indexReady(window));
        nova::EditorTestAccess::open(window, root + '/' + plain);
        auto doc = nova::EditorTestAccess::active(window);
        QProcess client;
        start(client, authorize(window, true), python, {QString::fromUtf8(NOVA_MCP_TEST_SDK), bridge(), "phase2"});
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QProcess::NotRunning, 60000);
        const auto output = client.readAllStandardOutput();
        qInfo().noquote() << "SDK phase2" << output;
        QVERIFY2(client.exitCode() == 0, client.readAllStandardError().constData());
        const auto summary = QJsonDocument::fromJson(output.trimmed().split('\n').last()).object();
        QCOMPARE(summary["saved"].toString(), plain);
        QCOMPARE(summary["tools"].toInt(), 39);
        QCOMPARE(QImage(root + '/' + plain).convertToFormat(QImage::Format_RGBA8888), doc->image.convertToFormat(QImage::Format_RGBA8888));
        QCOMPARE(at(doc->image, 0, 0), qRgba(0, 0, 0, 0));
        QVERIFY(!nova::EditorTestAccess::dirty(window, doc));
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    TexturePixelsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "texture_pixels_test.moc"
