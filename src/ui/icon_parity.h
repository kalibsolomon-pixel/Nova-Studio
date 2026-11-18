// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// "Compare Icons with Minecraft": the running test client draws inventory icons through Nova's
// companion mod, Nova draws the same items, and each pair is compared pixel by pixel. The game's
// images stay in Nova's test instance; nothing is written into the pack.
#include <QColor>
#include <QDialog>
#include <QImage>
#include <functional>
#include <memory>

class QJsonObject;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
namespace nova {
namespace mc {
class Library;
}
namespace testing {
class CompanionLink;
}

struct IconComparison {
    QImage nova, minecraft, difference; // nova is composited on the slot background
    int differing = 0, total = 0, maxDelta = 0;
    bool comparable() const { return total > 0; }
    bool matches() const { return comparable() && differing == 0; }
};
// Pixels differ when any channel differs by more than tolerance. Sizes must match.
IconComparison compareIcons(const QImage &nova, const QImage &minecraft, const QColor &background,
                            int tolerance = 2);
// Vanilla items that exercise each part of the renderer (blocks, cutout, translucency, tints,
// generated and handheld items, special models).
QStringList defaultParityItems();

class IconParityDialog : public QDialog {
    Q_OBJECT
  public:
    struct Context {
        testing::CompanionLink *companion = nullptr;
        std::function<std::shared_ptr<mc::Library>()> library;
        std::function<QString()> projectRoot;
        std::function<QString()> folder; // where the game saved its icons
    };
    explicit IconParityDialog(Context context, QWidget *parent = nullptr);
    void compare();

  private:
    void finished(bool ok, const QJsonObject &result, const QString &error);
    void usePackItems();
    QStringList items() const;
    Context context;
    QPlainTextEdit *itemList;
    QPushButton *compareButton;
    QLabel *status;
    QTableWidget *results;
    QStringList requested;
};

} // namespace nova
