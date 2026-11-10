// SPDX-License-Identifier: GPL-3.0-or-later
#include "projectdialogs.h"
#include "baseline_ui.h"
#include "core.h"
#include <QtConcurrent>
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
    // Texture baseline choice (created after the existing widgets; shown below the version).
    QWidget choice;
    QVBoxLayout choiceLayout(&choice);
    choiceLayout.setContentsMargins(0, 0, 0, 0);
    QRadioButton blank("Blank pack"), baselineChoice("Use vanilla textures as baseline");
    blank.setObjectName("projectBlank");
    baselineChoice.setObjectName("projectBaseline");
    blank.setToolTip("Start with pack.mcmeta and the folders you choose; nothing else is added.");
    baselineChoice.setToolTip("Show this version's vanilla textures under your own files. Only textures you edit "
                              "are added to the pack.");
    blank.setChecked(true);
    choiceLayout.addWidget(&blank);
    choiceLayout.addWidget(&baselineChoice);
    QLabel baselineHint;
    baselineHint.setObjectName("projectBaselineHint");
    baselineHint.setWordWrap(true);
    baselineHint.setProperty("role", "hint");
    baselineHint.hide();
    layout.insertRow(4, "Textures", &choice);
    layout.insertRow(5, "", &baselineHint);
    auto ticket = std::make_shared<int>(0);
    auto updateHint = [&, ticket] {
        baselineHint.setVisible(baselineChoice.isChecked());
        if (!baselineChoice.isChecked())
            return;
        const auto id = gameVersions()[version.currentIndex()].id;
        const int mine = ++*ticket;
        baselineHint.setText("Checking the local vanilla texture cache…");
        // Cache status is read off the GUI thread; only the newest request updates the hint.
        auto watcher = new QFutureWatcher<QString>(&dialog);
        QObject::connect(watcher, &QFutureWatcherBase::finished, &dialog, [&, watcher, mine, ticket, id] {
            watcher->deleteLater();
            if (mine == *ticket)
                baselineHint.setText(watcher->result().isEmpty() ? ui::baselineConsentText(id) : watcher->result());
        });
        watcher->setFuture(QtConcurrent::run([id] { return ui::cachedBaselineSummary(id); }));
    };
    QObject::connect(&baselineChoice, &QRadioButton::toggled, &dialog, updateHint);
    QObject::connect(&version, &QComboBox::currentIndexChanged, &dialog, updateHint);
    QString created;
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QString error;
        std::optional<BaselineRef> ref;
        if (baselineChoice.isChecked()) {
            // Validate the destination before any download; nothing is created unless it succeeds.
            const QFileInfo target(destination.text());
            if (destination.text().trimmed().isEmpty() || target.exists() || !target.absoluteDir().exists()) {
                issue.setText("Choose a new destination folder inside an existing directory; existing folders are preserved");
                return;
            }
            const auto id = gameVersions()[version.currentIndex()].id;
            vanilla::BaselineInstaller::Error kind;
            QString why;
            const auto entry = ui::obtainBaseline(&dialog, id, {}, false, &kind, &why);
            if (!entry) {
                QMessageBox box(QMessageBox::Warning, "Vanilla textures not available",
                                (why.isEmpty() ? QString("No vanilla textures were downloaded.") : why) +
                                    QStringLiteral("\n\nNo project was created. Create a blank pack instead?"),
                                QMessageBox::NoButton, &dialog);
                box.setObjectName("baselineCreationFailed");
                auto blankButton = box.addButton("Create Blank Pack", QMessageBox::AcceptRole);
                blankButton->setObjectName("createBlankInstead");
                auto back = box.addButton("Back", QMessageBox::RejectRole);
                box.setDefaultButton(back);
                box.setEscapeButton(back);
                box.exec();
                if (box.clickedButton() != blankButton) {
                    issue.setText("No project was created.");
                    return;
                }
            } else
                ref = ui::baselineRef(*entry);
        }
        if (!Project::create(destination.text(), name.text(),
                             gameVersions()[version.currentIndex()], description.text(), ns.text(),
                             icon.text(), folders.isChecked(), &error, ref ? &*ref : nullptr)) {
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
QString styleNotesSummary(const StyleNotes &notes) {
    if (notes.empty())
        return "None";
    QStringList parts;
    for (const auto &[label, text] : {std::pair{"Theme", notes.theme}, {"Audience", notes.audience},
                                      {"Palette", notes.palette}, {"Preserve", notes.preserve}, {"Notes", notes.notes}})
        if (!text.isEmpty())
            parts << QString("%1: %2").arg(label, text.simplified().left(60));
    return parts.join(" · ");
}
bool styleNotesDialog(QWidget *parent, const QString &root) {
    auto config = ProjectConfig::load(root);
    QDialog dialog(parent);
    dialog.setWindowTitle("Pack Style Notes");
    dialog.setObjectName("styleNotesDialog");
    dialog.resize(560, 520);
    QVBoxLayout layout(&dialog);
    QLabel intro("Optional notes about this pack's intended style. Authorized AI agents can read them as your "
                 "preferences; they never grant permissions. Stored in .nova/project.json and not exported "
                 "with the pack.");
    intro.setWordWrap(true);
    layout.addWidget(&intro);
    QFormLayout form;
    layout.addLayout(&form, 1);
    const auto notes = config.styleNotes();
    QLineEdit theme(notes.theme), audience(notes.audience);
    QPlainTextEdit palette(notes.palette), preserve(notes.preserve), free(notes.notes);
    theme.setObjectName("styleTheme");
    audience.setObjectName("styleAudience");
    palette.setObjectName("stylePalette");
    preserve.setObjectName("stylePreserve");
    free.setObjectName("styleNotes");
    theme.setMaxLength(StyleNotes::ShortLimit);
    audience.setMaxLength(StyleNotes::ShortLimit);
    theme.setPlaceholderText("For example: cozy medieval, high contrast");
    audience.setPlaceholderText("For example: survival players, younger players");
    palette.setPlaceholderText("Preferred colours or limits");
    preserve.setPlaceholderText("Assets or conventions to keep, for example vanilla ore shapes");
    for (auto edit : {&palette, &preserve, &free})
        edit->setTabChangesFocus(true);
    form.addRow("Theme", &theme);
    form.addRow("Audience", &audience);
    form.addRow("Palette", &palette);
    form.addRow("Preserve", &preserve);
    form.addRow("Notes", &free);
    QLabel feedback(config.diagnostic);
    feedback.setObjectName("styleNotesFeedback");
    feedback.setWordWrap(true);
    layout.addWidget(&feedback);
    QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons.button(QDialogButtonBox::Save)->setEnabled(config.diagnostic.isEmpty());
    layout.addWidget(&buttons);
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    bool saved = false;
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto edited = StyleNotes::fromJson(QJsonObject{{"theme", theme.text()}, {"audience", audience.text()},
            {"palette", palette.toPlainText()}, {"preserve", preserve.toPlainText()}, {"notes", free.toPlainText()}});
        if (edited.palette.size() < palette.toPlainText().trimmed().size() ||
            edited.preserve.size() < preserve.toPlainText().trimmed().size() ||
            edited.notes.size() < free.toPlainText().trimmed().size()) {
            feedback.setText(QString("Each text field is limited to %1 characters.").arg(StyleNotes::LongLimit));
            return;
        }
        // Reload so a concurrent change is detected by the revision check rather than overwritten.
        auto current = ProjectConfig::load(root);
        current.setStyleNotes(edited);
        QString error;
        if (!current.revision.sameContent(config.revision) || !current.save(root, &error)) {
            feedback.setText(error.isEmpty() ? QString("Project settings changed on disk. Close and reopen this "
                                                       "dialog; nothing was saved.") : error);
            return;
        }
        saved = true;
        dialog.accept();
    });
    dialog.exec();
    return saved;
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
    QLabel baselineInfo(project.baseline ? QString("Java %1 (identity %2)").arg(project.baseline->version,
                                                                                project.baseline->identity.left(12))
                                         : project.configDiagnostic.isEmpty() ? QString("None (blank pack)")
                                                                              : "None · " + project.configDiagnostic);
    baselineInfo.setObjectName("projectBaselineInfo");
    baselineInfo.setWordWrap(true);
    baselineInfo.setTextInteractionFlags(Qt::TextSelectableByMouse);
    form.addRow("Vanilla baseline", &baselineInfo);
    QLabel styleInfo(styleNotesSummary(ProjectConfig::load(root).styleNotes()));
    styleInfo.setObjectName("projectStyleNotes");
    styleInfo.setWordWrap(true);
    QPushButton editStyle("Edit style notes…");
    editStyle.setObjectName("editStyleNotes");
    auto styleRow = new QHBoxLayout;
    styleRow->addWidget(&styleInfo, 1);
    styleRow->addWidget(&editStyle);
    form.addRow("Style notes", styleRow);
    QObject::connect(&editStyle, &QPushButton::clicked, &dialog, [&] {
        if (styleNotesDialog(&dialog, root))
            styleInfo.setText(styleNotesSummary(ProjectConfig::load(root).styleNotes()));
    });
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
