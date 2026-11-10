// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"
#include "tools.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QSet>
#include <QtTest>
#include <iterator>

namespace {
struct IconCase {
    const char *name;
    char16_t codepoint;
};
// Generated from the pinned upstream .codepoints file after checking every cmap entry.
constexpr IconCase icons[] = {
    {"save", 0xe161}, {"undo", 0xe166}, {"redo", 0xe15a}, {"open", 0xe2c8},
    {"folder", 0xe2c7}, {"search", 0xef7a}, {"command", 0xef7a}, {"close", 0xe5cd},
    {"add", 0xe145}, {"duplicate", 0xe14d}, {"delete", 0xe92e}, {"rename", 0xf097},
    {"up", 0xe5d8}, {"down", 0xe5db}, {"parent", 0xe5d9}, {"tree", 0xe97a},
    {"grid", 0xe9b0}, {"sort", 0xe164}, {"zoom-in", 0xe8ff}, {"zoom-out", 0xe900},
    {"fit", 0xea10}, {"actual", 0xe693}, {"center", 0xe3b4}, {"settings", 0xe8b8},
    {"activity", 0xe7f5}, {"workspace", 0xe871}, {"keyboard", 0xe312}, {"transform", 0xe428},
    {"eye", 0xe8f4}, {"pencil", 0xf604}, {"brush", 0xe3ae}, {"eraser", 0xe6d0},
    {"replace", 0xf20a}, {"rect-select", 0xf74d}, {"ellipse-select", 0xef4a}, {"lasso", 0xeb03},
    {"polygon-lasso", 0xebbb}, {"magic-wand", 0xf31e}, {"color-range", 0xe40a}, {"move", 0xe89f},
    {"line", 0xf41e}, {"shape", 0xe602}, {"fill", 0xe23a}, {"gradient", 0xe3e9},
    {"eyedropper", 0xe3b8},
};
static_assert(std::size(icons) == 45);

int inkPixels(const QImage &image) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            count += qAlpha(image.pixel(x, y)) != 0;
    return count;
}

bool hasOpaqueInk(const QImage &image, const QColor &color) {
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qAlpha(pixel) == 255 && QColor::fromRgb(pixel).rgb() == color.rgb())
                return true;
        }
    return false;
}
}

class MaterialIconsTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase();
    void mapsValidatedGlyphs();
    void iconPixels_data();
    void iconPixels();
    void dprMetadataDoesNotCollide();
    void toolIconsUseSharedMapping();
    void themedNormalAndDisabledInk();
    void unknownNamesFailVisibly();
};

void MaterialIconsTest::initTestCase() {
    QFile fontFile(QStringLiteral(":/fonts/MaterialSymbolsOutlined.ttf"));
    QVERIFY(fontFile.open(QIODevice::ReadOnly));
    QCOMPARE(fontFile.size(), 10'744'832);
    QCOMPARE(QCryptographicHash::hash(fontFile.readAll(), QCryptographicHash::Sha256).toHex(),
             QByteArray("f7837ba8573d9a7a74931618ced8307bc81f96d49af296bf6c8052c772ed5282"));
    QVERIFY(!nova::ui::lineIcon(QStringLiteral("save")).isNull());
    QVERIFY(QFontDatabase::families().contains(QStringLiteral("Material Symbols Outlined")));
    QSet<uint> unique;
    for (const auto &entry : icons)
        unique.insert(entry.codepoint);
    QCOMPARE(unique.size(), 44);
}

void MaterialIconsTest::mapsValidatedGlyphs() {
    const QColor ink("#dee1eb"), disabled("#7c8291");
    QFont font(QStringLiteral("Material Symbols Outlined"));
    font.setWeight(QFont::Normal);
    font.setPixelSize(32);
    const QFontMetricsF metrics(font);
    const QRect rect(0, 0, 32, 32);
    for (const auto &entry : icons) {
        const QString name = QString::fromLatin1(entry.name);
        const QString text(QChar(entry.codepoint));
        QVERIFY2(metrics.inFont(QChar(entry.codepoint)), entry.name);
        const QPixmap actual = nova::ui::lineIcon(name, ink, disabled).pixmap(rect.size(), 1.0, QIcon::Normal);
        QPixmap expected(rect.size());
        expected.fill(Qt::transparent);
        QPainter painter(&expected);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setPen(ink);
        painter.setFont(font);
        painter.drawText(QRectF(rect).center() - metrics.tightBoundingRect(text).center(), text);
        painter.end();
        if (actual.toImage() != expected.toImage()) {
            const QString debugDir = qEnvironmentVariable("NOVA_MATERIAL_ICON_DEBUG");
            if (!debugDir.isEmpty()) {
                actual.save(debugDir + "/" + name + "-actual.png");
                expected.save(debugDir + "/" + name + "-expected.png");
            }
        }
        QVERIFY2(actual.toImage() == expected.toImage(), entry.name);
    }
}

void MaterialIconsTest::iconPixels_data() {
    QTest::addColumn<QString>("name");
    for (const auto &entry : icons)
        QTest::newRow(entry.name) << QString::fromLatin1(entry.name);
}

void MaterialIconsTest::iconPixels() {
    QFETCH(QString, name);
    const QIcon icon = nova::ui::lineIcon(name, QColor("#252a36"), QColor("#80879a"));
    QVERIFY(!icon.isNull());
    for (const int size : {14, 18, 22, 32, 36, 44}) {
        for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
            const QSize logical(size, size);
            const QSize device = (QSizeF(logical) * dpr).toSize();
            const QPixmap normal = icon.pixmap(logical, dpr, QIcon::Normal);
            QVERIFY(!normal.isNull());
            QCOMPARE(normal.size(), device);
            QCOMPARE(normal.devicePixelRatio(), dpr);
            const QImage normalImage = normal.toImage().convertToFormat(QImage::Format_ARGB32);
            QVERIFY(inkPixels(normalImage) > 0);
            const QPixmap disabled = icon.pixmap(logical, dpr, QIcon::Disabled);
            QCOMPARE(disabled.size(), device);
            QCOMPARE(disabled.devicePixelRatio(), dpr);
            const QImage disabledImage = disabled.toImage().convertToFormat(QImage::Format_ARGB32);
            QVERIFY(inkPixels(disabledImage) > 0);
            QVERIFY(normalImage != disabledImage);
        }
    }
}

void MaterialIconsTest::dprMetadataDoesNotCollide() {
    const QIcon icon = nova::ui::lineIcon(QStringLiteral("save"), QColor("#010203"), QColor("#040506"));
    const QPixmap atTwo = icon.pixmap(QSize(18, 18), 2.0);
    const QPixmap atOneAndHalf = icon.pixmap(QSize(24, 24), 1.5);
    QCOMPARE(atTwo.size(), QSize(36, 36));
    QCOMPARE(atOneAndHalf.size(), QSize(36, 36));
    QCOMPARE(atTwo.devicePixelRatio(), 2.0);
    QCOMPARE(atOneAndHalf.devicePixelRatio(), 1.5);
    QVERIFY(inkPixels(atTwo.toImage()) > 0);
    QVERIFY(inkPixels(atOneAndHalf.toImage()) > 0);
}

void MaterialIconsTest::toolIconsUseSharedMapping() {
    const auto tokens = nova::ui::tokens(true, "violet");
    for (const auto &info : nova::toolInfos()) {
        const QPixmap actual = nova::toolIcon(info.tool, tokens.text).pixmap(QSize(22, 22), 1.5, QIcon::Normal);
        const QPixmap expected = nova::ui::lineIcon(info.id, tokens.text, tokens.disabled)
                                     .pixmap(QSize(22, 22), 1.5, QIcon::Normal);
        QVERIFY2(actual.toImage() == expected.toImage(), info.id.toUtf8().constData());
        QVERIFY(inkPixels(actual.toImage()) > 0);
    }
}

void MaterialIconsTest::themedNormalAndDisabledInk() {
    for (const bool dark : {false, true}) {
        const auto tokens = nova::ui::tokens(dark, "violet");
        const QIcon icon = nova::ui::lineIcon(QStringLiteral("settings"), tokens.text, tokens.disabled);
        const QImage normal = icon.pixmap(QSize(32, 32), QIcon::Normal).toImage();
        const QImage disabled = icon.pixmap(QSize(32, 32), QIcon::Disabled).toImage();
        QVERIFY(hasOpaqueInk(normal, tokens.text));
        QVERIFY(hasOpaqueInk(disabled, tokens.disabled));
        QVERIFY(normal != disabled);
    }
}

void MaterialIconsTest::unknownNamesFailVisibly() {
    QVERIFY(nova::ui::lineIcon(QStringLiteral("not-a-material-icon")).isNull());
}

QTEST_MAIN(MaterialIconsTest)
#include "material_icons_test.moc"
