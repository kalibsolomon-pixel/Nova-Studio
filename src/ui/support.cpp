// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "build_identity.h"
#include "diagnostics.h"
#include "legal.h"
#include <QtWidgets>

namespace nova {
void Window::buildSupportMenu() {
    auto help = menuBar()->addMenu("&Help");
    help->setObjectName("menu-Help");
    auto guide = help->addAction("Getting started", QKeySequence::HelpContents);
    guide->setObjectName("gettingStartedCommand");
    connect(guide, &QAction::triggered, this, [this] {
        auto dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName("gettingStartedDialog");
        dialog->setWindowTitle("Getting started · Nova Studio");
        auto layout = new QVBoxLayout(dialog);
        auto text = new QTextBrowser;
        text->setAccessibleName("Getting started guide");
        text->setHtml(
            "<h2>Start with a resource pack</h2>"
            "<p>Use <b>File → New Resource Pack</b> to choose a Minecraft Java target "
            "version, or <b>Open Resource Pack</b> to select an existing folder. "
            "Recent Projects reopens a previous workspace.</p>"
            "<h3>Edit and save</h3><p>Double-click a PNG in Assets. <b>B</b> selects the pencil, "
            "<b>E</b> the eraser, and <b>Ctrl+Z / Ctrl+Y</b> undo and redo. "
            "<b>Ctrl+S</b> saves the texture into the pack; layered documents and animations also save "
            "their editable source. <b>File → Save All</b> (<b>Ctrl+Alt+Shift+S</b>) saves every unsaved "
            "document, and the status bar shows how many are unsaved.</p>"
            "<h3>Vanilla textures as a baseline</h3><p>A new pack can <b>use vanilla textures as baseline</b>: "
            "Nova downloads the official Minecraft Java client from Mojang once, keeps only its textures in a "
            "local cache and lists them dimmed in Assets. Edit one and save it to add your version to the pack; "
            "<b>Resources → Vanilla Baseline → Reset to Vanilla</b> moves it out again and Ctrl+Z in Assets "
            "undoes that. Exported packs contain only your own files.</p>"
            "<h3>Models and validation</h3><p>Open model JSON in Assets, then use the "
            "<b>Models / UV</b> panel. Choose the target version explicitly. "
            "<b>Resources → Validate resources</b> lists supported-field and dependency problems. "
            "The preview approximates isolated rendering; verify final results in Minecraft.</p>"
            "<h3>Workspaces</h3><p>The workspace switcher in the application bar arranges panels "
            "for <b>Essentials</b>, <b>Texture</b>, <b>Animation</b>, <b>Model</b> and <b>UV</b> work. "
            "Switching never changes documents. <b>View → Panels</b> shows or hides any panel; "
            "<b>View → Workspace → Reset Workspace</b> restores the default arrangement.</p>"
            "<h3>Find commands and recover work</h3><p><b>Ctrl+K</b> searches commands, tools, settings, "
            "workspaces and assets (<b>Ctrl+Shift+P</b> also works), and "
            "<b>Ctrl+/</b> lists every keyboard shortcut. Settings are in <b>Edit → Preferences</b>. "
            "Recovery restores unsaved work into memory and checks disk revisions before saving.</p>"
            "<h3>Local diagnostics</h3><p>Help → Export local diagnostics writes version and system "
            "information with recent messages. Messages may include file paths. "
            "Review the JSON before sharing it. Nothing is uploaded automatically.</p>");
        layout->addWidget(text);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        layout->addWidget(buttons);
        dialog->resize(560, 540);
        dialog->show();
    });
    auto shortcuts = help->addAction("Keyboard Shortcuts…");
    shortcuts->setObjectName("keyboardShortcutsCommand");
    shortcuts->setShortcut(QKeySequence("Ctrl+/"));
    shortcuts->setProperty("novaIcon", "keyboard");
    connect(shortcuts, &QAction::triggered, this, &Window::shortcutsDialog);
    help->addSeparator();
    auto exportAction = help->addAction("Export local diagnostics…");
    exportAction->setObjectName("exportDiagnosticsCommand");
    exportAction->setStatusTip("Save version, system information and recent logs; may include file paths");
    connect(exportAction, &QAction::triggered, this, [this] {
        const auto file = QFileDialog::getSaveFileName(this, "Export local diagnostics · may contain file paths",
                                                      "nova-diagnostics.json", "JSON (*.json)");
        if (file.isEmpty()) return;
        QString error;
        if (!diagnostics::exportReport(file, &error))
            QMessageBox::warning(this, "Diagnostic export failed", file + "\n" + error);
        else
            statusBar()->showMessage("Diagnostics saved locally · review before sharing", 5000);
    });
    auto licenses = help->addAction("Licenses and Source Code…");
    licenses->setObjectName("licensesCommand");
    licenses->setStatusTip("Nova Studio's license (GNU GPL v3 or later), third-party notices and source code");
    connect(licenses, &QAction::triggered, this, [this] { legal::createLicensesDialog(this)->show(); });
    auto about = help->addAction("About Nova Studio");
    about->setObjectName("aboutCommand");
    connect(about, &QAction::triggered, this, [this] {
        auto box = new QMessageBox(QMessageBox::NoIcon, "About Nova Studio",
                                   QString("Nova Studio %1 (preview)\nBuild %2 · Qt %3")
                                       .arg(AppVersion, BuildRevision, qVersion()),
                                   QMessageBox::Close, this);
        box->setObjectName("aboutDialog");
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setIconPixmap(windowIcon().pixmap(64, 64));
        box->setInformativeText(legal::notice());
        auto more = box->addButton("Licenses and Source Code…", QMessageBox::ActionRole);
        more->setObjectName("aboutLicensesButton");
        connect(more, &QPushButton::clicked, this, [this] { legal::createLicensesDialog(this)->show(); });
        box->open();
    });
}
}
