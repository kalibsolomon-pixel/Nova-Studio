// SPDX-License-Identifier: GPL-3.0-or-later
#include "legal.h"
#include "build_identity.h"
#include <QtWidgets>

namespace nova::legal {
QString notice() {
    return QString("%1\n\n"
                   "Nova Studio is free software: you can redistribute it and/or modify it under the terms "
                   "of the GNU General Public License as published by the Free Software Foundation, either "
                   "version 3 of the License, or (at your option) any later version (%2).\n\n"
                   "Nova Studio is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; "
                   "without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. "
                   "See the GNU General Public License for more details.\n\n"
                   "Help > Licenses and Source Code shows the full license, the third-party notices and "
                   "where to get the source code.\n\n"
                   "NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT. "
                   "\"Minecraft\" is a trademark of Mojang AB and Microsoft Corporation. "
                   "Vanilla textures shown for a baseline project are © Mojang Studios, are downloaded from "
                   "Mojang's official servers for local use and are not part of Nova Studio.")
        .arg(ProductCopyright, ProductLicense);
}

QString document(const QString &name) {
    QFile file(":/legal/" + name);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString sourceFolder() {
    const QDir folder(QCoreApplication::applicationDirPath() + "/source");
    return folder.exists() ? folder.absolutePath() : QString();
}

QString sourceNotice() {
    QString text = QString("This is Nova Studio %1, built from source revision %2.\n\n").arg(AppVersion, BuildRevision);
    const auto folder = sourceFolder();
    const auto archives = folder.isEmpty() ? QStringList{}
                                           : QDir(folder).entryList({"*-source.zip"}, QDir::Files, QDir::Name);
    if (!archives.isEmpty())
        text += QString("The complete corresponding source code of this build is included with this copy:\n%1\n\n")
                    .arg(QDir::toNativeSeparators(folder + "/" + archives.first()));
    else
        text += "This copy has no source folder (for example, a development build). Release downloads include "
                "the corresponding source code in their source folder and as a separate download next to them.\n\n";
    if (qstrlen(SourceUrl))
        text += QString("Source repository: %1\n\n").arg(SourceUrl);
    const QDir program(QCoreApplication::applicationDirPath());
    if (program.exists("licenses"))
        text += QString("The license and the third-party notices shown here are also installed with this copy:\n"
                        "%1\n%2\n\n")
                    .arg(QDir::toNativeSeparators(program.filePath("LICENSE.txt")),
                         QDir::toNativeSeparators(program.filePath("licenses/THIRD_PARTY_NOTICES.md")));
    text += "Qt is used under the GNU LGPL version 3. The exact Qt Base 6.12.0 source archive is in the "
            "licenses folder (qtbase-everywhere-src-6.12.0.tar.xz), and you may replace the Qt libraries "
            "with your own build of Qt 6.12.0.\n\n"
            "The corresponding Qt configuration and any source patches are in licenses\\qt-build. "
            "Third-party notices describe the compiler runtime source supplied with this build.\n\n"
            "docs\\BUILDING.md explains how to build Nova Studio from source.";
    return text;
}

QDialog *createLicensesDialog(QWidget *parent) {
    auto dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("licensesDialog");
    dialog->setWindowTitle("Licenses and Source Code · Nova Studio");
    auto layout = new QVBoxLayout(dialog);
    auto tabs = new QTabWidget;
    tabs->setObjectName("licensesTabs");
    const auto markdown = [](const QString &objectName, const QString &accessible, const QString &text) {
        auto browser = new QTextBrowser;
        browser->setObjectName(objectName);
        browser->setAccessibleName(accessible);
        browser->setOpenExternalLinks(true);
        browser->setMarkdown(text);
        return browser;
    };
    tabs->addTab(markdown("licenseNovaText", "Nova Studio copyright and license",
                          document("COPYRIGHT.md")), "Nova Studio");
    auto gpl = new QPlainTextEdit(document("LICENSE"));
    gpl->setObjectName("licenseGplText");
    gpl->setAccessibleName("GNU General Public License version 3");
    gpl->setReadOnly(true);
    gpl->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    tabs->addTab(gpl, "GNU GPL v3");
    tabs->addTab(markdown("licenseThirdPartyText", "Third-party notices", document("THIRD_PARTY_NOTICES.md")),
                 "Third-party notices");
    auto source = new QPlainTextEdit(sourceNotice());
    source->setObjectName("licenseSourceText");
    source->setAccessibleName("Source code");
    source->setReadOnly(true);
    tabs->addTab(source, "Source code");
    layout->addWidget(tabs);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    const QDir licenses(QCoreApplication::applicationDirPath() + "/licenses");
    if (licenses.exists()) {
        auto open = buttons->addButton("Open licenses folder", QDialogButtonBox::ActionRole);
        open->setObjectName("openLicensesFolder");
        QObject::connect(open, &QPushButton::clicked, dialog, [path = licenses.absolutePath()] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
    }
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->resize(720, 600);
    return dialog;
}
} // namespace nova::legal
