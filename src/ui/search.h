// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Phase 4-G: application-wide search (Ctrl+K). It extends the Phase 2-C3 command palette: the
// index is built from the live menus and actions each time search opens, so it cannot drift
// from what the menus show, and every command result triggers the same QAction as its menu.
// Settings, MCP pages and assets are added as a few explicit entries that open the existing
// screens. Asset results come from the in-memory project index; nothing touches the disk.
#include <QAction>
#include <QDialog>
#include <QPointer>
#include <QStringList>
#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;

namespace nova {
struct SearchEntry {
    QString title, category, location, description, shortcut;
    QStringList keywords;
    QPointer<QAction> action;        // commands, tools, panels, workspaces
    std::function<void()> activate;  // settings and other screens
    QString unavailable;             // why it cannot run now; empty when available
    bool current = false;            // the active workspace, a checked option
    QString haystack;                // lower-case title, keywords, location and category
};
struct SearchIndex {
    QList<SearchEntry> entries;
    QStringList assets, assetsLower; // project-relative paths, for asset results
    std::function<void(const QString &)> openAsset;
    void finish(); // prepares lower-case search text
    struct Result {
        int entry = -1;      // index into entries, or -1 for an asset
        QString asset;
        int score = 0;
    };
    QList<Result> query(const QString &text, int limit = 60) const;
};
int searchScore(const SearchEntry &entry, const QString &query);
QString searchShortcutText(const QString &text); // "Ctrl+Shift+P" -> "ctrl+shift+p"
// Object names of the actions and panels that have curated aliases (tests check they exist).
QStringList searchAliasNames();

class SearchDialog : public QDialog {
    SearchIndex index;
    QLineEdit *query;
    QListWidget *list;
    QLabel *detail;
    QList<SearchIndex::Result> results;
    void filter();
    void showDetail();
    bool activateCurrent();
  public:
    std::function<void()> chosen; // run after the dialog closes
    SearchDialog(SearchIndex index, QWidget *parent);
    bool eventFilter(QObject *object, QEvent *event) override;
};
} // namespace nova
