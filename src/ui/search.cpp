// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-G: application-wide search. See search.h.
#include "search.h"
#include "layers.h"
#include "modelpanel.h"
#include "theme.h"
#include "tools.h"
#include "window.h"
#include <QtWidgets>
#include <algorithm>

namespace nova {
namespace {
QString cleanText(QString text) { return text.remove('&').section('\t', 0, 0).trimmed(); }

// Words people use for things whose menu names differ. Keyed by action object name.
const QHash<QString, QStringList> &aliases() {
    static const QHash<QString, QStringList> table{
        {"editUndo", {"undo", "revert", "back", "history"}},
        {"editRedo", {"redo", "history"}},
        {"saveCommand", {"save", "write"}},
        {"exportPngCommand", {"export", "png", "update", "game files"}},
        {"exportPackCommand", {"export", "copy", "pack", "share", "folder"}},
        {"exportAnimation", {"export animation", "mcmeta", "frames", "animated"}},
        {"testInMinecraft", {"minecraft", "test", "game", "launch", "play", "fabric", "run"}},
        {"syncTestPack", {"minecraft", "test", "sync", "refresh", "reload"}},
        {"validateResources", {"problems", "errors", "warnings", "validate", "check", "lint"}},
        {"mcpAccess", {"mcp", "ai", "agent", "claude", "codex", "integration", "permissions", "access",
                       "model context protocol", "assistant"}},
        {"commandPaletteCommand", {"search", "find", "commands", "palette"}},
        {"licensesCommand", {"license", "gpl", "copyright", "legal", "notices", "source code", "third party"}},
        {"preferencesCommand", {"settings", "options", "preferences", "configuration", "appearance"}},
        {"darkThemeCommand", {"appearance", "theme", "dark", "light", "mode"}},
        {"pixelGridCommand", {"grid", "pixels", "view"}},
        {"openProjectCommand", {"open", "folder", "resource pack", "project"}},
        {"createResourceAsset", {"new", "create", "block", "item", "texture", "model"}},
        {"relatedAssets", {"references", "used by", "dependencies"}},
        {"workspacePreset-essentials", {"default", "workspace", "layout"}},
        {"workspacePreset-texture", {"paint", "draw", "pixels", "workspace"}},
        {"workspacePreset-animation", {"timeline", "frames", "animate", "workspace"}},
        {"workspacePreset-model", {"3d", "model", "cube", "workspace"}},
        {"workspacePreset-uv", {"uv", "unwrap", "texture coordinates", "faces", "workspace"}},
        {"keyboardShortcutsCommand", {"keyboard", "keys", "hotkeys", "shortcuts"}},
    };
    return table;
}
// Panels: keyed by dock object name.
const QHash<QString, QStringList> &panelAliases() {
    static const QHash<QString, QStringList> table{
        {"layers", {"layer", "opacity", "blend", "visibility"}},
        {"assets", {"files", "browser", "project tree", "textures"}},
        {"inspector", {"properties", "document", "zoom"}},
        {"colors", {"colour", "palette", "swatches", "picker"}},
        {"history", {"undo", "history", "steps"}},
        {"animationDock", {"timeline", "frames", "animation"}},
        {"resourceProblems", {"problems", "errors", "validation"}},
        {"modelsUv", {"uv", "models", "elements", "faces"}},
        {"preview", {"3d", "cube", "preview"}},
    };
    return table;
}
// Menus disabled while no texture document is open (see Window::editorRefresh).
bool requiresCanvas(const QString &name) {
    static const QStringList names{"menu-Adjust", "menu-Filters", "menu-Transform", "menu-Image",
                                   "menu-Shading", "menu-Generate"};
    return names.contains(name);
}
bool tokenInWords(const QString &text, const QString &token) {
    qsizetype at = 0;
    while ((at = text.indexOf(token, at)) >= 0) {
        if (at == 0 || !text[at - 1].isLetterOrNumber())
            return true;
        ++at;
    }
    return false;
}
bool subsequence(const QString &text, const QString &token) {
    qsizetype at = 0;
    for (const auto ch : token) {
        at = text.indexOf(ch, at);
        if (at < 0)
            return false;
        ++at;
    }
    return true;
}
} // namespace

QStringList searchAliasNames() { return aliases().keys() + panelAliases().keys(); }

QString searchShortcutText(const QString &text) {
    auto out = text.toLower().simplified();
    out.replace(" + ", "+").replace(' ', '+');
    return out;
}

int searchScore(const SearchEntry &entry, const QString &query) {
    const auto q = query.simplified().toLower();
    if (q.isEmpty())
        return 0;
    const auto title = entry.title.toLower();
    const auto shortcut = searchShortcutText(entry.shortcut);
    int total = 0;
    // A typed shortcut ("ctrl+z", "ctrl shift p", "f8") finds its command directly.
    const auto keys = searchShortcutText(q);
    if (!shortcut.isEmpty() && (keys == shortcut || (keys.contains('+') && shortcut.split(", ").contains(keys))))
        total += 150;
    for (const auto &token : q.split(' ', Qt::SkipEmptyParts)) {
        int best = -1;
        if (title.startsWith(token))
            best = 40;
        else if (tokenInWords(title, token))
            best = 32;
        else if (title.contains(token))
            best = 22;
        bool keywordMatch = false;
        for (const auto &keyword : entry.keywords) {
            if (keyword == token) {
                best = std::max(best, 28);
                keywordMatch = true;
            } else if (keyword.startsWith(token) || tokenInWords(keyword, token))
                best = std::max(best, 20);
        }
        // A curated alias that also names the token marks the primary destination for that word.
        if (keywordMatch && best > 28)
            best += 8;
        if (best < 0 && entry.haystack.contains(token))
            best = 10;
        if (best < 0 && total >= 150 && keys.contains(token))
            best = 0; // part of a matched shortcut
        if (best < 0 && token.size() >= 3 && subsequence(title, token))
            best = 4;
        if (best < 0)
            return -1;
        total += best;
    }
    if (title == q)
        total += 100;
    else if (title.startsWith(q))
        total += 50;
    else if (title.contains(q))
        total += 25;
    if (!entry.unavailable.isEmpty())
        total -= 12;
    return total;
}

void SearchIndex::finish() {
    for (auto &entry : entries) {
        for (auto &keyword : entry.keywords)
            keyword = keyword.toLower();
        entry.haystack = QStringList{entry.title, entry.keywords.join(' '), entry.location, entry.category,
                                     entry.description}.join(' ').toLower();
    }
    assetsLower.clear();
    assetsLower.reserve(assets.size());
    for (const auto &asset : assets)
        assetsLower << asset.toLower();
}

QList<SearchIndex::Result> SearchIndex::query(const QString &text, int limit) const {
    QList<Result> results;
    const auto q = text.simplified().toLower();
    for (int i = 0; i < entries.size(); ++i) {
        const int score = searchScore(entries[i], q);
        if (score >= 0)
            results.append({i, {}, score});
    }
    // Assets: only once a query is specific enough; file names rank above folders.
    if (q.size() >= 2 && !assets.isEmpty()) {
        const auto tokens = q.split(' ', Qt::SkipEmptyParts);
        QList<Result> found;
        for (qsizetype i = 0; i < assetsLower.size(); ++i) {
            const auto &path = assetsLower[i];
            bool all = true;
            for (const auto &token : tokens)
                if (!path.contains(token)) {
                    all = false;
                    break;
                }
            if (!all)
                continue;
            const auto name = path.mid(path.lastIndexOf('/') + 1);
            const int score = name.startsWith(q) ? 30 : name.contains(q) ? 20 : name.contains(tokens.first()) ? 14 : 8;
            found.append({-1, assets[i], score});
        }
        std::stable_sort(found.begin(), found.end(), [](const Result &a, const Result &b) { return a.score > b.score; });
        results += found.mid(0, 25);
    }
    std::stable_sort(results.begin(), results.end(), [this](const Result &a, const Result &b) {
        if (a.score != b.score)
            return a.score > b.score;
        const bool aAvailable = a.entry >= 0 && entries[a.entry].unavailable.isEmpty();
        const bool bAvailable = b.entry >= 0 && entries[b.entry].unavailable.isEmpty();
        return aAvailable && !bAvailable;
    });
    return results.mid(0, limit);
}

namespace {
// Name on the left; category and shortcut right-aligned in the muted colour.
class SearchDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const auto right = index.data(Qt::UserRole + 1).toString();
        const bool unavailable = index.data(Qt::UserRole + 2).toBool();
        const bool selected = opt.state & QStyle::State_Selected;
        const auto muted = selected ? opt.palette.color(QPalette::HighlightedText) : opt.palette.color(QPalette::PlaceholderText);
        const int rightWidth = right.isEmpty() ? 0 : opt.fontMetrics.horizontalAdvance(right) + 2 * ui::SpaceXL;
        const auto title = opt.text;
        opt.text.clear();
        opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);
        p->save();
        const auto textRect = opt.rect.adjusted(ui::SpaceL, 0, -rightWidth, 0);
        p->setPen(unavailable ? muted : selected ? opt.palette.color(QPalette::HighlightedText) : opt.palette.color(QPalette::Text));
        p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, opt.fontMetrics.elidedText(title, Qt::ElideRight, textRect.width()));
        if (!right.isEmpty()) {
            p->setPen(muted);
            p->drawText(opt.rect.adjusted(0, 0, -ui::SpaceXL, 0), Qt::AlignRight | Qt::AlignVCenter, right);
        }
        p->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        return {size.width(), std::max(size.height(), option.fontMetrics.height() + 2 * ui::SpaceM)};
    }
};
} // namespace

SearchDialog::SearchDialog(SearchIndex searchIndex, QWidget *parent) : QDialog(parent), index(std::move(searchIndex)) {
    // Object names kept from the Phase 2-C3 command palette.
    setObjectName("commandPalette");
    setWindowTitle("Search");
    auto layout = new QVBoxLayout(this);
    layout->setSpacing(ui::SpaceM);
    query = new QLineEdit;
    query->setObjectName("commandQuery");
    query->setPlaceholderText("Search commands, tools, settings, workspaces and assets");
    query->setAccessibleName("Search Nova Studio");
    query->setClearButtonEnabled(true);
    list = new QListWidget;
    list->setObjectName("commandList");
    list->setAccessibleName("Search results");
    list->setUniformItemSizes(true);
    list->setItemDelegate(new SearchDelegate(list));
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detail = new QLabel;
    detail->setObjectName("searchDetail");
    detail->setTextFormat(Qt::PlainText);
    detail->setWordWrap(true);
    detail->setProperty("role", "hint");
    detail->setMinimumHeight(detail->fontMetrics().lineSpacing() * 2);
    layout->addWidget(query);
    layout->addWidget(list, 1);
    layout->addWidget(detail);
    connect(query, &QLineEdit::textChanged, this, [this] { filter(); });
    connect(query, &QLineEdit::returnPressed, this, [this] { activateCurrent(); });
    connect(list, &QListWidget::itemActivated, this, [this] { activateCurrent(); });
    connect(list, &QListWidget::currentRowChanged, this, [this] { showDetail(); });
    query->installEventFilter(this);
    list->installEventFilter(this);
    query->setFocus(Qt::OtherFocusReason);
    const auto available = parent && parent->screen() ? parent->screen()->availableGeometry().size() * 0.8 : QSize(800, 600);
    resize(QSize(600, 440).boundedTo(available));
    filter();
}

void SearchDialog::filter() {
    results = index.query(query->text());
    const QSignalBlocker block(list);
    list->clear();
    for (const auto &result : std::as_const(results)) {
        auto item = new QListWidgetItem;
        QString title, right, accessible;
        bool unavailable = false;
        if (result.entry >= 0) {
            const auto &entry = index.entries[result.entry];
            title = entry.title + (entry.current ? "  (current)" : "");
            unavailable = !entry.unavailable.isEmpty();
            right = (unavailable ? "Unavailable · " : "") + entry.category +
                    (entry.shortcut.isEmpty() ? QString{} : "    " + entry.shortcut);
            accessible = QStringList{title, entry.category, entry.shortcut, unavailable ? "unavailable" : ""}
                             .filter(QRegularExpression(".")).join(", ");
        } else {
            title = QFileInfo(result.asset).fileName();
            right = "Asset";
            accessible = result.asset + ", asset";
        }
        item->setText(title);
        item->setData(Qt::UserRole + 1, right);
        item->setData(Qt::UserRole + 2, unavailable);
        item->setData(Qt::AccessibleTextRole, accessible);
        list->addItem(item);
    }
    if (results.isEmpty()) {
        auto item = new QListWidgetItem(QString("No results for “%1”").arg(query->text().trimmed()));
        item->setFlags(Qt::NoItemFlags);
        list->addItem(item);
    }
    list->setCurrentRow(results.isEmpty() ? -1 : 0);
    showDetail();
}

void SearchDialog::showDetail() {
    const auto row = list->currentRow();
    if (row < 0 || row >= results.size()) {
        detail->setText(query->text().trimmed().isEmpty() ? "Type to search. Enter runs the selected result." : "");
        return;
    }
    const auto &result = results[row];
    if (result.entry < 0) {
        detail->setText("Asset · " + result.asset);
        return;
    }
    const auto &entry = index.entries[result.entry];
    QStringList parts{entry.category + (entry.location.isEmpty() ? QString{} : " · " + entry.location)};
    if (!entry.description.isEmpty() && entry.description != entry.title)
        parts << entry.description;
    if (!entry.unavailable.isEmpty())
        parts << "Unavailable: " + entry.unavailable;
    detail->setText(parts.join('\n'));
}

bool SearchDialog::activateCurrent() {
    const auto row = list->currentRow();
    if (row < 0 || row >= results.size())
        return false;
    const auto result = results[row];
    if (result.entry < 0) {
        auto open = index.openAsset;
        chosen = [open, asset = result.asset] { if (open) open(asset); };
        accept();
        return true;
    }
    const auto &entry = index.entries[result.entry];
    if (!entry.unavailable.isEmpty()) {
        showDetail();
        QApplication::beep();
        return false;
    }
    if (entry.activate)
        chosen = entry.activate;
    else if (entry.action)
        chosen = [action = entry.action] { if (action && action->isEnabled()) action->trigger(); };
    accept();
    return true;
}

bool SearchDialog::eventFilter(QObject *object, QEvent *event) {
    if (object == query && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_Up || key == Qt::Key_PageDown || key == Qt::Key_PageUp) {
            const int step = key == Qt::Key_PageDown || key == Qt::Key_PageUp ? 8 : 1;
            const int direction = key == Qt::Key_Down || key == Qt::Key_PageDown ? 1 : -1;
            if (!results.isEmpty())
                list->setCurrentRow(std::clamp(list->currentRow() + direction * step, 0, int(results.size()) - 1));
            return true;
        }
    }
    // Typing while the list has focus continues the query.
    if (object == list && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent *>(event);
        if (!key->text().isEmpty() && key->text().at(0).isPrint() && !(key->modifiers() & Qt::ControlModifier)) {
            query->setFocus();
            query->insert(key->text());
            return true;
        }
    }
    return QDialog::eventFilter(object, event);
}

// ------------------------------------------------------------------ the Window's index
SearchIndex Window::searchIndex() {
    SearchIndex index;
    QSet<QAction *> seen;
    const auto palette = findChild<QAction *>("commandPaletteCommand");
    if (palette)
        seen.insert(palette); // search does not list itself
    const auto unavailableReason = [this](QAction *action, const QList<QMenu *> &chain) -> QString {
        bool menusEnabled = true;
        QString disabledMenu;
        for (auto menu : chain)
            if (!menu->isEnabled()) {
                menusEnabled = false;
                disabledMenu = menu->objectName();
            }
        if (action->isEnabled() && menusEnabled)
            return {};
        const auto name = action->objectName();
        if (name == "editUndo")
            return "Nothing to undo in the current document.";
        if (name == "editRedo")
            return "Nothing to redo in the current document.";
        if (QStringList{"editCut", "editCopy", "editClear", "selectNone", "selectInvert", "imageCropSelection",
                        "editDuplicateSelection"}.contains(name) && current())
            return "Select pixels on the canvas first.";
        if (requiresCanvas(disabledMenu) || !current())
            return "Open a texture to use this command.";
        return "Not available in the current context.";
    };
    const auto categoryFor = [](QAction *action, const QStringList &path) {
        const auto name = action->objectName();
        if (name.startsWith("tool-"))
            return QString("Tool");
        if (name.startsWith("workspacePreset-"))
            return QString("Workspace");
        if (qobject_cast<QDockWidget *>(action->parent()) || qobject_cast<QToolBar *>(action->parent()))
            return QString("Panel");
        if (name.startsWith("accent-") || name == "darkThemeCommand" || name == "preferencesCommand" ||
            name == "pixelGridCommand" || path.contains("Theme"))
            return QString("Setting");
        return QString("Command");
    };
    const auto add = [&](QAction *action, const QStringList &path, const QList<QMenu *> &chain) {
        if (!action || action->isSeparator() || seen.contains(action) || cleanText(action->text()).isEmpty())
            return;
        seen.insert(action);
        SearchEntry entry;
        entry.title = cleanText(action->text());
        entry.location = path.join(" › ");
        entry.category = categoryFor(action, path);
        QStringList shortcuts;
        for (const auto &key : action->shortcuts())
            shortcuts << key.toString(QKeySequence::NativeText);
        entry.shortcut = shortcuts.join(", ");
        entry.action = action;
        entry.keywords = aliases().value(action->objectName());
        const auto tip = action->statusTip().isEmpty() ? cleanText(action->toolTip()) : action->statusTip();
        const auto bare = [](QString text) { return text.section(" (", 0, 0).remove(QChar(0x2026)).remove("...").trimmed(); };
        if (bare(tip) != bare(entry.title))
            entry.description = tip;
        entry.unavailable = unavailableReason(action, chain);
        if (entry.category == "Workspace" || entry.category == "Setting")
            entry.current = action->isCheckable() && action->isChecked();
        if (auto dock = qobject_cast<QDockWidget *>(action->parent())) {
            // Searching for a panel shows it; it never hides an open panel.
            entry.keywords += panelAliases().value(dock->objectName());
            entry.description = dock->isVisible() ? "Panel is open. Enter brings it to the front." : "Shows the panel.";
            entry.activate = [dock] {
                dock->show();
                dock->raise();
                if (auto widget = dock->widget())
                    widget->setFocus(Qt::OtherFocusReason);
            };
        }
        index.entries << entry;
    };
    std::function<void(QMenu *, QStringList, QList<QMenu *>)> walk = [&](QMenu *menu, QStringList path, QList<QMenu *> chain) {
        for (auto action : menu->actions()) {
            if (auto sub = action->menu()) {
                // Saved layouts are user data rather than commands; keep the menu itself out.
                if (sub->objectName() == "activityMenu")
                    continue;
                walk(sub, path + QStringList{cleanText(action->text())}, chain + QList<QMenu *>{sub});
            } else
                add(action, path, chain);
        }
    };
    for (auto action : menuBar()->actions())
        if (auto menu = action->menu())
            walk(menu, {cleanText(action->text())}, {menu});
    // Tools and window commands that are not in any menu (tool strip, canvas shortcuts).
    for (auto action : findChildren<QAction *>()) {
        if (seen.contains(action))
            continue;
        const auto name = action->objectName();
        if (name.startsWith("tool-"))
            add(action, {"Tool strip"}, {});
        else if (actions().contains(action))
            add(action, {"Window"}, {});
    }
    for (auto dock : findChildren<QDockWidget *>())
        add(dock->toggleViewAction(), {"View", "Panels"}, {});

    // Settings: open Preferences at the control.
    const auto setting = [&](const QString &title, const QString &location, const QString &control,
                             const QStringList &keywords) {
        SearchEntry entry;
        entry.title = title;
        entry.category = "Setting";
        entry.location = "Preferences › " + location;
        entry.keywords = keywords;
        entry.activate = [this, control] { preferencesDialog(control); };
        index.entries << entry;
    };
    setting("Theme", "Appearance", "preferenceTheme", {"appearance", "dark", "light", "colour", "mode"});
    setting("Accent colour", "Appearance", "preferenceAccent", {"appearance", "accent", "color", "colour", "violet", "blue"});
    setting("Usage tips in the Inspector", "Appearance", "preferenceTips", {"appearance", "tips", "hints", "help"});
    setting("Pixel grid at high zoom", "Editing", "preferencePixelGrid", {"editor", "grid", "pixels"});
    setting("Undo history per document", "Editing", "preferenceHistory", {"editor", "undo", "history", "memory"});
    setting("Reopen the last project at startup", "Startup", "preferenceReopen", {"startup", "restore", "session", "recent"});
    setting("Reset workspace layout", "Workspace", "preferenceResetWorkspace", {"workspace", "panels", "layout", "reset"});
    // Controls that live in panels rather than Preferences.
    const auto control = [&](const QString &title, const QString &location, QDockWidget *dock, QWidget *bar,
                             const QString &name, const QStringList &keywords) {
        SearchEntry entry;
        entry.title = title;
        entry.category = "Setting";
        entry.location = location;
        entry.keywords = keywords;
        entry.activate = [this, dock, bar, name] {
            if (dock) {
                dock->show();
                dock->raise();
            }
            if (bar)
                bar->show();
            if (auto widget = findChild<QWidget *>(name))
                widget->setFocus(Qt::OtherFocusReason);
        };
        index.entries << entry;
    };
    control("Layer opacity", "Layers panel", layersDock, nullptr, "layerOpacitySlider", {"opacity", "alpha", "transparency", "layer"});
    control("Brush opacity", "Tool options bar", nullptr, toolOptions, "brushOpacity", {"opacity", "alpha", "brush", "flow"});
    control("Brush size", "Tool options bar", nullptr, toolOptions, "brushSize", {"brush", "size", "width", "radius"});
    // MCP pages.
    SearchEntry permissions;
    permissions.title = "MCP access and permissions";
    permissions.category = "Setting";
    permissions.location = "Resources › MCP Integration › Access";
    permissions.keywords = {"mcp", "ai", "agent", "permissions", "access", "revoke", "allow", "read-only", "editing"};
    permissions.description = "Allow or revoke an AI agent's access to the current project.";
    permissions.activate = [this] { showMcpAccess(0); };
    index.entries << permissions;
    SearchEntry setup = permissions;
    setup.title = "Set up an AI agent (MCP)";
    setup.location = "Resources › MCP Integration › Agent Setup";
    setup.keywords = {"mcp", "ai", "agent", "claude", "codex", "configure", "connect", "setup", "integration"};
    setup.description = "Add Nova Studio to Claude Desktop, Claude Code, Codex or another MCP client.";
    setup.activate = [this] { showMcpAccess(1); };
    index.entries << setup;

    // Assets from the in-memory index: textures and models that open in Nova.
    for (const auto &file : std::as_const(collection.files))
        if (file.endsWith(".png") || (file.endsWith(".json") && file.contains("/models/")))
            index.assets << file;
    index.openAsset = [this](const QString &asset) { openRelatedAsset(asset); };
    index.finish();
    return index;
}

void Window::commandPalette() {
    SearchDialog dialog(searchIndex(), this);
    if (dialog.exec() == QDialog::Accepted && dialog.chosen)
        dialog.chosen();
}
} // namespace nova
