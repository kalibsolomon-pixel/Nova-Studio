// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <memory>

namespace nova::mcp {
bool logicalAsset(const QString &path);

// Why a destination is or is not vacant. Missing, Denied and Unsafe name the first folder (or the
// file itself) that failed; Unsafe covers links, junctions, non-folders and escaped or invalid paths.
enum class Vacancy { Vacant, Occupied, Missing, Denied, Unsafe };

class Scope {
    struct Handles;
    std::unique_ptr<Handles> handles;
    Vacancy checkedVacancy(const QString &relative, QString *failed = nullptr);
    std::unique_ptr<QIODevice> openChecked(const QString &relative);
  public:
    explicit Scope(const QString &root, const QByteArray &identity = {});
    ~Scope();
    QByteArray identity() const;
    bool vacant(const QString &relative) { return vacancy(relative) == Vacancy::Vacant; }
    Vacancy vacancy(const QString &relative, QString *failed = nullptr);
    std::unique_ptr<QIODevice> open(const QString &relative);
    // Internal editor sidecars only. Their paths are derived from a scoped texture.
    std::unique_ptr<QIODevice> openEditableDocument(const QString &relative, Vacancy &status);
};
}
