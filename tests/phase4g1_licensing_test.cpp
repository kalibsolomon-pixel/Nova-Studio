// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-G.1: GPL license notices in the application. The About box and Help > Licenses and
// Source Code show the copyright, the GPL grant, the warranty disclaimer, the unmodified GPL text,
// the third-party notices and where the source code is ("Appropriate Legal Notices", GPL-3.0 §0).
#include "build_identity.h"
#include "legal.h"
#include "search.h"
#include "window.h"
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>

using namespace nova;

namespace {
// https://www.gnu.org/licenses/gpl-3.0.txt
constexpr char Gpl3Sha256[] = "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986";
QByteArray sha256(const QByteArray &data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex(); }
QAction *helpAction(Window &window, const QString &name) {
    for (auto top : window.menuBar()->actions())
        if (top->menu() && top->menu()->objectName() == "menu-Help")
            for (auto action : top->menu()->actions())
                if (action->objectName() == name)
                    return action;
    return nullptr;
}
} // namespace

class Phase4g1LicensingTest : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    std::unique_ptr<Window> window;
    QString evidence = qEnvironmentVariable("NOVA_PHASE4G1_EVIDENCE");

    void capture(QWidget *widget, const QString &name) {
        if (evidence.isEmpty())
            return;
        QDir().mkpath(evidence);
        QVERIFY(widget->grab().save(evidence + "/" + name + ".png"));
    }
    void captureTabs(QDialog *dialog) {
        auto tabs = dialog->findChild<QTabWidget *>("licensesTabs");
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->setCurrentIndex(i);
            QCoreApplication::processEvents();
            capture(dialog, QString("licenses-%1").arg(i + 1));
        }
        tabs->setCurrentIndex(0);
    }

    QDialog *licensesDialog() const { return window->findChild<QDialog *>("licensesDialog"); }
    void verifyLicensesDialog(QDialog *dialog) {
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->isVisible());
        auto tabs = dialog->findChild<QTabWidget *>("licensesTabs");
        QVERIFY(tabs);
        QStringList titles;
        for (int i = 0; i < tabs->count(); ++i)
            titles << tabs->tabText(i);
        QCOMPARE(titles, (QStringList{"Nova Studio", "GNU GPL v3", "Third-party notices", "Source code"}));
        auto gpl = dialog->findChild<QPlainTextEdit *>("licenseGplText");
        QVERIFY(gpl && gpl->isReadOnly());
        QCOMPARE(gpl->toPlainText().trimmed(), legal::document("LICENSE").trimmed());
        auto nova = dialog->findChild<QTextBrowser *>("licenseNovaText");
        QVERIFY(nova && nova->toPlainText().contains(ProductCopyright));
        QVERIFY(nova->toPlainText().contains("GPL-3.0-or-later"));
        auto notices = dialog->findChild<QTextBrowser *>("licenseThirdPartyText");
        QVERIFY(notices && notices->toPlainText().contains("LGPL-3.0-only"));
        auto source = dialog->findChild<QPlainTextEdit *>("licenseSourceText");
        QVERIFY(source && source->toPlainText().contains(BuildRevision));
        for (auto widget : {static_cast<QWidget *>(gpl), static_cast<QWidget *>(nova),
                            static_cast<QWidget *>(notices), static_cast<QWidget *>(source)})
            QVERIFY2(!widget->accessibleName().isEmpty(), qPrintable(widget->objectName()));
    }

private slots:
    void initTestCase() {
        qApp->setProperty("settingsFile", temporary.filePath("licensing.ini"));
        QElapsedTimer started;
        started.start();
        window = std::make_unique<Window>(QString{}, QString{}, started);
        window->resize(1100, 760);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
    void productIdentity() {
        QCOMPARE(QString(ProductLicense), QString("GPL-3.0-or-later"));
        QVERIFY(QString(ProductCopyright).startsWith("Copyright (C) 2026 "));
    }
    void embeddedTexts() {
        QFile gpl(":/legal/LICENSE");
        QVERIFY(gpl.open(QIODevice::ReadOnly));
        QCOMPARE(sha256(gpl.readAll()), QByteArray(Gpl3Sha256));
        const auto copyright = legal::document("COPYRIGHT.md");
        QVERIFY(copyright.contains(ProductCopyright));
        QVERIFY(copyright.contains("SPDX-License-Identifier: GPL-3.0-or-later"));
        QVERIFY(copyright.contains("gradle-wrapper-9.8.2.jar"));
        QVERIFY(copyright.contains("NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT."));
        const auto notices = legal::document("THIRD_PARTY_NOTICES.md");
        QVERIFY(notices.contains("Qt Base 6.12.0"));
        QVERIFY(notices.contains("LGPL-3.0-only"));
        QVERIFY(!notices.contains("LICENSE-DECISION"));
        QVERIFY(legal::document("missing").isEmpty());
    }
    void noticeText() {
        const auto text = legal::notice();
        for (const auto &required : {QString(ProductCopyright), QString("GNU General Public License"),
                                     QString("either version 3"), QString("any later version"),
                                     QString("WITHOUT ANY WARRANTY"), QString("Licenses and Source Code")})
            QVERIFY2(text.contains(required), qPrintable(required));
        QVERIFY(!text.contains("not authorized"));
        QVERIFY(text.contains("NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT."));
        const auto source = legal::sourceNotice();
        QVERIFY(source.contains(AppVersion));
        QVERIFY(source.contains(BuildRevision));
        QVERIFY(source.contains("qtbase-everywhere-src-6.12.0.tar.xz"));
        if (qstrlen(SourceUrl))
            QVERIFY(source.contains(QString("Source repository: %1").arg(SourceUrl)));
    }
    void licensesCommand() {
        auto action = helpAction(*window, "licensesCommand");
        QVERIFY2(action, "Help > Licenses and Source Code is missing");
        QVERIFY(action->isEnabled());
        action->trigger();
        verifyLicensesDialog(licensesDialog());
        captureTabs(licensesDialog());
        licensesDialog()->close();
        QTRY_VERIFY(!licensesDialog());
    }
    void aboutBox() {
        auto action = helpAction(*window, "aboutCommand");
        QVERIFY(action);
        action->trigger();
        QMessageBox *box = nullptr;
        QTRY_VERIFY((box = window->findChild<QMessageBox *>("aboutDialog")) && box->isVisible());
        QVERIFY(box->text().contains(AppVersion));
        QVERIFY(box->text().contains(BuildRevision));
        QVERIFY(box->informativeText().contains(ProductCopyright));
        QVERIFY(box->informativeText().contains("WITHOUT ANY WARRANTY"));
        QVERIFY(box->informativeText().contains("GPL-3.0-or-later"));
        capture(box, "about");
        auto more = box->findChild<QPushButton *>("aboutLicensesButton");
        QVERIFY(more);
        more->click();
        verifyLicensesDialog(licensesDialog());
        licensesDialog()->close();
        if (auto stillOpen = window->findChild<QMessageBox *>("aboutDialog"))
            stillOpen->close();
        QTRY_VERIFY(!licensesDialog());
        QTRY_VERIFY(!window->findChild<QMessageBox *>("aboutDialog"));
    }
    void searchFindsLicenses() {
        const auto &index = window->searchIndex();
        bool found = false;
        for (const auto &result : index.query("license", 10))
            if (result.entry >= 0 && index.entries[result.entry].title.startsWith("Licenses and Source Code"))
                found = true;
        QVERIFY2(found, "Ctrl+K search does not find Licenses and Source Code");
    }
    void cleanupTestCase() { window.reset(); }
};

QTEST_MAIN(Phase4g1LicensingTest)
#include "phase4g1_licensing_test.moc"
