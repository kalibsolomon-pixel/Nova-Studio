// SPDX-License-Identifier: GPL-3.0-or-later
#include "projectdialogs.h"
#include "core.h"
#include <QtWidgets>
namespace nova {
QString newProjectDialog(QWidget *parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle("New Resource Pack");
    dialog.setObjectName("newProjectDialog");
    dialog.resize(520, 430);
    QFormLayout layout(&dialog);
    QLineEdit name("My Resource Pack"), destination(QDir::homePath() + "/My Resource Pack"),
        ns("minecraft"), description("A Nova Studio resource pack"), icon;
    name.setObjectName("projectName");
    destination.setObjectName("projectDestination");
    ns.setObjectName("projectNamespace");
    description.setObjectName("packDescription");
    QComboBox version;
    for (auto v : gameVersions())
        version.addItem("Java " + v.id + " · format " + v.format.text());
    layout.addRow("Project name", &name);
    layout.addRow("New folder", &destination);
    QPushButton browse("Choose parent directory…");
    layout.addRow("", &browse);
    QObject::connect(&browse, &QPushButton::clicked, &dialog, [&] {
        auto path = QFileDialog::getExistingDirectory(&dialog, "Destination parent");
        if (!path.isEmpty())
            destination.setText(QDir(path).filePath(name.text()));
    });
    layout.addRow("Minecraft version", &version);
    layout.addRow("Description", &description);
    layout.addRow("Namespace", &ns);
    layout.addRow("Pack icon (PNG, optional)", &icon);
    QPushButton chooseIcon("Choose PNG…");
    layout.addRow("", &chooseIcon);
    QObject::connect(&chooseIcon, &QPushButton::clicked, &dialog, [&] {
        auto path = QFileDialog::getOpenFileName(&dialog, "Pack icon", {}, "PNG (*.png)");
        if (!path.isEmpty())
            icon.setText(path);
    });
    QCheckBox folders("Create common asset folders"), open("Open after creation");
    folders.setChecked(true);
    open.setChecked(true);
    layout.addRow(&folders);
    layout.addRow(&open);
    QLabel issue;
    issue.setWordWrap(true);
    layout.addRow(&issue);
    QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons.button(QDialogButtonBox::Save)->setText("Create pack");
    layout.addRow(&buttons);
    QString created;
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QString error;
        if (!Project::create(destination.text(), name.text(),
                             gameVersions()[version.currentIndex()], description.text(), ns.text(),
                             icon.text(), folders.isChecked(), &error)) {
            issue.setText(error);
            return;
        }
        if (open.isChecked())
            created = destination.text();
        dialog.accept();
    });
    dialog.exec();
    return created;
}
void projectPropertiesDialog(QWidget *parent, const QString &root) {
    auto project = Project::inspect(root);
    QDialog dialog(parent);
    dialog.setWindowTitle("Resource Pack Properties");
    dialog.setObjectName("projectPropertiesDialog");
    dialog.resize(620, 540);
    QVBoxLayout layout(&dialog);
    QFormLayout form;
    layout.addLayout(&form);
    QLabel name(project.name), namespaces(project.namespaces.join(", "));
    namespaces.setWordWrap(true);
    form.addRow("Name (folder)", &name);
    form.addRow("Namespaces", &namespaces);
    auto pack = project.json.value("pack").toObject();
    QLineEdit description(
        pack.value("description").isString() ? pack.value("description").toString() : QString());
    description.setPlaceholderText("Use JSON below for a structured text component");
    form.addRow("Description", &description);
    QComboBox version;
    for (auto v : gameVersions())
        version.addItem("Java " + v.id + " · " + v.format.text());
    form.addRow("Target version", &version);
    auto format = PackFormat::parse(pack.contains("min_format") ? pack.value("min_format")
                                                                : pack.value("pack_format"));
    if (format) {
        for (int i = 0; i < gameVersions().size(); ++i)
            if (gameVersions()[i].format == *format)
                version.setCurrentIndex(i);
    }
    QPlainTextEdit source;
    source.setObjectName("metadataSource");
    source.setPlainText(QString::fromUtf8(project.metadata));
    layout.addWidget(&source, 1);
    QPushButton transform("Apply description and target · reformat JSON explicitly");
    layout.addWidget(&transform);
    QLabel feedback(project.diagnostics.join("\n"));
    feedback.setWordWrap(true);
    feedback.setObjectName("metadataDiagnostics");
    layout.addWidget(&feedback);
    QObject::connect(&source, &QPlainTextEdit::textChanged, &dialog, [&] {
        auto issue = Project::validateMetadata(source.toPlainText().toUtf8());
        feedback.setText(
            issue.isEmpty() ? "Metadata schema valid; asset compatibility is unverified" : issue);
    });
    QObject::connect(&transform, &QPushButton::clicked, &dialog, [&] {
        QJsonParseError error;
        auto doc = QJsonDocument::fromJson(source.toPlainText().toUtf8(), &error);
        if (error.error != QJsonParseError::NoError && !source.toPlainText().trimmed().isEmpty()) {
            feedback.setText("Correct malformed JSON first; original data is preserved");
            return;
        }
        auto json = doc.object();
        auto p = json.value("pack").toObject();
        if (!description.text().isEmpty() || p.value("description").isString() ||
            !p.contains("description"))
            p["description"] = description.text();
        auto v = gameVersions()[version.currentIndex()];
        if (v.rangeSchema) {
            p["min_format"] = v.format.json();
            p["max_format"] = v.format.json();
            p.remove("supported_formats");
            p.remove("pack_format");
        } else {
            p["pack_format"] = v.format.major;
            p.remove("min_format");
            p.remove("max_format");
        }
        json["pack"] = p;
        source.setPlainText(QString::fromUtf8(QJsonDocument(json).toJson()));
    });
    QLabel icon;
    QPixmap pix(root + "/pack.png");
    if (!pix.isNull())
        icon.setPixmap(pix.scaled(48, 48, Qt::KeepAspectRatio));
    form.addRow("Pack icon", &icon);
    QPushButton replaceIcon("Replace pack icon now…");
    form.addRow("", &replaceIcon);
    QObject::connect(&replaceIcon, &QPushButton::clicked, &dialog, [&] {
        auto path = QFileDialog::getOpenFileName(&dialog, "Pack icon", root, "PNG (*.png)");
        if (path.isEmpty())
            return;
        QString error;
        auto image = loadPng(path, &error);
        if (image.isNull()) {
            feedback.setText(error);
            return;
        }
        auto target = root + "/pack.png";
        auto expected = Revision::read(target);
        if (QMessageBox::question(&dialog, "Save pack icon?",
                                  "Save this PNG as pack.png now? Any existing icon will be "
                                  "replaced after a fresh revision check.",
                                  QMessageBox::Save | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Save)
            return;
        Document document(target, image);
        if (!document.saveAsRevision(target, expected, &error))
            feedback.setText(error);
        else {
            icon.setPixmap(QPixmap::fromImage(image).scaled(48, 48, Qt::KeepAspectRatio));
            feedback.setText("Pack icon saved atomically");
        }
    });
    QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout.addWidget(&buttons);
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (source.toPlainText() == QString::fromUtf8(project.metadata).replace("\r\n", "\n")) {
            dialog.accept();
            return;
        }
        QString error;
        if (!project.saveMetadata(source.toPlainText().toUtf8(), &error)) {
            feedback.setText(error);
            return;
        }
        dialog.accept();
    });
    dialog.exec();
}
} // namespace nova
